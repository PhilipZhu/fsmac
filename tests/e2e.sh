#!/bin/bash
# End-to-end test: engine + NFS server + the real macOS NFS client.
# Runs unprivileged on disk images.
#
#   tests/e2e.sh [ext4|ntfs|all]
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
VB="$ROOT/build/vendor"
WORK="$ROOT/build/test/e2e"
OPTS="vers=3,tcp,locallocks,nfc,soft,intr,rsize=1048576,wsize=1048576,readahead=32,nordirplus"
FAIL=0

pass() { printf '  \033[32mPASS\033[0m %s\n' "$*"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$*"; FAIL=1; }

mkdir -p "$WORK"
cd "$WORK" || exit 1

make_source_tree() {
	local s=src
	rm -rf $s; mkdir -p $s/docs/nested/deeper $s/media $s/empty-dir
	printf '' > $s/empty.txt
	echo "hello world" > $s/hello.txt
	head -c 4095 /dev/urandom > $s/b4095.bin
	head -c 4096 /dev/urandom > $s/b4096.bin
	head -c 4097 /dev/urandom > $s/b4097.bin
	head -c 1048577 /dev/urandom > $s/media/1mb_plus.bin
	head -c 10000000 /dev/urandom > $s/media/10mb.bin
	echo "unicode" > "$s/docs/Ünïcødé 日本語 😀.txt"
	echo "nfd" > "$s/docs/$(printf 'e\xcc\x81')clair.txt"
	echo "q" > "$s/docs/what? *star* \"quote\" <lt> |pipe|.txt"
	echo "c" > "$s/docs/colon:name.txt"
	echo "trail" > "$s/docs/trailing dot."
	echo "sp" > "$s/docs/trailing space "
	echo "Case" > "$s/docs/CaseTest.txt"
	echo "deep" > $s/docs/nested/deeper/deep.txt
	ln -s hello.txt $s/link-to-hello
	ln -s docs/nested $s/link-to-dir
	ln $s/hello.txt $s/hardlink.txt
	xattr -w com.example.small "small value" $s/hello.txt
	xattr -w com.apple.metadata:_kMDItemUserTags '("Red\n6","Work")' $s/hello.txt
	xattr -w com.apple.quarantine "0083;6500000;Safari;" $s/b4096.bin
	head -c 3000 /dev/urandom | base64 > /tmp/fsmac_x.txt
	xattr -w com.example.medium "$(cat /tmp/fsmac_x.txt)" $s/b4097.bin
	xattr -w com.example.dirattr "dir" $s/docs
	head -c 100000 /dev/urandom > /tmp/fsmac_rsrc.bin
	xattr -wx com.apple.ResourceFork "$(xxd -p /tmp/fsmac_rsrc.bin | tr -d '\n')" $s/media/10mb.bin
	xattr -wx com.apple.FinderInfo "54455854747478740000000000000000 00000000000000000000000000000000" $s/docs/CaseTest.txt 2>/dev/null || true
}

nfc() { python3 -c 'import sys,unicodedata; [print(unicodedata.normalize("NFC", l), end="") for l in sys.stdin]'; }

# Prints "path<TAB>xattr-name<TAB>md5(value)" for every xattr in a tree,
# ignoring attributes the system rewrites on copy (quarantine, provenance)
# and hard-linked files (the macOS NFS client resolves AppleDouble names
# through one of the link names only).
xattr_manifest() {
	(cd "$1" && find . -print0 | sort -z | while IFS= read -r -d '' f; do
		[ -L "$f" ] && continue
		[ -f "$f" ] && [ "$(stat -f %l "$f")" -gt 1 ] && continue
		xattr "$f" 2>/dev/null | sort | while IFS= read -r n; do
			case $n in com.apple.provenance|com.apple.lastuseddate*|com.apple.quarantine) continue;; esac
			printf '%s\t%s\t%s\n' "$f" "$n" "$(xattr -px "$n" "$f" | md5)"
		done
	done)
}

tree_manifest() { # names, types, sizes, link targets, content digests
	(cd "$1" && find . -print0 | sort -z | while IFS= read -r -d '' f; do
		case "$f" in */._*) continue;; esac # spilled attribute files
		if [ -L "$f" ]; then printf 'l %s -> %s\n' "$f" "$(readlink "$f")"
		elif [ -d "$f" ]; then printf 'd %s\n' "$f"
		else printf 'f %s %s %s\n' "$f" "$(stat -f %z "$f")" "$(md5 -q "$f")"; fi
	done)
}

start() {
	"$BIN/fsmac-worker" --insecure -v "$1" > worker.status 2> worker.log &
	WORKER=$!
	for _ in $(seq 50); do grep -q '"event":"ready"' worker.status 2>/dev/null && break; sleep 0.1; done
	PORT=$(sed -n 's/.*"port":\([0-9]*\).*/\1/p' worker.status | head -1)
}
mnt() { mkdir -p mnt; /sbin/mount_nfs -o "$OPTS,port=$PORT,mountport=$PORT" 127.0.0.1:/ mnt; }
stop() { umount mnt 2>/dev/null; kill -TERM $WORKER 2>/dev/null; wait $WORKER 2>/dev/null; }

run() {
	local fs=$1
	local img="$WORK/$fs.img"
	echo "== $fs"
	rm -f "$img"; mkfile -n 1g "$img"
	case $fs in
	ext4) "$VB/e2fsprogs/misc/mke2fs" -q -F -t ext4 -L E2E "$img" ;;
	ntfs) "$VB/ntfs-3g/ntfsprogs/mkntfs" -q -F -Q -L E2E "$img" >/dev/null 2>&1 ;;
	esac
	start "$img"; mnt || { fail "mount"; stop; return; }

	ditto src mnt/copy && pass "ditto copy" || fail "ditto copy"
	[ "$(tree_manifest src | nfc | sort)" == "$(tree_manifest mnt/copy | nfc | sort)" ] && pass "tree + content identical" \
		|| { fail "tree differs"; diff <(tree_manifest src | nfc | sort) <(tree_manifest mnt/copy | nfc | sort) | head -20; }
	[ "$(xattr_manifest src | nfc | sort)" == "$(xattr_manifest mnt/copy | nfc | sort)" ] && pass "extended attributes identical" \
		|| { fail "xattrs differ"; diff <(xattr_manifest src | nfc | sort) <(xattr_manifest mnt/copy | nfc | sort) | head -20; }
	[ "$(xattr -p com.apple.quarantine mnt/copy/b4096.bin 2>/dev/null)" != "" ] && pass "quarantine kept" || fail "quarantine lost"
	[ "$(stat -f %i mnt/copy/hello.txt)" == "$(stat -f %i mnt/copy/hardlink.txt)" ] \
		&& pass "hard link preserved" || fail "hard link not preserved (ditto may copy)"

	# Namespace operations through the kernel
	mv mnt/copy/docs mnt/copy/documents && [ -f "mnt/copy/documents/nested/deeper/deep.txt" ] \
		&& pass "rename directory" || fail "rename directory"
	mv mnt/copy/hello.txt mnt/copy/media/ && [ "$(xattr -p com.example.small mnt/copy/media/hello.txt)" == "small value" ] \
		&& pass "move keeps xattrs" || fail "move keeps xattrs"
	echo new > mnt/copy/overwrite.txt; echo old > mnt/copy/old.txt
	mv -f mnt/copy/old.txt mnt/copy/overwrite.txt && [ "$(cat mnt/copy/overwrite.txt)" == "old" ] \
		&& pass "rename over existing" || fail "rename over existing"
	rm -rf mnt/copy/media && [ ! -e mnt/copy/media ] && pass "recursive delete" || fail "recursive delete"
	xattr -d com.example.dirattr mnt/copy/documents && ! xattr -p com.example.dirattr mnt/copy/documents 2>/dev/null \
		&& pass "remove xattr" || fail "remove xattr"
	if [ "$fs" == ntfs ]; then
		echo x > mnt/copy/casetest; ls mnt/copy/CASETEST >/dev/null 2>&1 \
			&& pass "case-insensitive lookup" || fail "case-insensitive lookup"
		mv mnt/copy/casetest mnt/copy/CaseTest2 && ls mnt/copy | grep -qx CaseTest2 \
			&& pass "case-only rename" || fail "case-only rename"
	fi
	dd if=/dev/urandom of=mnt/copy/rand.bin bs=1m count=64 2>/dev/null
	local h1; h1=$(md5 -q mnt/copy/rand.bin)

	# Remount and verify persistence
	umount mnt || echo "    UMOUNT FAILED"; echo "    mounts now: $(mount | grep -c e2e/mnt)"; mnt
	[ "$(md5 -q mnt/copy/rand.bin)" == "$h1" ] && pass "data persists across remount" || fail "data after remount"
	local m1 m2 tg
	m1=$(xattr -p com.example.medium "mnt/copy/b4097.bin" 2>&1 | md5)
	m2=$(xattr -p com.example.medium src/b4097.bin 2>&1 | md5)
	tg=$(xattr -p com.apple.metadata:_kMDItemUserTags "mnt/copy/hardlink.txt" 2>&1)
	[ "$m1" == "$m2" ] && [ -n "$tg" ] && pass "xattrs persist across remount" \
		|| { fail "xattrs after remount"; echo "    medium $m1 vs $m2; tags: $tg"; }
	stop
	grep -q '"event":"stopped","clean":true' worker.status && pass "clean shutdown" || fail "clean shutdown"

	# On-disk checks
	local junk
	junk=$("$BIN/fsmac-shell" -r "$img" tree / 2>/dev/null | grep -c '/\._' || true)
	if [ "$fs" == ext4 ]; then
		[ "$junk" -eq 0 ] && pass "no AppleDouble junk on disk" || fail "$junk ._ files on disk"
		"$VB/e2fsprogs/e2fsck/e2fsck" -fn "$img" >/dev/null 2>&1 && pass "e2fsck clean" || fail "e2fsck reports errors"
	else
		[ "$junk" -eq 0 ] && pass "no AppleDouble junk on disk" || fail "$junk ._ files on disk"
		"$VB/ntfs-3g/ntfsprogs/ntfsfix" -n "$img" >/dev/null 2>&1 && pass "ntfsfix clean" || fail "ntfsfix reports errors"
		# Independent reader: Apple's NTFS driver must see the same data.
		local dev
		dev=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$img" 2>/dev/null | awk '{print $1}' | head -1)
		mkdir -p "$WORK/apple"
		if [ -n "$dev" ] && diskutil mount readOnly -mountPoint "$WORK/apple" "$dev" >/dev/null 2>&1; then
			[ "$(cat "$WORK/apple/copy/documents/Ünïcødé 日本語 😀.txt" 2>/dev/null)" == "unicode" ] \
				&& [ -f "$WORK/apple/copy/documents/what? *star* \"quote\" <lt> |pipe|.txt" ] \
				&& pass "Apple NTFS driver reads names" || fail "Apple NTFS driver names"
			[ "$(xattr -p com.apple.metadata:_kMDItemUserTags "$WORK/apple/copy/hardlink.txt" 2>/dev/null)" != "" ] \
				&& pass "Apple NTFS driver reads xattrs" || fail "Apple NTFS driver xattrs"
			[ "$(md5 -q "$WORK/apple/copy/rand.bin")" == "$h1" ] && pass "Apple NTFS driver reads data" \
				|| fail "Apple NTFS driver data"
			diskutil unmount "$WORK/apple" >/dev/null 2>&1
		else
			fail "could not attach image with Apple's driver"
		fi
		[ -n "$dev" ] && hdiutil detach "$dev" >/dev/null 2>&1
	fi
	grep -E '"level":"error"' worker.status | head -5
}

make_source_tree
case "${1:-all}" in
ext4|ntfs) run "$1" ;;
all) run ext4; run ntfs ;;
esac
exit $FAIL
