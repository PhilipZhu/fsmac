/*
 * NFS version 3 procedures (RFC 1813) on top of the FSMac engine.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "appledouble.h"
#include "nfsd.h"

enum {
	NFSPROC3_NULL = 0, NFSPROC3_GETATTR, NFSPROC3_SETATTR, NFSPROC3_LOOKUP,
	NFSPROC3_ACCESS, NFSPROC3_READLINK, NFSPROC3_READ, NFSPROC3_WRITE,
	NFSPROC3_CREATE, NFSPROC3_MKDIR, NFSPROC3_SYMLINK, NFSPROC3_MKNOD,
	NFSPROC3_REMOVE, NFSPROC3_RMDIR, NFSPROC3_RENAME, NFSPROC3_LINK,
	NFSPROC3_READDIR, NFSPROC3_READDIRPLUS, NFSPROC3_FSSTAT,
	NFSPROC3_FSINFO, NFSPROC3_PATHCONF, NFSPROC3_COMMIT,
};

enum { NF3REG = 1, NF3DIR, NF3BLK, NF3CHR, NF3LNK, NF3SOCK, NF3FIFO };

#define ACCESS3_READ 0x0001
#define ACCESS3_LOOKUP 0x0002
#define ACCESS3_MODIFY 0x0004
#define ACCESS3_EXTEND 0x0008
#define ACCESS3_DELETE 0x0010
#define ACCESS3_EXECUTE 0x0020

#define UNSTABLE 0
#define DATA_SYNC 1
#define FILE_SYNC 2

#define FSF3_LINK 0x0001
#define FSF3_SYMLINK 0x0002
#define FSF3_HOMOGENEOUS 0x0008
#define FSF3_CANSETTIME 0x0010

#define NFS3_COOKIEVERFSIZE 8
#define NFS3_MAXNAMLEN 255
#define NFS3_MAXPATHLEN 4096

/* ---------------------------------------------------------------------- */
/* Utilities                                                               */

double nfs_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void nfs_log(nfs_server *srv, int level, const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	if (level > srv->verbose)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fsc_log(level > 0 ? FSC_LOG_DEBUG : FSC_LOG_INFO, "%s", buf);
}

uint32_t nfs3_status(int err)
{
	switch (-err) {
	case 0: return NFS3_OK;
	case EPERM: return NFS3ERR_PERM;
	case ENOENT: return NFS3ERR_NOENT;
	case ENXIO: return NFS3ERR_NXIO;
	case EACCES: return NFS3ERR_ACCES;
	case EEXIST: return NFS3ERR_EXIST;
	case EXDEV: return NFS3ERR_XDEV;
	case ENODEV: return NFS3ERR_NODEV;
	case ENOTDIR: return NFS3ERR_NOTDIR;
	case EISDIR: return NFS3ERR_ISDIR;
	case EINVAL: case EILSEQ: return NFS3ERR_INVAL;
	case EFBIG: case E2BIG: return NFS3ERR_FBIG;
	case ENOSPC: return NFS3ERR_NOSPC;
	case EROFS: return NFS3ERR_ROFS;
	case EMLINK: return NFS3ERR_MLINK;
	case ENAMETOOLONG: return NFS3ERR_NAMETOOLONG;
	case ENOTEMPTY: return NFS3ERR_NOTEMPTY;
	case EDQUOT: return NFS3ERR_DQUOT;
	case ESTALE: return NFS3ERR_STALE;
	case ENOTSUP: return NFS3ERR_NOTSUPP;
	case ELOOP: return NFS3ERR_INVAL;
	default: return NFS3ERR_IO;
	}
}

static uint32_t nfs_type(fsc_type t)
{
	switch (t) {
	case FSC_TYPE_DIR: return NF3DIR;
	case FSC_TYPE_LNK: return NF3LNK;
	case FSC_TYPE_CHR: return NF3CHR;
	case FSC_TYPE_BLK: return NF3BLK;
	case FSC_TYPE_FIFO: return NF3FIFO;
	case FSC_TYPE_SOCK: return NF3SOCK;
	default: return NF3REG;
	}
}

static void put_time(xdr_out *o, struct timespec ts)
{
	if (ts.tv_sec < 0) {
		ts.tv_sec = 0;
		ts.tv_nsec = 0;
	} else if ((uint64_t)ts.tv_sec > 0xFFFFFFFFull) {
		ts.tv_sec = 0xFFFFFFFF;
	}
	xout_u32(o, (uint32_t)ts.tv_sec);
	xout_u32(o, (uint32_t)ts.tv_nsec);
}

static struct timespec get_time(xdr_in *x)
{
	struct timespec ts;

	ts.tv_sec = xin_u32(x);
	ts.tv_nsec = xin_u32(x);
	if (ts.tv_nsec >= 1000000000)
		ts.tv_nsec = 999999999;
	return ts;
}

nfs_fh nfs_make_fh(nfs_server *srv, fsc_ino ino, uint32_t gen, uint32_t kind)
{
	nfs_fh fh = {srv->fsid32, ino, gen, kind};
	return fh;
}

void nfs_put_fh(nfs_server *srv, xdr_out *o, const nfs_fh *fh)
{
	uint8_t b[FH_SIZE];
	uint32_t w[6] = {FH_MAGIC, fh->fsid, (uint32_t)(fh->ino >> 32),
			 (uint32_t)fh->ino, fh->gen, fh->kind};

	(void)srv;
	for (int i = 0; i < 6; i++) {
		b[i * 4] = (uint8_t)(w[i] >> 24);
		b[i * 4 + 1] = (uint8_t)(w[i] >> 16);
		b[i * 4 + 2] = (uint8_t)(w[i] >> 8);
		b[i * 4 + 3] = (uint8_t)w[i];
	}
	xout_var(o, b, FH_SIZE);
}

/* Decodes a handle; returns an nfsstat3 (or -1 for undecodable XDR). */
static int get_fh(nfs_server *srv, xdr_in *x, nfs_fh *fh)
{
	size_t len;
	const uint8_t *b = xin_var(x, 64, &len);
	uint32_t w[6];

	if (x->err)
		return -1;
	if (len != FH_SIZE)
		return NFS3ERR_BADHANDLE;
	for (int i = 0; i < 6; i++)
		w[i] = ((uint32_t)b[i * 4] << 24) | ((uint32_t)b[i * 4 + 1] << 16) |
		       ((uint32_t)b[i * 4 + 2] << 8) | b[i * 4 + 3];
	if (w[0] != FH_MAGIC)
		return NFS3ERR_BADHANDLE;
	if (w[1] != srv->fsid32)
		return NFS3ERR_STALE;
	fh->fsid = w[1];
	fh->ino = ((uint64_t)w[2] << 32) | w[3];
	fh->gen = w[4];
	fh->kind = w[5];
	if (fh->kind != FH_KIND_NORMAL && fh->kind != FH_KIND_APPLEDOUBLE)
		return NFS3ERR_BADHANDLE;
	return NFS3_OK;
}

static void present(nfs_server *srv, fsc_attr *a)
{
	if (srv->ignore_ownership) {
		a->uid = srv->owner_uid;
		a->gid = srv->owner_gid;
	}
}

/* Resolves a handle to current attributes, detecting stale handles. */
static int fh_attr(nfs_server *srv, const nfs_fh *fh, fsc_attr *a)
{
	int err;

	if (fh->kind == FH_KIND_APPLEDOUBLE)
		return ad_getattr(srv, fh, a);
	err = fsc_getattr(srv->vol, fh->ino, a);
	if (err)
		return err == -ENOENT ? -ESTALE : err;
	if (a->gen != fh->gen)
		return -ESTALE;
	return 0;
}

static void put_fattr(nfs_server *srv, xdr_out *o, const fsc_attr *in)
{
	fsc_attr a = *in;

	present(srv, &a);
	xout_u32(o, nfs_type(a.type));
	xout_u32(o, a.mode & 07777);
	xout_u32(o, a.nlink ? a.nlink : 1);
	xout_u32(o, a.uid);
	xout_u32(o, a.gid);
	xout_u64(o, a.size);
	xout_u64(o, a.used);
	xout_u32(o, a.rdev_major);
	xout_u32(o, a.rdev_minor);
	xout_u64(o, srv->fsid);
	xout_u64(o, a.ino);
	put_time(o, a.atime);
	put_time(o, a.mtime);
	put_time(o, a.ctime);
}

static void put_post_attr(nfs_server *srv, xdr_out *o, const fsc_attr *a)
{
	if (a) {
		xout_bool(o, true);
		put_fattr(srv, o, a);
	} else {
		xout_bool(o, false);
	}
}

/* post_op_attr for an inode, fetched now. */
static void put_post_ino(nfs_server *srv, xdr_out *o, fsc_ino ino)
{
	fsc_attr a;

	put_post_attr(srv, o, fsc_getattr(srv->vol, ino, &a) ? NULL : &a);
}

static void put_post_fh(nfs_server *srv, xdr_out *o, const nfs_fh *fh)
{
	fsc_attr a;

	put_post_attr(srv, o, fh_attr(srv, fh, &a) ? NULL : &a);
}

typedef struct wcc_pre {
	bool ok;
	uint64_t size;
	struct timespec mtime, ctime;
} wcc_pre;

static void pre_from_attr(wcc_pre *pre, const fsc_attr *a)
{
	pre->ok = true;
	pre->size = a->size;
	pre->mtime = a->mtime;
	pre->ctime = a->ctime;
}

static void put_wcc(nfs_server *srv, xdr_out *o, const wcc_pre *pre,
		    const nfs_fh *fh)
{
	if (pre && pre->ok) {
		xout_bool(o, true);
		xout_u64(o, pre->size);
		put_time(o, pre->mtime);
		put_time(o, pre->ctime);
	} else {
		xout_bool(o, false);
	}
	if (fh)
		put_post_fh(srv, o, fh);
	else
		xout_bool(o, false);
}

static void put_wcc_ino(nfs_server *srv, xdr_out *o, const wcc_pre *pre,
			fsc_ino ino)
{
	if (pre && pre->ok) {
		xout_bool(o, true);
		xout_u64(o, pre->size);
		put_time(o, pre->mtime);
		put_time(o, pre->ctime);
	} else {
		xout_bool(o, false);
	}
	put_post_ino(srv, o, ino);
}

/* Reads a filename; returns an nfsstat3 or -1 for undecodable XDR. */
static int get_name(xdr_in *x, const char **name, size_t *len)
{
	const uint8_t *p = xin_var(x, NFS3_MAXPATHLEN, len);

	if (x->err)
		return -1;
	*name = (const char *)p;
	if (*len == 0)
		return NFS3ERR_NOENT;
	if (*len > NFS3_MAXNAMLEN)
		return NFS3ERR_NAMETOOLONG;
	if (memchr(p, '/', *len) || memchr(p, 0, *len))
		return NFS3ERR_INVAL;
	return NFS3_OK;
}

typedef struct sattr3 {
	bool set_mode, set_uid, set_gid, set_size;
	uint32_t mode, uid, gid;
	uint64_t size;
	int atime_how, mtime_how; /* 0 don't change, 1 server time, 2 client */
	struct timespec atime, mtime;
} sattr3;

static void get_sattr(xdr_in *x, sattr3 *s)
{
	memset(s, 0, sizeof(*s));
	if ((s->set_mode = xin_bool(x)))
		s->mode = xin_u32(x);
	if ((s->set_uid = xin_bool(x)))
		s->uid = xin_u32(x);
	if ((s->set_gid = xin_bool(x)))
		s->gid = xin_u32(x);
	if ((s->set_size = xin_bool(x)))
		s->size = xin_u64(x);
	s->atime_how = (int)xin_u32(x);
	if (s->atime_how == 2)
		s->atime = get_time(x);
	s->mtime_how = (int)xin_u32(x);
	if (s->mtime_how == 2)
		s->mtime = get_time(x);
	if (s->atime_how > 2 || s->mtime_how > 2)
		x->err = true;
}

/* Converts a client sattr3 into an engine request, applying policy. */
static fsc_sattr to_engine_sattr(nfs_server *srv, const sattr3 *s)
{
	fsc_sattr sa;

	memset(&sa, 0, sizeof(sa));
	if (s->set_mode) {
		sa.mask |= FSC_SET_MODE;
		sa.mode = s->mode & 07777;
	}
	if (!srv->ignore_ownership) {
		if (s->set_uid) {
			sa.mask |= FSC_SET_UID;
			sa.uid = s->uid;
		}
		if (s->set_gid) {
			sa.mask |= FSC_SET_GID;
			sa.gid = s->gid;
		}
	}
	if (s->set_size) {
		sa.mask |= FSC_SET_SIZE;
		sa.size = s->size;
	}
	if (s->atime_how == 1)
		sa.mask |= FSC_SET_ATIME_NOW;
	else if (s->atime_how == 2) {
		sa.mask |= FSC_SET_ATIME;
		sa.atime = s->atime;
	}
	if (s->mtime_how == 1)
		sa.mask |= FSC_SET_MTIME_NOW;
	else if (s->mtime_how == 2) {
		sa.mask |= FSC_SET_MTIME;
		sa.mtime = s->mtime;
	}
	return sa;
}

static void changed(nfs_server *srv)
{
	srv->last_change = nfs_now();
}

/* Credentials used for ownership of new objects. */
static fsc_cred create_cred(nfs_server *srv, const nfs_cred *cred,
			    const fsc_attr *parent)
{
	fsc_cred c;

	if (srv->ignore_ownership || cred->flavor != AUTH_SYS) {
		/* Keep the volume's existing ownership scheme: new objects
		 * belong to whoever owns the directory. */
		c.uid = parent->uid;
		c.gid = parent->gid;
	} else {
		c.uid = cred->uid;
		c.gid = cred->gid;
	}
	return c;
}

static uint32_t access_bits(nfs_server *srv, const fsc_attr *a,
			    const nfs_cred *cred)
{
	uint32_t bits;

	if (srv->ignore_ownership) {
		bits = (a->mode >> 6) & 7;
	} else if (cred->flavor == AUTH_SYS && cred->uid == 0) {
		bits = 6 | ((a->type == FSC_TYPE_DIR || (a->mode & 0111)) ? 1 : 0);
	} else if (cred->flavor == AUTH_SYS && cred->uid == a->uid) {
		bits = (a->mode >> 6) & 7;
	} else {
		bool ingroup = cred->flavor == AUTH_SYS && cred->gid == a->gid;
		for (uint32_t i = 0; !ingroup && i < cred->ngids; i++)
			ingroup = cred->gids[i] == a->gid;
		bits = ingroup ? (a->mode >> 3) & 7 : a->mode & 7;
	}
	return bits;
}

static uint32_t compute_access(nfs_server *srv, const fsc_attr *a,
			       const nfs_cred *cred, uint32_t want)
{
	uint32_t bits = access_bits(srv, a, cred), allow = 0;

	if (bits & 4)
		allow |= ACCESS3_READ;
	if (a->type == FSC_TYPE_DIR) {
		if (bits & 1)
			allow |= ACCESS3_LOOKUP | ACCESS3_EXECUTE;
		if (bits & 2)
			allow |= ACCESS3_MODIFY | ACCESS3_EXTEND | ACCESS3_DELETE;
	} else {
		if (bits & 1)
			allow |= ACCESS3_EXECUTE;
		if (bits & 2)
			allow |= ACCESS3_MODIFY | ACCESS3_EXTEND | ACCESS3_DELETE;
	}
	if (a->type == FSC_TYPE_LNK)
		allow |= ACCESS3_READ;
	if (srv->readonly || (a->flags & FSC_FLAG_IMMUTABLE))
		allow &= ~(uint32_t)(ACCESS3_MODIFY | ACCESS3_EXTEND | ACCESS3_DELETE);
	if (a->flags & FSC_FLAG_ENCRYPTED)
		allow &= ~(uint32_t)(ACCESS3_READ | ACCESS3_MODIFY | ACCESS3_EXTEND);
	return want & allow;
}

/* Every handler returns an RPC accept status; NFS errors are results. */
#define GARBAGE() return RPC_GARBAGE_ARGS
#define CHECK_XDR(x) do { if ((x)->err) GARBAGE(); } while (0)

/* Decodes a handle argument; on NFS error writes status + `fail` and
 * returns from the handler. */
#define GET_FH(fhv, failstmt)                                    \
	do {                                                     \
		int _s = get_fh(srv, args, &(fhv));               \
		if (_s < 0)                                       \
			GARBAGE();                                \
		if (_s != NFS3_OK) {                              \
			xout_u32(res, (uint32_t)_s);              \
			failstmt;                                 \
			return RPC_SUCCESS;                       \
		}                                                 \
	} while (0)

/* ---------------------------------------------------------------------- */
/* Procedures                                                              */

static int proc_getattr(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	int err;

	GET_FH(fh, (void)0);
	CHECK_XDR(args);
	err = fh_attr(srv, &fh, &a);
	xout_u32(res, nfs3_status(err));
	if (!err)
		put_fattr(srv, res, &a);
	return RPC_SUCCESS;
}

static int proc_setattr(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	sattr3 s;
	fsc_attr a;
	wcc_pre pre = {0};
	bool guard;
	struct timespec guard_ctime = {0};
	int err;

	GET_FH(fh, put_wcc(srv, res, NULL, NULL));
	get_sattr(args, &s);
	if ((guard = xin_bool(args)))
		guard_ctime = get_time(args);
	CHECK_XDR(args);

	err = fh_attr(srv, &fh, &a);
	if (!err) {
		pre_from_attr(&pre, &a);
		if (guard && (guard_ctime.tv_sec != a.ctime.tv_sec ||
			      guard_ctime.tv_nsec != a.ctime.tv_nsec)) {
			xout_u32(res, NFS3ERR_NOT_SYNC);
			put_wcc(srv, res, &pre, &fh);
			return RPC_SUCCESS;
		}
		if (fh.kind == FH_KIND_APPLEDOUBLE) {
			if (s.set_size)
				err = ad_truncate(srv, &fh, s.size);
		} else {
			fsc_sattr sa = to_engine_sattr(srv, &s);
			if (sa.mask)
				err = fsc_setattr(srv->vol, fh.ino, &sa, NULL);
		}
		if (!err)
			changed(srv);
		else
			nfs_log(srv, 1, "SETATTR ino %llu failed: %s",
				(unsigned long long)fh.ino, strerror(-err));
	}
	xout_u32(res, nfs3_status(err));
	put_wcc(srv, res, pre.ok ? &pre : NULL, &fh);
	return RPC_SUCCESS;
}

static int proc_lookup(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh dfh, ofh;
	fsc_attr da, oa;
	const char *name;
	size_t len;
	fsc_ino ino;
	int st, err;

	GET_FH(dfh, put_post_attr(srv, res, NULL));
	st = get_name(args, &name, &len);
	if (st < 0)
		GARBAGE();
	CHECK_XDR(args);
	err = fh_attr(srv, &dfh, &da);
	if (err) {
		xout_u32(res, nfs3_status(err));
		put_post_attr(srv, res, NULL);
		return RPC_SUCCESS;
	}
	if (st != NFS3_OK) {
		xout_u32(res, (uint32_t)st);
		put_post_attr(srv, res, &da);
		return RPC_SUCCESS;
	}
	if (da.type != FSC_TYPE_DIR) {
		xout_u32(res, NFS3ERR_NOTDIR);
		put_post_attr(srv, res, &da);
		return RPC_SUCCESS;
	}
	err = fsc_lookup(srv->vol, dfh.ino, name, len, &ino, &oa);
	if (err == -ENOENT && srv->appledouble && ad_is_name(name, len)) {
		err = ad_lookup(srv, dfh.ino, name, len, false, &ofh, &oa);
	} else if (!err) {
		ofh = nfs_make_fh(srv, ino, oa.gen, FH_KIND_NORMAL);
	}
	nfs_log(srv, 3, "LOOKUP %llu/%.*s -> %s", (unsigned long long)dfh.ino,
		(int)len, name, err ? strerror(-err) : "ok");
	xout_u32(res, nfs3_status(err));
	if (!err) {
		nfs_put_fh(srv, res, &ofh);
		put_post_attr(srv, res, &oa);
	}
	put_post_attr(srv, res, &da);
	return RPC_SUCCESS;
}

static int proc_access(nfs_server *srv, xdr_in *args, const nfs_cred *cred,
		       xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	uint32_t want;
	int err;

	GET_FH(fh, put_post_attr(srv, res, NULL));
	want = xin_u32(args);
	CHECK_XDR(args);
	err = fh_attr(srv, &fh, &a);
	xout_u32(res, nfs3_status(err));
	if (err) {
		put_post_attr(srv, res, NULL);
		return RPC_SUCCESS;
	}
	put_post_attr(srv, res, &a);
	if (fh.kind == FH_KIND_APPLEDOUBLE)
		xout_u32(res, want & (ACCESS3_READ | (srv->readonly ? 0 :
			ACCESS3_MODIFY | ACCESS3_EXTEND | ACCESS3_DELETE)));
	else
		xout_u32(res, compute_access(srv, &a, cred, want));
	return RPC_SUCCESS;
}

static int proc_readlink(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	char buf[NFS3_MAXPATHLEN];
	size_t len = 0;
	int err;

	GET_FH(fh, put_post_attr(srv, res, NULL));
	CHECK_XDR(args);
	err = fh_attr(srv, &fh, &a);
	if (!err && a.type != FSC_TYPE_LNK)
		err = -EINVAL;
	if (!err)
		err = fsc_readlink(srv->vol, fh.ino, buf, sizeof(buf), &len);
	xout_u32(res, nfs3_status(err));
	put_post_attr(srv, res, err == -ESTALE ? NULL : &a);
	if (!err)
		xout_var(res, buf, len);
	return RPC_SUCCESS;
}

static int proc_read(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	uint64_t off;
	uint32_t count;
	size_t got = 0, status_pos, hdr_pos;
	uint8_t *p;
	bool eof;
	int err;

	GET_FH(fh, put_post_attr(srv, res, NULL));
	off = xin_u64(args);
	count = xin_u32(args);
	CHECK_XDR(args);
	if (count > srv->rtmax)
		count = srv->rtmax;
	err = fh_attr(srv, &fh, &a);
	if (!err && a.type == FSC_TYPE_DIR)
		err = -EISDIR;
	else if (!err && a.type != FSC_TYPE_REG)
		err = -EINVAL;
	if (err) {
		xout_u32(res, nfs3_status(err));
		put_post_attr(srv, res, err == -ESTALE ? NULL : &a);
		return RPC_SUCCESS;
	}
	/* Lay out the reply and read the data straight into it:
	 * status, attributes, count, eof, opaque<data>. */
	status_pos = res->len;
	xout_u32(res, NFS3_OK);
	put_post_attr(srv, res, &a);
	hdr_pos = res->len;
	p = xout_reserve(res, 12 + (size_t)count + 3);
	if (!p)
		return RPC_SYSTEM_ERR;
	if (fh.kind == FH_KIND_APPLEDOUBLE)
		err = ad_read(srv, &fh, off, count, p + 12, &got);
	else
		err = fsc_read(srv->vol, fh.ino, off, count, p + 12, &got);
	if (err) {
		res->len = status_pos;
		xout_u32(res, nfs3_status(err));
		put_post_attr(srv, res, &a);
		return RPC_SUCCESS;
	}
	eof = off + got >= a.size;
	/* Capacity was reserved above, so these writes don't reallocate. */
	xout_u32(res, (uint32_t)got);
	xout_bool(res, eof);
	xout_u32(res, (uint32_t)got);
	res->len = hdr_pos + 12 + got;
	while (res->len & 3)
		res->buf[res->len++] = 0;
	srv->bytes_read += got;
	return RPC_SUCCESS;
}

static int proc_write(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	wcc_pre pre = {0};
	uint64_t off;
	uint32_t count, stable;
	const uint8_t *data;
	size_t dlen, written = 0;
	int err;

	GET_FH(fh, put_wcc(srv, res, NULL, NULL));
	off = xin_u64(args);
	count = xin_u32(args);
	stable = xin_u32(args);
	data = xin_var(args, (size_t)srv->wtmax + 65536, &dlen);
	CHECK_XDR(args);
	if (dlen < count)
		count = (uint32_t)dlen;

	err = fh_attr(srv, &fh, &a);
	if (!err && a.type == FSC_TYPE_DIR)
		err = -EISDIR;
	else if (!err && a.type != FSC_TYPE_REG)
		err = -EINVAL;
	if (!err) {
		pre_from_attr(&pre, &a);
		if (fh.kind == FH_KIND_APPLEDOUBLE) {
			err = ad_write(srv, &fh, off, count, data);
			written = err ? 0 : count;
		} else {
			err = fsc_write(srv->vol, fh.ino, off, count, data, &written);
		}
		if (written)
			changed(srv);
		if (!err && stable != UNSTABLE) {
			if (fh.kind == FH_KIND_APPLEDOUBLE)
				err = ad_commit(srv, &fh);
			else
				err = fsc_sync(srv->vol, FSC_SYNC_DATA);
		}
	}
	xout_u32(res, nfs3_status(err));
	put_wcc(srv, res, pre.ok ? &pre : NULL, &fh);
	if (!err) {
		xout_u32(res, (uint32_t)written);
		xout_u32(res, stable == UNSTABLE ? UNSTABLE : FILE_SYNC);
		xout_fixed(res, srv->write_verf, 8);
		srv->bytes_written += written;
	}
	return RPC_SUCCESS;
}

/* Common tail for CREATE/MKDIR/SYMLINK/MKNOD replies. */
static void put_create_res(nfs_server *srv, xdr_out *res, int err,
			   const nfs_fh *ofh, const fsc_attr *oa,
			   const wcc_pre *dpre, fsc_ino dir)
{
	xout_u32(res, nfs3_status(err));
	if (!err) {
		xout_bool(res, true);
		nfs_put_fh(srv, res, ofh);
		put_post_attr(srv, res, oa);
	}
	put_wcc_ino(srv, res, dpre, dir);
}

/* Validates a directory handle for a namespace-changing operation. */
static int dir_for_change(nfs_server *srv, const nfs_fh *dfh, fsc_attr *da,
			  wcc_pre *pre)
{
	int err = fh_attr(srv, dfh, da);

	pre->ok = false;
	if (err)
		return err;
	pre_from_attr(pre, da);
	if (da->type != FSC_TYPE_DIR)
		return -ENOTDIR;
	if (srv->readonly)
		return -EROFS;
	return 0;
}

static int proc_create(nfs_server *srv, xdr_in *args, const nfs_cred *cred,
		       xdr_out *res)
{
	nfs_fh dfh, ofh;
	fsc_attr da, oa;
	wcc_pre dpre = {0};
	const char *name;
	size_t len;
	uint32_t how;
	sattr3 s;
	uint8_t verf[8] = {0};
	fsc_ino ino = 0;
	int st, err;

	GET_FH(dfh, put_wcc(srv, res, NULL, NULL));
	st = get_name(args, &name, &len);
	if (st < 0)
		GARBAGE();
	how = xin_u32(args);
	if (how == 2) {
		const uint8_t *v = xin_fixed(args, 8);
		if (v)
			memcpy(verf, v, 8);
		memset(&s, 0, sizeof(s));
	} else {
		get_sattr(args, &s);
	}
	CHECK_XDR(args);
	if (how > 2)
		GARBAGE();

	err = dir_for_change(srv, &dfh, &da, &dpre);
	if (!err && st != NFS3_OK)
		err = st == NFS3ERR_NAMETOOLONG ? -ENAMETOOLONG : -EINVAL;
	if (err)
		goto out;

	if (srv->appledouble && ad_is_name(name, len)) {
		/* A companion for an existing object is virtual. */
		fsc_ino real;
		if (fsc_lookup(srv->vol, dfh.ino, name, len, &real, NULL) == -ENOENT) {
			err = ad_lookup(srv, dfh.ino, name, len, true, &ofh, &oa);
			if (err != -ENOENT) {
				if (!err && how == 1 && oa.size > 0)
					err = -EEXIST;
				if (!err && s.set_size && s.size == 0)
					err = ad_truncate(srv, &ofh, 0);
				if (!err)
					err = ad_getattr(srv, &ofh, &oa);
				goto out_virtual;
			}
		}
	}

	err = fsc_lookup(srv->vol, dfh.ino, name, len, &ino, &oa);
	if (err == 0) {
		/* The name exists. */
		if (how == 1) {
			err = -EEXIST;
		} else if (how == 2) {
			/* Exclusive: succeed only for a retransmission, which we
			 * recognise by the verifier stored in the timestamps. */
			uint32_t v0 = ((uint32_t)verf[0] << 24) | ((uint32_t)verf[1] << 16) |
				      ((uint32_t)verf[2] << 8) | verf[3];
			uint32_t v1 = ((uint32_t)verf[4] << 24) | ((uint32_t)verf[5] << 16) |
				      ((uint32_t)verf[6] << 8) | verf[7];
			if (oa.type == FSC_TYPE_REG && (uint32_t)oa.atime.tv_sec == v0 &&
			    (uint32_t)oa.mtime.tv_sec == v1)
				err = 0;
			else
				err = -EEXIST;
		} else if (oa.type != FSC_TYPE_REG) {
			err = oa.type == FSC_TYPE_DIR ? -EISDIR : -EEXIST;
		} else {
			fsc_sattr sa = to_engine_sattr(srv, &s);
			sa.mask &= FSC_SET_SIZE; /* UNCHECKED on existing file */
			if (sa.mask)
				err = fsc_setattr(srv->vol, ino, &sa, &oa);
		}
	} else if (err == -ENOENT) {
		fsc_cred c = create_cred(srv, cred, &da);
		uint32_t mode = s.set_mode ? s.mode : 0644;

		err = fsc_create(srv->vol, dfh.ino, name, len, FSC_TYPE_REG, mode,
				 0, 0, &c, &ino, &oa);
		if (!err) {
			fsc_sattr sa;
			if (how == 2) {
				memset(&sa, 0, sizeof(sa));
				sa.mask = FSC_SET_ATIME | FSC_SET_MTIME;
				sa.atime.tv_sec = ((uint32_t)verf[0] << 24) |
						  ((uint32_t)verf[1] << 16) |
						  ((uint32_t)verf[2] << 8) | verf[3];
				sa.mtime.tv_sec = ((uint32_t)verf[4] << 24) |
						  ((uint32_t)verf[5] << 16) |
						  ((uint32_t)verf[6] << 8) | verf[7];
			} else {
				sa = to_engine_sattr(srv, &s);
				sa.mask &= ~FSC_SET_MODE; /* applied at creation */
			}
			if (sa.mask)
				err = fsc_setattr(srv->vol, ino, &sa, &oa);
		}
		if (!err)
			changed(srv);
	}
	if (!err)
		ofh = nfs_make_fh(srv, ino, oa.gen, FH_KIND_NORMAL);
out_virtual:
out:
	if (err && err != -EEXIST)
		nfs_log(srv, 1, "CREATE %.*s (how %u) failed: %s", (int)len, name,
			how, strerror(-err));
	put_create_res(srv, res, err, &ofh, &oa, &dpre, dfh.ino);
	return RPC_SUCCESS;
}

static int proc_mkdir(nfs_server *srv, xdr_in *args, const nfs_cred *cred,
		      xdr_out *res)
{
	nfs_fh dfh, ofh;
	fsc_attr da, oa;
	wcc_pre dpre = {0};
	const char *name;
	size_t len;
	sattr3 s;
	fsc_ino ino;
	int st, err;

	GET_FH(dfh, put_wcc(srv, res, NULL, NULL));
	st = get_name(args, &name, &len);
	if (st < 0)
		GARBAGE();
	get_sattr(args, &s);
	CHECK_XDR(args);
	err = dir_for_change(srv, &dfh, &da, &dpre);
	if (!err && st != NFS3_OK)
		err = st == NFS3ERR_NAMETOOLONG ? -ENAMETOOLONG : -EINVAL;
	if (!err) {
		fsc_cred c = create_cred(srv, cred, &da);
		err = fsc_create(srv->vol, dfh.ino, name, len, FSC_TYPE_DIR,
				 s.set_mode ? s.mode : 0755, 0, 0, &c, &ino, &oa);
		if (!err) {
			fsc_sattr sa = to_engine_sattr(srv, &s);
			sa.mask &= ~(FSC_SET_MODE | FSC_SET_SIZE);
			if (sa.mask)
				fsc_setattr(srv->vol, ino, &sa, &oa);
			ofh = nfs_make_fh(srv, ino, oa.gen, FH_KIND_NORMAL);
			changed(srv);
		}
	}
	put_create_res(srv, res, err, &ofh, &oa, &dpre, dfh.ino);
	return RPC_SUCCESS;
}

static int proc_symlink(nfs_server *srv, xdr_in *args, const nfs_cred *cred,
			xdr_out *res)
{
	nfs_fh dfh, ofh;
	fsc_attr da, oa;
	wcc_pre dpre = {0};
	const char *name;
	const uint8_t *target;
	size_t len, tlen;
	sattr3 s;
	fsc_ino ino;
	int st, err;

	GET_FH(dfh, put_wcc(srv, res, NULL, NULL));
	st = get_name(args, &name, &len);
	if (st < 0)
		GARBAGE();
	get_sattr(args, &s);
	target = xin_var(args, NFS3_MAXPATHLEN, &tlen);
	CHECK_XDR(args);
	err = dir_for_change(srv, &dfh, &da, &dpre);
	if (!err && st != NFS3_OK)
		err = st == NFS3ERR_NAMETOOLONG ? -ENAMETOOLONG : -EINVAL;
	if (!err) {
		fsc_cred c = create_cred(srv, cred, &da);
		err = fsc_symlink(srv->vol, dfh.ino, name, len, (const char *)target,
				  tlen, &c, &ino, &oa);
		if (!err) {
			ofh = nfs_make_fh(srv, ino, oa.gen, FH_KIND_NORMAL);
			changed(srv);
		}
	}
	put_create_res(srv, res, err, &ofh, &oa, &dpre, dfh.ino);
	return RPC_SUCCESS;
}

static int proc_mknod(nfs_server *srv, xdr_in *args, const nfs_cred *cred,
		      xdr_out *res)
{
	nfs_fh dfh, ofh;
	fsc_attr da, oa;
	wcc_pre dpre = {0};
	const char *name;
	size_t len;
	uint32_t type, major = 0, minor = 0;
	sattr3 s;
	fsc_type ft;
	fsc_ino ino;
	int st, err;

	GET_FH(dfh, put_wcc(srv, res, NULL, NULL));
	st = get_name(args, &name, &len);
	if (st < 0)
		GARBAGE();
	type = xin_u32(args);
	memset(&s, 0, sizeof(s));
	switch (type) {
	case NF3CHR:
	case NF3BLK:
		get_sattr(args, &s);
		major = xin_u32(args);
		minor = xin_u32(args);
		break;
	case NF3SOCK:
	case NF3FIFO:
		get_sattr(args, &s);
		break;
	default:
		break;
	}
	CHECK_XDR(args);
	err = dir_for_change(srv, &dfh, &da, &dpre);
	if (!err && st != NFS3_OK)
		err = st == NFS3ERR_NAMETOOLONG ? -ENAMETOOLONG : -EINVAL;
	if (!err) {
		switch (type) {
		case NF3CHR: ft = FSC_TYPE_CHR; break;
		case NF3BLK: ft = FSC_TYPE_BLK; break;
		case NF3SOCK: ft = FSC_TYPE_SOCK; break;
		case NF3FIFO: ft = FSC_TYPE_FIFO; break;
		default: ft = FSC_TYPE_NONE; break;
		}
		if (ft == FSC_TYPE_NONE) {
			xout_u32(res, NFS3ERR_BADTYPE);
			put_wcc_ino(srv, res, &dpre, dfh.ino);
			return RPC_SUCCESS;
		}
		fsc_cred c = create_cred(srv, cred, &da);
		err = fsc_create(srv->vol, dfh.ino, name, len, ft,
				 s.set_mode ? s.mode : 0644, major, minor, &c,
				 &ino, &oa);
		if (!err) {
			ofh = nfs_make_fh(srv, ino, oa.gen, FH_KIND_NORMAL);
			changed(srv);
		}
	}
	put_create_res(srv, res, err, &ofh, &oa, &dpre, dfh.ino);
	return RPC_SUCCESS;
}

static int proc_remove(nfs_server *srv, xdr_in *args, xdr_out *res,
		       bool isdir)
{
	nfs_fh dfh;
	fsc_attr da;
	wcc_pre dpre = {0};
	const char *name;
	size_t len;
	int st, err;

	GET_FH(dfh, put_wcc(srv, res, NULL, NULL));
	st = get_name(args, &name, &len);
	if (st < 0)
		GARBAGE();
	CHECK_XDR(args);
	err = dir_for_change(srv, &dfh, &da, &dpre);
	if (!err && st != NFS3_OK)
		err = st == NFS3ERR_NAMETOOLONG ? -ENAMETOOLONG : -ENOENT;
	if (!err) {
		if (isdir) {
			err = fsc_rmdir(srv->vol, dfh.ino, name, len);
		} else {
			fsc_ino victim = 0;
			fsc_attr va;

			if (fsc_lookup(srv->vol, dfh.ino, name, len, &victim, &va) == 0)
				ad_forget(srv, victim);
			err = fsc_unlink(srv->vol, dfh.ino, name, len);
			if (err == -ENOENT && srv->appledouble &&
			    ad_is_name(name, len)) {
				nfs_fh afh;
				fsc_attr aa;
				err = ad_lookup(srv, dfh.ino, name, len, false, &afh, &aa);
				if (!err)
					err = ad_remove(srv, &afh);
			}
		}
		if (!err)
			changed(srv);
		else if (err != -ENOENT)
			nfs_log(srv, 1, "REMOVE %.*s failed: %s", (int)len, name,
				strerror(-err));
	}
	xout_u32(res, nfs3_status(err));
	put_wcc_ino(srv, res, &dpre, dfh.ino);
	return RPC_SUCCESS;
}

static int proc_rename(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh sfh, dfh;
	fsc_attr sa, da;
	wcc_pre spre = {0}, dpre = {0};
	const char *sname, *dname;
	size_t slen, dlen;
	int s1, s2, err;

	GET_FH(sfh, (put_wcc(srv, res, NULL, NULL), put_wcc(srv, res, NULL, NULL)));
	s1 = get_name(args, &sname, &slen);
	if (s1 < 0)
		GARBAGE();
	{
		int st = get_fh(srv, args, &dfh);
		if (st < 0)
			GARBAGE();
		if (st != NFS3_OK) {
			xout_u32(res, (uint32_t)st);
			put_wcc(srv, res, NULL, NULL);
			put_wcc(srv, res, NULL, NULL);
			return RPC_SUCCESS;
		}
	}
	s2 = get_name(args, &dname, &dlen);
	if (s2 < 0)
		GARBAGE();
	CHECK_XDR(args);

	err = dir_for_change(srv, &sfh, &sa, &spre);
	if (!err)
		err = dir_for_change(srv, &dfh, &da, &dpre);
	if (!err && (s1 != NFS3_OK || s2 != NFS3_OK)) {
		int st = s1 != NFS3_OK ? s1 : s2;
		err = st == NFS3ERR_NAMETOOLONG ? -ENAMETOOLONG : -ENOENT;
	}
	if (!err) {
		fsc_ino victim;
		fsc_attr va;

		if (fsc_lookup(srv->vol, dfh.ino, dname, dlen, &victim, &va) == 0)
			ad_forget(srv, victim);
		err = fsc_rename(srv->vol, sfh.ino, sname, slen, dfh.ino, dname,
				 dlen);
		if (err == -ENOENT && srv->appledouble && ad_is_name(sname, slen)) {
			/* The client renames companions after their base file,
			 * whose virtual companion has already moved with it. */
			err = 0;
		}
		if (!err)
			changed(srv);
	}
	if (err)
		nfs_log(srv, 1, "RENAME %.*s -> %.*s failed: %s", (int)slen, sname,
			(int)dlen, dname, strerror(-err));
	xout_u32(res, nfs3_status(err));
	put_wcc_ino(srv, res, &spre, sfh.ino);
	put_wcc_ino(srv, res, &dpre, dfh.ino);
	return RPC_SUCCESS;
}

static int proc_link(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh, dfh;
	fsc_attr a, da;
	wcc_pre dpre = {0};
	const char *name;
	size_t len;
	int st, err;

	GET_FH(fh, (put_post_attr(srv, res, NULL), put_wcc(srv, res, NULL, NULL)));
	{
		int s = get_fh(srv, args, &dfh);
		if (s < 0)
			GARBAGE();
		if (s != NFS3_OK) {
			xout_u32(res, (uint32_t)s);
			put_post_attr(srv, res, NULL);
			put_wcc(srv, res, NULL, NULL);
			return RPC_SUCCESS;
		}
	}
	st = get_name(args, &name, &len);
	if (st < 0)
		GARBAGE();
	CHECK_XDR(args);
	err = fh_attr(srv, &fh, &a);
	if (!err && fh.kind == FH_KIND_APPLEDOUBLE)
		err = -ENOTSUP;
	if (!err)
		err = dir_for_change(srv, &dfh, &da, &dpre);
	if (!err && st != NFS3_OK)
		err = st == NFS3ERR_NAMETOOLONG ? -ENAMETOOLONG : -EINVAL;
	if (!err) {
		err = fsc_link(srv->vol, fh.ino, dfh.ino, name, len);
		if (!err)
			changed(srv);
	}
	xout_u32(res, nfs3_status(err));
	put_post_fh(srv, res, &fh);
	put_wcc_ino(srv, res, &dpre, dfh.ino);
	return RPC_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Directory enumeration                                                   */

typedef struct rd_ent {
	char name[NFS3_MAXNAMLEN + 1];
	uint32_t len;
	fsc_ino ino;
	uint64_t cookie; /* NFS cookie that resumes after this entry */
} rd_ent;

typedef struct rd_collect {
	rd_ent *v;
	size_t n, max;
	size_t budget, used;    /* estimated encoded bytes */
	size_t per_entry;       /* fixed bytes per entry */
	bool full;
} rd_collect;

static size_t pad4(size_t n) { return (n + 3) & ~(size_t)3; }

static bool rd_add(rd_collect *c, const char *name, size_t len, fsc_ino ino,
		   uint64_t cookie)
{
	size_t need = c->per_entry + pad4(len);

	if (c->n >= c->max || c->used + need > c->budget) {
		c->full = true;
		return false;
	}
	memcpy(c->v[c->n].name, name, len);
	c->v[c->n].name[len] = 0;
	c->v[c->n].len = (uint32_t)len;
	c->v[c->n].ino = ino;
	c->v[c->n].cookie = cookie;
	c->n++;
	c->used += need;
	return true;
}

static int rd_cb(void *ctx, const char *name, size_t len, fsc_ino ino,
		 fsc_type type, uint64_t next)
{
	rd_collect *c = ctx;

	(void)type;
	if (len > NFS3_MAXNAMLEN)
		return 0; /* cannot be represented; skip */
	return rd_add(c, name, len, ino, next + 2) ? 0 : 1;
}

/*
 * Collects directory entries for READDIR/READDIRPLUS. NFS cookies: 0 is
 * the start, 1 follows ".", 2 follows "..", and engine cookie e maps to
 * e + 2.
 */
static int rd_gather(nfs_server *srv, fsc_ino dir, uint64_t cookie,
		     rd_collect *c, bool *eof)
{
	int err;

	*eof = false;
	if (cookie == 0 && !rd_add(c, ".", 1, dir, 1))
		return 0;
	if (cookie <= 1) {
		fsc_ino parent;
		err = fsc_parent(srv->vol, dir, &parent);
		if (err)
			return err;
		if (!rd_add(c, "..", 2, parent, 2))
			return 0;
	}
	err = fsc_readdir(srv->vol, dir, cookie <= 2 ? 0 : cookie - 2, rd_cb, c);
	if (err)
		return err;
	*eof = !c->full;
	return 0;
}

static void put_cookieverf(xdr_out *o)
{
	static const uint8_t verf[NFS3_COOKIEVERFSIZE] = {'F', 'S', 'M', 'A',
							  'C', 'v', '0', '1'};
	xout_fixed(o, verf, sizeof(verf));
}

static int proc_readdir(nfs_server *srv, xdr_in *args, xdr_out *res,
			bool plus)
{
	nfs_fh dfh;
	fsc_attr da;
	uint64_t cookie;
	uint32_t count, dircount = 0;
	rd_collect c;
	bool eof;
	int err;

	GET_FH(dfh, put_post_attr(srv, res, NULL));
	cookie = xin_u64(args);
	(void)xin_fixed(args, NFS3_COOKIEVERFSIZE); /* verifier: not enforced */
	if (plus)
		dircount = xin_u32(args);
	count = xin_u32(args);
	CHECK_XDR(args);
	(void)dircount;

	err = fh_attr(srv, &dfh, &da);
	if (!err && da.type != FSC_TYPE_DIR)
		err = -ENOTDIR;
	if (err) {
		xout_u32(res, nfs3_status(err));
		put_post_attr(srv, res, err == -ESTALE ? NULL : &da);
		return RPC_SUCCESS;
	}
	if (count > 1024 * 1024)
		count = 1024 * 1024;

	memset(&c, 0, sizeof(c));
	/* Fixed part of the reply: status, dir attributes, verifier, eof. */
	c.budget = count > 160 ? count - 160 : 0;
	c.per_entry = plus ? 4 + 8 + 4 + 8 + 4 + 84 + 4 + 4 + FH_SIZE : 4 + 8 + 4 + 8;
	c.max = c.budget / (c.per_entry + 4) + 1;
	if (c.max > 4096)
		c.max = 4096;
	c.v = malloc(c.max * sizeof(rd_ent));
	if (!c.v)
		return RPC_SYSTEM_ERR;
	err = rd_gather(srv, dfh.ino, cookie, &c, &eof);
	if (!err && c.n == 0 && !eof)
		err = -EOVERFLOW; /* buffer too small for one entry */

	if (err) {
		xout_u32(res, err == -EOVERFLOW ? NFS3ERR_TOOSMALL : nfs3_status(err));
		put_post_attr(srv, res, &da);
		free(c.v);
		return RPC_SUCCESS;
	}
	xout_u32(res, NFS3_OK);
	put_post_attr(srv, res, &da);
	put_cookieverf(res);
	for (size_t i = 0; i < c.n; i++) {
		rd_ent *e = &c.v[i];

		xout_bool(res, true);
		xout_u64(res, e->ino);
		xout_var(res, e->name, e->len);
		xout_u64(res, e->cookie);
		if (plus) {
			fsc_attr ea;
			if (fsc_getattr(srv->vol, e->ino, &ea) == 0) {
				nfs_fh efh = nfs_make_fh(srv, e->ino, ea.gen,
							 FH_KIND_NORMAL);
				put_post_attr(srv, res, &ea);
				xout_bool(res, true);
				nfs_put_fh(srv, res, &efh);
			} else {
				put_post_attr(srv, res, NULL);
				xout_bool(res, false);
			}
		}
	}
	xout_bool(res, false); /* no more entries in this reply */
	xout_bool(res, eof);
	free(c.v);
	return RPC_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* Filesystem information                                                  */

static int proc_fsstat(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	fsc_fsstat st;
	int err;

	GET_FH(fh, put_post_attr(srv, res, NULL));
	CHECK_XDR(args);
	err = fh_attr(srv, &fh, &a);
	if (!err)
		err = fsc_statfs(srv->vol, &st);
	xout_u32(res, nfs3_status(err));
	put_post_attr(srv, res, err == -ESTALE ? NULL : &a);
	if (!err) {
		xout_u64(res, st.total_bytes);
		xout_u64(res, st.free_bytes);
		xout_u64(res, st.avail_bytes);
		xout_u64(res, st.total_files);
		xout_u64(res, st.free_files);
		xout_u64(res, st.free_files);
		xout_u32(res, 0); /* invarsec */
	}
	return RPC_SUCCESS;
}

static int proc_fsinfo(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	int err;
	struct timespec delta;

	GET_FH(fh, put_post_attr(srv, res, NULL));
	CHECK_XDR(args);
	err = fh_attr(srv, &fh, &a);
	xout_u32(res, nfs3_status(err));
	put_post_attr(srv, res, err ? NULL : &a);
	if (!err) {
		xout_u32(res, srv->rtmax);   /* rtmax */
		xout_u32(res, srv->rtmax);   /* rtpref */
		xout_u32(res, 4096);         /* rtmult */
		xout_u32(res, srv->wtmax);   /* wtmax */
		xout_u32(res, srv->wtmax);   /* wtpref */
		xout_u32(res, 4096);         /* wtmult */
		xout_u32(res, 65536);        /* dtpref */
		xout_u64(res, srv->info.max_file_size);
		delta.tv_sec = srv->info.time_gran_ns >= 1000000000u ? 1 : 0;
		delta.tv_nsec = srv->info.time_gran_ns >= 1000000000u
					? 0 : srv->info.time_gran_ns;
		put_time(res, delta);
		xout_u32(res, FSF3_HOMOGENEOUS | FSF3_CANSETTIME |
			 (srv->info.supports_hardlinks ? FSF3_LINK : 0) |
			 (srv->info.supports_symlinks ? FSF3_SYMLINK : 0));
	}
	return RPC_SUCCESS;
}

static int proc_pathconf(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	int err;

	GET_FH(fh, put_post_attr(srv, res, NULL));
	CHECK_XDR(args);
	err = fh_attr(srv, &fh, &a);
	xout_u32(res, nfs3_status(err));
	put_post_attr(srv, res, err ? NULL : &a);
	if (!err) {
		xout_u32(res, srv->info.link_max);
		xout_u32(res, NFS3_MAXNAMLEN);
		xout_bool(res, true);  /* no_trunc */
		xout_bool(res, true);  /* chown_restricted */
		xout_bool(res, srv->info.case_insensitive);
		xout_bool(res, srv->info.case_preserving);
	}
	return RPC_SUCCESS;
}

static int proc_commit(nfs_server *srv, xdr_in *args, xdr_out *res)
{
	nfs_fh fh;
	fsc_attr a;
	wcc_pre pre = {0};
	int err;

	GET_FH(fh, put_wcc(srv, res, NULL, NULL));
	(void)xin_u64(args); /* offset */
	(void)xin_u32(args); /* count */
	CHECK_XDR(args);
	err = fh_attr(srv, &fh, &a);
	if (!err) {
		pre_from_attr(&pre, &a);
		if (fh.kind == FH_KIND_APPLEDOUBLE)
			err = ad_commit(srv, &fh);
		else
			err = fsc_sync(srv->vol, FSC_SYNC_DATA);
	}
	xout_u32(res, nfs3_status(err));
	put_wcc(srv, res, pre.ok ? &pre : NULL, &fh);
	if (!err)
		xout_fixed(res, srv->write_verf, 8);
	return RPC_SUCCESS;
}

int nfs3_dispatch(nfs_server *srv, uint32_t proc, xdr_in *args,
		  const nfs_cred *cred, xdr_out *res)
{
	switch (proc) {
	case NFSPROC3_NULL: return RPC_SUCCESS;
	case NFSPROC3_GETATTR: return proc_getattr(srv, args, res);
	case NFSPROC3_SETATTR: return proc_setattr(srv, args, res);
	case NFSPROC3_LOOKUP: return proc_lookup(srv, args, res);
	case NFSPROC3_ACCESS: return proc_access(srv, args, cred, res);
	case NFSPROC3_READLINK: return proc_readlink(srv, args, res);
	case NFSPROC3_READ: return proc_read(srv, args, res);
	case NFSPROC3_WRITE: return proc_write(srv, args, res);
	case NFSPROC3_CREATE: return proc_create(srv, args, cred, res);
	case NFSPROC3_MKDIR: return proc_mkdir(srv, args, cred, res);
	case NFSPROC3_SYMLINK: return proc_symlink(srv, args, cred, res);
	case NFSPROC3_MKNOD: return proc_mknod(srv, args, cred, res);
	case NFSPROC3_REMOVE: return proc_remove(srv, args, res, false);
	case NFSPROC3_RMDIR: return proc_remove(srv, args, res, true);
	case NFSPROC3_RENAME: return proc_rename(srv, args, res);
	case NFSPROC3_LINK: return proc_link(srv, args, res);
	case NFSPROC3_READDIR: return proc_readdir(srv, args, res, false);
	case NFSPROC3_READDIRPLUS: return proc_readdir(srv, args, res, true);
	case NFSPROC3_FSSTAT: return proc_fsstat(srv, args, res);
	case NFSPROC3_FSINFO: return proc_fsinfo(srv, args, res);
	case NFSPROC3_PATHCONF: return proc_pathconf(srv, args, res);
	case NFSPROC3_COMMIT: return proc_commit(srv, args, res);
	default: return RPC_PROC_UNAVAIL;
	}
}

/* ---------------------------------------------------------------------- */
/* Background maintenance                                                  */

#define FULL_SYNC_IDLE 1.0     /* seconds of quiet before a full sync */
#define FULL_SYNC_MAX_AGE 5.0  /* never leave metadata dirty longer */

int nfs_tick(nfs_server *srv)
{
	double now = nfs_now();
	int next = 1000, adn;

	adn = ad_tick(srv);
	if (adn < next)
		next = adn;
	if (!srv->readonly && fsc_is_dirty(srv->vol)) {
		double idle = now - srv->last_change;
		double age = now - srv->last_full_sync;

		if (idle >= FULL_SYNC_IDLE || age >= FULL_SYNC_MAX_AGE) {
			int err = fsc_sync(srv->vol, FSC_SYNC_FULL);
			if (err)
				fsc_log(FSC_LOG_ERROR, "background sync failed: %s",
					strerror(-err));
			srv->last_full_sync = now;
		} else {
			int ms = (int)((FULL_SYNC_IDLE - idle) * 1000) + 1;
			if (ms < next)
				next = ms;
		}
	} else {
		srv->last_full_sync = now;
	}
	return next;
}

int nfs_flush_all(nfs_server *srv)
{
	int err = ad_flush_all(srv);
	int e2 = srv->readonly ? 0 : fsc_sync(srv->vol, FSC_SYNC_FULL);

	return err ? err : e2;
}
