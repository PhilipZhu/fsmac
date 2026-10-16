#!/bin/bash
# Runs tests/fsx.py through the macOS NFS client on ext4 and NTFS images.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/bin"
WORK="$ROOT/build/test/fsx"
OPTS="vers=3,tcp,locallocks,nfc,hard,intr,rsize=1048576,wsize=1048576,readahead=32,nordirplus,noresvport"
PROCS=${PROCS:-6}
OPS=${OPS:-3000}
RC=0
mkdir -p "$WORK" && cd "$WORK" || exit 1

for fs in ext4 ntfs; do
    rm -f $fs.img; mkfile -n 512m $fs.img
    if [ $fs = ntfs ]; then "$BIN/mkntfs" -Q -F -q $fs.img >/dev/null 2>&1
    else "$BIN/mke2fs" -q -F -t ext4 $fs.img; fi
    "$BIN/fsmac-worker" --insecure $fs.img > status 2> worker.log &
    W=$!
    for _ in $(seq 50); do grep -q '"event":"ready"' status 2>/dev/null && break; sleep 0.1; done
    PORT=$(sed -n 's/.*"port":\([0-9]*\).*/\1/p' status | head -1)
    mkdir -p mnt
    /sbin/mount_nfs -o "$OPTS,port=$PORT,mountport=$PORT" 127.0.0.1:/ mnt || { RC=1; kill $W; continue; }
    echo "== $fs: $PROCS processes x $OPS operations"
    python3 -I "$ROOT/tests/fsx.py" mnt --procs "$PROCS" --ops "$OPS" --seed 1 || RC=1
    umount mnt; kill -TERM $W; wait $W
    if [ $fs = ntfs ]; then "$BIN/ntfsfix" -n $fs.img >/dev/null || RC=1
    else "$BIN/e2fsck" -fn $fs.img >/dev/null || RC=1; fi
    rm -f $fs.img
done
exit $RC
