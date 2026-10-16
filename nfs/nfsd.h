/*
 * FSMac loopback NFS server: shared definitions.
 *
 * One server instance serves exactly one mounted volume over NFSv3 (RFC
 * 1813) plus the MOUNT v3 protocol, to the macOS NFS client on the same
 * machine. All filesystem work happens on the server's single event-loop
 * thread, so the engine needs no locking.
 */
#ifndef FSMAC_NFSD_H
#define FSMAC_NFSD_H

#include <stdbool.h>
#include <stdint.h>

#include <fsmac/fsc.h>

#include "xdr.h"

#define NFS_PROGRAM 100003
#define NFS_V3 3
#define MOUNT_PROGRAM 100005
#define MOUNT_V3 3

/* RPC accept_stat */
enum {
	RPC_SUCCESS = 0,
	RPC_PROG_UNAVAIL = 1,
	RPC_PROG_MISMATCH = 2,
	RPC_PROC_UNAVAIL = 3,
	RPC_GARBAGE_ARGS = 4,
	RPC_SYSTEM_ERR = 5,
};

/* NFSv3 status codes */
enum {
	NFS3_OK = 0,
	NFS3ERR_PERM = 1,
	NFS3ERR_NOENT = 2,
	NFS3ERR_IO = 5,
	NFS3ERR_NXIO = 6,
	NFS3ERR_ACCES = 13,
	NFS3ERR_EXIST = 17,
	NFS3ERR_XDEV = 18,
	NFS3ERR_NODEV = 19,
	NFS3ERR_NOTDIR = 20,
	NFS3ERR_ISDIR = 21,
	NFS3ERR_INVAL = 22,
	NFS3ERR_FBIG = 27,
	NFS3ERR_NOSPC = 28,
	NFS3ERR_ROFS = 30,
	NFS3ERR_MLINK = 31,
	NFS3ERR_NAMETOOLONG = 63,
	NFS3ERR_NOTEMPTY = 66,
	NFS3ERR_DQUOT = 69,
	NFS3ERR_STALE = 70,
	NFS3ERR_REMOTE = 71,
	NFS3ERR_BADHANDLE = 10001,
	NFS3ERR_NOT_SYNC = 10002,
	NFS3ERR_BAD_COOKIE = 10003,
	NFS3ERR_NOTSUPP = 10004,
	NFS3ERR_TOOSMALL = 10005,
	NFS3ERR_SERVERFAULT = 10006,
	NFS3ERR_BADTYPE = 10007,
	NFS3ERR_JUKEBOX = 10008,
};

#define AUTH_NONE 0
#define AUTH_SYS 1

typedef struct nfs_cred {
	uint32_t flavor;
	uint32_t uid, gid;
	uint32_t ngids;
	uint32_t gids[16];
} nfs_cred;

/* File handle contents (encoded big-endian, 24 bytes on the wire). */
#define FH_SIZE 24
#define FH_MAGIC 0x46534d33u /* "FSM3" */
enum { FH_KIND_NORMAL = 0, FH_KIND_APPLEDOUBLE = 1 };

typedef struct nfs_fh {
	uint32_t fsid;
	uint64_t ino;
	uint32_t gen;
	uint32_t kind;
} nfs_fh;

struct ad_state;

typedef struct nfs_server {
	fsc_volume *vol;
	fsc_volinfo info;
	fsc_ino root;
	uint32_t fsid32;          /* identifies the volume in file handles */
	uint64_t fsid;            /* reported in attributes */
	uint8_t write_verf[8];    /* changes when the server restarts */

	/* Presentation policy */
	bool ignore_ownership;    /* show everything as owned by owner_uid */
	uint32_t owner_uid, owner_gid;
	bool appledouble;         /* serve ._ files from native xattrs */
	bool readonly;

	uint32_t rtmax, wtmax;    /* largest READ/WRITE payloads */

	/* Sync bookkeeping (monotonic seconds) */
	double last_change;       /* last modifying operation */
	double last_full_sync;

	/* AppleDouble translation state */
	struct ad_state *ad;

	/* Statistics */
	uint64_t ops, bytes_read, bytes_written, errors;
	int verbose;
} nfs_server;

/* Handles one complete RPC call record; writes the reply (without record
 * marking) to out. Returns false if no reply must be sent. */
bool rpc_handle(nfs_server *srv, const uint8_t *req, size_t len, xdr_out *out);

int nfs3_dispatch(nfs_server *srv, uint32_t proc, xdr_in *args,
		  const nfs_cred *cred, xdr_out *res);
int mount3_dispatch(nfs_server *srv, uint32_t proc, xdr_in *args,
		    const nfs_cred *cred, xdr_out *res);

/* Encodes the handle for an object. */
void nfs_put_fh(nfs_server *srv, xdr_out *o, const nfs_fh *fh);
nfs_fh nfs_make_fh(nfs_server *srv, fsc_ino ino, uint32_t gen, uint32_t kind);

/* errno (negative) to nfsstat3 */
uint32_t nfs3_status(int err);

double nfs_now(void);

/* Periodic maintenance (sync policy, AppleDouble commits); returns the
 * suggested number of milliseconds until the next call. */
int nfs_tick(nfs_server *srv);
/* Writes everything back and flushes the device. */
int nfs_flush_all(nfs_server *srv);

void nfs_log(nfs_server *srv, int level, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

#endif /* FSMAC_NFSD_H */
