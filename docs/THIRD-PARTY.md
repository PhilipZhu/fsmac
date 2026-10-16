# Third-party software in FSMac

FSMac is distributed under the GNU General Public License, version 2 (see
`LICENSE`). It includes the following projects, unmodified apart from build
configuration. Their complete source code is in `vendor/`.

## ntfs-3g

- Source: https://github.com/tuxera/ntfs-3g (commit 7f0f841f, 2026-09-28)
- Used: `libntfs-3g` (the NTFS implementation inside `fsmac-worker` and
  `fsmac-probe`), and the `mkntfs`, `ntfsfix` and `ntfslabel` tools.
- License: GNU GPL v2 or later. Copyright the ntfs-3g / Linux-NTFS
  contributors (Szabolcs Szakacsits, Jean-Pierre André, Anton Altaparmakov,
  Richard Russon, Yura Pakhuchiy and others).

## e2fsprogs

- Source: https://git.kernel.org/pub/scm/fs/ext2/e2fsprogs.git (v1.47.4,
  commit 7ee1d505)
- Used: `libext2fs`, `libe2p`, `libblkid`, `libuuid`, `libcom_err` and the
  journal recovery code (inside `fsmac-worker` and `fsmac-probe`), and the
  `mke2fs`, `e2fsck` and `tune2fs` tools.
- License: GNU GPL v2; `lib/ext2fs` and `lib/e2p` under the GNU LGPL v2;
  `lib/uuid` under a BSD-style license; `lib/et` and `lib/ss` under an
  MIT-style license. Copyright Theodore Ts'o and contributors.
- Some timestamp helpers in `engine/ext4.c` follow e2fsprogs' `fuse2fs`
  (GPL v2, Darrick J. Wong).

## Apple

The AppleDouble layout produced by `nfs/appledouble.c` mirrors the files
written by the macOS kernel (xnu, APSL); no Apple code is included.
