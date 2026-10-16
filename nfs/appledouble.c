/*
 * AppleDouble translation (see appledouble.h).
 *
 * Format (as written by xnu's vfs_xattr.c; all fields big-endian):
 *
 *   0x00  magic 0x00051607, version 0x00020000, filler "Mac OS X        "
 *   0x18  numEntries (u16), then 2 entries of {type, offset, length}:
 *         FinderInfo (9) at 0x32, covering the attribute area as well,
 *         ResourceFork (2) after it
 *   0x32  FinderInfo (32 bytes), 2 pad bytes
 *   0x54  attribute header: magic 'ATTR', debug_tag, total_size,
 *         data_start, data_length, reserved[3], flags (u16), num_attrs (u16)
 *   0x78  attribute entries {offset, length, flags (u16), namelen (u8),
 *         name (NUL terminated)}, each padded to 4 bytes, then the data.
 *
 * A companion's bytes live in memory while the client works on it; once it
 * parses cleanly and the client commits (or goes quiet) the contents are
 * applied to the base object's native extended attributes.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "appledouble.h"

#define ADH_MAGIC 0x00051607u
#define ADH_VERSION 0x00020000u
#define AD_RESOURCE 2u
#define AD_FINDERINFO 9u
#define ATTR_HDR_MAGIC 0x41545452u /* 'ATTR' */
#define FINFO_OFF 0x32
#define ATTR_HDR_OFF 0x54
#define ATTR_ENTRIES_OFF 0x78
#define ATTR_BUF_SIZE 4096
#define MAX_AD_SIZE (64u << 20)

#define XATTR_FINDERINFO "com.apple.FinderInfo"
#define XATTR_RESOURCEFORK "com.apple.ResourceFork"

#define AD_BUCKETS 1024
#define AD_MAX_OBJS 4096
#define AD_APPLY_IDLE 0.5 /* seconds */

/* The placeholder resource fork xnu puts into every AppleDouble file
 * ("This resource fork intentionally left blank"). */
static const uint8_t empty_rsrc[286] = {
	0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x1e, 0x54, 0x68, 0x69, 0x73, 0x20, 0x72, 0x65, 0x73,
	0x6f, 0x75, 0x72, 0x63, 0x65, 0x20, 0x66, 0x6f, 0x72, 0x6b, 0x20, 0x69,
	0x6e, 0x74, 0x65, 0x6e, 0x74, 0x69, 0x6f, 0x6e, 0x61, 0x6c, 0x6c, 0x79,
	0x20, 0x6c, 0x65, 0x66, 0x74, 0x20, 0x62, 0x6c, 0x61, 0x6e, 0x6b, 0x20,
	0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x1e, 0xff, 0xff,
};


typedef struct ad_obj {
	fsc_ino ino;
	uint32_t gen;
	uint8_t *buf;
	size_t len, cap;
	bool loaded;
	bool dirty;
	double dirty_since;
	double last_used;
	/* Where the companion was last looked up (for spilling to disk). */
	fsc_ino dir;
	char name[256];
	struct ad_obj *next;
} ad_obj;

typedef struct ad_state {
	ad_obj *bucket[AD_BUCKETS];
	size_t count;
	bool ntfs;
} ad_state;

/* ---------------------------------------------------------------------- */

static inline uint32_t rd32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline void wr32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static inline void wr16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

static inline size_t entry_len(size_t namelen)
{
	return (11 + namelen + 3) & ~(size_t)3;
}

static bool all_zero(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (p[i])
			return false;
	return true;
}

/* ---------------------------------------------------------------------- */
/* Object cache                                                            */

static size_t slot(fsc_ino ino)
{
	return (size_t)((ino * 0x9E3779B97F4A7C15ull) >> 40) % AD_BUCKETS;
}

static ad_obj *find_obj(ad_state *st, fsc_ino ino)
{
	for (ad_obj *o = st->bucket[slot(ino)]; o; o = o->next)
		if (o->ino == ino)
			return o;
	return NULL;
}

static void free_obj(ad_state *st, ad_obj *o)
{
	ad_obj **pp = &st->bucket[slot(o->ino)];

	while (*pp && *pp != o)
		pp = &(*pp)->next;
	if (*pp)
		*pp = o->next;
	free(o->buf);
	free(o);
	st->count--;
}

static int reserve(ad_obj *o, size_t need)
{
	uint8_t *nb;
	size_t ncap;

	if (need > MAX_AD_SIZE)
		return -EFBIG;
	if (o->cap >= need)
		return 0;
	ncap = o->cap ? o->cap : 4096;
	while (ncap < need)
		ncap *= 2;
	nb = realloc(o->buf, ncap);
	if (!nb)
		return -ENOMEM;
	o->buf = nb;
	o->cap = ncap;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Native attribute access                                                 */

typedef struct xattr_kv {
	char *name;
	uint8_t *val;
	size_t len;
} xattr_kv;

typedef struct xattr_set {
	xattr_kv *v;
	size_t n;
} xattr_set;

static void set_free(xattr_set *s)
{
	for (size_t i = 0; i < s->n; i++) {
		free(s->v[i].name);
		free(s->v[i].val);
	}
	free(s->v);
	s->v = NULL;
	s->n = 0;
}

static xattr_kv *set_find(xattr_set *s, const char *name)
{
	for (size_t i = 0; i < s->n; i++)
		if (!strcmp(s->v[i].name, name))
			return &s->v[i];
	return NULL;
}

static int set_add(xattr_set *s, const char *name, const void *val, size_t len)
{
	xattr_kv *nv = realloc(s->v, (s->n + 1) * sizeof(*nv));

	if (!nv)
		return -ENOMEM;
	s->v = nv;
	nv[s->n].name = strdup(name);
	nv[s->n].val = malloc(len ? len : 1);
	nv[s->n].len = len;
	if (!nv[s->n].name || !nv[s->n].val) {
		free(nv[s->n].name);
		free(nv[s->n].val);
		return -ENOMEM;
	}
	if (len)
		memcpy(nv[s->n].val, val, len);
	s->n++;
	return 0;
}

/* Reads all native extended attributes of ino. */
static int native_read(nfs_server *srv, fsc_ino ino, xattr_set *s)
{
	size_t len = 0, i;
	char *names;
	int err;

	memset(s, 0, sizeof(*s));
	err = fsc_listxattr(srv->vol, ino, NULL, 0, &len);
	if (err)
		return err;
	if (!len)
		return 0;
	names = malloc(len);
	if (!names)
		return -ENOMEM;
	err = fsc_listxattr(srv->vol, ino, names, len, &len);
	for (i = 0; !err && i < len; i += strlen(names + i) + 1) {
		size_t vlen = 0;
		uint8_t *val;

		if (fsc_getxattr(srv->vol, ino, names + i, NULL, 0, &vlen))
			continue;
		val = malloc(vlen ? vlen : 1);
		if (!val) {
			err = -ENOMEM;
			break;
		}
		if (fsc_getxattr(srv->vol, ino, names + i, val, vlen, &vlen) == 0)
			err = set_add(s, names + i, val, vlen);
		free(val);
	}
	free(names);
	if (err)
		set_free(s);
	return err;
}

/* ---------------------------------------------------------------------- */
/* Synthesis: native attributes -> AppleDouble image                       */

static int synthesize(nfs_server *srv, ad_obj *o, const fsc_attr *base)
{
	ad_state *st = srv->ad;
	xattr_set s;
	uint8_t finfo[32] = {0};
	const xattr_kv *rsrc = NULL;
	size_t nattrs = 0, ents = 0, data = 0, total, rsrc_off, pos, dpos;
	int err;

	o->len = 0;
	o->loaded = true;
	if (base->type == FSC_TYPE_LNK)
		return 0;
	err = native_read(srv, o->ino, &s);
	if (err)
		return err;
	for (size_t i = 0; i < s.n; i++) {
		if (!strcmp(s.v[i].name, XATTR_FINDERINFO)) {
			memcpy(finfo, s.v[i].val, s.v[i].len < 32 ? s.v[i].len : 32);
		} else if (!strcmp(s.v[i].name, XATTR_RESOURCEFORK)) {
			rsrc = &s.v[i];
		} else if (strlen(s.v[i].name) < 128) {
			nattrs++;
			ents += entry_len(strlen(s.v[i].name) + 1);
			data += s.v[i].len;
		}
	}
	/* Windows hidden files are invisible in Finder too. */
	if (st->ntfs && (base->flags & FSC_FLAG_HIDDEN))
		finfo[8] |= 0x40;
	if (!nattrs && !rsrc && all_zero(finfo, 32)) {
		set_free(&s);
		return 0; /* nothing to present */
	}

	/* Mirror xnu's layout exactly: the attribute area fills the
	 * FinderInfo entry, which ends where the resource fork begins (the
	 * attribute header's total_size), and an empty fork is the 286-byte
	 * placeholder. A file without large attributes is 4096 bytes. */
	total = (ATTR_ENTRIES_OFF + ents + data + 3) & ~(size_t)3;
	{
		size_t rlen = rsrc ? rsrc->len : sizeof(empty_rsrc);
		size_t min_off = rsrc ? ATTR_BUF_SIZE - sizeof(empty_rsrc)
				      : ATTR_BUF_SIZE - rlen;
		rsrc_off = total > min_off ? total : min_off;
		err = reserve(o, rsrc_off + rlen);
		if (err) {
			set_free(&s);
			return err;
		}
		memset(o->buf, 0, rsrc_off);
		wr32(o->buf + 0, ADH_MAGIC);
		wr32(o->buf + 4, ADH_VERSION);
		memcpy(o->buf + 8, "Mac OS X        ", 16);
		wr16(o->buf + 24, 2);
		wr32(o->buf + 26, AD_FINDERINFO);
		wr32(o->buf + 30, FINFO_OFF);
		wr32(o->buf + 34, (uint32_t)(rsrc_off - FINFO_OFF));
		wr32(o->buf + 38, AD_RESOURCE);
		wr32(o->buf + 42, (uint32_t)rsrc_off);
		wr32(o->buf + 46, (uint32_t)rlen);
		memcpy(o->buf + FINFO_OFF, finfo, 32);
		if (rsrc)
			memcpy(o->buf + rsrc_off, rsrc->val, rsrc->len);
		else
			memcpy(o->buf + rsrc_off, empty_rsrc, sizeof(empty_rsrc));
		o->len = rsrc_off + rlen;
	}
	wr32(o->buf + ATTR_HDR_OFF, ATTR_HDR_MAGIC);
	wr32(o->buf + ATTR_HDR_OFF + 4, 0);
	wr32(o->buf + ATTR_HDR_OFF + 8, (uint32_t)rsrc_off);
	wr32(o->buf + ATTR_HDR_OFF + 12, (uint32_t)(ATTR_ENTRIES_OFF + ents));
	wr32(o->buf + ATTR_HDR_OFF + 16, (uint32_t)data);
	wr16(o->buf + ATTR_HDR_OFF + 32, 0);
	wr16(o->buf + ATTR_HDR_OFF + 34, (uint16_t)nattrs);
	pos = ATTR_ENTRIES_OFF;
	dpos = ATTR_ENTRIES_OFF + ents;
	for (size_t i = 0; i < s.n; i++) {
		const char *n = s.v[i].name;
		size_t nl = strlen(n) + 1;

		if (!strcmp(n, XATTR_FINDERINFO) || !strcmp(n, XATTR_RESOURCEFORK) ||
		    nl > 128)
			continue;
		wr32(o->buf + pos, (uint32_t)dpos);
		wr32(o->buf + pos + 4, (uint32_t)s.v[i].len);
		wr16(o->buf + pos + 8, 0);
		o->buf[pos + 10] = (uint8_t)nl;
		memcpy(o->buf + pos + 11, n, nl);
		pos += entry_len(nl);
		memcpy(o->buf + dpos, s.v[i].val, s.v[i].len);
		dpos += s.v[i].len;
	}
	set_free(&s);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Parsing: AppleDouble image -> attribute set                             */

/* Returns 0 and fills the desired attributes (FinderInfo and resource fork
 * included by name), or -EINVAL if the image is not (yet) well formed. */
static int parse(const uint8_t *b, size_t len, xattr_set *out,
		 uint8_t finfo[32])
{
	uint16_t nent;
	uint32_t foff = 0, flen = 0, roff = 0, rlen = 0;
	bool have_f = false;

	memset(out, 0, sizeof(*out));
	memset(finfo, 0, 32);
	if (len < 26 || rd32(b) != ADH_MAGIC || rd32(b + 4) != ADH_VERSION)
		return -EINVAL;
	nent = rd16(b + 24);
	if (nent < 1 || nent > 15 || 26 + (size_t)nent * 12 > len)
		return -EINVAL;
	for (uint16_t i = 0; i < nent; i++) {
		const uint8_t *e = b + 26 + i * 12;
		uint32_t type = rd32(e), off = rd32(e + 4), l = rd32(e + 8);

		if ((uint64_t)off + l > len)
			return -EINVAL;
		if (type == AD_FINDERINFO) {
			foff = off;
			flen = l;
			have_f = true;
		} else if (type == AD_RESOURCE) {
			roff = off;
			rlen = l;
		}
	}
	if (have_f && flen >= 32)
		memcpy(finfo, b + foff, 32);
	/* Extended attributes live inside the FinderInfo entry. */
	if (have_f && foff == FINFO_OFF && flen >= ATTR_ENTRIES_OFF - FINFO_OFF &&
	    rd32(b + ATTR_HDR_OFF) == ATTR_HDR_MAGIC) {
		uint32_t total = rd32(b + ATTR_HDR_OFF + 8);
		uint16_t nattrs = rd16(b + ATTR_HDR_OFF + 34);
		size_t pos = ATTR_ENTRIES_OFF;

		if (total > len)
			return -EINVAL;
		for (uint16_t i = 0; i < nattrs; i++) {
			uint32_t off, l;
			uint8_t nl;
			char name[256];

			if (pos + 11 > total)
				return -EINVAL;
			off = rd32(b + pos);
			l = rd32(b + pos + 4);
			nl = b[pos + 10];
			if (nl < 2 || pos + 11 + nl > total ||
			    b[pos + 11 + nl - 1] != 0 || (uint64_t)off + l > total)
				return -EINVAL;
			memcpy(name, b + pos + 11, nl);
			if (memchr(name, 0, nl - 1) || set_add(out, name, b + off, l)) {
				set_free(out);
				return -EINVAL;
			}
			pos += entry_len(nl);
		}
	}
	/* macOS writes a 286-byte placeholder fork into every AppleDouble
	 * file; it is not a real resource fork. */
	if (rlen == 286 && !memcmp(b + roff + 16,
				   "This resource fork intentionally left blank", 43))
		rlen = 0;
	if (rlen && set_add(out, XATTR_RESOURCEFORK, b + roff, rlen)) {
		set_free(out);
		return -ENOMEM;
	}
	return 0;
}

/*
 * Stores the companion as a real "._name" file. Used when native attributes
 * cannot hold the data (ext4 limits an inode's attributes to about one
 * block). A real companion takes precedence over the virtual one from then
 * on, so nothing is lost.
 */
static int spill(nfs_server *srv, ad_obj *o)
{
	fsc_ino dir_ino = o->dir, file;
	fsc_attr da;
	fsc_cred cred;
	fsc_sattr sa = {0};
	size_t nlen = strlen(o->name), w;
	int err;

	if (!dir_ino || !nlen)
		return -E2BIG;
	err = fsc_lookup(srv->vol, dir_ino, o->name, nlen, &file, NULL);
	if (err == -ENOENT) {
		err = fsc_getattr(srv->vol, dir_ino, &da);
		if (err)
			return err;
		cred.uid = da.uid;
		cred.gid = da.gid;
		err = fsc_create(srv->vol, dir_ino, o->name, nlen, FSC_TYPE_REG, 0644,
				 0, 0, &cred, &file, NULL);
	}
	if (err)
		return err;
	sa.mask = FSC_SET_SIZE;
	sa.size = 0;
	err = fsc_setattr(srv->vol, file, &sa, NULL);
	if (!err && o->len)
		err = fsc_write(srv->vol, file, 0, o->len, o->buf, &w);
	if (!err) {
		/* The file is now the single source of truth. */
		xattr_set have;
		if (native_read(srv, o->ino, &have) == 0) {
			for (size_t i = 0; i < have.n; i++)
				fsc_removexattr(srv->vol, o->ino, have.v[i].name);
			set_free(&have);
		}
		fsc_log(FSC_LOG_INFO, "attributes of inode %llu stored in %s",
			(unsigned long long)o->ino, o->name);
	}
	return err;
}

/* Applies a parsed image to the base object's native attributes. */
static int apply(nfs_server *srv, ad_obj *o)
{
	nfs_log(srv, 2, "AD apply inode %llu (%zu bytes)",
		(unsigned long long)o->ino, o->len);
	ad_state *st = srv->ad;
	xattr_set want, have;
	uint8_t finfo[32];
	fsc_attr base;
	int err, first_err = 0;

	if (fsc_getattr(srv->vol, o->ino, &base) || base.gen != o->gen) {
		o->dirty = false; /* base is gone */
		return 0;
	}
	if (base.type == FSC_TYPE_LNK) {
		/* Neither NTFS reparse points nor Linux symlinks carry user
		 * attributes: accept them for this session, store nothing. */
		o->dirty = false;
		return 0;
	}
	if (o->len == 0) {
		memset(&want, 0, sizeof(want));
		memset(finfo, 0, sizeof(finfo));
	} else if ((err = parse(o->buf, o->len, &want, finfo))) {
		return err;
	}

	if (st->ntfs) {
		/* The Finder "invisible" bit is the Windows hidden attribute. */
		bool hide = (finfo[8] & 0x40) != 0;
		if (hide != ((base.flags & FSC_FLAG_HIDDEN) != 0)) {
			fsc_sattr sa = {0};
			sa.mask = FSC_SET_FLAGS;
			sa.flags = hide ? FSC_FLAG_HIDDEN : 0;
			fsc_setattr(srv->vol, o->ino, &sa, NULL);
		}
		finfo[8] &= (uint8_t)~0x40;
	}
	if (!all_zero(finfo, 32))
		set_add(&want, XATTR_FINDERINFO, finfo, 32);

	err = native_read(srv, o->ino, &have);
	if (err) {
		set_free(&want);
		return err;
	}
	for (size_t i = 0; i < want.n; i++) {
		xattr_kv *h = set_find(&have, want.v[i].name);
		if (h && h->len == want.v[i].len &&
		    !memcmp(h->val, want.v[i].val, h->len))
			continue;
		err = fsc_setxattr(srv->vol, o->ino, want.v[i].name, want.v[i].val,
				   want.v[i].len, 0);
		if (err) {
			fsc_log(FSC_LOG_WARN, "cannot store attribute %s on inode %llu: %s",
				want.v[i].name, (unsigned long long)o->ino,
				strerror(-err));
			if (!first_err)
				first_err = err;
		}
	}
	for (size_t i = 0; i < have.n; i++) {
		if (set_find(&want, have.v[i].name))
			continue;
		err = fsc_removexattr(srv->vol, o->ino, have.v[i].name);
		if (err && err != -ENOATTR && !first_err)
			first_err = err;
	}
	set_free(&want);
	set_free(&have);
	if (first_err == -E2BIG || first_err == -ENOSPC || first_err == -ENOTSUP)
		first_err = spill(srv, o);
	o->dirty = false;
	return first_err;
}

/* ---------------------------------------------------------------------- */
/* Public interface                                                        */

int ad_init(nfs_server *srv)
{
	srv->ad = calloc(1, sizeof(ad_state));
	if (!srv->ad)
		return -ENOMEM;
	srv->ad->ntfs = !strcmp(srv->info.fstype, "ntfs");
	return 0;
}

void ad_shutdown(nfs_server *srv)
{
	ad_state *st = srv->ad;

	if (!st)
		return;
	for (size_t i = 0; i < AD_BUCKETS; i++)
		while (st->bucket[i])
			free_obj(st, st->bucket[i]);
	free(st);
	srv->ad = NULL;
}

/*
 * Keeps the cache bounded. Clean companions can always be re-synthesised
 * from the native attributes, so they are dropped freely (least recently
 * used first); dirty ones are applied before being dropped.
 */
static void evict_some(nfs_server *srv)
{
	ad_state *st = srv->ad;
	double now = nfs_now();

	for (int pass = 0; pass < 2 && st->count > AD_MAX_OBJS / 2; pass++) {
		for (size_t i = 0; i < AD_BUCKETS; i++) {
			ad_obj *o = st->bucket[i], *next;
			for (; o; o = next) {
				next = o->next;
				if (pass == 0 && now - o->last_used < 1.0)
					continue;
				if (o->dirty && apply(srv, o) == -EINVAL)
					continue; /* still being written */
				if (!o->dirty)
					free_obj(st, o);
			}
		}
	}
}

/* Gets (and loads) the companion state for a live base object. */
static int get_obj(nfs_server *srv, fsc_ino ino, const fsc_attr *base,
		   ad_obj **out)
{
	ad_state *st = srv->ad;
	ad_obj *o = find_obj(st, ino);
	int err;

	if (o && o->gen != base->gen) {
		free_obj(st, o);
		o = NULL;
	}
	if (!o) {
		if (st->count >= AD_MAX_OBJS)
			evict_some(srv);
		o = calloc(1, sizeof(*o));
		if (!o)
			return -ENOMEM;
		o->ino = ino;
		o->gen = base->gen;
		o->next = st->bucket[slot(ino)];
		st->bucket[slot(ino)] = o;
		st->count++;
	}
	o->last_used = nfs_now();
	if (!o->loaded) {
		err = synthesize(srv, o, base);
		if (err)
			return err;
	}
	*out = o;
	return 0;
}

static void fill_attr(const fsc_attr *base, const ad_obj *o, fsc_attr *a)
{
	*a = *base;
	a->ino = AD_FILEID(base->ino);
	a->type = FSC_TYPE_REG;
	a->mode = 0644;
	a->nlink = 1;
	a->size = o->len;
	a->used = (o->len + 4095) & ~(uint64_t)4095;
	a->flags = 0;
	a->mtime = base->ctime;
}

int ad_lookup(nfs_server *srv, fsc_ino dir, const char *name, size_t len,
	      bool for_create, nfs_fh *fh, fsc_attr *attr)
{
	fsc_attr base;
	fsc_ino ino;
	ad_obj *o;
	int err;

	if (!srv->appledouble || !ad_is_name(name, len))
		return -ENOENT;
	err = fsc_lookup(srv->vol, dir, name + 2, len - 2, &ino, &base);
	if (err)
		return -ENOENT;
	err = get_obj(srv, ino, &base, &o);
	if (err)
		return err;
	nfs_log(srv, 2, "AD lookup %.*s -> inode %llu len %zu loaded %d dirty %d%s",
		(int)len, name, (unsigned long long)ino, o->len, o->loaded, o->dirty,
		for_create ? " (create)" : "");
	if (o->len == 0 && !for_create && !o->dirty)
		return -ENOENT; /* no attributes: no companion */
	o->dir = dir;
	if (len < sizeof(o->name)) {
		memcpy(o->name, name, len);
		o->name[len] = 0;
	}
	*fh = nfs_make_fh(srv, ino, base.gen, FH_KIND_APPLEDOUBLE);
	fill_attr(&base, o, attr);
	return 0;
}

static int resolve(nfs_server *srv, const nfs_fh *fh, fsc_attr *base,
		   ad_obj **o)
{
	int err = fsc_getattr(srv->vol, fh->ino, base);

	if (err)
		return err == -ENOENT ? -ESTALE : err;
	if (base->gen != fh->gen)
		return -ESTALE;
	return get_obj(srv, fh->ino, base, o);
}

int ad_getattr(nfs_server *srv, const nfs_fh *fh, fsc_attr *attr)
{
	fsc_attr base;
	ad_obj *o;
	int err = resolve(srv, fh, &base, &o);

	if (err)
		return err;
	fill_attr(&base, o, attr);
	return 0;
}

int ad_read(nfs_server *srv, const nfs_fh *fh, uint64_t off, size_t len,
	    void *buf, size_t *got)
{
	fsc_attr base;
	ad_obj *o;
	int err = resolve(srv, fh, &base, &o);

	*got = 0;
	if (err)
		return err;
	if (off >= o->len)
		return 0;
	if (len > o->len - off)
		len = (size_t)(o->len - off);
	memcpy(buf, o->buf + off, len);
	*got = len;
	return 0;
}

int ad_write(nfs_server *srv, const nfs_fh *fh, uint64_t off, size_t len,
	     const void *buf)
{
	fsc_attr base;
	ad_obj *o;
	int err;

	if (srv->readonly)
		return -EROFS;
	err = resolve(srv, fh, &base, &o);
	if (err)
		return err;
	if (off + len > MAX_AD_SIZE)
		return -EFBIG;
	err = reserve(o, (size_t)(off + len));
	if (err)
		return err;
	if (off > o->len)
		memset(o->buf + o->len, 0, (size_t)(off - o->len));
	memcpy(o->buf + off, buf, len);
	if (off + len > o->len)
		o->len = (size_t)(off + len);
	if (!o->dirty)
		o->dirty_since = nfs_now();
	o->dirty = true;
	return 0;
}

int ad_truncate(nfs_server *srv, const nfs_fh *fh, uint64_t size)
{
	fsc_attr base;
	ad_obj *o;
	int err;

	if (srv->readonly)
		return -EROFS;
	err = resolve(srv, fh, &base, &o);
	if (err)
		return err;
	if (size > o->len) {
		err = reserve(o, (size_t)size);
		if (err)
			return err;
		memset(o->buf + o->len, 0, (size_t)(size - o->len));
	}
	o->len = (size_t)size;
	if (!o->dirty)
		o->dirty_since = nfs_now();
	o->dirty = true;
	return 0;
}

int ad_remove(nfs_server *srv, const nfs_fh *fh)
{
	fsc_attr base;
	ad_obj *o;
	int err;

	nfs_log(srv, 2, "AD remove companion of inode %llu",
		(unsigned long long)fh->ino);
	if (srv->readonly)
		return -EROFS;
	err = resolve(srv, fh, &base, &o);
	if (err)
		return err;
	o->len = 0;
	o->dirty = true;
	err = apply(srv, o);
	o->loaded = false; /* re-synthesise (e.g. hidden files stay hidden) */
	return err;
}

int ad_commit(nfs_server *srv, const nfs_fh *fh)
{
	ad_obj *o = find_obj(srv->ad, fh->ino);
	int err;

	if (!o || !o->dirty)
		return 0;
	err = apply(srv, o);
	return err == -EINVAL ? 0 : err; /* incomplete image: wait for more */
}

int ad_tick(nfs_server *srv)
{
	ad_state *st = srv->ad;
	double now = nfs_now();
	int next = 1000;

	if (!st)
		return next;
	for (size_t i = 0; i < AD_BUCKETS; i++) {
		for (ad_obj *o = st->bucket[i]; o; o = o->next) {
			if (!o->dirty)
				continue;
			if (now - o->dirty_since >= AD_APPLY_IDLE) {
				if (apply(srv, o) == -EINVAL)
					o->dirty_since = now; /* retry later */
			} else {
				int ms = (int)((AD_APPLY_IDLE - (now - o->dirty_since)) *
					       1000) + 1;
				if (ms < next)
					next = ms;
			}
		}
	}
	return next;
}

int ad_flush_all(nfs_server *srv)
{
	ad_state *st = srv->ad;
	int first = 0;

	if (!st)
		return 0;
	for (size_t i = 0; i < AD_BUCKETS; i++)
		for (ad_obj *o = st->bucket[i]; o; o = o->next)
			if (o->dirty) {
				int err = apply(srv, o);
				if (err == -EINVAL)
					fsc_log(FSC_LOG_WARN, "discarding incomplete "
						"attribute data for inode %llu",
						(unsigned long long)o->ino);
				else if (err && !first)
					first = err;
			}
	return first;
}

void ad_forget(nfs_server *srv, fsc_ino ino)
{
	ad_obj *o;

	if (!srv->ad)
		return;
	o = find_obj(srv->ad, ino);
	if (o) {
		if (o->dirty)
			apply(srv, o);
		free_obj(srv->ad, o);
	}
}
