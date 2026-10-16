#!/usr/bin/env python3
"""fsx-style randomised file exerciser run through a mounted filesystem.

Each worker process owns a few files and performs random pwrite / pread /
truncate / rename / fsync / append operations, verifying every read and,
at the end, every file against an in-memory model.

    fsx.py <directory> [--procs N] [--ops N] [--seed S]
"""
import argparse
import multiprocessing
import os
import random
import sys

MAX_SIZE = 6 << 20


def worker(directory, wid, ops, seed):
    rnd = random.Random(seed * 1000 + wid)
    names = [os.path.join(directory, f"w{wid}-f{i}") for i in range(3)]
    model = {n: bytearray() for n in names}
    for n in names:
        open(n, "wb").close()
    fds = {n: os.open(n, os.O_RDWR) for n in names}
    errors = 0

    def check(n, off, size, why):
        nonlocal errors
        got = os.pread(fds[n], size, off)
        want = bytes(model[n][off:off + size])
        if got != want:
            errors += 1
            first = next((i for i in range(min(len(got), len(want)))
                          if got[i] != want[i]), min(len(got), len(want)))
            print(f"[w{wid}] MISMATCH after {why}: {os.path.basename(n)} "
                  f"off={off} size={size} got={len(got)} want={len(want)} "
                  f"first-diff=+{first}", flush=True)
            return False
        return True

    for i in range(ops):
        n = rnd.choice(names)
        m = model[n]
        op = rnd.random()
        if op < 0.35:  # write
            off = rnd.randrange(0, MAX_SIZE)
            size = rnd.choice([1, 17, 512, 4095, 4096, 4097, 65536,
                               rnd.randrange(1, 400000)])
            if off + size > MAX_SIZE:
                size = MAX_SIZE - off
            data = rnd.randbytes(size)
            os.pwrite(fds[n], data, off)
            if off > len(m):
                m.extend(b"\0" * (off - len(m)))
            m[off:off + size] = data
            if rnd.random() < 0.1:
                check(n, off, size, "write")
        elif op < 0.45:  # truncate
            size = rnd.randrange(0, MAX_SIZE)
            os.ftruncate(fds[n], size)
            if size < len(m):
                del m[size:]
            else:
                m.extend(b"\0" * (size - len(m)))
        elif op < 0.55:  # append
            data = rnd.randbytes(rnd.randrange(1, 70000))
            os.pwrite(fds[n], data, len(m))
            m.extend(data)
        elif op < 0.80:  # read
            if len(m):
                off = rnd.randrange(0, len(m))
                size = rnd.randrange(1, min(len(m) - off, 300000) + 1)
                check(n, off, size, "read")
        elif op < 0.85:  # size
            st = os.fstat(fds[n])
            if st.st_size != len(m):
                errors += 1
                print(f"[w{wid}] SIZE {os.path.basename(n)} {st.st_size} != {len(m)}",
                      flush=True)
        elif op < 0.90:  # fsync
            os.fsync(fds[n])
        elif op < 0.95:  # close + reopen (drops client state)
            os.close(fds[n])
            fds[n] = os.open(n, os.O_RDWR)
        else:  # rename between two names of this worker
            tmp = n + ".tmp"
            os.close(fds[n])
            os.rename(n, tmp)
            os.rename(tmp, n)
            fds[n] = os.open(n, os.O_RDWR)
        if errors > 5:
            break

    for n in names:
        os.fsync(fds[n])
        os.close(fds[n])
        with open(n, "rb") as f:
            data = f.read()
        if data != bytes(model[n]):
            errors += 1
            print(f"[w{wid}] FINAL mismatch {os.path.basename(n)} "
                  f"({len(data)} vs {len(model[n])})", flush=True)
    return errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("directory")
    ap.add_argument("--procs", type=int, default=4)
    ap.add_argument("--ops", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    os.makedirs(a.directory, exist_ok=True)
    with multiprocessing.Pool(a.procs) as pool:
        res = pool.starmap(worker, [(a.directory, w, a.ops, a.seed)
                                    for w in range(a.procs)])
    total = sum(res)
    print(f"fsx: {a.procs} processes x {a.ops} ops, {total} errors")
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
