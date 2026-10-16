#!/bin/bash
# Daemon lifecycle test: runs fsmacd in user mode against attached disk
# images and drives it with the fsmac CLI.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORK="$ROOT/build/test/daemon"
VOLS="$HOME/FSMacVolumes"
FAIL=0
pass() { printf '  \033[32mPASS\033[0m %s\n' "$*"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=1; }
check() { local what=$1; shift; if "$@" >/dev/null 2>&1; then pass "$what"; else fail "$what"; fi; }
state() { "$BIN/fsmac" list --json | python3 -I -c "import json,sys; v=[x for x in json.load(sys.stdin) if x['label']==sys.argv[1]]; print(v[0]['state'] if v else 'absent')" "$1"; }
wait_state() { for _ in $(seq 100); do [ "$(state "$1")" = "$2" ] && return 0; sleep 0.1; done; return 1; }
attach() { hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$1" 2>/dev/null | awk '{print $1; exit}' | sed 's#/dev/##'; }

mkdir -p "$WORK" && cd "$WORK" || exit 1
if pgrep -u "$(id -u)" -x fsmacd >/dev/null; then echo "a user-mode fsmacd is already running; stop it first"; exit 1; fi
if pgrep -u 0 -x fsmacd >/dev/null; then
    echo "SKIP: the installed FSMac helper is running and would take over the test disks."
    echo "      Stop it to run this test: sudo launchctl bootout system/com.fsmac.daemon"
    exit 0
fi
# Talk to the test helper even if the system helper is installed.
export FSMAC_SOCKET="$HOME/Library/Application Support/FSMac/fsmac.sock"
rm -f ntfs.img ext4.img
mkfile -n 256m ntfs.img; "$BIN/mkntfs" -Q -F -q -L DTestNTFS ntfs.img >/dev/null 2>&1
mkfile -n 256m ext4.img; "$BIN/mke2fs" -q -F -t ext4 -L DTestExt ext4.img

"$BIN/fsmacd" --user-mode > fsmacd.log 2>&1 &
D=$!
trap 'kill -TERM $D 2>/dev/null; wait $D 2>/dev/null; for d in ${N:-} ${E:-}; do hdiutil detach -force $d >/dev/null 2>&1; done' EXIT
for _ in $(seq 50); do "$BIN/fsmac" status >/dev/null 2>&1 && break; sleep 0.1; done
check "daemon answers" "$BIN/fsmac" status

N=$(attach ntfs.img); E=$(attach ext4.img)
check "NTFS volume auto-mounted" wait_state DTestNTFS mounted
check "ext4 volume auto-mounted" wait_state DTestExt mounted

head -c 20000000 /dev/urandom > data.bin
cp data.bin "$VOLS/DTestNTFS/" && cp data.bin "$VOLS/DTestExt/"
check "unmount (CLI)" "$BIN/fsmac" unmount DTestNTFS
check "mount (CLI)" "$BIN/fsmac" mount DTestNTFS
check "data intact after remount" cmp data.bin "$VOLS/DTestNTFS/data.bin"
# A volume with an open file is not unmounted behind the user's back.
sleep 30 3<"$VOLS/DTestNTFS/data.bin" &
H=$!
sleep 0.5
if "$BIN/fsmac" unmount DTestNTFS 2>busy.txt; then fail "unmount refused while a file is open"
elif grep -q "in use" busy.txt; then pass "unmount refused while a file is open ($(cat busy.txt | sed 's/^fsmac: //'))"
else fail "unmount refused while a file is open: $(cat busy.txt)"; fi
check "still mounted after the refused unmount" wait_state DTestNTFS mounted
kill $H 2>/dev/null; wait $H 2>/dev/null
check "read-only mount" "$BIN/fsmac" mount DTestExt --read-only
"$BIN/fsmac" unmount DTestExt >/dev/null; "$BIN/fsmac" mount DTestExt --read-only >/dev/null
if touch "$VOLS/DTestExt/x" 2>/dev/null; then fail "read-only mount refuses writes"; else pass "read-only mount refuses writes"; fi
"$BIN/fsmac" unmount DTestExt >/dev/null; "$BIN/fsmac" mount DTestExt >/dev/null

pkill -9 -f "fsmac-worker.*DTestExt"
check "worker crash detected" wait_state DTestExt error
check "remount after crash (auto fsck)" "$BIN/fsmac" mount DTestExt
check "data intact after crash" cmp data.bin "$VOLS/DTestExt/data.bin"
check "check (e2fsck -n)" "$BIN/fsmac" check DTestExt
check "check (ntfsfix -n)" "$BIN/fsmac" check DTestNTFS
check "mounted again after check" wait_state DTestNTFS mounted

# Two volumes with the same name get separate mount points.
rm -f same1.img same2.img
mkfile -n 64m same1.img; "$BIN/mke2fs" -q -F -t ext4 -L Same same1.img
mkfile -n 64m same2.img; "$BIN/mke2fs" -q -F -t ext4 -L Same same2.img
S1=$(attach same1.img); S2=$(attach same2.img)
mountpoints() { "$BIN/fsmac" list --json | python3 -I -c "import json,sys; print(len({v.get('mountPoint') for v in json.load(sys.stdin) if v['label']=='Same' and v['state']=='mounted'}))"; }
for _ in $(seq 100); do [ "$(mountpoints)" = 2 ] && break; sleep 0.1; done
check "same-named volumes mounted separately" test "$(mountpoints)" = 2
"$BIN/fsmac" eject "$S1" >/dev/null; "$BIN/fsmac" eject "$S2" >/dev/null
rm -f same1.img same2.img

# A Finder-style unmount must not eject a disk that has other volumes.
rm -f mixed.img; mkfile -n 128m mixed.img
M=$(attach mixed.img)
diskutil partitionDisk "$M" GPT ExFAT MIXA 50% ExFAT MIXB 50% >/dev/null 2>&1
"$BIN/fsmac" format "${M}s1" --type ntfs --label MixNTFS --yes >/dev/null 2>&1
check "format a partition next to an exFAT volume" wait_state MixNTFS mounted
umount "$VOLS/MixNTFS"
check "unmounted volume reported" wait_state MixNTFS unmounted
sleep 1
check "disk with another mounted volume not ejected" diskutil info "$M"
check "the other volume stays mounted" bash -c "mount | grep -q MIXB"
hdiutil detach -force "$M" >/dev/null 2>&1
rm -f mixed.img

umount "$VOLS/DTestExt"
check "external unmount handled" wait_state DTestExt absent
check "disk ejected after Finder-style unmount" bash -c "! diskutil info $E >/dev/null 2>&1"
E=
check "diskutil eject unmounts cleanly" diskutil eject "$N"
check "volume gone after eject" wait_state DTestNTFS absent
N=
check "NTFS image clean" "$BIN/ntfsfix" -n ntfs.img
check "ext4 image clean" "$BIN/e2fsck" -fn ext4.img

kill -TERM $D; wait $D
check "daemon stops cleanly" grep -q "fsmacd stopped" fsmacd.log
rm -f ntfs.img ext4.img data.bin
exit $FAIL
