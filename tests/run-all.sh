#!/bin/bash
# Runs the automated test suite (unprivileged; uses disk images).
#
#   tests/run-all.sh [quick]
#
# 1. block cache randomized test (AddressSanitizer)
# 2. engine stress test against a host directory, all filesystem variants
# 3. end-to-end tests through the macOS NFS client
# 4. concurrent fsx-style exerciser through the macOS NFS client
# 5. daemon lifecycle test (auto-mount, eject, crash recovery)
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORK="$ROOT/build/test"
QUICK=${1:-}
FAILED=()

mkdir -p "$WORK"
section() { printf '\n\033[1m### %s\033[0m\n' "$*"; }
result() { if [ "$2" -eq 0 ]; then printf '\033[32mok\033[0m   %s\n' "$1"; else printf '\033[31mFAIL\033[0m %s\n' "$1"; FAILED+=("$1"); fi; }

section "Block cache"
iters=${QUICK:+20000}; iters=${iters:-200000}
if clang -O1 -g -fsanitize=address -DFSC_BLOCKDEV_DEBUG -I"$ROOT/engine/include" \
        "$ROOT/tests/blockdev-test.c" "$ROOT/engine/blockdev.c" -o "$WORK/blockdev-test"; then
    head -c 33554432 /dev/urandom > "$WORK/blockdev.img"
    # sector size, cache size: tiny caches force constant eviction.
    for cfg in "512 65536" "4096 65536" "512 1048576" "4096 8388608"; do
        set -- $cfg
        "$WORK/blockdev-test" "$WORK/blockdev.img" "$1" "$2" "$iters" 7 >"$WORK/blockdev-test.log" 2>&1
        result "blockdev randomized test (sector $1, cache $(($2 / 1024)) KB)" $?
    done
    rm -f "$WORK/blockdev.img"
else
    result "blockdev test build" 1
fi

section "Engine stress (differential against APFS)"
ops=${QUICK:+3000}; ops=${ops:-20000}
variants=(
    "ext4|-t ext4" "ext4-1k|-t ext4 -b 1024" "ext4-inline|-t ext4 -O inline_data"
    "ext4-bigalloc|-t ext4 -O bigalloc -C 16384" "ext3|-t ext3" "ext2|-t ext2"
    "ntfs-4k|-c 4096" "ntfs-512|-c 512" "ntfs-64k|-c 65536"
)
for v in "${variants[@]}"; do
    name=${v%%|*}; opts=${v#*|}
    img="$WORK/stress-$name.img"
    rm -f "$img"; mkfile -n 256m "$img"
    if [[ $name == ntfs* ]]; then
        "$BIN/mkntfs" -Q -F -q $opts "$img" >/dev/null 2>&1
        check=("$BIN/ntfsfix" -n "$img")
    else
        "$BIN/mke2fs" -q -F $opts "$img" >/dev/null 2>&1
        check=("$BIN/e2fsck" -fn "$img")
    fi
    mkdir -p "$WORK/stress-host-$name"
    "$BIN/fsmac-shell" "$img" stress "$WORK/stress-host-$name" "$ops" 42 >"$WORK/stress-$name.log" 2>&1 &&
        "${check[@]}" >>"$WORK/stress-$name.log" 2>&1
    result "stress $name ($ops ops)" $?
    rm -f "$img"; rm -rf "$WORK/stress-host-$name"
done

section "End-to-end (macOS NFS client)"
"$ROOT/tests/e2e.sh" all >"$WORK/e2e.log" 2>&1
rc=$?
grep -E "PASS|FAIL" "$WORK/e2e.log" | sed 's/^/    /'
result "e2e" $rc

if [ -z "$QUICK" ]; then
    section "Concurrent exerciser (fsx)"
    "$ROOT/tests/fsx-run.sh" >"$WORK/fsx.log" 2>&1
    result "fsx ext4 + ntfs" $?
fi

section "Daemon lifecycle"
"$ROOT/tests/daemon-test.sh" >"$WORK/daemon-test.log" 2>&1
rc=$?
grep -E "PASS|FAIL" "$WORK/daemon-test.log" | sed 's/^/    /'
result "daemon lifecycle" $rc

echo
if [ ${#FAILED[@]} -eq 0 ]; then
    printf '\033[32mAll tests passed.\033[0m Logs: %s\n' "$WORK"
else
    printf '\033[31m%d failed:\033[0m %s\nLogs: %s\n' "${#FAILED[@]}" "${FAILED[*]}" "$WORK"
    exit 1
fi
