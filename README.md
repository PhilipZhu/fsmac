# FSMac

**Read and write NTFS and Linux ext2/ext3/ext4 disks on your Mac.**

Connect a Windows or Linux disk and it appears in Finder, ready to use: copy,
edit, rename and delete files the same way as on any Mac disk. FSMac lives in
the menu bar, mounts supported disks automatically and ejects them safely.

- **NTFS read & write.** Uses ntfs-3g, the most widely used NTFS
  implementation outside Windows. File names with characters Windows forbids
  (`? : * " < > |`) are stored as look-alikes, the same way macOS and Windows'
  Services for Mac do, so Windows can open them. Files starting with "." are
  hidden from Windows. Hibernated Windows volumes (Fast Startup) are mounted
  read-only to protect them, with a one-click way to discard the hibernation
  data.
- **ext2/ext3/ext4 read & write.** Uses e2fsprogs' libext2fs, with fast
  extent-based I/O. Journals left by Linux are replayed. Volumes that were not
  unmounted cleanly are checked and repaired with `e2fsck` before mounting, as
  Linux does at boot.
- **Finder works.** Extended attributes, Finder tags and colors, resource
  forks, the "hidden" flag and Gatekeeper quarantine are kept. They are stored
  natively (NTFS named streams, ext4 `user.*` attributes), so copies on Windows
  and Linux keep them too, and there is no `._` clutter.
- **Safe ejecting.** Eject from Finder, Disk Utility, the menu bar or the
  command line. Data is flushed and the disk is marked clean before it is
  released. If a disk is unplugged without ejecting, FSMac cleans up and tells
  you.
- **Disk tools.** Check and repair volumes (`e2fsck`, `ntfsfix`), and erase
  disks or partitions as NTFS or ext4.
- **Per-volume preferences.** Read & write, read-only, or ignore. Choose
  whether to mount automatically.
- **Command-line tool** (`fsmac`) for scripts.

Speed on an Apple-silicon Mac (disk image on internal SSD, through Finder's
file system path): about 4.7 GB/s sequential read and write on NTFS, and about
3 GB/s write and 4 GB/s read on ext4. Real disks run at their own speed.

The full manual (installation, settings, command line, NTFS and ext4 details,
troubleshooting, architecture) is [docs/FSMac Manual.html](docs/FSMac%20Manual.html).

## Install

1. Open `FSMac-<version>.dmg` and drag **FSMac** to **Applications**.
2. Open FSMac. A drive icon appears in the menu bar.
3. Click it, then **Install Helper**. macOS asks you to allow FSMac in
   **System Settings › General › Login Items & Extensions** (under *Allow in
   the Background*). Turn it on.

   If that does not work on your Mac, use **Install with Password…** instead.
   It installs the same helper as a LaunchDaemon after asking for an
   administrator password.
4. Give the helper **Full Disk Access**. macOS doesn't let background
   services read connected disks without it, and can't ask on their behalf.
   When you first connect a disk, the menu bar window shows **Allow FSMac to
   read disks**. Click **Open Settings**, then drag **fsmacd** from the Finder
   window that opens into the Full Disk Access list. The disk is mounted a few
   seconds later.
5. Connect a disk.

The helper runs in the background and mounts disks even when the menu bar
app is closed. To start the app at login, use **Settings › General › Open
FSMac at login**.

> FSMac builds are signed with the developer's certificate. If you received
> the app from someone else and macOS refuses to open it, Control-click the
> app, choose **Open**, then confirm. Apps signed with a Developer ID and
> notarized do not need this step.

## Use

| To… | Do this |
| --- | --- |
| Open a disk | Click it in Finder's sidebar, or click the folder button in the menu |
| Eject | Finder's eject button, the menu's eject button, or `fsmac eject <name>` |
| Mount read-only | Menu › **⋯** › **Mount Read-Only** (or **Remount Read-Only**) |
| Never mount a disk automatically | **⋯** › **When Connected** › **Don't Mount Automatically** or **Ignore This Volume** |
| Check or repair | **⋯** › **Check Disk…** |
| Erase as NTFS or ext4 | **⋯** › **Erase…** (a volume, or the entire disk) |

Command line (install it via **Settings › Helper › Install Command-Line
Tool…**, or call `/Applications/FSMac.app/Contents/Helpers/fsmac`):

```text
fsmac list                       show NTFS and ext volumes
fsmac mount <volume> [--read-only]
fsmac unmount <volume> [--force]
fsmac eject <volume|disk>
fsmac check <volume> [--repair]
fsmac format <disk> --type ntfs|ext4 --label NAME
fsmac policy <volume> read-write|read-only|ignore|default [--no-auto-mount]
fsmac config [<key> <value>]     e.g. fsmac config ntfsMode read-only
fsmac watch                      follow volume events
fsmac log [-f]                   the helper's log
```

`<volume>` is a device name (`disk4s1`), a volume name, or a mount point.

## Good to know

- **No Trash on these disks.** Like other network-style volumes, these
  volumes have no Trash, so Finder offers to delete files immediately.
- **Spotlight doesn't index them**, and Time Machine can't back up to them.
- **Ownership on ext volumes.** By default all files appear to belong to
  you (the user logged in at the Mac, who changes with fast user switching). New files get the owner and group of their folder, so they stay
  accessible on Linux. To see and set the owners and permissions stored on
  the disk, turn on **Settings › ext4 › Show the Linux owners**.
- **NTFS permissions.** Windows ACLs are not translated. Files show the
  permissions chosen in **Settings › NTFS**. The read-only attribute maps to
  the write permission.
- **Hard links.** Hard links are preserved, but extended attributes of a
  hard-linked file are only visible through one of its names.
- **Unsupported, mounted read-only:** ext4 volumes using casefolding, fs-verity,
  quotas, shared blocks or an external journal, and ext volumes with errors
  that `e2fsck` could not fix automatically. NTFS files encrypted with EFS
  cannot be read.
- **Crash safety.** NTFS uses ntfs-3g's metadata ordering. On ext4, data is
  written in place and flushed every few seconds. If a disk is unplugged
  without ejecting, the next mount runs `e2fsck` automatically. Always eject
  before unplugging.

## How it works

macOS doesn't let third-party apps add file system drivers without
entitlements that only Apple can grant. FSMac therefore serves each volume
from a small user-space server, and macOS's own NFS client mounts it:

```text
Finder / apps
     │  VFS
macOS NFS client (kernel)
     │  NFSv3 over 127.0.0.1 (never leaves the Mac)
fsmac-worker  ── one per volume; NFS server + AppleDouble translation
     │  page cache with read-ahead and write coalescing
libntfs-3g / libext2fs
     │
/dev/rdiskN
```

- `fsmacd` (root helper, LaunchDaemon) watches Disk Arbitration. It
  identifies NTFS and ext volumes, keeps macOS's read-only NTFS driver off
  volumes it mounts, starts a worker per volume and mounts it at
  `/Volumes/<name>`. It unmounts and ejects on request, and cleans up after
  surprise removals and worker failures.
- `fsmac-worker` opens the device, mounts it with ntfs-3g or libext2fs, and
  serves it. It accepts loopback connections only, from privileged ports
  only. Once set up it drops root privileges. Extended attributes travel
  through virtual AppleDouble (`._name`) files that the worker translates to
  and from native attributes on disk.
- `fsmac_ext.fs`, installed in `/Library/Filesystems`, lets macOS recognize
  ext volumes. Without it, macOS says they are unreadable and offers to erase
  them.
- `FSMac.app` is a SwiftUI menu bar app. It and `fsmac` talk to the helper
  over a Unix socket. Mounting and ejecting are open to every local user, as
  in Finder. Settings, checking and erasing need an administrator.

## Build from source

Requirements: macOS 14 or later, Xcode or the Command Line Tools, and
autoconf, automake, libtool and pkg-config (e.g. `brew install autoconf
automake libtool pkg-config`).

```sh
make            # vendor libraries, engine, worker, tools, FSMac.app (build/FSMac.app)
make test       # full test suite (unprivileged, uses disk images)
make dist       # build/dist/FSMac-<version>.dmg
```

`scripts/build-app.sh` signs with the first "Developer ID Application" or
"Apple Development" identity it finds. Set `SIGN_IDENTITY` to choose one.

Development without installing anything: `build/bin/fsmacd --user-mode`
runs the helper unprivileged. It handles disks you can open yourself, such as
attached disk images, and mounts them under `~/FSMacVolumes`. The app and
`fsmac` connect to it automatically.

### Layout

```text
engine/     C engine: block cache, NTFS (ntfs-3g) and ext (libext2fs) backends
nfs/        NFSv3 + MOUNT server, AppleDouble translation, duplicate request cache
worker/     fsmac-worker (one per mounted volume)
tools/      fsmac-shell (debug shell), fsmac-probe, fsmac_ext.util
daemon/     fsmacd (Swift): Disk Arbitration, mounting, IPC
app/        FSMac.app (SwiftUI)
cli/        fsmac command-line tool
shared/     protocol shared by the daemon, app and CLI
tests/      block cache, engine stress, end-to-end, fsx, daemon lifecycle tests
vendor/     ntfs-3g and e2fsprogs sources
```

### Tests

`make test` (or `tests/run-all.sh`) runs:

- a randomized block-cache test under AddressSanitizer;
- a differential stress test of the engine against APFS on nine filesystem
  variants (ext2, ext3, ext4 with 1 KB blocks, inline data and bigalloc, and
  NTFS with 512 B, 4 KB and 64 KB clusters), each followed by `e2fsck` or
  `ntfsfix`;
- end-to-end tests through the real macOS NFS client: `ditto` copies of a
  tree with Unicode names, xattrs, resource forks, symlinks and hard links;
  renames, case-only renames, persistence across remounts, no `._` files on
  disk, filesystem checks, and a cross-check that Apple's own NTFS driver
  reads what FSMac wrote;
- a concurrent fsx-style exerciser (6 processes × 3000 operations per
  filesystem);
- a daemon lifecycle test: auto-mount, unmount and remount, refusing to
  unmount a volume with open files, read-only mounts, worker crash and
  automatic repair, checks, same-named volumes, formatting a partition next to
  another volume, Finder-style ejects (without ejecting disks that hold other
  volumes) and `diskutil eject`.

## Uninstall

In **Settings › Helper**, click **Uninstall…**. It unmounts everything and
removes the helper and `/Library/Filesystems/fsmac_ext.fs`. Then move FSMac to
the Trash. If you installed with a password: `sudo
/Applications/FSMac.app/Contents/Helpers/fsmac uninstall-helper`.

## License

FSMac is free software under the GNU General Public License v2 (see
`LICENSE`). It includes ntfs-3g and e2fsprogs; see
[docs/THIRD-PARTY.md](docs/THIRD-PARTY.md).
