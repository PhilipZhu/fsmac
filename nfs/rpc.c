/*
 * ONC RPC v2 (RFC 5531) call parsing and reply framing.
 */
#include <stdlib.h>
#include <string.h>

#include "nfsd.h"

#define RPC_CALL 0
#define RPC_REPLY 1
#define MSG_ACCEPTED 0
#define MSG_DENIED 1
#define RPC_MISMATCH 0
#define AUTH_ERROR 1
#define AUTH_BADCRED 1
#define AUTH_TOOWEAK 5

static void reply_header(xdr_out *o, uint32_t xid)
{
	xout_u32(o, xid);
	xout_u32(o, RPC_REPLY);
}

static void accepted(xdr_out *o, uint32_t xid, uint32_t stat)
{
	reply_header(o, xid);
	xout_u32(o, MSG_ACCEPTED);
	xout_u32(o, AUTH_NONE); /* verifier */
	xout_u32(o, 0);
	xout_u32(o, stat);
}

static bool parse_auth_sys(const uint8_t *body, size_t len, nfs_cred *cred)
{
	xdr_in x;
	uint32_t n;

	xin_init(&x, body, len);
	(void)xin_u32(&x);           /* stamp */
	xin_skip_var(&x, 255);       /* machine name */
	cred->uid = xin_u32(&x);
	cred->gid = xin_u32(&x);
	n = xin_u32(&x);
	if (n > 16)
		return false;
	cred->ngids = n;
	for (uint32_t i = 0; i < n; i++)
		cred->gids[i] = xin_u32(&x);
	return !x.err;
}

/*
 * Duplicate request cache. A client retransmits a request when a reply is
 * slow (e.g. deleting a huge file) or after reconnecting; executing a
 * non-idempotent operation twice would turn a successful REMOVE, RENAME or
 * CREATE into a spurious error, so their replies are kept and replayed.
 */
#define DRC_SIZE 256

struct drc_entry {
	uint32_t xid;
	uint32_t proc;
	uint64_t hash;      /* of the whole request */
	uint8_t *reply;
	size_t len;
};

static struct drc_entry drc[DRC_SIZE];
static unsigned drc_next;

static bool drc_wanted(uint32_t prog, uint32_t proc)
{
	if (prog != NFS_PROGRAM)
		return false;
	switch (proc) {
	case 8:  /* CREATE */
	case 9:  /* MKDIR */
	case 10: /* SYMLINK */
	case 11: /* MKNOD */
	case 12: /* REMOVE */
	case 13: /* RMDIR */
	case 14: /* RENAME */
	case 15: /* LINK */
		return true;
	}
	return false;
}

static uint64_t drc_hash(const uint8_t *p, size_t len)
{
	uint64_t h = 1469598103934665603ull;

	/* Skip the xid so that the hash covers what the call asks for. */
	for (size_t i = 4; i < len; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

static struct drc_entry *drc_find(uint32_t xid, uint32_t proc, uint64_t hash)
{
	for (unsigned i = 0; i < DRC_SIZE; i++)
		if (drc[i].reply && drc[i].xid == xid && drc[i].proc == proc &&
		    drc[i].hash == hash)
			return &drc[i];
	return NULL;
}

static void drc_store(uint32_t xid, uint32_t proc, uint64_t hash,
		      const uint8_t *reply, size_t len)
{
	struct drc_entry *e = &drc[drc_next];
	uint8_t *copy = malloc(len);

	if (!copy)
		return;
	drc_next = (drc_next + 1) % DRC_SIZE;
	free(e->reply);
	memcpy(copy, reply, len);
	e->xid = xid;
	e->proc = proc;
	e->hash = hash;
	e->reply = copy;
	e->len = len;
}

static bool rpc_execute(nfs_server *srv, const uint8_t *req, size_t len, xdr_out *out);

bool rpc_handle(nfs_server *srv, const uint8_t *req, size_t len, xdr_out *out)
{
	size_t start = out->len;
	uint32_t xid, prog, proc;
	uint64_t hash = 0;
	bool cache;

	if (len < 24)
		return rpc_execute(srv, req, len, out);
	xid = (uint32_t)req[0] << 24 | req[1] << 16 | req[2] << 8 | req[3];
	prog = (uint32_t)req[12] << 24 | req[13] << 16 | req[14] << 8 | req[15];
	proc = (uint32_t)req[20] << 24 | req[21] << 16 | req[22] << 8 | req[23];
	cache = drc_wanted(prog, proc);
	if (cache) {
		struct drc_entry *e;

		hash = drc_hash(req, len);
		e = drc_find(xid, proc, hash);
		if (e) {
			if (srv->verbose)
				fsc_log(FSC_LOG_INFO, "nfs: replaying reply to retransmitted xid %08x", xid);
			xout_fixed(out, e->reply, e->len);
			return !out->err;
		}
	}
	if (!rpc_execute(srv, req, len, out))
		return false;
	if (cache && !out->err)
		drc_store(xid, proc, hash, out->buf + start, out->len - start);
	return !out->err;
}

static bool rpc_execute(nfs_server *srv, const uint8_t *req, size_t len, xdr_out *out)
{
	xdr_in x;
	uint32_t xid, mtype, rpcvers, prog, vers, proc, flavor;
	const uint8_t *body;
	size_t blen, stat_pos, res_start;
	nfs_cred cred;
	int stat;

	xin_init(&x, req, len);
	xid = xin_u32(&x);
	mtype = xin_u32(&x);
	if (x.err || mtype != RPC_CALL)
		return false; /* not a call: ignore */
	rpcvers = xin_u32(&x);
	prog = xin_u32(&x);
	vers = xin_u32(&x);
	proc = xin_u32(&x);

	memset(&cred, 0, sizeof(cred));
	cred.uid = cred.gid = 65534;
	flavor = xin_u32(&x);
	body = xin_var(&x, 400, &blen);
	(void)xin_u32(&x);          /* verifier flavor */
	xin_skip_var(&x, 400);      /* verifier body */
	if (x.err)
		return false;

	if (rpcvers != 2) {
		reply_header(out, xid);
		xout_u32(out, MSG_DENIED);
		xout_u32(out, RPC_MISMATCH);
		xout_u32(out, 2);
		xout_u32(out, 2);
		return true;
	}
	cred.flavor = flavor;
	if (flavor == AUTH_SYS) {
		if (!parse_auth_sys(body, blen, &cred)) {
			reply_header(out, xid);
			xout_u32(out, MSG_DENIED);
			xout_u32(out, AUTH_ERROR);
			xout_u32(out, AUTH_BADCRED);
			return true;
		}
	} else if (flavor != AUTH_NONE) {
		reply_header(out, xid);
		xout_u32(out, MSG_DENIED);
		xout_u32(out, AUTH_ERROR);
		xout_u32(out, AUTH_TOOWEAK);
		return true;
	}

	if (prog != NFS_PROGRAM && prog != MOUNT_PROGRAM) {
		accepted(out, xid, RPC_PROG_UNAVAIL);
		return true;
	}
	if ((prog == NFS_PROGRAM && vers != NFS_V3) ||
	    (prog == MOUNT_PROGRAM && vers != MOUNT_V3)) {
		accepted(out, xid, RPC_PROG_MISMATCH);
		xout_u32(out, 3);
		xout_u32(out, 3);
		return true;
	}

	accepted(out, xid, RPC_SUCCESS);
	stat_pos = out->len - 4;
	res_start = out->len;
	srv->ops++;
	if (prog == NFS_PROGRAM)
		stat = nfs3_dispatch(srv, proc, &x, &cred, out);
	else
		stat = mount3_dispatch(srv, proc, &x, &cred, out);
	if (stat != RPC_SUCCESS) {
		out->len = res_start; /* drop partial results */
		xout_put_u32_at(out, stat_pos, (uint32_t)stat);
		if (stat == RPC_GARBAGE_ARGS)
			srv->errors++;
	}
	return !out->err;
}
