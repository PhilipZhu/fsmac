/*
 * TCP transport for the loopback NFS server: connection handling and ONC
 * RPC record marking (RFC 5531 section 11), driven by a poll() loop on a
 * single thread.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "nfsd.h"
#include "server.h"

#define MAX_CONNS 32
#define MAX_RECORD (4u << 20)      /* largest accepted RPC record */
#define MAX_PENDING_OUT (16u << 20) /* stop reading above this backlog */
#define RECORD_LAST 0x80000000u

typedef struct conn {
	int fd;
	uint8_t *in;         /* raw bytes received, not yet consumed */
	size_t in_len, in_cap;
	uint8_t *rec;        /* record being assembled from fragments */
	size_t rec_len, rec_cap;
	uint8_t *out;        /* bytes waiting to be sent */
	size_t out_len, out_off, out_cap;
	char peer[64];
} conn;

static int grow(uint8_t **buf, size_t *cap, size_t need)
{
	if (*cap >= need)
		return 0;
	size_t ncap = *cap ? *cap : 65536;
	while (ncap < need)
		ncap *= 2;
	uint8_t *nb = realloc(*buf, ncap);
	if (!nb)
		return -ENOMEM;
	*buf = nb;
	*cap = ncap;
	return 0;
}

static void conn_free(conn *c)
{
	if (c->fd >= 0)
		close(c->fd);
	free(c->in);
	free(c->rec);
	free(c->out);
	free(c);
}

static int queue_reply(conn *c, const xdr_out *reply)
{
	uint32_t mark = htonl(RECORD_LAST | (uint32_t)reply->len);

	/* Compact already-sent bytes before growing. */
	if (c->out_off && c->out_off == c->out_len)
		c->out_len = c->out_off = 0;
	if (grow(&c->out, &c->out_cap, c->out_len + 4 + reply->len))
		return -ENOMEM;
	memcpy(c->out + c->out_len, &mark, 4);
	memcpy(c->out + c->out_len + 4, reply->buf, reply->len);
	c->out_len += 4 + reply->len;
	return 0;
}

/* Parses complete fragments from c->in, handling whole records. */
static int process_input(nfs_server *srv, conn *c, xdr_out *reply)
{
	size_t pos = 0;
	int err = 0;

	while (c->in_len - pos >= 4) {
		uint32_t hdr;
		size_t flen;
		bool last;

		memcpy(&hdr, c->in + pos, 4);
		hdr = ntohl(hdr);
		flen = hdr & ~RECORD_LAST;
		last = (hdr & RECORD_LAST) != 0;
		if (c->rec_len + flen > MAX_RECORD) {
			fsc_log(FSC_LOG_WARN, "nfs: oversized record from %s",
				c->peer);
			return -EMSGSIZE;
		}
		if (c->in_len - pos - 4 < flen)
			break; /* incomplete fragment */
		if (grow(&c->rec, &c->rec_cap, c->rec_len + flen))
			return -ENOMEM;
		memcpy(c->rec + c->rec_len, c->in + pos + 4, flen);
		c->rec_len += flen;
		pos += 4 + flen;
		if (!last)
			continue;
		xout_reset(reply);
		if (rpc_handle(srv, c->rec, c->rec_len, reply))
			err = queue_reply(c, reply);
		c->rec_len = 0;
		if (err)
			break;
	}
	if (pos) {
		memmove(c->in, c->in + pos, c->in_len - pos);
		c->in_len -= pos;
	}
	return err;
}

static int do_read(nfs_server *srv, conn *c, xdr_out *reply)
{
	for (;;) {
		ssize_t n;

		if (grow(&c->in, &c->in_cap, c->in_len + 256 * 1024))
			return -ENOMEM;
		n = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return 0;
			return -errno;
		}
		if (n == 0)
			return -ECONNRESET;
		c->in_len += (size_t)n;
		int err = process_input(srv, c, reply);
		if (err)
			return err;
		if (c->out_len - c->out_off > MAX_PENDING_OUT)
			return 0;
	}
}

static int do_write(conn *c)
{
	while (c->out_off < c->out_len) {
		ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN)
				return 0;
			return -errno;
		}
		c->out_off += (size_t)n;
	}
	c->out_len = c->out_off = 0;
	return 0;
}

static void tune_socket(int fd)
{
	int one = 1, buf = 4 << 20;

	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static conn *accept_conn(int lfd, bool insecure)
{
	struct sockaddr_storage ss;
	socklen_t sl = sizeof(ss);
	int fd = accept(lfd, (struct sockaddr *)&ss, &sl);
	conn *c;
	uint16_t port = 0;
	bool loopback = false;

	if (fd < 0)
		return NULL;
	if (ss.ss_family == AF_INET) {
		struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
		loopback = ntohl(sin->sin_addr.s_addr) >> 24 == 127;
		port = ntohs(sin->sin_port);
	} else if (ss.ss_family == AF_INET6) {
		struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&ss;
		loopback = IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr);
		port = ntohs(sin6->sin6_port);
	}
	/* Only the local kernel NFS client (which uses a reserved port when
	 * mounted by root) may talk to us. */
	if (!loopback || (!insecure && port >= 1024)) {
		fsc_log(FSC_LOG_WARN, "nfs: rejected connection from port %u (%s)",
			port, loopback ? "not a privileged port; mount with resvport"
				       : "not loopback");
		close(fd);
		return NULL;
	}
	c = calloc(1, sizeof(*c));
	if (!c) {
		close(fd);
		return NULL;
	}
	c->fd = fd;
	snprintf(c->peer, sizeof(c->peer), "127.0.0.1:%u", port);
	tune_socket(fd);
	return c;
}

/*
 * Commands from the supervisor on the control pipe, one per line:
 *   owner <uid> <gid>    present files as owned by this user (the user
 *                        logged in at the console changed)
 */
static void control_input(nfs_server *srv, const char *data, size_t len)
{
	static char line[128];
	static size_t used;

	for (size_t i = 0; i < len; i++) {
		unsigned uid, gid;

		if (data[i] != '\n') {
			if (used < sizeof(line) - 1)
				line[used++] = data[i];
			continue;
		}
		line[used] = 0;
		used = 0;
		if (sscanf(line, "owner %u %u", &uid, &gid) == 2) {
			srv->owner_uid = uid;
			srv->owner_gid = gid;
			fsc_log(FSC_LOG_INFO, "nfs: files now shown as owned by %u:%u", uid, gid);
		}
	}
}

int nfs_serve(nfs_server *srv, int lfd, const nfs_serve_opts *opts)
{
	conn *conns[MAX_CONNS] = {0};
	struct pollfd pfd[MAX_CONNS + 2];
	xdr_out reply;
	int timeout, n, i, err = 0;

	xout_init(&reply, 1u << 20);
	fcntl(lfd, F_SETFL, fcntl(lfd, F_GETFL) | O_NONBLOCK);

	while (!*opts->stop) {
		int np = 0, extra = -1;

		timeout = nfs_tick(srv);
		if (fsc_dev_failed(fsc_get_dev(srv->vol))) {
			fsc_log(FSC_LOG_ERROR, "nfs: device failed, stopping");
			err = -ENXIO;
			break;
		}
		pfd[np].fd = lfd;
		pfd[np].events = POLLIN;
		np++;
		if (opts->wake_fd >= 0) {
			extra = np;
			pfd[np].fd = opts->wake_fd;
			pfd[np].events = POLLIN;
			np++;
		}
		int base = np;
		for (i = 0; i < MAX_CONNS; i++) {
			conn *c = conns[i];
			pfd[base + i].fd = c ? c->fd : -1;
			pfd[base + i].events = 0;
			pfd[base + i].revents = 0;
			if (!c)
				continue;
			if (c->out_len - c->out_off < MAX_PENDING_OUT)
				pfd[base + i].events |= POLLIN;
			if (c->out_off < c->out_len)
				pfd[base + i].events |= POLLOUT;
		}
		n = poll(pfd, (nfds_t)(base + MAX_CONNS), timeout);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			err = -errno;
			break;
		}
		if (extra >= 0 && (pfd[extra].revents & (POLLIN | POLLHUP))) {
			char b[64];
			ssize_t r = read(opts->wake_fd, b, sizeof(b));

			if (r <= 0) {
				/* Supervisor went away: shut down cleanly. */
				fsc_log(FSC_LOG_INFO, "nfs: stop requested");
				break;
			}
			control_input(srv, b, (size_t)r);
		}
		if (pfd[0].revents & POLLIN) {
			conn *c = accept_conn(lfd, opts->insecure);
			if (c) {
				for (i = 0; i < MAX_CONNS && conns[i]; i++)
					;
				if (i < MAX_CONNS)
					conns[i] = c;
				else
					conn_free(c);
			}
		}
		for (i = 0; i < MAX_CONNS; i++) {
			conn *c = conns[i];
			short re = pfd[base + i].revents;
			int cerr = 0;

			if (!c || !re)
				continue;
			if (re & (POLLIN | POLLHUP | POLLERR))
				cerr = do_read(srv, c, &reply);
			if (!cerr && c->out_off < c->out_len)
				cerr = do_write(c);
			if (cerr) {
				if (cerr != -ECONNRESET)
					fsc_log(FSC_LOG_DEBUG, "nfs: closing %s: %s",
						c->peer, strerror(-cerr));
				conn_free(c);
				conns[i] = NULL;
			}
		}
	}
	for (i = 0; i < MAX_CONNS; i++)
		if (conns[i])
			conn_free(conns[i]);
	xout_free(&reply);
	return err;
}

int nfs_listen_loopback(uint16_t port, uint16_t *bound)
{
	struct sockaddr_in sin;
	socklen_t sl = sizeof(sin);
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;

	if (fd < 0)
		return -errno;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_len = sizeof(sin);
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);
	if (bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0 ||
	    listen(fd, 16) < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	if (getsockname(fd, (struct sockaddr *)&sin, &sl) == 0 && bound)
		*bound = ntohs(sin.sin_port);
	return fd;
}
