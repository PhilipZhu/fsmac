/*
 * FSMac filesystem engine API.
 *
 * A uniform, inode-based interface over the supported on-disk formats
 * (NTFS via libntfs-3g, ext2/3/4 via libext2fs). The NFS server and the test
 * tools are written against this interface only.
 *
 * Conventions:
 *   - Every function returns 0 on success or a negative errno.
 *   - Names are byte strings with explicit length (not NUL terminated),
 *     normally UTF-8. They never contain '/' or NUL.
 *   - Inode numbers are stable for the lifetime of a file and survive
 *     remounts. Generation numbers distinguish reused inode numbers.
 *   - The engine is not thread safe: callers serialise all calls on a volume.
 */
#ifndef FSMAC_FSC_H
#define FSMAC_FSC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <fsmac/blockdev.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t fsc_ino;

typedef enum fsc_type {
	FSC_TYPE_NONE = 0,
	FSC_TYPE_REG,
	FSC_TYPE_DIR,
	FSC_TYPE_LNK,
	FSC_TYPE_CHR,
	FSC_TYPE_BLK,
	FSC_TYPE_FIFO,
	FSC_TYPE_SOCK,
} fsc_type;

/* BSD-style file flags reported in fsc_attr.flags (subset of chflags(2)). */
#define FSC_FLAG_HIDDEN     0x0001u /* UF_HIDDEN */
#define FSC_FLAG_IMMUTABLE  0x0002u /* UF_IMMUTABLE */
#define FSC_FLAG_APPEND     0x0004u /* UF_APPEND */
#define FSC_FLAG_NODUMP     0x0008u /* UF_NODUMP */
#define FSC_FLAG_COMPRESSED 0x0010u /* stored compressed on disk */
#define FSC_FLAG_ENCRYPTED  0x0020u /* encrypted on disk (not accessible) */
#define FSC_FLAG_SYSTEM     0x0040u /* NTFS system attribute */
#define FSC_FLAG_ARCHIVE    0x0080u /* NTFS archive attribute */
#define FSC_FLAG_SPARSE     0x0100u

typedef struct fsc_attr {
	fsc_ino ino;
	uint32_t gen;           /* generation (for stale handle detection) */
	fsc_type type;
	uint32_t mode;          /* permission bits including 07000 */
	uint32_t nlink;
	uint32_t uid, gid;
	uint64_t size;          /* logical size in bytes */
	uint64_t used;          /* allocated bytes on disk */
	uint32_t rdev_major, rdev_minor;
	uint32_t flags;         /* FSC_FLAG_* */
	struct timespec atime, mtime, ctime, btime;
} fsc_attr;

/* fsc_sattr.mask */
#define FSC_SET_MODE      0x0001u
#define FSC_SET_UID       0x0002u
#define FSC_SET_GID       0x0004u
#define FSC_SET_SIZE      0x0008u
#define FSC_SET_ATIME     0x0010u /* use .atime */
#define FSC_SET_MTIME     0x0020u /* use .mtime */
#define FSC_SET_ATIME_NOW 0x0040u
#define FSC_SET_MTIME_NOW 0x0080u
#define FSC_SET_BTIME     0x0100u
#define FSC_SET_FLAGS     0x0200u /* FSC_FLAG_HIDDEN etc. */

typedef struct fsc_sattr {
	uint32_t mask;
	uint32_t mode, uid, gid;
	uint64_t size;
	struct timespec atime, mtime, btime;
	uint32_t flags;
} fsc_sattr;

typedef struct fsc_fsstat {
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint64_t avail_bytes;   /* free bytes available to ordinary users */
	uint64_t total_files;
	uint64_t free_files;
	uint32_t block_size;    /* allocation unit */
	uint32_t name_max;      /* maximum name length in bytes */
} fsc_fsstat;

typedef struct fsc_volinfo {
	char fstype[16];        /* "ntfs", "ext2", "ext3", "ext4" */
	char label[256];        /* UTF-8 volume label (may be empty) */
	uint8_t uuid[16];       /* stable volume identifier */
	bool readonly;
	bool case_insensitive;  /* lookups ignore case */
	bool case_preserving;
	bool supports_symlinks;
	bool supports_hardlinks;
	bool supports_xattrs;
	bool supports_unix_perms; /* modes/owners are stored on disk */
	uint32_t name_max;      /* bytes */
	uint32_t link_max;
	uint64_t max_file_size;
	uint32_t time_gran_ns;  /* timestamp granularity */
	fsc_ino root_ino;
	char details[256];      /* human readable version/features */
	char notice[512];       /* conditions worth telling the user about */
} fsc_volinfo;

typedef struct fsc_mount_opts {
	bool readonly;
	/* NTFS: delete hiberfil.sys contents to allow writing to a volume of a
	 * hibernated Windows (discards the hibernated session!). */
	bool ntfs_remove_hiberfile;
	/* NTFS: map characters Windows forbids in names (" * : < > ? \ | and
	 * trailing space/dot) to the private-use range, like macOS SMB/SFM. */
	bool ntfs_windows_names;
	/* NTFS: set the hidden attribute on files whose name starts with '.' */
	bool ntfs_hide_dot_files;
	/* Owner reported for files on filesystems without Unix ownership. */
	uint32_t uid, gid;
	/* Permission masks (umask style) for files without stored modes. */
	uint32_t fmask, dmask;
	/* Where the volume will appear; used to translate absolute NTFS
	 * junction/symlink targets that point inside the volume. */
	char mount_point[1024];
} fsc_mount_opts;

/* Credentials of the caller, used for ownership of newly created objects. */
typedef struct fsc_cred {
	uint32_t uid, gid;
} fsc_cred;

/*
 * Directory enumeration callback. Return 0 to continue, non-zero to stop;
 * a stopped entry is NOT consumed (it is returned again when enumeration is
 * resumed with the same cookie). next_cookie resumes after this entry.
 * The "." and ".." entries are never reported.
 */
typedef int (*fsc_filldir_fn)(void *ctx, const char *name, size_t namelen,
			      fsc_ino ino, fsc_type type, uint64_t next_cookie);

/* fsc_setxattr flags */
#define FSC_XATTR_CREATE  0x1u /* fail with -EEXIST if present */
#define FSC_XATTR_REPLACE 0x2u /* fail with -ENOATTR if absent */

/* fsc_sync levels */
#define FSC_SYNC_DATA 0  /* file data and inodes reach the device */
#define FSC_SYNC_FULL 1  /* all metadata written, device cache flushed */

typedef struct fsc_volume fsc_volume;

/* Returns the filesystem type name if recognised on dev, else NULL. */
const char *fsc_probe(fsc_dev *dev, char *label, size_t labelsz,
		      uint8_t uuid[16]);

/* Mounts the filesystem found on dev. The volume takes ownership of dev. */
int fsc_mount(fsc_dev *dev, const fsc_mount_opts *opts, fsc_volume **out);
/* Flushes everything, marks the filesystem clean and frees the volume. */
int fsc_unmount(fsc_volume *v);

void fsc_get_volinfo(fsc_volume *v, fsc_volinfo *info);
int fsc_statfs(fsc_volume *v, fsc_fsstat *st);
fsc_dev *fsc_get_dev(fsc_volume *v);

int fsc_getattr(fsc_volume *v, fsc_ino ino, fsc_attr *attr);
/* Looks up name in dir; "." and ".." are handled. attr may be NULL. */
int fsc_lookup(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
	       fsc_ino *ino, fsc_attr *attr);
/* Parent of a directory (root's parent is root). */
int fsc_parent(fsc_volume *v, fsc_ino dir, fsc_ino *parent);
int fsc_readdir(fsc_volume *v, fsc_ino dir, uint64_t cookie,
		fsc_filldir_fn fn, void *ctx);
/* A value that changes whenever dir's contents change (NFS cookieverf). */
int fsc_dir_version(fsc_volume *v, fsc_ino dir, uint64_t *version);

/* Reads up to len bytes; *got < len only at end of file. */
int fsc_read(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len, void *buf,
	     size_t *got);
int fsc_write(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len,
	      const void *buf, size_t *written);

/* Creates a regular file, directory, device node, fifo or socket. */
int fsc_create(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
	       fsc_type type, uint32_t mode, uint32_t rdev_major,
	       uint32_t rdev_minor, const fsc_cred *cred, fsc_ino *ino,
	       fsc_attr *attr);
int fsc_symlink(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
		const char *target, size_t tlen, const fsc_cred *cred,
		fsc_ino *ino, fsc_attr *attr);
int fsc_readlink(fsc_volume *v, fsc_ino ino, char *buf, size_t bufsz,
		 size_t *len);
int fsc_link(fsc_volume *v, fsc_ino ino, fsc_ino dir, const char *name,
	     size_t len);
/* Removes a non-directory entry. */
int fsc_unlink(fsc_volume *v, fsc_ino dir, const char *name, size_t len);
/* Removes an empty directory. */
int fsc_rmdir(fsc_volume *v, fsc_ino dir, const char *name, size_t len);
/* POSIX rename semantics: replaces an existing compatible target. */
int fsc_rename(fsc_volume *v, fsc_ino sdir, const char *sname, size_t slen,
	       fsc_ino ddir, const char *dname, size_t dlen);
int fsc_setattr(fsc_volume *v, fsc_ino ino, const fsc_sattr *sa,
		fsc_attr *attr);

/* Extended attributes (macOS-style names without namespace prefix). */
int fsc_listxattr(fsc_volume *v, fsc_ino ino, char *buf, size_t bufsz,
		  size_t *len);
int fsc_getxattr(fsc_volume *v, fsc_ino ino, const char *name, void *buf,
		 size_t bufsz, size_t *len);
int fsc_setxattr(fsc_volume *v, fsc_ino ino, const char *name,
		 const void *val, size_t len, unsigned flags);
int fsc_removexattr(fsc_volume *v, fsc_ino ino, const char *name);

int fsc_sync(fsc_volume *v, int level);
/* True if there are modifications not yet written with FSC_SYNC_FULL. */
bool fsc_is_dirty(fsc_volume *v);
int fsc_set_label(fsc_volume *v, const char *label);

/* Logging hook: the engine reports diagnostics through this function. */
typedef enum { FSC_LOG_DEBUG, FSC_LOG_INFO, FSC_LOG_WARN, FSC_LOG_ERROR }
	fsc_log_level;
typedef void (*fsc_log_fn)(fsc_log_level level, const char *msg);
void fsc_set_log_fn(fsc_log_fn fn);
void fsc_set_log_level(fsc_log_level min_level);
void fsc_log(fsc_log_level level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

const char *fsc_type_name(fsc_type t);

#ifdef __cplusplus
}
#endif
#endif /* FSMAC_FSC_H */
