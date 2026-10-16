/*
 * MOUNT protocol version 3 (RFC 1813, appendix I). The server exports a
 * single filesystem; any path mounts its root.
 */
#include <string.h>

#include "nfsd.h"

enum {
	MOUNTPROC3_NULL = 0,
	MOUNTPROC3_MNT = 1,
	MOUNTPROC3_DUMP = 2,
	MOUNTPROC3_UMNT = 3,
	MOUNTPROC3_UMNTALL = 4,
	MOUNTPROC3_EXPORT = 5,
};

#define MNT3_OK 0
#define MNTPATHLEN 1024

int mount3_dispatch(nfs_server *srv, uint32_t proc, xdr_in *args,
		    const nfs_cred *cred, xdr_out *res)
{
	(void)cred;
	switch (proc) {
	case MOUNTPROC3_NULL:
		return RPC_SUCCESS;
	case MOUNTPROC3_MNT: {
		size_t len;
		fsc_attr a;
		nfs_fh fh;

		(void)xin_var(args, MNTPATHLEN, &len);
		if (args->err)
			return RPC_GARBAGE_ARGS;
		if (fsc_getattr(srv->vol, srv->root, &a)) {
			xout_u32(res, NFS3ERR_IO);
			return RPC_SUCCESS;
		}
		fh = nfs_make_fh(srv, srv->root, a.gen, FH_KIND_NORMAL);
		xout_u32(res, MNT3_OK);
		nfs_put_fh(srv, res, &fh);
		xout_u32(res, 1);        /* one auth flavor */
		xout_u32(res, AUTH_SYS);
		return RPC_SUCCESS;
	}
	case MOUNTPROC3_DUMP:
		xout_bool(res, false); /* empty mount list */
		return RPC_SUCCESS;
	case MOUNTPROC3_UMNT: {
		size_t len;
		(void)xin_var(args, MNTPATHLEN, &len);
		return args->err ? RPC_GARBAGE_ARGS : RPC_SUCCESS;
	}
	case MOUNTPROC3_UMNTALL:
		return RPC_SUCCESS;
	case MOUNTPROC3_EXPORT:
		/* exports: one entry "/" with no group restrictions */
		xout_bool(res, true);
		xout_var(res, "/", 1);
		xout_bool(res, false); /* groups */
		xout_bool(res, false); /* end of list */
		return RPC_SUCCESS;
	default:
		return RPC_PROC_UNAVAIL;
	}
}
