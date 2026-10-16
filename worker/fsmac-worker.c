/*
 * fsmac-worker: serves one NTFS or ext2/3/4 volume to the local macOS NFS
 * client. Started by fsmacd for every mounted volume (one process per
 * volume, so a problem with one disk cannot affect another).
 *
 * Lifecycle:
 *   1. open the device (or use an inherited descriptor), mount the engine;
 *   2. listen on 127.0.0.1 and report {"event":"ready",...} on the status
 *      descriptor - the supervisor then runs mount_nfs;
 *   3. serve until SIGTERM/SIGINT or EOF on the control descriptor;
 *   4. flush everything, mark the filesystem clean and exit 0.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <getopt.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <fsmac/fsc.h>

#include "appledouble.h"
#include "nfsd.h"
#include "server.h"

enum {
	EXIT_OK = 0,
	EXIT_FAILURE_ = 1,
	EXIT_USAGE = 2,
	EXIT_MOUNT = 3,
	EXIT_DEVICE_LOST = 4,
};

static volatile sig_atomic_t stop_flag;
static FILE *status_out;
static FILE *log_out;
static int verbose;

static void on_signal(int sig)
{
	(void)sig;
	stop_flag = 1;
}

static void json_escape(FILE *f, const char *s)
{
	fputc('"', f);
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			fprintf(f, "\\%c", *p);
		else if (*p < 0x20)
			fprintf(f, "\\u%04x", *p);
		else
			fputc(*p, f);
	}
	fputc('"', f);
}

static void log_fn(fsc_log_level level, const char *msg)
{
	static const char *const tag[] = {"debug", "info", "warn", "error"};
	char ts[32];
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(log_out, "%s fsmac-worker[%d] %s: %s\n", ts, getpid(), tag[level],
		msg);
	fflush(log_out);
	/* Warnings and errors are also forwarded to the supervisor. */
	if (level >= FSC_LOG_WARN && status_out) {
		fprintf(status_out, "{\"event\":\"log\",\"level\":\"%s\",\"message\":",
			tag[level]);
		json_escape(status_out, msg);
		fprintf(status_out, "}\n");
		fflush(status_out);
	}
}

static void status_error(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fprintf(log_out, "fsmac-worker: %s\n", buf);
	if (status_out) {
		fprintf(status_out, "{\"event\":\"error\",\"message\":");
		json_escape(status_out, buf);
		fprintf(status_out, "}\n");
		fflush(status_out);
	}
}

static void usage(void)
{
	fprintf(stderr,
"usage: fsmac-worker [options] <device|image>\n"
"  --fd N                 use inherited open descriptor N for the device\n"
"  --readonly             mount read-only\n"
"  --listen-fd N          serve on inherited listening socket N\n"
"  --port P               listen on 127.0.0.1:P (default: ephemeral)\n"
"  --status-fd N          JSON status events go to descriptor N (default 1)\n"
"  --control-fd N         stop when descriptor N reaches EOF\n"
"  --uid U --gid G        owner shown for files (default: caller)\n"
"  --keep-ownership       show real owners stored on ext2/3/4\n"
"  --mount-point PATH     where the volume will be mounted\n"
"  --no-appledouble       do not map ._ files to extended attributes\n"
"  --no-windows-names     store names verbatim on NTFS\n"
"  --no-hide-dotfiles     do not set the hidden attribute on NTFS dotfiles\n"
"  --ntfs-remove-hiberfile  allow writing to a hibernated Windows volume\n"
"  --file-mode MODE --dir-mode MODE  permissions shown on NTFS\n"
"  --cache-mb N           block cache size\n"
"  --rwsize BYTES         maximum NFS read/write size\n"
"  --user NAME            drop privileges to NAME after setup\n"
"  --insecure             accept unprivileged client ports (testing)\n"
"  --log FILE             append log messages to FILE (default stderr)\n"
"  -v                     verbose logging\n");
}

int main(int argc, char **argv)
{
	enum {
		O_FD = 256, O_RO, O_LFD, O_PORT, O_SFD, O_CFD, O_UID, O_GID,
		O_KEEPOWN, O_MNT, O_NOAD, O_NOWIN, O_NODOT, O_HIBER, O_FMODE,
		O_DMODE, O_CACHE, O_RWSIZE, O_USER, O_INSECURE, O_LOG,
	};
	static const struct option longopts[] = {
		{"fd", required_argument, 0, O_FD},
		{"readonly", no_argument, 0, O_RO},
		{"listen-fd", required_argument, 0, O_LFD},
		{"port", required_argument, 0, O_PORT},
		{"status-fd", required_argument, 0, O_SFD},
		{"control-fd", required_argument, 0, O_CFD},
		{"uid", required_argument, 0, O_UID},
		{"gid", required_argument, 0, O_GID},
		{"keep-ownership", no_argument, 0, O_KEEPOWN},
		{"mount-point", required_argument, 0, O_MNT},
		{"no-appledouble", no_argument, 0, O_NOAD},
		{"no-windows-names", no_argument, 0, O_NOWIN},
		{"no-hide-dotfiles", no_argument, 0, O_NODOT},
		{"ntfs-remove-hiberfile", no_argument, 0, O_HIBER},
		{"file-mode", required_argument, 0, O_FMODE},
		{"dir-mode", required_argument, 0, O_DMODE},
		{"cache-mb", required_argument, 0, O_CACHE},
		{"rwsize", required_argument, 0, O_RWSIZE},
		{"user", required_argument, 0, O_USER},
		{"insecure", no_argument, 0, O_INSECURE},
		{"log", required_argument, 0, O_LOG},
		{0, 0, 0, 0},
	};
	int dev_fd = -1, lfd = -1, status_fd = 1, control_fd = -1, ch, err;
	uint16_t port = 0;
	bool readonly = false, keep_ownership = false, appledouble = true;
	bool insecure = false;
	uint32_t uid = getuid(), gid = getgid(), fmode = 0644, dmode = 0755;
	uint32_t rwsize = 1u << 20;
	size_t cache_mb = 64;
	const char *mount_point = "", *user = NULL, *logfile = NULL;
	fsc_mount_opts mo;
	fsc_dev_opts dopts;
	fsc_dev *dev;
	fsc_volume *vol;
	nfs_server srv;
	struct sigaction sa;

	memset(&mo, 0, sizeof(mo));
	mo.ntfs_windows_names = true;
	mo.ntfs_hide_dot_files = true;
	log_out = stderr;

	while ((ch = getopt_long(argc, argv, "vh", longopts, NULL)) != -1) {
		switch (ch) {
		case O_FD: dev_fd = atoi(optarg); break;
		case O_RO: readonly = true; break;
		case O_LFD: lfd = atoi(optarg); break;
		case O_PORT: port = (uint16_t)atoi(optarg); break;
		case O_SFD: status_fd = atoi(optarg); break;
		case O_CFD: control_fd = atoi(optarg); break;
		case O_UID: uid = (uint32_t)strtoul(optarg, NULL, 10); break;
		case O_GID: gid = (uint32_t)strtoul(optarg, NULL, 10); break;
		case O_KEEPOWN: keep_ownership = true; break;
		case O_MNT: mount_point = optarg; break;
		case O_NOAD: appledouble = false; break;
		case O_NOWIN: mo.ntfs_windows_names = false; break;
		case O_NODOT: mo.ntfs_hide_dot_files = false; break;
		case O_HIBER: mo.ntfs_remove_hiberfile = true; break;
		case O_FMODE: fmode = (uint32_t)strtoul(optarg, NULL, 8) & 0777; break;
		case O_DMODE: dmode = (uint32_t)strtoul(optarg, NULL, 8) & 0777; break;
		case O_CACHE: cache_mb = (size_t)atol(optarg); break;
		case O_RWSIZE: rwsize = (uint32_t)strtoul(optarg, NULL, 0); break;
		case O_USER: user = optarg; break;
		case O_INSECURE: insecure = true; break;
		case O_LOG: logfile = optarg; break;
		case 'v': verbose++; break;
		default: usage(); return EXIT_USAGE;
		}
	}
	if (optind != argc - 1 && dev_fd < 0) {
		usage();
		return EXIT_USAGE;
	}
	if (logfile) {
		log_out = fopen(logfile, "a");
		if (!log_out)
			log_out = stderr;
	}
	status_out = fdopen(dup(status_fd), "w");
	fsc_set_log_fn(log_fn);
	fsc_set_log_level(verbose ? FSC_LOG_DEBUG : FSC_LOG_INFO);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* Device */
	memset(&dopts, 0, sizeof(dopts));
	dopts.cache_bytes = cache_mb << 20;
	if (dev_fd >= 0) {
		dev = fsc_dev_open_fd(dev_fd, readonly, &dopts, &err);
	} else {
		dev = fsc_dev_open_path(argv[optind], readonly, &dopts, &err);
		if (!dev && !readonly && (err == -EACCES || err == -EROFS ||
					  err == -EPERM)) {
			fsc_log(FSC_LOG_WARN, "device is not writable; mounting read-only");
			readonly = true;
			dev = fsc_dev_open_path(argv[optind], true, &dopts, &err);
		}
	}
	if (!dev) {
		status_error("cannot open device: %s", strerror(-err));
		return EXIT_MOUNT;
	}

	/* Filesystem */
	mo.readonly = readonly;
	mo.uid = uid;
	mo.gid = gid;
	mo.fmask = ~fmode & 0777;
	mo.dmask = ~dmode & 0777;
	snprintf(mo.mount_point, sizeof(mo.mount_point), "%s", mount_point);
	err = fsc_mount(dev, &mo, &vol);
	if (err) {
		char label[256];
		const char *type = fsc_probe(dev, label, sizeof(label), NULL);
		if (!type)
			status_error("no NTFS or ext2/3/4 filesystem found");
		else
			status_error("cannot mount %s filesystem: %s", type,
				     strerror(-err));
		fsc_dev_close(dev);
		return EXIT_MOUNT;
	}

	/* Server */
	memset(&srv, 0, sizeof(srv));
	srv.vol = vol;
	fsc_get_volinfo(vol, &srv.info);
	srv.root = srv.info.root_ino;
	srv.readonly = srv.info.readonly;
	srv.ignore_ownership = !keep_ownership;
	srv.owner_uid = uid;
	srv.owner_gid = gid;
	srv.appledouble = appledouble && srv.info.supports_xattrs;
	srv.rtmax = srv.wtmax = rwsize;
	srv.verbose = verbose;
	{
		uint32_t h = 2166136261u;
		for (int i = 0; i < 16; i++)
			h = (h ^ srv.info.uuid[i]) * 16777619u;
		srv.fsid32 = h;
		srv.fsid = ((uint64_t)h << 32) | (uint32_t)getpid();
	}
	{
		uint64_t t = (uint64_t)time(NULL) << 20 ^ (uint64_t)getpid();
		memcpy(srv.write_verf, &t, 8);
	}
	srv.last_change = srv.last_full_sync = nfs_now();
	if (ad_init(&srv)) {
		status_error("out of memory");
		fsc_unmount(vol);
		return EXIT_FAILURE_;
	}

	if (lfd < 0) {
		lfd = nfs_listen_loopback(port, &port);
		if (lfd < 0) {
			status_error("cannot listen: %s", strerror(-lfd));
			fsc_unmount(vol);
			return EXIT_FAILURE_;
		}
	} else {
		struct sockaddr_storage ss;
		socklen_t sl = sizeof(ss);
		if (getsockname(lfd, (struct sockaddr *)&ss, &sl) == 0 &&
		    ss.ss_family == AF_INET)
			port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
	}

	if (user && getuid() == 0) {
		struct passwd *pw = getpwnam(user);
		if (!pw || setgroups(0, NULL) || setgid(pw->pw_gid) ||
		    setuid(pw->pw_uid)) {
			status_error("cannot drop privileges to %s", user);
			fsc_unmount(vol);
			return EXIT_FAILURE_;
		}
	}

	fprintf(status_out, "{\"event\":\"ready\",\"port\":%u,\"type\":", port);
	json_escape(status_out, srv.info.fstype);
	fprintf(status_out, ",\"label\":");
	json_escape(status_out, srv.info.label);
	fprintf(status_out, ",\"uuid\":\"");
	for (int i = 0; i < 16; i++)
		fprintf(status_out, "%02X%s", srv.info.uuid[i],
			(i == 3 || i == 5 || i == 7 || i == 9) ? "-" : "");
	fprintf(status_out, "\",\"readonly\":%s,\"caseInsensitive\":%s,\"details\":",
		srv.readonly ? "true" : "false",
		srv.info.case_insensitive ? "true" : "false");
	json_escape(status_out, srv.info.details);
	fprintf(status_out, ",\"notice\":");
	json_escape(status_out, srv.info.notice);
	fprintf(status_out, "}\n");
	fflush(status_out);

	{
		nfs_serve_opts so = {&stop_flag, control_fd, insecure};
		err = nfs_serve(&srv, lfd, &so);
	}
	close(lfd);

	fsc_log(FSC_LOG_INFO, "shutting down (%llu operations, %llu MB read, "
		"%llu MB written)", (unsigned long long)srv.ops,
		(unsigned long long)(srv.bytes_read >> 20),
		(unsigned long long)(srv.bytes_written >> 20));
	{
		bool lost = fsc_dev_failed(fsc_get_dev(vol));
		int e1 = lost ? 0 : nfs_flush_all(&srv);
		int e2;

		ad_shutdown(&srv);
		e2 = fsc_unmount(vol);
		if (lost) {
			status_error("the disk was disconnected before it was ejected");
			return EXIT_DEVICE_LOST;
		}
		if (e1 || e2) {
			status_error("errors while writing back data: %s",
				     strerror(-(e1 ? e1 : e2)));
			return EXIT_FAILURE_;
		}
	}
	fprintf(status_out, "{\"event\":\"stopped\",\"clean\":true}\n");
	fflush(status_out);
	return err == -ENXIO ? EXIT_DEVICE_LOST : EXIT_OK;
}
