/*
 * FSMac NTFS backend, built on libntfs-3g.
 *
 * The operation logic follows ntfs-3g's own low-level FUSE driver
 * (src/lowntfs-3g.c), which is inode based like our engine API: every
 * operation opens the inodes it needs, works on them and closes them again,
 * relying on libntfs-3g's internal inode cache for speed.
 *
 * Additions over lowntfs-3g:
 *   - case-insensitive, case-preserving lookups (Windows semantics) without
 *     lowercasing names in directory listings;
 *   - optional mapping of characters Windows forbids in names to the Unicode
 *     private-use range (the convention used by macOS SMB and Windows SFM);
 *   - the Windows read-only attribute is reflected in the write permission
 *     bits of regular files;
 *   - deferred final compression of written compressed files (lowntfs does
 *     this at close(), which a stateless protocol does not have).
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/disk.h>
#include <fcntl.h>
#include <sys/types.h>

#include "types.h"
#include "attrib.h"
#include "bootsect.h"
#include "cache.h"
#include "device.h"
#include "dir.h"
#include "inode.h"
#include "layout.h"
#include "logging.h"
#include "misc.h"
#include "ntfstime.h"
#include "reparse.h"
#include "security.h"
#include "unistr.h"
#include "volume.h"

#include "fsc_internal.h"

#define NTFS_NAME_MAX_UNITS 255
#define NTFS_NAME_MAX_BYTES 1020 /* 255 UTF-16 units, up to 4 bytes each */
#define MAX_PCLOSE 256

typedef struct ntfs_vol {
	fsc_volume base;
	ntfs_volume *vol;
	struct ntfs_device *ndev;
	bool winnames;
	bool dirty;
	uint64_t serial;
	char notice[512];
	/* Compressed files written since the last sync; their last
	 * compression block still has to be compressed (ntfs_attr_pclose). */
	uint64_t pclose[MAX_PCLOSE];
	size_t npclose;
} ntfs_vol;

static inline ntfs_vol *NV(fsc_volume *v) { return (ntfs_vol *)v; }

static inline int nerr(int fallback)
{
	return -(errno ? errno : fallback);
}

/* ---------------------------------------------------------------------- */
/* Device glue: libntfs-3g device operations on top of fsc_dev            */

struct ndev_priv {
	fsc_dev *dev;
	s64 pos;
};

static fsc_dev *pdev(struct ntfs_device *d)
{
	return ((struct ndev_priv *)d->d_private)->dev;
}

static int nd_open(struct ntfs_device *d, int flags)
{
	if (NDevOpen(d)) {
		errno = EBUSY;
		return -1;
	}
	if ((flags & O_ACCMODE) != O_RDONLY && fsc_dev_is_readonly(pdev(d))) {
		errno = EROFS;
		return -1;
	}
	if ((flags & O_ACCMODE) == O_RDONLY)
		NDevSetReadOnly(d);
	NDevSetBlock(d);
	NDevSetOpen(d);
	return 0;
}

static int nd_close(struct ntfs_device *d)
{
	NDevClearOpen(d);
	return 0;
}

static s64 nd_seek(struct ntfs_device *d, s64 offset, int whence)
{
	struct ndev_priv *p = d->d_private;
	s64 size = (s64)fsc_dev_size(p->dev);

	switch (whence) {
	case SEEK_SET: p->pos = offset; break;
	case SEEK_CUR: p->pos += offset; break;
	case SEEK_END: p->pos = size + offset; break;
	default: errno = EINVAL; return -1;
	}
	return p->pos;
}

static s64 nd_pread(struct ntfs_device *d, void *buf, s64 count, s64 offset)
{
	fsc_dev *dev = pdev(d);
	uint64_t size = fsc_dev_size(dev);
	int err;

	if (count < 0 || offset < 0) {
		errno = EINVAL;
		return -1;
	}
	if ((uint64_t)offset >= size)
		return 0;
	if ((uint64_t)(offset + count) > size)
		count = (s64)(size - (uint64_t)offset);
	err = fsc_dev_pread(dev, buf, (size_t)count, (uint64_t)offset);
	if (err) {
		errno = -err;
		return -1;
	}
	return count;
}

static s64 nd_pwrite(struct ntfs_device *d, const void *buf, s64 count,
		     s64 offset)
{
	int err;

	if (NDevReadOnly(d)) {
		errno = EROFS;
		return -1;
	}
	if (count < 0 || offset < 0) {
		errno = EINVAL;
		return -1;
	}
	NDevSetDirty(d);
	err = fsc_dev_pwrite(pdev(d), buf, (size_t)count, (uint64_t)offset);
	if (err) {
		errno = -err;
		return -1;
	}
	return count;
}

static s64 nd_read(struct ntfs_device *d, void *buf, s64 count)
{
	struct ndev_priv *p = d->d_private;
	s64 n = nd_pread(d, buf, count, p->pos);

	if (n > 0)
		p->pos += n;
	return n;
}

static s64 nd_write(struct ntfs_device *d, const void *buf, s64 count)
{
	struct ndev_priv *p = d->d_private;
	s64 n = nd_pwrite(d, buf, count, p->pos);

	if (n > 0)
		p->pos += n;
	return n;
}

static int nd_sync(struct ntfs_device *d)
{
	int err;

	if (NDevReadOnly(d))
		return 0;
	err = fsc_dev_flush(pdev(d), true);
	if (err) {
		errno = -err;
		return -1;
	}
	NDevClearDirty(d);
	return 0;
}

static int nd_stat(struct ntfs_device *d, struct stat *st)
{
	memset(st, 0, sizeof(*st));
	st->st_mode = S_IFBLK | 0600;
	st->st_size = (off_t)fsc_dev_size(pdev(d));
	return 0;
}

static int nd_ioctl(struct ntfs_device *d, unsigned long request, void *argp)
{
	fsc_dev *dev = pdev(d);

	switch (request) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)argp = fsc_dev_sector_size(dev);
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)argp = fsc_dev_size(dev) / fsc_dev_sector_size(dev);
		return 0;
	default:
		errno = EOPNOTSUPP;
		return -1;
	}
}

static struct ntfs_device_operations fsc_ndev_ops = {
	.open = nd_open,
	.close = nd_close,
	.seek = nd_seek,
	.read = nd_read,
	.write = nd_write,
	.pread = nd_pread,
	.pwrite = nd_pwrite,
	.sync = nd_sync,
	.stat = nd_stat,
	.ioctl = nd_ioctl,
};

/* Route libntfs-3g diagnostics into the engine log. */
static int ntfs_log_bridge(const char *function, const char *file, int line,
			   u32 level, void *data, const char *format,
			   va_list args)
{
	char buf[768];
	size_t n;
	fsc_log_level lvl;

	(void)function; (void)file; (void)line; (void)data;
	vsnprintf(buf, sizeof(buf), format, args);
	n = strlen(buf);
	while (n && buf[n - 1] == '\n')
		buf[--n] = 0;
	if (level & NTFS_LOG_LEVEL_PERROR) {
		size_t l = strlen(buf);
		snprintf(buf + l, sizeof(buf) - l, ": %s", strerror(errno));
	}
	if (level & (NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_ERROR |
		     NTFS_LOG_LEVEL_PERROR))
		lvl = FSC_LOG_ERROR;
	else if (level & NTFS_LOG_LEVEL_WARNING)
		lvl = FSC_LOG_WARN;
	else if (level & (NTFS_LOG_LEVEL_INFO | NTFS_LOG_LEVEL_QUIET |
			  NTFS_LOG_LEVEL_VERBOSE | NTFS_LOG_LEVEL_PROGRESS))
		lvl = FSC_LOG_INFO;
	else
		lvl = FSC_LOG_DEBUG;
	if (n)
		fsc_log(lvl, "ntfs: %s", buf);
	return (int)n;
}

/* ---------------------------------------------------------------------- */
/* Name conversion                                                         */

static const struct { uint16_t ch, mapped; } sfm_table[] = {
	{'"', 0xF020}, {'*', 0xF021}, {':', 0xF022}, {'<', 0xF023},
	{'>', 0xF024}, {'?', 0xF025}, {'\\', 0xF026}, {'|', 0xF027},
};

/* Maps characters Windows cannot handle to the private-use range.
 * Returns true if anything was changed. */
static bool sfm_encode(ntfschar *u, int len)
{
	bool changed = false;

	for (int i = 0; i < len; i++) {
		uint16_t c = le16_to_cpu(u[i]), m = c;

		if (c >= 0x01 && c <= 0x1F) {
			m = 0xF000 + c;
		} else {
			for (size_t k = 0; k < sizeof(sfm_table) / sizeof(sfm_table[0]); k++)
				if (sfm_table[k].ch == c)
					m = sfm_table[k].mapped;
		}
		if (i == len - 1 && c == ' ')
			m = 0xF028;
		if (i == len - 1 && c == '.')
			m = 0xF029;
		if (m != c) {
			u[i] = cpu_to_le16(m);
			changed = true;
		}
	}
	return changed;
}

static void sfm_decode(ntfschar *u, int len)
{
	for (int i = 0; i < len; i++) {
		uint16_t c = le16_to_cpu(u[i]), m = c;

		if (c >= 0xF001 && c <= 0xF01F)
			m = c - 0xF000;
		else if (c == 0xF028)
			m = ' ';
		else if (c == 0xF029)
			m = '.';
		else
			for (size_t k = 0; k < sizeof(sfm_table) / sizeof(sfm_table[0]); k++)
				if (sfm_table[k].mapped == c)
					m = sfm_table[k].ch;
		u[i] = cpu_to_le16(m);
	}
}

/*
 * Converts a UTF-8 name to UTF-16 (as stored on disk). If raw is false and
 * Windows name mapping is enabled, forbidden characters are mapped.
 * *mapped reports whether mapping changed the name.
 */
static int to_uname(ntfs_vol *nv, const char *name, size_t len, bool raw,
		    ntfschar **out, int *outlen, bool *mapped)
{
	char buf[NTFS_NAME_MAX_BYTES + 1];
	ntfschar *u = NULL;
	int ulen;

	*out = NULL;
	if (mapped)
		*mapped = false;
	if (len == 0)
		return -EINVAL;
	if (len > NTFS_NAME_MAX_BYTES)
		return -ENAMETOOLONG;
	if (memchr(name, 0, len) || memchr(name, '/', len))
		return -EINVAL;
	if (!fsc_utf8_valid(name, len))
		return -EILSEQ;
	memcpy(buf, name, len);
	buf[len] = 0;
	errno = 0;
	ulen = ntfs_mbstoucs(buf, &u);
	if (ulen < 0)
		return nerr(EILSEQ);
	if (ulen == 0) {
		free(u);
		return -EINVAL;
	}
	if (ulen > NTFS_NAME_MAX_UNITS) {
		free(u);
		return -ENAMETOOLONG;
	}
	if (!raw && nv->winnames) {
		bool m = sfm_encode(u, ulen);
		if (mapped)
			*mapped = m;
	}
	*out = u;
	*outlen = ulen;
	return 0;
}

/* Converts an on-disk UTF-16 name to a freshly allocated UTF-8 string. */
static int from_uname(ntfs_vol *nv, const ntfschar *u, int ulen, char **out,
		      int *outlen)
{
	ntfschar tmp[NTFS_NAME_MAX_UNITS + 1];
	const ntfschar *src = u;
	int n;

	*out = NULL;
	if (nv->winnames && ulen <= NTFS_NAME_MAX_UNITS) {
		memcpy(tmp, u, (size_t)ulen * sizeof(ntfschar));
		sfm_decode(tmp, ulen);
		/* Never let decoding produce "." or "..". */
		if (!((ulen == 1 && le16_to_cpu(tmp[0]) == '.') ||
		      (ulen == 2 && le16_to_cpu(tmp[0]) == '.' &&
		       le16_to_cpu(tmp[1]) == '.')))
			src = tmp;
	}
	errno = 0;
	n = ntfs_ucstombs(src, ulen, out, 0);
	if (n < 0)
		return nerr(EILSEQ);
	*outlen = n;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Inode helpers                                                           */

static bool is_user_ino(fsc_ino ino)
{
	return ino == FILE_root || ino >= FILE_first_user;
}

static ntfs_inode *open_ino(ntfs_vol *nv, fsc_ino ino, int *err)
{
	ntfs_inode *ni;

	if (ino == 0 || ino > (fsc_ino)MREF((u64)-1)) {
		*err = -ESTALE;
		return NULL;
	}
	errno = 0;
	ni = ntfs_inode_open(nv->vol, (MFT_REF)ino);
	if (!ni) {
		*err = nerr(EIO);
		if (*err == -ENOENT)
			*err = -ESTALE;
		return NULL;
	}
	*err = 0;
	return ni;
}

static void close_ino(ntfs_inode *ni, int *err)
{
	if (ni && ntfs_inode_close(ni) && err && !*err)
		*err = nerr(EIO);
}

static inline void set_archive(ntfs_inode *ni)
{
	ni->flags |= FILE_ATTR_ARCHIVE;
}

static bool ni_is_dir(ntfs_inode *ni)
{
	return (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) &&
	       !(ni->flags & FILE_ATTR_REPARSE_POINT);
}

/* Reads the Interix special-file payload of a system file, if any. */
static int read_intx(ntfs_inode *ni, INTX_FILE **out, s64 *size)
{
	ntfs_attr *na;
	INTX_FILE *ix;

	*out = NULL;
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na)
		return nerr(EIO);
	*size = na->data_size;
	if (na->data_size <= (s64)sizeof(INTX_FILE_TYPES) ||
	    na->data_size > (s64)(sizeof(INTX_FILE_TYPES) +
				  sizeof(ntfschar) * PATH_MAX)) {
		ntfs_attr_close(na);
		return 0;
	}
	ix = ntfs_malloc(na->data_size);
	if (!ix) {
		ntfs_attr_close(na);
		return -ENOMEM;
	}
	if (ntfs_attr_pread(na, 0, na->data_size, ix) != na->data_size) {
		free(ix);
		ntfs_attr_close(na);
		return nerr(EIO);
	}
	ntfs_attr_close(na);
	*out = ix;
	return 0;
}

static void fill_times(ntfs_inode *ni, fsc_attr *a)
{
	a->atime = ntfs2timespec(ni->last_access_time);
	a->mtime = ntfs2timespec(ni->last_data_change_time);
	a->ctime = ntfs2timespec(ni->last_mft_change_time);
	a->btime = ntfs2timespec(ni->creation_time);
}

static int stat_ni(ntfs_vol *nv, ntfs_inode *ni, fsc_attr *a)
{
	const fsc_mount_opts *o = &nv->base.opts;
	int err = 0;

	a->ino = ni->mft_no;
	a->gen = le16_to_cpu(ni->mrec->sequence_number);
	a->nlink = le16_to_cpu(ni->mrec->link_count);
	a->uid = o->uid;
	a->gid = o->gid;
	fill_times(ni, a);
	/* Windows marks the root directory hidden+system; never hide it. */
	if ((ni->flags & FILE_ATTR_HIDDEN) && ni->mft_no != FILE_root)
		a->flags |= FSC_FLAG_HIDDEN;
	if (ni->flags & FILE_ATTR_SYSTEM)
		a->flags |= FSC_FLAG_SYSTEM;
	if (ni->flags & FILE_ATTR_ARCHIVE)
		a->flags |= FSC_FLAG_ARCHIVE;
	if (ni->flags & FILE_ATTR_COMPRESSED)
		a->flags |= FSC_FLAG_COMPRESSED;
	if (ni->flags & FILE_ATTR_ENCRYPTED)
		a->flags |= FSC_FLAG_ENCRYPTED;
	if (ni->flags & FILE_ATTR_SPARSE_FILE)
		a->flags |= FSC_FLAG_SPARSE;

	if (ni->flags & FILE_ATTR_REPARSE_POINT) {
		char *target;

		errno = 0;
		target = ntfs_make_symlink(ni, o->mount_point);
		if (target || errno == EOPNOTSUPP) {
			/* Junctions, symlinks and unknown reparse points are
			 * all presented as symbolic links. */
			a->type = FSC_TYPE_LNK;
			a->mode = 0777;
			a->size = target ? strlen(target) : 0;
			a->used = ni->allocated_size;
			free(target);
			return 0;
		}
		return nerr(EIO);
	}
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		a->type = FSC_TYPE_DIR;
		a->mode = 0777 & ~o->dmask;
		a->nlink = 1; /* "unknown": tells fts not to count subdirs */
		if (!test_nino_flag(ni, KnownSize)) {
			ntfs_attr *na = ntfs_attr_open(ni, AT_INDEX_ALLOCATION,
						       NTFS_INDEX_I30, 4);
			if (na) {
				ni->data_size = na->data_size;
				ni->allocated_size = na->allocated_size;
				set_nino_flag(ni, KnownSize);
				ntfs_attr_close(na);
			}
		}
		a->size = (uint64_t)ni->data_size;
		a->used = (uint64_t)ni->allocated_size;
		return 0;
	}

	a->type = FSC_TYPE_REG;
	a->size = (uint64_t)ni->data_size;
	a->used = (uint64_t)ni->allocated_size;
	a->mode = 0777 & ~o->fmask;
	if (ni->flags & FILE_ATTR_READONLY)
		a->mode &= ~0222u;
	if (ni->flags & FILE_ATTR_SYSTEM) {
		/* Interix (SFU/ntfs-3g) special files are system files. */
		INTX_FILE *ix = NULL;
		s64 dsize = -1;

		err = read_intx(ni, &ix, &dsize);
		if (err) {
			if (err == -ENOENT) /* no unnamed data stream */
				err = 0;
			return err;
		}
		if (!(ni->flags & FILE_ATTR_HIDDEN)) {
			if (dsize == 0)
				a->type = FSC_TYPE_FIFO;
			else if (dsize == 1)
				a->type = FSC_TYPE_SOCK;
		}
		if (ix) {
			if ((ix->magic == INTX_BLOCK_DEVICE ||
			     ix->magic == INTX_CHARACTER_DEVICE) &&
			    dsize == (s64)offsetof(INTX_FILE, device_end)) {
				a->type = ix->magic == INTX_BLOCK_DEVICE
						  ? FSC_TYPE_BLK : FSC_TYPE_CHR;
				a->rdev_major = (uint32_t)le64_to_cpu(ix->major);
				a->rdev_minor = (uint32_t)le64_to_cpu(ix->minor);
			} else if (ix->magic == INTX_SYMBOLIC_LINK) {
				char *t = NULL;
				int len = ntfs_ucstombs(ix->target,
					(int)((dsize - (s64)offsetof(INTX_FILE, target)) /
					      (s64)sizeof(ntfschar)), &t, 0);
				if (len >= 0) {
					a->type = FSC_TYPE_LNK;
					a->mode = 0777;
					a->size = (uint64_t)len;
				}
				free(t);
			}
			free(ix);
		}
	}
	return 0;
}

static fsc_type presented_type(ntfs_vol *nv, ntfs_inode *ni)
{
	fsc_attr a;

	memset(&a, 0, sizeof(a));
	if (stat_ni(nv, ni, &a))
		return ni_is_dir(ni) ? FSC_TYPE_DIR : FSC_TYPE_REG;
	return a.type;
}

/*
 * Looks name up in dir_ni: exact match first, then case-insensitively, then
 * (if Windows name mapping changed the name) the unmapped spelling.
 */
static int dir_lookup(ntfs_vol *nv, ntfs_inode *dir_ni, const char *name,
		      size_t len, u64 *mref)
{
	ntfschar *u;
	int ulen, err;
	bool mapped;
	u64 r;

	err = to_uname(nv, name, len, false, &u, &ulen, &mapped);
	if (err)
		return err == -EILSEQ ? -ENOENT : err;
	for (int pass = 0; pass < 2; pass++) {
		errno = 0;
		r = ntfs_inode_lookup_by_name(dir_ni, u, ulen);
		if (r == (u64)-1 && errno == ENOENT) {
			NVolClearCaseSensitive(nv->vol);
			errno = 0;
			r = ntfs_inode_lookup_by_name(dir_ni, u, ulen);
			NVolSetCaseSensitive(nv->vol);
		}
		if (r != (u64)-1 || errno != ENOENT || !mapped || pass)
			break;
		free(u);
		err = to_uname(nv, name, len, true, &u, &ulen, NULL);
		if (err)
			return err == -EILSEQ ? -ENOENT : err;
	}
	free(u);
	if (r == (u64)-1)
		return nerr(ENOENT);
	if (!is_user_ino(MREF(r)) || MREF(r) == FILE_root)
		return -ENOENT; /* metadata files are not exposed */
	*mref = r;
	return 0;
}

/*
 * Finds the exact on-disk spelling of the (non-DOS) name by which ni is
 * linked into directory dir that matches u case-insensitively.
 */
static int actual_name(ntfs_vol *nv, ntfs_inode *ni, u64 dir, const ntfschar *u,
		       int ulen, ntfschar *out, int *outlen)
{
	ntfs_attr_search_ctx *ctx = ntfs_attr_get_search_ctx(ni, NULL);
	int found = -ENOENT;

	if (!ctx)
		return nerr(ENOMEM);
	while (!ntfs_attr_lookup(AT_FILE_NAME, AT_UNNAMED, 0, CASE_SENSITIVE, 0,
				 NULL, 0, ctx)) {
		FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)((u8 *)ctx->attr +
			le16_to_cpu(ctx->attr->value_offset));

		if (MREF_LE(fn->parent_directory) != dir ||
		    fn->file_name_type == FILE_NAME_DOS)
			continue;
		if (ntfs_names_are_equal(fn->file_name, fn->file_name_length, u,
					 (size_t)ulen, IGNORE_CASE,
					 nv->vol->upcase, nv->vol->upcase_len)) {
			memcpy(out, fn->file_name,
			       fn->file_name_length * sizeof(ntfschar));
			*outlen = fn->file_name_length;
			found = 0;
			/* Prefer an exact-case match if there is one. */
			if (ntfs_names_are_equal(fn->file_name,
						 fn->file_name_length, u,
						 (size_t)ulen, CASE_SENSITIVE,
						 nv->vol->upcase,
						 nv->vol->upcase_len))
				break;
		}
	}
	ntfs_attr_put_search_ctx(ctx);
	return found;
}

/* Remembers a compressed file that needs ntfs_attr_pclose() later. */
static void note_pclose(ntfs_vol *nv, uint64_t ino)
{
	for (size_t i = 0; i < nv->npclose; i++)
		if (nv->pclose[i] == ino)
			return;
	if (nv->npclose < MAX_PCLOSE)
		nv->pclose[nv->npclose++] = ino;
}

static void forget_pclose(ntfs_vol *nv, uint64_t ino)
{
	for (size_t i = 0; i < nv->npclose; i++)
		if (nv->pclose[i] == ino) {
			nv->pclose[i] = nv->pclose[--nv->npclose];
			return;
		}
}

static int do_pclose(ntfs_vol *nv, uint64_t ino)
{
	ntfs_inode *ni;
	ntfs_attr *na;
	int err;

	ni = open_ino(nv, ino, &err);
	if (!ni)
		return err == -ESTALE ? 0 : err;
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (na) {
		if ((na->data_flags & ATTR_COMPRESSION_MASK) &&
		    ntfs_attr_pclose(na))
			err = nerr(EIO);
		ntfs_attr_close(na);
	}
	close_ino(ni, &err);
	return err;
}

static int flush_pclose(ntfs_vol *nv)
{
	int err = 0;

	while (nv->npclose) {
		uint64_t ino = nv->pclose[--nv->npclose];
		int e = do_pclose(nv, ino);
		if (e && !err)
			err = e;
	}
	return err;
}

/* ---------------------------------------------------------------------- */
/* Probe / mount                                                           */

static bool ntfs_probe(fsc_dev *dev, char *label, size_t labelsz,
		       uint8_t uuid[16])
{
	NTFS_BOOT_SECTOR bs;
	uint64_t serial;

	(void)label; (void)labelsz;
	if (fsc_dev_size(dev) < 64 * 1024)
		return false;
	if (fsc_dev_pread(dev, &bs, sizeof(bs), 0))
		return false;
	if (memcmp(&bs.oem_id, "NTFS    ", 8) != 0)
		return false;
	if (!ntfs_boot_sector_is_ntfs(&bs))
		return false;
	serial = le64_to_cpu(bs.volume_serial_number);
	memcpy(uuid, &serial, 8);
	memcpy(uuid + 8, "NTFS\x00\x00\x00\x01", 8);
	return true;
}

static int ntfs_mount_vol(fsc_dev *dev, const fsc_mount_opts *o,
			  fsc_volume **out)
{
	ntfs_vol *nv;
	struct ndev_priv *priv;
	unsigned long flags;
	NTFS_BOOT_SECTOR bs;
	int err;

	ntfs_log_set_handler(ntfs_log_bridge);
	ntfs_log_set_levels(NTFS_LOG_LEVEL_INFO | NTFS_LOG_LEVEL_WARNING |
			    NTFS_LOG_LEVEL_ERROR | NTFS_LOG_LEVEL_PERROR |
			    NTFS_LOG_LEVEL_CRITICAL | NTFS_LOG_LEVEL_QUIET);

	nv = calloc(1, sizeof(*nv));
	priv = calloc(1, sizeof(*priv));
	if (!nv || !priv) {
		free(nv);
		free(priv);
		return -ENOMEM;
	}
	nv->base.ops = &fsc_ntfs_ops;
	nv->base.dev = dev;
	nv->base.opts = *o;
	nv->winnames = o->ntfs_windows_names;
	priv->dev = dev;

	if (fsc_dev_pread(dev, &bs, sizeof(bs), 0) == 0)
		nv->serial = le64_to_cpu(bs.volume_serial_number);

	nv->ndev = ntfs_device_alloc(fsc_dev_name(dev), 0, &fsc_ndev_ops, priv);
	if (!nv->ndev) {
		err = nerr(ENOMEM);
		goto fail;
	}

	flags = NTFS_MNT_RECOVER;
	if (o->readonly || fsc_dev_is_readonly(dev))
		flags |= NTFS_MNT_RDONLY;
	else if (o->ntfs_remove_hiberfile)
		flags |= NTFS_MNT_IGNORE_HIBERFILE;
	else
		flags |= NTFS_MNT_MAY_RDONLY;

	errno = 0;
	nv->vol = ntfs_device_mount(nv->ndev, flags);
	if (!nv->vol) {
		err = nerr(EIO);
		fsc_error("ntfs: mount failed: %s", strerror(-err));
		goto fail;
	}
	ntfs_create_lru_caches(nv->vol);
	nv->base.readonly = NVolReadOnly(nv->vol) ? true : false;
	if (nv->base.readonly && !(flags & NTFS_MNT_RDONLY))
		snprintf(nv->notice, sizeof(nv->notice),
			 "Windows is hibernated (Fast Startup); mounted "
			 "read-only.");

	NVolSetCompression(nv->vol);
	if (ntfs_set_shown_files(nv->vol, FALSE, TRUE, o->ntfs_hide_dot_files)) {
		err = nerr(EIO);
		goto fail_umount;
	}
	if (ntfs_volume_get_free_space(nv->vol)) {
		err = nerr(EIO);
		goto fail_umount;
	}
	{
		ntfs_attr *na = nv->vol->mftbmp_na;
		s64 nr_free = ntfs_attr_get_free_bits(na);

		if (nr_free >= 0)
			nr_free += (na->allocated_size - na->data_size) << 3;
		nv->vol->free_mft_records = nr_free;
	}

	if (!nv->base.readonly && o->ntfs_remove_hiberfile &&
	    ntfs_volume_check_hiberfile(nv->vol, 0)) {
		/* Delete the hibernation image so that Windows cold boots. */
		ntfs_inode *root = ntfs_inode_open(nv->vol, FILE_root);
		u64 mref;

		if (root && dir_lookup(nv, root, "hiberfil.sys", 12, &mref) == 0) {
			ntfs_inode *hni = ntfs_inode_open(nv->vol, MREF(mref));
			ntfschar *u;
			int ulen;

			if (hni && to_uname(nv, "hiberfil.sys", 12, true, &u,
					    &ulen, NULL) == 0) {
				ntfs_delete(nv->vol, NULL, hni, root, u, (u8)ulen);
				free(u);
				root = NULL; /* closed by ntfs_delete */
			} else if (hni) {
				ntfs_inode_close(hni);
			}
		}
		if (root)
			ntfs_inode_close(root);
		snprintf(nv->notice, sizeof(nv->notice),
			 "Windows hibernation data removed.");
	}

	fsc_info("ntfs: mounted '%s' (NTFS %u.%u, %u-byte clusters, %s)",
		 nv->vol->vol_name ? nv->vol->vol_name : "",
		 nv->vol->major_ver, nv->vol->minor_ver,
		 nv->vol->cluster_size, nv->base.readonly ? "read-only"
							  : "read-write");
	*out = &nv->base;
	return 0;

fail_umount:
	ntfs_umount(nv->vol, TRUE); /* frees ndev */
	nv->ndev = NULL;
fail:
	if (nv->ndev)
		ntfs_device_free(nv->ndev);
	free(priv);
	free(nv);
	return err;
}

static int ntfs_unmount_vol(fsc_volume *v)
{
	ntfs_vol *nv = NV(v);
	void *priv = nv->ndev->d_private;
	int err = 0, e;

	if (!v->readonly)
		err = flush_pclose(nv);
	if (ntfs_umount(nv->vol, FALSE) && !err)
		err = nerr(EIO);
	free(priv);
	e = fsc_dev_flush(v->dev, true);
	if (e && !err)
		err = e;
	fsc_dev_close(v->dev);
	free(nv);
	return err;
}

static void ntfs_volinfo(fsc_volume *v, fsc_volinfo *info)
{
	ntfs_vol *nv = NV(v);

	snprintf(info->fstype, sizeof(info->fstype), "ntfs");
	snprintf(info->label, sizeof(info->label), "%s",
		 nv->vol->vol_name ? nv->vol->vol_name : "");
	memcpy(info->uuid, &nv->serial, 8);
	memcpy(info->uuid + 8, "NTFS\x00\x00\x00\x01", 8);
	info->case_insensitive = true;
	info->case_preserving = true;
	info->supports_symlinks = true;
	info->supports_hardlinks = true;
	info->supports_xattrs = true;
	info->supports_unix_perms = false;
	info->name_max = 255;
	info->link_max = 1024;
	info->max_file_size = (uint64_t)1 << 44; /* 16 TiB at 4 KiB clusters */
	info->time_gran_ns = 100;
	info->root_ino = FILE_root;
	snprintf(info->details, sizeof(info->details),
		 "NTFS %u.%u, %u-byte clusters, %u-byte MFT records",
		 nv->vol->major_ver, nv->vol->minor_ver, nv->vol->cluster_size,
		 nv->vol->mft_record_size);
	snprintf(info->notice, sizeof(info->notice), "%s", nv->notice);
}

static int ntfs_statfs_vol(fsc_volume *v, fsc_fsstat *st)
{
	ntfs_volume *vol = NV(v)->vol;
	s64 size = vol->free_clusters;
	int delta_bits;

	if (size < 0)
		size = 0;
	st->block_size = vol->cluster_size;
	st->total_bytes = (uint64_t)vol->nr_clusters << vol->cluster_size_bits;
	st->free_bytes = (uint64_t)size << vol->cluster_size_bits;
	st->avail_bytes = st->free_bytes;
	delta_bits = vol->cluster_size_bits - vol->mft_record_size_bits;
	if (delta_bits >= 0)
		size <<= delta_bits;
	else
		size >>= -delta_bits;
	st->total_files = (uint64_t)(vol->mftbmp_na->allocated_size << 3) +
			  (uint64_t)size;
	size += vol->free_mft_records;
	st->free_files = size < 0 ? 0 : (uint64_t)size;
	st->name_max = 255;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Namespace operations                                                    */

static int ntfs_getattr_op(fsc_volume *v, fsc_ino ino, fsc_attr *a)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	int err;

	ni = open_ino(nv, ino, &err);
	if (!ni)
		return err;
	err = stat_ni(nv, ni, a);
	close_ino(ni, &err);
	return err;
}

static int ntfs_lookup_op(fsc_volume *v, fsc_ino dir, const char *name,
			  size_t len, fsc_ino *ino)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *dir_ni;
	u64 mref = 0;
	int err;

	dir_ni = open_ino(nv, dir, &err);
	if (!dir_ni)
		return err;
	if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY))
		err = -ENOTDIR;
	else
		err = dir_lookup(nv, dir_ni, name, len, &mref);
	close_ino(dir_ni, &err);
	if (!err)
		*ino = MREF(mref);
	return err;
}

static int ntfs_parent_op(fsc_volume *v, fsc_ino dir, fsc_ino *parent)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni, *pni;
	int err;

	if (dir == FILE_root) {
		*parent = FILE_root;
		return 0;
	}
	ni = open_ino(nv, dir, &err);
	if (!ni)
		return err;
	errno = 0;
	pni = ntfs_dir_parent_inode(ni);
	if (pni) {
		*parent = pni->mft_no;
		ntfs_inode_close(pni);
	} else {
		err = nerr(ENOENT);
	}
	close_ino(ni, &err);
	return err;
}

struct rd_ctx {
	ntfs_vol *nv;
	fsc_filldir_fn fn;
	void *ctx;
	int err;
	bool stopped;
};

static int rd_filler(void *dirent, const ntfschar *name, const int name_len,
		     const int name_type, const s64 pos, const MFT_REF mref,
		     const unsigned dt_type)
{
	struct rd_ctx *rc = dirent;
	char *utf8 = NULL;
	int ulen, ret = 0;
	fsc_type type;

	if (name_type == FILE_NAME_DOS)
		return 0;
	if (MREF(mref) <= 1 || !is_user_ino(MREF(mref)))
		return 0;
	if (pos < 2)
		return 0; /* emulated "." and ".." */
	rc->err = from_uname(rc->nv, name, name_len, &utf8, &ulen);
	if (rc->err) {
		/* Skip names we cannot represent rather than failing. */
		fsc_warn("ntfs: skipping undecodable name in inode %llu",
			 (unsigned long long)MREF(mref));
		rc->err = 0;
		return 0;
	}
	switch (dt_type) {
	case NTFS_DT_DIR: type = FSC_TYPE_DIR; break;
	case NTFS_DT_LNK:
	case NTFS_DT_REPARSE: type = FSC_TYPE_LNK; break;
	case NTFS_DT_FIFO: type = FSC_TYPE_FIFO; break;
	case NTFS_DT_SOCK: type = FSC_TYPE_SOCK; break;
	case NTFS_DT_BLK: type = FSC_TYPE_BLK; break;
	case NTFS_DT_CHR: type = FSC_TYPE_CHR; break;
	default: type = FSC_TYPE_REG; break;
	}
	if (rc->fn(rc->ctx, utf8, (size_t)ulen, MREF(mref), type,
		   (uint64_t)pos + 1)) {
		rc->stopped = true;
		ret = -1;
	}
	free(utf8);
	return ret;
}

static int ntfs_readdir_op(fsc_volume *v, fsc_ino dir, uint64_t cookie,
			   fsc_filldir_fn fn, void *ctx)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	struct rd_ctx rc = {nv, fn, ctx, 0, false};
	s64 pos = cookie ? (s64)cookie : 2;
	int err;

	ni = open_ino(nv, dir, &err);
	if (!ni)
		return err;
	if (!ni_is_dir(ni)) {
		err = -ENOTDIR;
	} else {
		errno = 0;
		if (ntfs_readdir(ni, &pos, &rc, rd_filler) && !rc.stopped) {
			err = rc.err ? rc.err : nerr(EIO);
			/* A cookie can point past the end after the directory
			 * shrank; treat that as end of directory. */
			if (cookie && !rc.err) {
				fsc_warn("ntfs: readdir of %llu resumed past end",
					 (unsigned long long)dir);
				err = 0;
			}
		}
	}
	close_ino(ni, &err);
	return err;
}

static int ntfs_dir_version_op(fsc_volume *v, fsc_ino dir, uint64_t *version)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	int err;

	ni = open_ino(nv, dir, &err);
	if (!ni)
		return err;
	*version = (uint64_t)sle64_to_cpu(ni->last_data_change_time);
	close_ino(ni, &err);
	return err;
}

static int ntfs_read_op(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len,
			void *buf, size_t *got)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	ntfs_attr *na;
	size_t total = 0;
	int err;

	ni = open_ino(nv, ino, &err);
	if (!ni)
		return err;
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		err = -EISDIR;
		goto out;
	}
	if (ni->flags & FILE_ATTR_REPARSE_POINT) {
		err = -EINVAL;
		goto out;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		err = nerr(EIO);
		goto out;
	}
	if (off < (uint64_t)na->data_size) {
		if (len > (uint64_t)na->data_size - off)
			len = (size_t)((uint64_t)na->data_size - off);
		while (total < len) {
			s64 r = ntfs_attr_pread(na, (s64)(off + total),
						(s64)(len - total),
						(char *)buf + total);
			if (r <= 0) {
				err = r < 0 ? nerr(EIO) : -EIO;
				break;
			}
			total += (size_t)r;
		}
	}
	ntfs_attr_close(na);
out:
	close_ino(ni, &err);
	*got = total;
	return err;
}

static int ntfs_write_op(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len,
			 const void *buf, size_t *written)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	ntfs_attr *na;
	size_t total = 0;
	int err;

	if (!is_user_ino(ino) || ino == FILE_root)
		return -EPERM;
	ni = open_ino(nv, ino, &err);
	if (!ni)
		return err;
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		err = -EISDIR;
		goto out;
	}
	if (ni->flags & FILE_ATTR_REPARSE_POINT) {
		err = -EINVAL;
		goto out;
	}
	na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
	if (!na) {
		err = nerr(EIO);
		goto out;
	}
	while (total < len) {
		s64 r = ntfs_attr_pwrite(na, (s64)(off + total),
					 (s64)(len - total),
					 (const char *)buf + total);
		if (r <= 0) {
			err = r < 0 ? nerr(EIO) : -EIO;
			break;
		}
		total += (size_t)r;
	}
	if (na->data_flags & ATTR_COMPRESSION_MASK)
		note_pclose(nv, ino);
	ntfs_attr_close(na);
	if (total) {
		ntfs_inode_update_times(ni, NTFS_UPDATE_MCTIME);
		set_archive(ni);
		nv->dirty = true;
	}
out:
	close_ino(ni, &err);
	*written = total;
	/* A short write is reported as success if anything was written. */
	if (total && err != -EIO)
		err = 0;
	return err;
}

/* Fails with -EEXIST if a name equal (ignoring case) exists in dir. */
static int check_free_name(ntfs_vol *nv, ntfs_inode *dir_ni, const char *name,
			   size_t len)
{
	u64 mref;
	int err = dir_lookup(nv, dir_ni, name, len, &mref);

	if (err == 0)
		return -EEXIST;
	return err == -ENOENT ? 0 : err;
}

static int ntfs_create_op(fsc_volume *v, fsc_ino dir, const char *name,
			  size_t len, fsc_type type, uint32_t mode,
			  uint32_t major, uint32_t minor, const fsc_cred *cred,
			  fsc_ino *ino)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *dir_ni, *ni = NULL;
	ntfschar *u = NULL;
	int ulen, err;

	(void)mode; (void)cred;
	if (!is_user_ino(dir))
		return -EPERM;
	err = to_uname(nv, name, len, false, &u, &ulen, NULL);
	if (err)
		return err;
	dir_ni = open_ino(nv, dir, &err);
	if (!dir_ni)
		goto out_free;
	if (!ni_is_dir(dir_ni)) {
		err = -ENOTDIR;
		goto out;
	}
	err = check_free_name(nv, dir_ni, name, len);
	if (err)
		goto out;

	errno = 0;
	switch (type) {
	case FSC_TYPE_REG:
		ni = ntfs_create(dir_ni, const_cpu_to_le32(0), u, (u8)ulen, S_IFREG);
		break;
	case FSC_TYPE_DIR:
		ni = ntfs_create(dir_ni, const_cpu_to_le32(0), u, (u8)ulen, S_IFDIR);
		break;
	case FSC_TYPE_FIFO:
		ni = ntfs_create(dir_ni, const_cpu_to_le32(0), u, (u8)ulen, S_IFIFO);
		break;
	case FSC_TYPE_SOCK:
		ni = ntfs_create(dir_ni, const_cpu_to_le32(0), u, (u8)ulen, S_IFSOCK);
		break;
	case FSC_TYPE_CHR:
	case FSC_TYPE_BLK:
		ni = ntfs_create_device(dir_ni, const_cpu_to_le32(0), u, (u8)ulen,
					type == FSC_TYPE_CHR ? S_IFCHR : S_IFBLK,
					makedev(major, minor));
		break;
	default:
		err = -EINVAL;
		goto out;
	}
	if (!ni) {
		err = nerr(EIO);
		goto out;
	}
	set_archive(ni);
	NInoSetDirty(ni);
	*ino = ni->mft_no;
	/* Closing ni needs dir_ni to sync the index; avoid double opening. */
	if (ntfs_inode_close_in_dir(ni, dir_ni))
		err = nerr(EIO);
	ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
	nv->dirty = true;
out:
	close_ino(dir_ni, &err);
out_free:
	free(u);
	return err;
}

static int ntfs_symlink_op(fsc_volume *v, fsc_ino dir, const char *name,
			   size_t len, const char *target, size_t tlen,
			   const fsc_cred *cred, fsc_ino *ino)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *dir_ni, *ni;
	ntfschar *u = NULL, *ut = NULL;
	char *tbuf;
	int ulen, utlen, err;

	(void)cred;
	if (!is_user_ino(dir))
		return -EPERM;
	if (tlen >= PATH_MAX)
		return -ENAMETOOLONG;
	err = to_uname(nv, name, len, false, &u, &ulen, NULL);
	if (err)
		return err;
	tbuf = malloc(tlen + 1);
	if (!tbuf) {
		free(u);
		return -ENOMEM;
	}
	memcpy(tbuf, target, tlen);
	tbuf[tlen] = 0;
	errno = 0;
	utlen = ntfs_mbstoucs(tbuf, &ut);
	free(tbuf);
	if (utlen < 0) {
		err = nerr(EILSEQ);
		free(u);
		return err;
	}
	dir_ni = open_ino(nv, dir, &err);
	if (!dir_ni)
		goto out_free;
	if (!ni_is_dir(dir_ni)) {
		err = -ENOTDIR;
		goto out;
	}
	err = check_free_name(nv, dir_ni, name, len);
	if (err)
		goto out;
	errno = 0;
	ni = ntfs_create_symlink(dir_ni, const_cpu_to_le32(0), u, (u8)ulen, ut,
				 utlen);
	if (!ni) {
		err = nerr(EIO);
		goto out;
	}
	set_archive(ni);
	NInoSetDirty(ni);
	*ino = ni->mft_no;
	if (ntfs_inode_close_in_dir(ni, dir_ni))
		err = nerr(EIO);
	ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
	nv->dirty = true;
out:
	close_ino(dir_ni, &err);
out_free:
	free(u);
	free(ut);
	return err;
}

static int ntfs_readlink_op(fsc_volume *v, fsc_ino ino, char *buf,
			    size_t bufsz, size_t *len)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	char *target = NULL;
	int err;

	ni = open_ino(nv, ino, &err);
	if (!ni)
		return err;
	if (ni->flags & FILE_ATTR_REPARSE_POINT) {
		errno = 0;
		target = ntfs_make_symlink(ni, nv->base.opts.mount_point);
		if (!target) {
			if (errno == EOPNOTSUPP) {
				/* Unknown reparse point: show something
				 * harmless rather than failing. */
				target = strdup("unsupported-reparse-point");
			} else {
				err = nerr(EIO);
			}
		}
	} else if (ni->flags & FILE_ATTR_SYSTEM) {
		INTX_FILE *ix = NULL;
		s64 dsize = 0;

		err = read_intx(ni, &ix, &dsize);
		if (!err && ix && ix->magic == INTX_SYMBOLIC_LINK) {
			if (ntfs_ucstombs(ix->target,
					  (int)((dsize - (s64)offsetof(INTX_FILE, target)) /
						(s64)sizeof(ntfschar)),
					  &target, 0) < 0)
				err = nerr(EILSEQ);
		} else if (!err) {
			err = -EINVAL;
		}
		free(ix);
	} else {
		err = -EINVAL;
	}
	close_ino(ni, &err);
	if (!err && target) {
		size_t n = strlen(target);

		if (n > bufsz)
			n = bufsz;
		memcpy(buf, target, n);
		*len = n;
	} else if (!err) {
		err = -EIO;
	}
	free(target);
	return err;
}

static int ntfs_link_op(fsc_volume *v, fsc_ino ino, fsc_ino dir,
			const char *name, size_t len)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni, *dir_ni = NULL;
	ntfschar *u = NULL;
	int ulen, err;

	if (!is_user_ino(ino) || ino == FILE_root || !is_user_ino(dir))
		return -EPERM;
	err = to_uname(nv, name, len, false, &u, &ulen, NULL);
	if (err)
		return err;
	ni = open_ino(nv, ino, &err);
	if (!ni)
		goto out_free;
	if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
		err = -EPERM;
		goto out;
	}
	dir_ni = open_ino(nv, dir, &err);
	if (!dir_ni)
		goto out;
	if (!ni_is_dir(dir_ni)) {
		err = -ENOTDIR;
		goto out;
	}
	err = check_free_name(nv, dir_ni, name, len);
	if (err)
		goto out;
	errno = 0;
	if (ntfs_link(ni, dir_ni, u, (u8)ulen)) {
		err = nerr(EIO);
		goto out;
	}
	set_archive(ni);
	ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
	ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
	nv->dirty = true;
out:
	/* Close the directory first, see lowntfs-3g ntfs_fuse_newlink(). */
	close_ino(dir_ni, &err);
	close_ino(ni, &err);
out_free:
	free(u);
	return err;
}

/* Low-level link by inode with a UTF-16 name (used by rename). */
static int link_raw(ntfs_vol *nv, fsc_ino ino, fsc_ino dir, ntfschar *u,
		    int ulen)
{
	ntfs_inode *ni, *dir_ni = NULL;
	int err;

	ni = open_ino(nv, ino, &err);
	if (!ni)
		return err;
	dir_ni = open_ino(nv, dir, &err);
	if (dir_ni) {
		errno = 0;
		if (ntfs_link(ni, dir_ni, u, (u8)ulen))
			err = nerr(EIO);
		else {
			ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
			ntfs_inode_update_times(dir_ni, NTFS_UPDATE_MCTIME);
		}
	}
	close_ino(dir_ni, &err);
	close_ino(ni, &err);
	return err;
}

/* Removes the name u (exact spelling) of ino from dir. */
static int unlink_raw(ntfs_vol *nv, fsc_ino ino, fsc_ino dir, ntfschar *u,
		      int ulen)
{
	ntfs_inode *ni, *dir_ni;
	int err;

	dir_ni = open_ino(nv, dir, &err);
	if (!dir_ni)
		return err;
	ni = open_ino(nv, ino, &err);
	if (!ni) {
		close_ino(dir_ni, NULL);
		return err;
	}
	errno = 0;
	if (ntfs_delete(nv->vol, NULL, ni, dir_ni, u, (u8)ulen))
		err = nerr(EIO);
	/* ntfs_delete() always closes ni and dir_ni. */
	if (!err)
		forget_pclose(nv, ino);
	return err;
}

/* Resolves name in dir to an inode and its exact on-disk spelling. */
static int resolve_entry(ntfs_vol *nv, fsc_ino dir, const char *name,
			 size_t len, fsc_ino *ino, ntfschar *exact,
			 int *exact_len, fsc_type *type)
{
	ntfs_inode *dir_ni, *ni;
	ntfschar *u;
	int ulen, err;
	u64 mref;

	dir_ni = open_ino(nv, dir, &err);
	if (!dir_ni)
		return err;
	if (!(dir_ni->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
		close_ino(dir_ni, NULL);
		return -ENOTDIR;
	}
	err = dir_lookup(nv, dir_ni, name, len, &mref);
	close_ino(dir_ni, &err);
	if (err)
		return err;
	*ino = MREF(mref);
	ni = open_ino(nv, *ino, &err);
	if (!ni)
		return err;
	if (type)
		*type = presented_type(nv, ni);
	/* Find the exact on-disk spelling: mapped name, then unmapped. */
	err = -ENOENT;
	for (int raw = 0; raw < 2 && err; raw++) {
		if (to_uname(nv, name, len, raw, &u, &ulen, NULL))
			continue;
		err = actual_name(nv, ni, dir, u, ulen, exact, exact_len);
		free(u);
	}
	if (err) {
		/* Matched through a DOS (8.3) name: hand that to
		 * ntfs_delete(), which removes the DOS and Win32 names. */
		err = to_uname(nv, name, len, false, &u, &ulen, NULL);
		if (!err) {
			memcpy(exact, u, (size_t)ulen * sizeof(ntfschar));
			*exact_len = ulen;
			free(u);
		}
	}
	close_ino(ni, &err);
	return err;
}

static int ntfs_unlink_op(fsc_volume *v, fsc_ino dir, const char *name,
			  size_t len, bool isdir)
{
	ntfs_vol *nv = NV(v);
	ntfschar exact[NTFS_NAME_MAX_UNITS + 1];
	int elen = 0, err;
	fsc_ino ino;
	fsc_type type;

	if (!is_user_ino(dir) || dir == FILE_Extend)
		return -EPERM;
	err = resolve_entry(nv, dir, name, len, &ino, exact, &elen, &type);
	if (err)
		return err;
	if (!is_user_ino(ino) || ino == FILE_root)
		return -EPERM;
	if (isdir && type != FSC_TYPE_DIR)
		return -ENOTDIR;
	if (!isdir && type == FSC_TYPE_DIR)
		return -EPERM; /* POSIX unlink(2) on a directory */
	if (isdir) {
		ntfs_inode *ni = open_ino(nv, ino, &err);
		if (!ni)
			return err;
		errno = 0;
		if (ntfs_check_empty_dir(ni))
			err = nerr(ENOTEMPTY);
		close_ino(ni, &err);
		if (err)
			return err;
	}
	err = unlink_raw(nv, ino, dir, exact, elen);
	if (!err)
		nv->dirty = true;
	return err;
}

static int ntfs_rename_op(fsc_volume *v, fsc_ino sdir, const char *sname,
			  size_t slen, fsc_ino ddir, const char *dname,
			  size_t dlen)
{
	ntfs_vol *nv = NV(v);
	ntfschar sexact[NTFS_NAME_MAX_UNITS + 1], texact[NTFS_NAME_MAX_UNITS + 1];
	ntfschar *du = NULL, *tmpu = NULL;
	int selen = 0, telen = 0, dulen, tmplen, err;
	fsc_ino src, tgt = 0;
	fsc_type stype, ttype = FSC_TYPE_NONE;
	char tmpname[64];
	static uint32_t seq;

	if (!is_user_ino(sdir) || !is_user_ino(ddir) || sdir == FILE_Extend ||
	    ddir == FILE_Extend)
		return -EPERM;
	err = resolve_entry(nv, sdir, sname, slen, &src, sexact, &selen, &stype);
	if (err)
		return err;
	if (!is_user_ino(src) || src == FILE_root)
		return -EPERM;
	err = to_uname(nv, dname, dlen, false, &du, &dulen, NULL);
	if (err)
		return err;

	err = resolve_entry(nv, ddir, dname, dlen, &tgt, texact, &telen, &ttype);
	if (err == -ENOENT) {
		tgt = 0;
		telen = 0;
	} else if (err) {
		goto out;
	}

	snprintf(tmpname, sizeof(tmpname), ".fsmac-rename-%08x-%llx", ++seq,
		 (unsigned long long)src);
	err = to_uname(nv, tmpname, strlen(tmpname), true, &tmpu, &tmplen, NULL);
	if (err)
		goto out;

	if (telen && tgt == src) {
		/* Same inode. Either a case-only rename of one name, or two
		 * hard links to the same file (POSIX: do nothing). */
		if (sdir == ddir && telen == selen &&
		    !memcmp(texact, sexact, (size_t)selen * sizeof(ntfschar)) &&
		    !(selen == dulen &&
		      !memcmp(sexact, du, (size_t)selen * sizeof(ntfschar)))) {
			/* Case-only change: go through a temporary name so the
			 * inode always keeps at least one link. */
			err = link_raw(nv, src, sdir, tmpu, tmplen);
			if (err)
				goto out;
			err = unlink_raw(nv, src, sdir, sexact, selen);
			if (!err)
				err = link_raw(nv, src, ddir, du, dulen);
			if (err) {
				link_raw(nv, src, sdir, sexact, selen);
				unlink_raw(nv, src, sdir, tmpu, tmplen);
				goto out;
			}
			err = unlink_raw(nv, src, sdir, tmpu, tmplen);
		} else {
			err = 0;
		}
		goto out;
	}

	if (telen) {
		/* Replace an existing target, keeping it recoverable until the
		 * new name is in place (lowntfs-3g "safe rename"). */
		if (stype == FSC_TYPE_DIR && ttype != FSC_TYPE_DIR) {
			err = -ENOTDIR;
			goto out;
		}
		if (stype != FSC_TYPE_DIR && ttype == FSC_TYPE_DIR) {
			err = -EISDIR;
			goto out;
		}
		if (ttype == FSC_TYPE_DIR) {
			ntfs_inode *tni = open_ino(nv, tgt, &err);
			if (!tni)
				goto out;
			errno = 0;
			if (ntfs_check_empty_dir(tni))
				err = nerr(ENOTEMPTY);
			close_ino(tni, &err);
			if (err)
				goto out;
		}
		err = link_raw(nv, tgt, ddir, tmpu, tmplen);
		if (err)
			goto out;
		err = unlink_raw(nv, tgt, ddir, texact, telen);
		if (err) {
			unlink_raw(nv, tgt, ddir, tmpu, tmplen);
			goto out;
		}
		err = link_raw(nv, src, ddir, du, dulen);
		if (err) {
			link_raw(nv, tgt, ddir, texact, telen);
			unlink_raw(nv, tgt, ddir, tmpu, tmplen);
			goto out;
		}
		err = unlink_raw(nv, src, sdir, sexact, selen);
		if (err) {
			unlink_raw(nv, src, ddir, du, dulen);
			link_raw(nv, tgt, ddir, texact, telen);
			unlink_raw(nv, tgt, ddir, tmpu, tmplen);
			goto out;
		}
		/* Finally drop the old target (frees it if last link). */
		if (unlink_raw(nv, tgt, ddir, tmpu, tmplen))
			fsc_warn("ntfs: rename left temporary name %s", tmpname);
	} else {
		err = link_raw(nv, src, ddir, du, dulen);
		if (err)
			goto out;
		err = unlink_raw(nv, src, sdir, sexact, selen);
		if (err)
			unlink_raw(nv, src, ddir, du, dulen);
	}
out:
	if (!err)
		nv->dirty = true;
	free(du);
	free(tmpu);
	return err;
}

static int ntfs_setattr_op(fsc_volume *v, fsc_ino ino, const fsc_sattr *sa)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	int err = 0;
	ntfs_time_update_flags mask = 0;

	if (!is_user_ino(ino))
		return -EPERM;
	ni = open_ino(nv, ino, &err);
	if (!ni)
		return err;

	if (sa->mask & FSC_SET_SIZE) {
		ntfs_attr *na;
		s64 oldsize;

		if (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY ||
		    ni->flags & FILE_ATTR_REPARSE_POINT || ino == FILE_root) {
			err = (ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) ? -EISDIR
									 : -EINVAL;
			goto out;
		}
		na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
		if (!na) {
			err = nerr(EIO);
			goto out;
		}
		oldsize = na->data_size;
		errno = 0;
		if ((na->data_flags & ATTR_COMPRESSION_MASK) &&
		    (s64)sa->size > na->initialized_size) {
			char zero = 0;
			/* Upsizing a compressed file: write a final zero, which
			 * becomes a hole where possible (as lowntfs-3g does). */
			if (ntfs_attr_pwrite(na, (s64)sa->size - 1, 1, &zero) <= 0)
				err = nerr(EIO);
			note_pclose(nv, ino);
		} else if (ntfs_attr_truncate(na, (s64)sa->size)) {
			err = nerr(EIO);
		}
		ntfs_attr_close(na);
		if (err)
			goto out;
		if (oldsize != (s64)sa->size)
			set_archive(ni);
		mask |= NTFS_UPDATE_MCTIME;
	}
	if (sa->mask & FSC_SET_MODE) {
		/* No Unix permissions on NTFS: map the owner write bit of
		 * regular files to the Windows read-only attribute. */
		if (!(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY) &&
		    !(ni->flags & FILE_ATTR_REPARSE_POINT)) {
			le32 old = ni->flags;
			if (sa->mode & 0200)
				ni->flags &= ~FILE_ATTR_READONLY;
			else
				ni->flags |= FILE_ATTR_READONLY;
			if (old != ni->flags) {
				NInoSetDirty(ni);
				NInoFileNameSetDirty(ni);
				mask |= NTFS_UPDATE_CTIME;
			}
		}
	}
	if (sa->mask & FSC_SET_FLAGS) {
		le32 old = ni->flags;
		if (sa->flags & FSC_FLAG_HIDDEN)
			ni->flags |= FILE_ATTR_HIDDEN;
		else
			ni->flags &= ~FILE_ATTR_HIDDEN;
		if (old != ni->flags) {
			NInoSetDirty(ni);
			NInoFileNameSetDirty(ni);
			mask |= NTFS_UPDATE_CTIME;
		}
	}
	/* uid/gid are not stored: accept silently so copies don't fail. */
	if (sa->mask & (FSC_SET_ATIME | FSC_SET_MTIME | FSC_SET_ATIME_NOW |
			FSC_SET_MTIME_NOW | FSC_SET_BTIME)) {
		if (sa->mask & FSC_SET_ATIME_NOW)
			mask |= NTFS_UPDATE_ATIME;
		else if (sa->mask & FSC_SET_ATIME)
			ni->last_access_time = timespec2ntfs(sa->atime);
		if (sa->mask & FSC_SET_MTIME_NOW)
			mask |= NTFS_UPDATE_MTIME;
		else if (sa->mask & FSC_SET_MTIME)
			ni->last_data_change_time = timespec2ntfs(sa->mtime);
		if (sa->mask & FSC_SET_BTIME)
			ni->creation_time = timespec2ntfs(sa->btime);
		mask |= NTFS_UPDATE_CTIME;
		NInoSetDirty(ni);
		NInoFileNameSetDirty(ni);
	}
	if (mask)
		ntfs_inode_update_times(ni, mask);
	nv->dirty = true;
out:
	close_ino(ni, &err);
	return err;
}

/* ---------------------------------------------------------------------- */
/* Extended attributes = named data streams                                */

static bool xattrs_allowed(ntfs_inode *ni)
{
	/* No streams on reparse points (symlinks) and Interix specials. */
	return !(ni->flags & (FILE_ATTR_REPARSE_POINT)) &&
	       !((ni->flags & FILE_ATTR_SYSTEM) &&
		 !(ni->mrec->flags & MFT_RECORD_IS_DIRECTORY));
}

static int xattr_uname(ntfs_vol *nv, const char *name, ntfschar **u, int *ulen)
{
	size_t len = strlen(name);
	int err = to_uname(nv, name, len, false, u, ulen, NULL);

	return err == -EILSEQ ? -EINVAL : err;
}

static int ntfs_listxattr_op(fsc_volume *v, fsc_ino ino, char *buf,
			     size_t bufsz, size_t *len)
{
	ntfs_vol *nv = NV(v);
	ntfs_attr_search_ctx *ctx;
	ntfs_inode *ni;
	size_t total = 0;
	int err;

	ni = open_ino(nv, ino, &err);
	if (!ni)
		return err;
	if (!xattrs_allowed(ni))
		goto out;
	ctx = ntfs_attr_get_search_ctx(ni, NULL);
	if (!ctx) {
		err = nerr(ENOMEM);
		goto out;
	}
	while (!ntfs_attr_lookup(AT_DATA, NULL, 0, CASE_SENSITIVE, 0, NULL, 0,
				 ctx)) {
		ATTR_RECORD *ar = ctx->attr;
		char *name = NULL;
		int nlen;

		if (!ar->name_length)
			continue;
		if (from_uname(nv, (ntfschar *)((u8 *)ar +
				le16_to_cpu(ar->name_offset)),
			       ar->name_length, &name, &nlen))
			continue;
		if (buf) {
			if (total + (size_t)nlen + 1 > bufsz) {
				free(name);
				err = -ERANGE;
				break;
			}
			memcpy(buf + total, name, (size_t)nlen + 1);
		}
		total += (size_t)nlen + 1;
		free(name);
	}
	ntfs_attr_put_search_ctx(ctx);
out:
	close_ino(ni, &err);
	*len = total;
	return err;
}

static int ntfs_getxattr_op(fsc_volume *v, fsc_ino ino, const char *name,
			    void *buf, size_t bufsz, size_t *len)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	ntfs_attr *na = NULL;
	ntfschar *u = NULL;
	int ulen, err;

	err = xattr_uname(nv, name, &u, &ulen);
	if (err)
		return err;
	ni = open_ino(nv, ino, &err);
	if (!ni)
		goto out_free;
	if (!xattrs_allowed(ni)) {
		err = -ENOATTR;
		goto out;
	}
	na = ntfs_attr_open(ni, AT_DATA, u, (u32)ulen);
	if (!na) {
		err = -ENOATTR;
		goto out;
	}
	*len = (size_t)na->data_size;
	if (buf) {
		if ((size_t)na->data_size > bufsz) {
			err = -ERANGE;
		} else if (na->data_size &&
			   ntfs_attr_pread(na, 0, na->data_size, buf) !=
				   na->data_size) {
			err = nerr(EIO);
		}
	}
	ntfs_attr_close(na);
out:
	close_ino(ni, &err);
out_free:
	free(u);
	return err;
}

static int ntfs_setxattr_op(fsc_volume *v, fsc_ino ino, const char *name,
			    const void *val, size_t len, unsigned flags)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	ntfs_attr *na;
	ntfschar *u = NULL;
	int ulen, err;

	if (!is_user_ino(ino))
		return -EPERM;
	err = xattr_uname(nv, name, &u, &ulen);
	if (err)
		return err;
	ni = open_ino(nv, ino, &err);
	if (!ni)
		goto out_free;
	if (!xattrs_allowed(ni)) {
		err = -EPERM;
		goto out;
	}
	na = ntfs_attr_open(ni, AT_DATA, u, (u32)ulen);
	if (na && (flags & FSC_XATTR_CREATE)) {
		ntfs_attr_close(na);
		err = -EEXIST;
		goto out;
	}
	if (!na) {
		if (flags & FSC_XATTR_REPLACE) {
			err = -ENOATTR;
			goto out;
		}
		errno = 0;
		if (ntfs_attr_add(ni, AT_DATA, u, (u8)ulen, NULL, 0)) {
			err = nerr(EIO);
			goto out;
		}
		set_archive(ni);
		NInoFileNameSetDirty(ni);
		na = ntfs_attr_open(ni, AT_DATA, u, (u32)ulen);
		if (!na) {
			err = nerr(EIO);
			goto out;
		}
	}
	errno = 0;
	if (ntfs_attr_truncate(na, (s64)len) ||
	    (len && ntfs_attr_pwrite(na, 0, (s64)len, val) != (s64)len))
		err = nerr(EIO);
	ntfs_attr_close(na);
	if (!err) {
		ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
		nv->dirty = true;
	}
out:
	close_ino(ni, &err);
out_free:
	free(u);
	return err;
}

static int ntfs_removexattr_op(fsc_volume *v, fsc_ino ino, const char *name)
{
	ntfs_vol *nv = NV(v);
	ntfs_inode *ni;
	ntfschar *u = NULL;
	int ulen, err;

	if (!is_user_ino(ino))
		return -EPERM;
	err = xattr_uname(nv, name, &u, &ulen);
	if (err)
		return err;
	ni = open_ino(nv, ino, &err);
	if (!ni)
		goto out_free;
	if (!xattrs_allowed(ni) ||
	    !ntfs_attr_exist(ni, AT_DATA, u, (u32)ulen)) {
		err = -ENOATTR;
		goto out;
	}
	errno = 0;
	if (ntfs_attr_remove(ni, AT_DATA, u, (u32)ulen)) {
		err = errno == ENOENT ? -ENOATTR : nerr(EIO);
	} else {
		set_archive(ni);
		ntfs_inode_update_times(ni, NTFS_UPDATE_CTIME);
		nv->dirty = true;
	}
out:
	close_ino(ni, &err);
out_free:
	free(u);
	return err;
}

/* ---------------------------------------------------------------------- */
/* Maintenance                                                             */

static int ntfs_sync_op(fsc_volume *v, int level)
{
	ntfs_vol *nv = NV(v);
	int err = flush_pclose(nv);
	int e = fsc_dev_flush(v->dev, level == FSC_SYNC_FULL);

	if (e && !err)
		err = e;
	if (!err && level == FSC_SYNC_FULL)
		nv->dirty = false;
	return err;
}

static bool ntfs_is_dirty_op(fsc_volume *v)
{
	fsc_dev_stats st;

	fsc_dev_get_stats(v->dev, &st);
	return NV(v)->dirty || NV(v)->npclose || st.dirty_pages;
}

static int ntfs_set_label_op(fsc_volume *v, const char *label)
{
	ntfs_vol *nv = NV(v);
	ntfschar *u = NULL;
	int ulen = 0, err;
	char buf[NTFS_NAME_MAX_BYTES + 1];
	size_t len = strlen(label);

	if (len > NTFS_NAME_MAX_BYTES || !fsc_utf8_valid(label, len))
		return -EINVAL;
	memcpy(buf, label, len + 1);
	if (len) {
		errno = 0;
		ulen = ntfs_mbstoucs(buf, &u);
		if (ulen < 0)
			return nerr(EINVAL);
		if (ulen > 128) {
			free(u);
			return -ENAMETOOLONG;
		}
	}
	errno = 0;
	err = ntfs_volume_rename(nv->vol, u, ulen) ? nerr(EIO) : 0;
	free(u);
	if (!err)
		nv->dirty = true;
	return err;
}

const fsc_ops fsc_ntfs_ops = {
	.name = "ntfs",
	.probe = ntfs_probe,
	.mount = ntfs_mount_vol,
	.unmount = ntfs_unmount_vol,
	.volinfo = ntfs_volinfo,
	.statfs = ntfs_statfs_vol,
	.getattr = ntfs_getattr_op,
	.lookup = ntfs_lookup_op,
	.parent = ntfs_parent_op,
	.readdir = ntfs_readdir_op,
	.dir_version = ntfs_dir_version_op,
	.read = ntfs_read_op,
	.write = ntfs_write_op,
	.create = ntfs_create_op,
	.symlink = ntfs_symlink_op,
	.readlink = ntfs_readlink_op,
	.link = ntfs_link_op,
	.unlink = ntfs_unlink_op,
	.rename = ntfs_rename_op,
	.setattr = ntfs_setattr_op,
	.listxattr = ntfs_listxattr_op,
	.getxattr = ntfs_getxattr_op,
	.setxattr = ntfs_setxattr_op,
	.removexattr = ntfs_removexattr_op,
	.sync = ntfs_sync_op,
	.is_dirty = ntfs_is_dirty_op,
	.set_label = ntfs_set_label_op,
};
