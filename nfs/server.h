/*
 * TCP transport for the loopback NFS server.
 */
#ifndef FSMAC_NFS_SERVER_H
#define FSMAC_NFS_SERVER_H

#include <signal.h>
#include <stdint.h>

#include "nfsd.h"

typedef struct nfs_serve_opts {
	volatile sig_atomic_t *stop; /* set to non-zero to stop serving */
	int wake_fd;                 /* readable fd that ends the loop on EOF,
				      * or -1 */
	bool insecure;               /* accept unprivileged client ports */
} nfs_serve_opts;

/* Serves RPCs on listening socket lfd until *stop or a fatal error. */
int nfs_serve(nfs_server *srv, int lfd, const nfs_serve_opts *opts);

/* Creates a listening TCP socket on 127.0.0.1 (port 0 = ephemeral). */
int nfs_listen_loopback(uint16_t port, uint16_t *bound);

#endif /* FSMAC_NFS_SERVER_H */
