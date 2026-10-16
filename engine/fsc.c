/*
 * FSMac filesystem engine: backend registry and generic dispatch.
 *
 * Wrappers here enforce invariants common to all backends (read-only
 * volumes, "." / ".." handling, attribute refresh after modifications) so the
 * backends can stay focused on the on-disk format.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fsc_internal.h"

static const fsc_ops *const backends[] = {
	&fsc_ntfs_ops,
	&fsc_ext4_ops,
};

/* ---------------------------------------------------------------------- */
/* Logging                                                                 */

static fsc_log_fn log_fn;
static fsc_log_level log_min = FSC_LOG_INFO;

void fsc_set_log_fn(fsc_log_fn fn) { log_fn = fn; }
void fsc_set_log_level(fsc_log_level min_level) { log_min = min_level; }

void fsc_log(fsc_log_level level, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	if (level < log_min)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (log_fn) {
		log_fn(level, buf);
	} else {
		static const char *const tag[] = {"debug", "info", "warn",
						  "error"};
		fprintf(stderr, "fsmac[%s]: %s\n", tag[level], buf);
	}
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */

struct timespec fsc_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return ts;
}

bool fsc_utf8_valid(const char *s, size_t len)
{
	const unsigned char *p = (const unsigned char *)s, *end = p + len;

	while (p < end) {
		unsigned char c = *p;
		size_t n;
		uint32_t cp;

		if (c < 0x80) {
			p++;
			continue;
		} else if ((c & 0xE0) == 0xC0) {
			n = 1;
			cp = c & 0x1F;
		} else if ((c & 0xF0) == 0xE0) {
			n = 2;
			cp = c & 0x0F;
		} else if ((c & 0xF8) == 0xF0) {
			n = 3;
			cp = c & 0x07;
		} else {
			return false;
		}
		if ((size_t)(end - p) <= n)
			return false;
		for (size_t i = 1; i <= n; i++) {
			if ((p[i] & 0xC0) != 0x80)
				return false;
			cp = (cp << 6) | (p[i] & 0x3F);
		}
		/* Reject overlong forms, surrogates and out-of-range values. */
		if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
		    (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
		    (cp >= 0xD800 && cp <= 0xDFFF))
			return false;
		p += n + 1;
	}
	return true;
}

const char *fsc_type_name(fsc_type t)
{
	switch (t) {
	case FSC_TYPE_REG: return "file";
	case FSC_TYPE_DIR: return "dir";
	case FSC_TYPE_LNK: return "symlink";
	case FSC_TYPE_CHR: return "chardev";
	case FSC_TYPE_BLK: return "blockdev";
	case FSC_TYPE_FIFO: return "fifo";
	case FSC_TYPE_SOCK: return "socket";
	default: return "unknown";
	}
}

static bool is_dot(const char *name, size_t len)
{
	return len == 1 && name[0] == '.';
}

static bool is_dotdot(const char *name, size_t len)
{
	return len == 2 && name[0] == '.' && name[1] == '.';
}

/* ---------------------------------------------------------------------- */
/* Volume lifecycle                                                        */

const char *fsc_probe(fsc_dev *dev, char *label, size_t labelsz,
		      uint8_t uuid[16])
{
	char lbuf[256];
	uint8_t ubuf[16];

	if (!label) {
		label = lbuf;
		labelsz = sizeof(lbuf);
	}
	if (!uuid)
		uuid = ubuf;
	for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
		label[0] = 0;
		memset(uuid, 0, 16);
		if (backends[i]->probe(dev, label, labelsz, uuid))
			return backends[i]->name;
	}
	return NULL;
}

int fsc_mount(fsc_dev *dev, const fsc_mount_opts *opts, fsc_volume **out)
{
	fsc_mount_opts defaults;
	char label[256];
	uint8_t uuid[16];

	*out = NULL;
	if (!opts) {
		memset(&defaults, 0, sizeof(defaults));
		defaults.uid = 99; /* "unknown" on macOS */
		defaults.gid = 99;
		defaults.ntfs_windows_names = true;
		opts = &defaults;
	}
	for (size_t i = 0; i < sizeof(backends) / sizeof(backends[0]); i++) {
		if (!backends[i]->probe(dev, label, sizeof(label), uuid))
			continue;
		return backends[i]->mount(dev, opts, out);
	}
	return -EINVAL; /* unrecognised filesystem */
}

int fsc_unmount(fsc_volume *v)
{
	if (!v)
		return 0;
	return v->ops->unmount(v);
}

void fsc_get_volinfo(fsc_volume *v, fsc_volinfo *info)
{
	memset(info, 0, sizeof(*info));
	v->ops->volinfo(v, info);
	info->readonly = v->readonly;
}

int fsc_statfs(fsc_volume *v, fsc_fsstat *st)
{
	memset(st, 0, sizeof(*st));
	return v->ops->statfs(v, st);
}

fsc_dev *fsc_get_dev(fsc_volume *v) { return v->dev; }

/* ---------------------------------------------------------------------- */
/* Namespace                                                               */

int fsc_getattr(fsc_volume *v, fsc_ino ino, fsc_attr *attr)
{
	memset(attr, 0, sizeof(*attr));
	return v->ops->getattr(v, ino, attr);
}

int fsc_parent(fsc_volume *v, fsc_ino dir, fsc_ino *parent)
{
	return v->ops->parent(v, dir, parent);
}

int fsc_lookup(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
	       fsc_ino *ino, fsc_attr *attr)
{
	fsc_ino found;
	int err;

	if (len == 0)
		return -ENOENT;
	if (is_dot(name, len))
		err = (found = dir, 0);
	else if (is_dotdot(name, len))
		err = v->ops->parent(v, dir, &found);
	else
		err = v->ops->lookup(v, dir, name, len, &found);
	if (err)
		return err;
	if (ino)
		*ino = found;
	if (attr)
		return fsc_getattr(v, found, attr);
	return 0;
}

int fsc_readdir(fsc_volume *v, fsc_ino dir, uint64_t cookie,
		fsc_filldir_fn fn, void *ctx)
{
	return v->ops->readdir(v, dir, cookie, fn, ctx);
}

int fsc_dir_version(fsc_volume *v, fsc_ino dir, uint64_t *version)
{
	return v->ops->dir_version(v, dir, version);
}

int fsc_read(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len, void *buf,
	     size_t *got)
{
	*got = 0;
	if (len == 0)
		return 0;
	return v->ops->read(v, ino, off, len, buf, got);
}

int fsc_write(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len,
	      const void *buf, size_t *written)
{
	*written = 0;
	if (v->readonly)
		return -EROFS;
	if (len == 0)
		return 0;
	if (off + len < off)
		return -EFBIG;
	return v->ops->write(v, ino, off, len, buf, written);
}

int fsc_create(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
	       fsc_type type, uint32_t mode, uint32_t rdev_major,
	       uint32_t rdev_minor, const fsc_cred *cred, fsc_ino *ino,
	       fsc_attr *attr)
{
	fsc_ino created;
	int err;

	if (v->readonly)
		return -EROFS;
	if (is_dot(name, len) || is_dotdot(name, len))
		return -EEXIST;
	err = v->ops->create(v, dir, name, len, type, mode & 07777, rdev_major,
			     rdev_minor, cred, &created);
	if (err)
		return err;
	if (ino)
		*ino = created;
	return attr ? fsc_getattr(v, created, attr) : 0;
}

int fsc_symlink(fsc_volume *v, fsc_ino dir, const char *name, size_t len,
		const char *target, size_t tlen, const fsc_cred *cred,
		fsc_ino *ino, fsc_attr *attr)
{
	fsc_ino created;
	int err;

	if (v->readonly)
		return -EROFS;
	if (is_dot(name, len) || is_dotdot(name, len))
		return -EEXIST;
	if (tlen == 0)
		return -ENOENT;
	if (memchr(target, 0, tlen))
		return -EINVAL;
	err = v->ops->symlink(v, dir, name, len, target, tlen, cred, &created);
	if (err)
		return err;
	if (ino)
		*ino = created;
	return attr ? fsc_getattr(v, created, attr) : 0;
}

int fsc_readlink(fsc_volume *v, fsc_ino ino, char *buf, size_t bufsz,
		 size_t *len)
{
	*len = 0;
	return v->ops->readlink(v, ino, buf, bufsz, len);
}

int fsc_link(fsc_volume *v, fsc_ino ino, fsc_ino dir, const char *name,
	     size_t len)
{
	if (v->readonly)
		return -EROFS;
	if (is_dot(name, len) || is_dotdot(name, len))
		return -EEXIST;
	return v->ops->link(v, ino, dir, name, len);
}

int fsc_unlink(fsc_volume *v, fsc_ino dir, const char *name, size_t len)
{
	if (v->readonly)
		return -EROFS;
	if (is_dot(name, len) || is_dotdot(name, len))
		return -EISDIR;
	return v->ops->unlink(v, dir, name, len, false);
}

int fsc_rmdir(fsc_volume *v, fsc_ino dir, const char *name, size_t len)
{
	if (v->readonly)
		return -EROFS;
	if (is_dot(name, len))
		return -EINVAL;
	if (is_dotdot(name, len))
		return -ENOTEMPTY;
	return v->ops->unlink(v, dir, name, len, true);
}

/* Is anc an ancestor of (or equal to) dir? Used to reject renaming a
 * directory into its own subtree. */
static int is_ancestor(fsc_volume *v, fsc_ino anc, fsc_ino dir)
{
	fsc_ino cur = dir, parent;
	int depth = 0, err;

	for (;;) {
		if (cur == anc)
			return 1;
		err = v->ops->parent(v, cur, &parent);
		if (err)
			return err;
		if (parent == cur)
			return 0; /* reached the root */
		cur = parent;
		if (++depth > 4096)
			return -ELOOP;
	}
}

int fsc_rename(fsc_volume *v, fsc_ino sdir, const char *sname, size_t slen,
	       fsc_ino ddir, const char *dname, size_t dlen)
{
	fsc_ino src;
	fsc_attr sattr;
	int err;

	if (v->readonly)
		return -EROFS;
	if (is_dot(sname, slen) || is_dotdot(sname, slen) ||
	    is_dot(dname, dlen) || is_dotdot(dname, dlen))
		return -EINVAL;
	err = v->ops->lookup(v, sdir, sname, slen, &src);
	if (err)
		return err;
	err = fsc_getattr(v, src, &sattr);
	if (err)
		return err;
	if (sattr.type == FSC_TYPE_DIR && sdir != ddir) {
		err = is_ancestor(v, src, ddir);
		if (err < 0)
			return err;
		if (err)
			return -EINVAL;
	}
	return v->ops->rename(v, sdir, sname, slen, ddir, dname, dlen);
}

int fsc_setattr(fsc_volume *v, fsc_ino ino, const fsc_sattr *sa,
		fsc_attr *attr)
{
	int err;

	if (v->readonly)
		return -EROFS;
	if (sa->mask) {
		err = v->ops->setattr(v, ino, sa);
		if (err)
			return err;
	}
	return attr ? fsc_getattr(v, ino, attr) : 0;
}

/* ---------------------------------------------------------------------- */
/* Extended attributes                                                     */

int fsc_listxattr(fsc_volume *v, fsc_ino ino, char *buf, size_t bufsz,
		  size_t *len)
{
	*len = 0;
	if (!v->ops->listxattr)
		return 0;
	return v->ops->listxattr(v, ino, buf, bufsz, len);
}

int fsc_getxattr(fsc_volume *v, fsc_ino ino, const char *name, void *buf,
		 size_t bufsz, size_t *len)
{
	*len = 0;
	if (!v->ops->getxattr)
		return -ENOATTR;
	if (!name[0])
		return -EINVAL;
	return v->ops->getxattr(v, ino, name, buf, bufsz, len);
}

int fsc_setxattr(fsc_volume *v, fsc_ino ino, const char *name,
		 const void *val, size_t len, unsigned flags)
{
	if (v->readonly)
		return -EROFS;
	if (!v->ops->setxattr)
		return -ENOTSUP;
	if (!name[0] || strlen(name) > 255)
		return -EINVAL;
	return v->ops->setxattr(v, ino, name, val, len, flags);
}

int fsc_removexattr(fsc_volume *v, fsc_ino ino, const char *name)
{
	if (v->readonly)
		return -EROFS;
	if (!v->ops->removexattr)
		return -ENOATTR;
	return v->ops->removexattr(v, ino, name);
}

/* ---------------------------------------------------------------------- */
/* Maintenance                                                             */

int fsc_sync(fsc_volume *v, int level)
{
	if (v->readonly)
		return 0;
	return v->ops->sync(v, level);
}

bool fsc_is_dirty(fsc_volume *v)
{
	return !v->readonly && v->ops->is_dirty(v);
}

int fsc_set_label(fsc_volume *v, const char *label)
{
	if (v->readonly)
		return -EROFS;
	if (!v->ops->set_label)
		return -ENOTSUP;
	return v->ops->set_label(v, label);
}
