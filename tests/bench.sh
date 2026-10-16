#!/bin/bash
# Throughput benchmark of the complete stack (engine + NFS server + macOS
# NFS client) on image files. Runs unprivileged.
#
#   tests/bench.sh [ext4|ntfs|all] [size-mb]
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORK="$ROOT/build/test/bench"
SIZE_MB="${2:-2000}"
OPTS="vers=3,tcp,locallocks,nfc,soft,intr,rsize=1048576,wsize=1048576,readahead=32"

mkdir -p "$WORK"
cd "$WORK" || exit 1

# Incompressible, non-zero test data (zeros are optimised into holes).
if [ ! -f rand.bin ] || [ "$(stat -f %z rand.bin)" -ne $((SIZE_MB * 1048576)) ]; then
	dd if=/dev/urandom of=rand.bin bs=1m count="$SIZE_MB" 2>/dev/null
fi

mk_image() {
	local fs=$1 img=$2
	rm -f "$img"
	mkfile -n 8g "$img"
	case $fs in
	ext4) "$ROOT/build/vendor/e2fsprogs/misc/mke2fs" -q -F -t ext4 "$img" ;;
	ntfs) "$ROOT/build/vendor/ntfs-3g/ntfsprogs/mkntfs" -q -F -Q "$img" >/dev/null 2>&1 ;;
	esac
}

start_worker() {
	local img=$1
	"$BIN/fsmac-worker" --insecure "$img" > worker.status 2> worker.log &
	WORKER=$!
	for _ in $(seq 50); do
		grep -q ready worker.status 2>/dev/null && break
		sleep 0.1
	done
	PORT=$(sed -n 's/.*"port":\([0-9]*\).*/\1/p' worker.status | head -1)
}

nfs_mount() {
	mkdir -p mnt
	/sbin/mount_nfs -o "$OPTS,port=$PORT,mountport=$PORT" 127.0.0.1:/ mnt
}

mbps() { # bytes seconds
	python3 -c "print(f'{$1/1048576/$2:.0f} MB/s')"
}

timed() { # command...
	local t0 t1
	t0=$(python3 -c 'import time;print(time.time())')
	"$@" >/dev/null 2>&1
	t1=$(python3 -c 'import time;print(time.time())')
	python3 -c "print(f'{$t1-$t0:.2f}')"
}

bench() {
	local fs=$1 t
	local img="$WORK/$fs.img"
	echo "== $fs"
	mk_image "$fs" "$img"
	start_worker "$img"
	nfs_mount || { echo "mount failed"; kill $WORKER; return 1; }

	cat rand.bin > /dev/null # warm the source in the page cache
	t=$(timed python3 -c '
import os
src = open("rand.bin", "rb")
fd = os.open("mnt/seq.bin", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
while True:
    b = src.read(1 << 20)
    if not b:
        break
    os.write(fd, b)
os.fsync(fd)
os.close(fd)')
	echo "  sequential write ${SIZE_MB} MB: ${t}s ($(mbps $((SIZE_MB * 1048576)) "$t"))"
	umount mnt; nfs_mount   # drop client caches
	t=$(timed dd if=mnt/seq.bin of=/dev/null bs=1m)
	echo "  sequential read  ${SIZE_MB} MB: ${t}s ($(mbps $((SIZE_MB * 1048576)) "$t"))"
	cmp -s rand.bin mnt/seq.bin && echo "  data verified" || echo "  DATA MISMATCH"

	mkdir -p mnt/small
	t=$(timed sh -c 'for i in $(seq 1 2000); do echo "$i" > mnt/small/f$i; done')
	echo "  create 2000 small files: ${t}s"
	umount mnt; nfs_mount
	t=$(timed ls -lR mnt/small)
	echo "  list 2000 files (cold): ${t}s"
	t=$(timed rm -rf mnt/small)
	echo "  delete 2000 files: ${t}s"

	umount mnt
	kill -TERM $WORKER; wait $WORKER
	case $fs in
	ext4) "$ROOT/build/vendor/e2fsprogs/e2fsck/e2fsck" -fn "$img" >/dev/null 2>&1 \
		&& echo "  e2fsck: clean" || echo "  e2fsck: ERRORS" ;;
	ntfs) "$ROOT/build/vendor/ntfs-3g/ntfsprogs/ntfsfix" -n "$img" >/dev/null 2>&1 \
		&& echo "  ntfsfix: clean" || echo "  ntfsfix: ERRORS" ;;
	esac
	rm -f "$img"
}

case "${1:-all}" in
ext4) bench ext4 ;;
ntfs) bench ntfs ;;
all) bench ext4; bench ntfs ;;
esac
