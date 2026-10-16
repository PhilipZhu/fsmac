/*
 * FSMac engine internals shared by the filesystem backends.
 */
#ifndef FSMAC_FSC_INTERNAL_H
#define FSMAC_FSC_INTERNAL_H

#include <errno.h>
#include <fsmac/fsc.h>

#ifndef ENOATTR
#define ENOATTR 93
#endif

typedef struct fsc_ops {
	const char *name;
	/* Returns true if the device holds this filesystem. */
	bool (*probe)(fsc_dev *dev, char *label, size_t labelsz,
		      uint8_t uuid[16]);
	int (*mount)(fsc_dev *dev, const fsc_mount_opts *o, fsc_volume **out);
	int (*unmount)(fsc_volume *v);
	void (*volinfo)(fsc_volume *v, fsc_volinfo *info);
	int (*statfs)(fsc_volume *v, fsc_fsstat *st);
	int (*getattr)(fsc_volume *v, fsc_ino ino, fsc_attr *a);
	int (*lookup)(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
		      fsc_ino *ino);
	int (*parent)(fsc_volume *v, fsc_ino dir, fsc_ino *parent);
	int (*readdir)(fsc_volume *v, fsc_ino dir, uint64_t cookie,
		       fsc_filldir_fn fn, void *ctx);
	int (*dir_version)(fsc_volume *v, fsc_ino dir, uint64_t *version);
	int (*read)(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len,
		    void *buf, size_t *got);
	int (*write)(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len,
		     const void *buf, size_t *written);
	int (*create)(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
		      fsc_type type, uint32_t mode, uint32_t major,
		      uint32_t minor, const fsc_cred *cred, fsc_ino *ino);
	int (*symlink)(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
		       const char *target, size_t tlen, const fsc_cred *cred,
		       fsc_ino *ino);
	int (*readlink)(fsc_volume *v, fsc_ino ino, char *buf, size_t bufsz,
			size_t *len);
	int (*link)(fsc_volume *v, fsc_ino ino, fsc_ino dir, const char *name,
		    size_t len);
	int (*unlink)(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
		      bool isdir);
	int (*rename)(fsc_volume *v, fsc_ino sdir, const char *sname,
		      size_t slen, fsc_ino ddir, const char *dname,
		      size_t dlen);
	int (*setattr)(fsc_volume *v, fsc_ino ino, const fsc_sattr *sa);
	int (*listxattr)(fsc_volume *v, fsc_ino ino, char *buf, size_t bufsz,
			 size_t *len);
	int (*getxattr)(fsc_volume *v, fsc_ino ino, const char *name,
			void *buf, size_t bufsz, size_t *len);
	int (*setxattr)(fsc_volume *v, fsc_ino ino, const char *name,
			const void *val, size_t len, unsigned flags);
	int (*removexattr)(fsc_volume *v, fsc_ino ino, const char *name);
	int (*sync)(fsc_volume *v, int level);
	bool (*is_dirty)(fsc_volume *v);
	int (*set_label)(fsc_volume *v, const char *label);
} fsc_ops;

/* Every backend volume structure starts with this header. */
struct fsc_volume {
	const fsc_ops *ops;
	fsc_dev *dev;
	fsc_mount_opts opts;
	bool readonly;
};

extern const fsc_ops fsc_ntfs_ops;
extern const fsc_ops fsc_ext4_ops;

#define fsc_debug(...) fsc_log(FSC_LOG_DEBUG, __VA_ARGS__)
#define fsc_info(...)  fsc_log(FSC_LOG_INFO, __VA_ARGS__)
#define fsc_warn(...)  fsc_log(FSC_LOG_WARN, __VA_ARGS__)
#define fsc_error(...) fsc_log(FSC_LOG_ERROR, __VA_ARGS__)

/* Current wall-clock time. */
struct timespec fsc_now(void);

/* Validates that a name is acceptable as a single path component. */
static inline int fsc_check_name(const char *name, size_t len, size_t max)
{
	size_t i;

	if (len == 0)
		return -EINVAL;
	if (len > max)
		return -ENAMETOOLONG;
	if ((len == 1 && name[0] == '.') ||
	    (len == 2 && name[0] == '.' && name[1] == '.'))
		return -EINVAL;
	for (i = 0; i < len; i++)
		if (name[i] == '/' || name[i] == '\0')
			return -EINVAL;
	return 0;
}

/* Returns true if s[0..len) is well-formed UTF-8. */
bool fsc_utf8_valid(const char *s, size_t len);

#endif /* FSMAC_FSC_INTERNAL_H */
