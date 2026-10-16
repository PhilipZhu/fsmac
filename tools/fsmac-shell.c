/*
 * fsmac-shell: drives the FSMac filesystem engine directly on a disk image
 * or device, without any mounting. Used for development and by the test
 * suite.
 *
 *   fsmac-shell [-r] [-S sectorsize] [-v] <image> [command [args...]]
 *
 * Without a command, commands are read from standard input, one per line.
 * Run "help" for the command list.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <fsmac/fsc.h>

static fsc_volume *vol;
static fsc_dev *dev;
static const char *image_path;
static bool opt_readonly;
static uint32_t opt_sector;
static fsc_ino root_ino;
static int failures;

static const fsc_cred default_cred = {501, 20};

static fsc_mount_opts mount_opts(void)
{
	fsc_mount_opts o;

	memset(&o, 0, sizeof(o));
	o.readonly = opt_readonly;
	o.uid = 501;
	o.gid = 20;
	o.ntfs_windows_names = true;
	o.ntfs_hide_dot_files = true;
	o.fmask = 022;
	o.dmask = 022;
	snprintf(o.mount_point, sizeof(o.mount_point), "/Volumes/TEST");
	return o;
}

static int do_mount(void)
{
	fsc_dev_opts dopts = {0};
	fsc_mount_opts o = mount_opts();
	fsc_volinfo info;
	int err;

	dopts.sector_size = opt_sector;
	dev = fsc_dev_open_path(image_path, opt_readonly, &dopts, &err);
	if (!dev) {
		fprintf(stderr, "open %s: %s\n", image_path, strerror(-err));
		return err;
	}
	err = fsc_mount(dev, &o, &vol);
	if (err) {
		fprintf(stderr, "mount: %s\n", strerror(-err));
		fsc_dev_close(dev);
		dev = NULL;
		return err;
	}
	fsc_get_volinfo(vol, &info);
	root_ino = info.root_ino;
	return 0;
}

static int do_unmount(void)
{
	int err = fsc_unmount(vol);

	vol = NULL;
	dev = NULL;
	if (err)
		fprintf(stderr, "unmount: %s\n", strerror(-err));
	return err;
}

/* ---------------------------------------------------------------------- */

static int resolve(const char *path, fsc_ino *ino)
{
	fsc_ino cur = root_ino;
	const char *p = path;

	while (*p) {
		const char *s;
		int err;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		s = strchr(p, '/');
		size_t len = s ? (size_t)(s - p) : strlen(p);
		err = fsc_lookup(vol, cur, p, len, &cur, NULL);
		if (err)
			return err;
		p += len;
	}
	*ino = cur;
	return 0;
}

/* Splits path into parent inode and final component. */
static int resolve_parent(const char *path, fsc_ino *dir, const char **name,
			  size_t *len)
{
	const char *slash = strrchr(path, '/');
	char buf[4096];

	if (!slash) {
		*dir = root_ino;
		*name = path;
		*len = strlen(path);
		return 0;
	}
	if ((size_t)(slash - path) >= sizeof(buf))
		return -ENAMETOOLONG;
	memcpy(buf, path, (size_t)(slash - path));
	buf[slash - path] = 0;
	*name = slash + 1;
	*len = strlen(slash + 1);
	return resolve(buf, dir);
}

static const char *type_char(fsc_type t)
{
	switch (t) {
	case FSC_TYPE_DIR: return "d";
	case FSC_TYPE_LNK: return "l";
	case FSC_TYPE_CHR: return "c";
	case FSC_TYPE_BLK: return "b";
	case FSC_TYPE_FIFO: return "p";
	case FSC_TYPE_SOCK: return "s";
	default: return "-";
	}
}

struct ls_ctx {
	fsc_ino dir;
	bool long_fmt;
	int count;
};

static int ls_cb(void *c, const char *name, size_t len, fsc_ino ino,
		 fsc_type type, uint64_t next)
{
	struct ls_ctx *lc = c;
	fsc_attr a;

	(void)next;
	lc->count++;
	if (!lc->long_fmt) {
		printf("%.*s\n", (int)len, name);
		return 0;
	}
	if (fsc_getattr(vol, ino, &a) == 0)
		printf("%s%04o %3u %10llu %8llu %.*s\n", type_char(a.type),
		       a.mode, a.nlink, (unsigned long long)a.size,
		       (unsigned long long)ino, (int)len, name);
	else
		printf("?????     ? %10s %8llu %.*s\n", "?",
		       (unsigned long long)ino, (int)len, name);
	(void)type;
	return 0;
}

static int cmd_ls(const char *path, bool long_fmt)
{
	fsc_ino ino;
	struct ls_ctx lc = {0, long_fmt, 0};
	int err = resolve(path ? path : "/", &ino);

	if (err)
		return err;
	lc.dir = ino;
	return fsc_readdir(vol, ino, 0, ls_cb, &lc);
}

/* Paged readdir: exercises cookie resumption by stopping every n entries. */
struct page_ctx {
	int n, got;
	uint64_t last;
	char names[64][256];
};

static int page_cb(void *c, const char *name, size_t len, fsc_ino ino,
		   fsc_type type, uint64_t next)
{
	struct page_ctx *pc = c;

	(void)ino; (void)type;
	if (pc->got >= pc->n)
		return 1;
	snprintf(pc->names[pc->got], 256, "%.*s", (int)len, name);
	pc->got++;
	pc->last = next;
	return 0;
}

static int cmd_lspaged(const char *path, int n)
{
	fsc_ino ino;
	uint64_t cookie = 0;
	int err = resolve(path, &ino);

	if (err)
		return err;
	if (n < 1 || n > 64)
		n = 7;
	for (;;) {
		struct page_ctx pc;

		memset(&pc, 0, sizeof(pc));
		pc.n = n;
		err = fsc_readdir(vol, ino, cookie, page_cb, &pc);
		if (err)
			return err;
		for (int i = 0; i < pc.got; i++)
			printf("%s\n", pc.names[i]);
		if (pc.got < n)
			break;
		cookie = pc.last;
	}
	return 0;
}

static void print_ts(const char *label, struct timespec ts)
{
	char buf[64];
	time_t t = ts.tv_sec;
	struct tm tm;

	gmtime_r(&t, &tm);
	strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
	printf("  %s %s.%09ld\n", label, buf, ts.tv_nsec);
}

static int cmd_stat(const char *path)
{
	fsc_ino ino;
	fsc_attr a;
	int err = resolve(path, &ino);

	if (err)
		return err;
	err = fsc_getattr(vol, ino, &a);
	if (err)
		return err;
	printf("%s: ino=%llu gen=%u type=%s mode=%04o nlink=%u uid=%u gid=%u "
	       "size=%llu used=%llu flags=0x%x\n",
	       path, (unsigned long long)a.ino, a.gen, fsc_type_name(a.type),
	       a.mode, a.nlink, a.uid, a.gid, (unsigned long long)a.size,
	       (unsigned long long)a.used, a.flags);
	print_ts("atime", a.atime);
	print_ts("mtime", a.mtime);
	print_ts("ctime", a.ctime);
	print_ts("btime", a.btime);
	return 0;
}

static int cmd_cat(const char *path, FILE *out)
{
	fsc_ino ino;
	static char buf[1 << 20];
	uint64_t off = 0;
	int err = resolve(path, &ino);

	if (err)
		return err;
	for (;;) {
		size_t got = 0;
		err = fsc_read(vol, ino, off, sizeof(buf), buf, &got);
		if (err)
			return err;
		if (got == 0)
			break;
		fwrite(buf, 1, got, out);
		off += got;
	}
	return 0;
}

static int open_or_create(const char *path, fsc_ino *ino)
{
	fsc_ino dir;
	const char *name;
	size_t len;
	int err = resolve(path, ino);

	if (err != -ENOENT)
		return err;
	err = resolve_parent(path, &dir, &name, &len);
	if (err)
		return err;
	return fsc_create(vol, dir, name, len, FSC_TYPE_REG, 0644, 0, 0,
			  &default_cred, ino, NULL);
}

static int put_fd(int fd, const char *path)
{
	fsc_ino ino;
	fsc_sattr sa = {0};
	static char buf[1 << 20];
	uint64_t off = 0;
	int err = open_or_create(path, &ino);

	if (err)
		return err;
	sa.mask = FSC_SET_SIZE;
	sa.size = 0;
	err = fsc_setattr(vol, ino, &sa, NULL);
	if (err)
		return err;
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		size_t w = 0;
		if (n < 0)
			return -errno;
		if (n == 0)
			break;
		err = fsc_write(vol, ino, off, (size_t)n, buf, &w);
		if (err)
			return err;
		if (w != (size_t)n)
			return -EIO;
		off += (uint64_t)n;
	}
	return 0;
}

static int cmd_put(const char *local, const char *path)
{
	int fd = open(local, O_RDONLY), err;

	if (fd < 0)
		return -errno;
	err = put_fd(fd, path);
	close(fd);
	return err;
}

static int cmd_get(const char *path, const char *local)
{
	FILE *f = fopen(local, "wb");
	int err;

	if (!f)
		return -errno;
	err = cmd_cat(path, f);
	if (fclose(f) && !err)
		err = -errno;
	return err;
}

static int cmd_mkdir(const char *path)
{
	fsc_ino dir;
	const char *name;
	size_t len;
	int err = resolve_parent(path, &dir, &name, &len);

	if (err)
		return err;
	return fsc_create(vol, dir, name, len, FSC_TYPE_DIR, 0755, 0, 0,
			  &default_cred, NULL, NULL);
}

static int cmd_rm(const char *path, bool isdir)
{
	fsc_ino dir;
	const char *name;
	size_t len;
	int err = resolve_parent(path, &dir, &name, &len);

	if (err)
		return err;
	return isdir ? fsc_rmdir(vol, dir, name, len)
		     : fsc_unlink(vol, dir, name, len);
}

static int cmd_mv(const char *from, const char *to)
{
	fsc_ino sdir, ddir;
	const char *sn, *dn;
	size_t sl, dl;
	int err = resolve_parent(from, &sdir, &sn, &sl);

	if (err)
		return err;
	err = resolve_parent(to, &ddir, &dn, &dl);
	if (err)
		return err;
	return fsc_rename(vol, sdir, sn, sl, ddir, dn, dl);
}

static int cmd_link(const char *target, const char *path)
{
	fsc_ino ino, dir;
	const char *name;
	size_t len;
	int err = resolve(target, &ino);

	if (err)
		return err;
	err = resolve_parent(path, &dir, &name, &len);
	if (err)
		return err;
	return fsc_link(vol, ino, dir, name, len);
}

static int cmd_symlink(const char *target, const char *path)
{
	fsc_ino dir;
	const char *name;
	size_t len;
	int err = resolve_parent(path, &dir, &name, &len);

	if (err)
		return err;
	return fsc_symlink(vol, dir, name, len, target, strlen(target),
			   &default_cred, NULL, NULL);
}

static int cmd_readlink(const char *path)
{
	fsc_ino ino;
	char buf[4096];
	size_t len;
	int err = resolve(path, &ino);

	if (err)
		return err;
	err = fsc_readlink(vol, ino, buf, sizeof(buf), &len);
	if (err)
		return err;
	printf("%.*s\n", (int)len, buf);
	return 0;
}

static int cmd_setattr(const char *path, uint32_t mask, uint64_t val)
{
	fsc_ino ino;
	fsc_sattr sa = {0};
	int err = resolve(path, &ino);

	if (err)
		return err;
	sa.mask = mask;
	sa.size = val;
	sa.mode = (uint32_t)val;
	sa.uid = sa.gid = (uint32_t)val;
	sa.mtime.tv_sec = (time_t)val;
	sa.flags = (uint32_t)val;
	return fsc_setattr(vol, ino, &sa, NULL);
}

static int cmd_listxattr(const char *path)
{
	fsc_ino ino;
	char buf[65536];
	size_t len, i;
	int err = resolve(path, &ino);

	if (err)
		return err;
	err = fsc_listxattr(vol, ino, buf, sizeof(buf), &len);
	if (err)
		return err;
	for (i = 0; i < len; i += strlen(buf + i) + 1)
		printf("%s\n", buf + i);
	return 0;
}

static int cmd_getxattr(const char *path, const char *name)
{
	fsc_ino ino;
	char buf[65536];
	size_t len;
	int err = resolve(path, &ino);

	if (err)
		return err;
	err = fsc_getxattr(vol, ino, name, buf, sizeof(buf), &len);
	if (err)
		return err;
	fwrite(buf, 1, len, stdout);
	printf("\n");
	return 0;
}

static int cmd_setxattr(const char *path, const char *name, const char *val)
{
	fsc_ino ino;
	int err = resolve(path, &ino);

	if (err)
		return err;
	return fsc_setxattr(vol, ino, name, val, strlen(val), 0);
}

static int cmd_rmxattr(const char *path, const char *name)
{
	fsc_ino ino;
	int err = resolve(path, &ino);

	if (err)
		return err;
	return fsc_removexattr(vol, ino, name);
}

static int cmd_info(void)
{
	fsc_volinfo vi;
	fsc_fsstat st;
	char uuid[40];
	int err;

	fsc_get_volinfo(vol, &vi);
	err = fsc_statfs(vol, &st);
	if (err)
		return err;
	snprintf(uuid, sizeof(uuid),
		 "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
		 vi.uuid[0], vi.uuid[1], vi.uuid[2], vi.uuid[3], vi.uuid[4],
		 vi.uuid[5], vi.uuid[6], vi.uuid[7], vi.uuid[8], vi.uuid[9],
		 vi.uuid[10], vi.uuid[11], vi.uuid[12], vi.uuid[13], vi.uuid[14],
		 vi.uuid[15]);
	printf("type:      %s\nlabel:     %s\nuuid:      %s\nmode:      %s\n"
	       "details:   %s\n", vi.fstype, vi.label, uuid,
	       vi.readonly ? "read-only" : "read-write", vi.details);
	printf("size:      %llu bytes, %llu free, %llu available\n",
	       (unsigned long long)st.total_bytes,
	       (unsigned long long)st.free_bytes,
	       (unsigned long long)st.avail_bytes);
	printf("files:     %llu total, %llu free\nblock:     %u\n",
	       (unsigned long long)st.total_files,
	       (unsigned long long)st.free_files, st.block_size);
	if (vi.notice[0])
		printf("notice:    %s\n", vi.notice);
	return 0;
}

/* Recursive tree listing with a content digest, for comparing trees. */
static uint64_t digest_data(fsc_ino ino, uint64_t size, int *err)
{
	static char buf[1 << 20];
	uint64_t h = 1469598103934665603ull, off = 0;

	while (off < size) {
		size_t got = 0;
		*err = fsc_read(vol, ino, off, sizeof(buf), buf, &got);
		if (*err)
			return 0;
		if (!got)
			break;
		for (size_t i = 0; i < got; i++) {
			h ^= (unsigned char)buf[i];
			h *= 1099511628211ull;
		}
		off += got;
	}
	if (off != size)
		*err = -EIO;
	return h;
}

struct tree_ent {
	char name[1024];
	fsc_ino ino;
};

struct tree_list {
	struct tree_ent *v;
	size_t n, cap;
};

static int tree_cb(void *c, const char *name, size_t len, fsc_ino ino,
		   fsc_type type, uint64_t next)
{
	struct tree_list *tl = c;

	(void)type; (void)next;
	if (tl->n == tl->cap) {
		tl->cap = tl->cap ? tl->cap * 2 : 64;
		tl->v = realloc(tl->v, tl->cap * sizeof(*tl->v));
	}
	snprintf(tl->v[tl->n].name, sizeof(tl->v[tl->n].name), "%.*s",
		 (int)len, name);
	tl->v[tl->n].ino = ino;
	tl->n++;
	return 0;
}

static int ent_cmp(const void *a, const void *b)
{
	return strcmp(((const struct tree_ent *)a)->name,
		      ((const struct tree_ent *)b)->name);
}

static int walk(fsc_ino dir, const char *prefix, bool digest)
{
	struct tree_list tl = {0};
	int err = fsc_readdir(vol, dir, 0, tree_cb, &tl);

	if (err)
		return err;
	qsort(tl.v, tl.n, sizeof(*tl.v), ent_cmp);
	for (size_t i = 0; i < tl.n && !err; i++) {
		char path[4096];
		fsc_attr a;

		snprintf(path, sizeof(path), "%s/%s", prefix, tl.v[i].name);
		err = fsc_getattr(vol, tl.v[i].ino, &a);
		if (err)
			break;
		if (a.type == FSC_TYPE_DIR) {
			printf("d %s\n", path);
			err = walk(tl.v[i].ino, path, digest);
		} else if (a.type == FSC_TYPE_LNK) {
			char t[4096];
			size_t tlen;
			err = fsc_readlink(vol, tl.v[i].ino, t, sizeof(t), &tlen);
			printf("l %s -> %.*s\n", path, (int)tlen, t);
		} else if (a.type == FSC_TYPE_REG) {
			if (digest)
				printf("f %s %llu %016llx\n", path,
				       (unsigned long long)a.size,
				       (unsigned long long)digest_data(
					       tl.v[i].ino, a.size, &err));
			else
				printf("f %s %llu\n", path,
				       (unsigned long long)a.size);
		} else {
			printf("%s %s\n", type_char(a.type), path);
		}
	}
	free(tl.v);
	return err;
}

static int cmd_tree(const char *path, bool digest)
{
	fsc_ino ino;
	int err = resolve(path ? path : "/", &ino);

	if (err)
		return err;
	return walk(ino, "", digest);
}

/* Recursively copies a host directory into the volume. */
static int import_dir(const char *local, fsc_ino dir)
{
	DIR *d = opendir(local);
	struct dirent *de;
	int err = 0;

	if (!d)
		return -errno;
	while (!err && (de = readdir(d))) {
		char lpath[4096];
		struct stat sb;
		fsc_ino child;

		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		snprintf(lpath, sizeof(lpath), "%s/%s", local, de->d_name);
		if (lstat(lpath, &sb))
			continue;
		size_t nl = strlen(de->d_name);
		if (S_ISDIR(sb.st_mode)) {
			err = fsc_create(vol, dir, de->d_name, nl, FSC_TYPE_DIR,
					 sb.st_mode & 07777, 0, 0, &default_cred,
					 &child, NULL);
			if (!err)
				err = import_dir(lpath, child);
		} else if (S_ISLNK(sb.st_mode)) {
			char t[4096];
			ssize_t tl = readlink(lpath, t, sizeof(t));
			if (tl > 0)
				err = fsc_symlink(vol, dir, de->d_name, nl, t,
						  (size_t)tl, &default_cred, NULL,
						  NULL);
		} else if (S_ISREG(sb.st_mode)) {
			static char buf[1 << 20];
			int fd = open(lpath, O_RDONLY);
			uint64_t off = 0;

			if (fd < 0)
				continue;
			err = fsc_create(vol, dir, de->d_name, nl, FSC_TYPE_REG,
					 sb.st_mode & 07777, 0, 0, &default_cred,
					 &child, NULL);
			while (!err) {
				ssize_t n = read(fd, buf, sizeof(buf));
				size_t w;
				if (n <= 0)
					break;
				err = fsc_write(vol, child, off, (size_t)n, buf, &w);
				off += (uint64_t)n;
			}
			close(fd);
		}
		if (err)
			fprintf(stderr, "import %s: %s\n", lpath, strerror(-err));
	}
	closedir(d);
	return err;
}

static int cmd_import(const char *local, const char *path)
{
	fsc_ino ino;
	int err = resolve(path, &ino);

	if (err)
		return err;
	return import_dir(local, ino);
}

/* ---------------------------------------------------------------------- */
/* Randomised differential stress test against a host directory           */

#define NSTRESS_FILES 48

static uint64_t rng_state;

static uint64_t rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return rng_state;
}

static void stress_name(char *buf, size_t sz, int i)
{
	static const char *const names[] = {
		"a", "B", "file.txt", "Report final.docx", "über", "日本語",
		"emoji😀", "dot.", "q?mark", "colon:x", "back\\slash",
		"pipe|x", "star*", "lt<gt>", "quote\"", "trail ", "x",
	};
	int n = (int)(sizeof(names) / sizeof(names[0]));
	snprintf(buf, sz, "%s%d", names[i % n], i);
}

static int host_read(const char *path, char **data, size_t *len)
{
	FILE *f = fopen(path, "rb");
	struct stat sb;

	if (!f)
		return -errno;
	fstat(fileno(f), &sb);
	*len = (size_t)sb.st_size;
	*data = malloc(*len + 1);
	if (*len && fread(*data, 1, *len, f) != *len) {
		fclose(f);
		free(*data);
		return -EIO;
	}
	fclose(f);
	return 0;
}

static int verify_file(const char *host, const char *vpath)
{
	char *hd = NULL, *vd;
	size_t hl = 0, got = 0;
	fsc_ino ino;
	fsc_attr a;
	int err;

	err = host_read(host, &hd, &hl);
	if (err)
		return 0; /* host side doesn't have it either; checked elsewhere */
	err = resolve(vpath, &ino);
	if (err) {
		fprintf(stderr, "VERIFY: %s missing on volume (%s)\n", vpath,
			strerror(-err));
		free(hd);
		return -1;
	}
	fsc_getattr(vol, ino, &a);
	vd = malloc(hl + 1);
	err = fsc_read(vol, ino, 0, hl + 1, vd, &got);
	if (err || got != hl || a.size != hl || memcmp(hd, vd, hl)) {
		fprintf(stderr, "VERIFY: %s differs (host %zu bytes, volume %zu/%llu, "
			"err %d)\n", vpath, hl, got, (unsigned long long)a.size, err);
		free(hd);
		free(vd);
		return -1;
	}
	free(hd);
	free(vd);
	return 0;
}

static int cmd_stress(const char *hostdir, long iterations, uint64_t seed)
{
	char host[NSTRESS_FILES][4096], vpath[NSTRESS_FILES][1024];
	static char buf[1 << 20];
	int mismatches = 0;

	char vdir[64];
	int err;

	rng_state = seed ? seed : 0x123456789abcdefull;
	mkdir(hostdir, 0755);
	snprintf(vdir, sizeof(vdir), "stress-%llx", (unsigned long long)rng_state);
	err = cmd_mkdir(vdir);
	if (err) {
		fprintf(stderr, "stress: cannot create %s: %s\n", vdir, strerror(-err));
		return err;
	}
	for (int i = 0; i < NSTRESS_FILES; i++) {
		char n[256];
		stress_name(n, sizeof(n), i);
		snprintf(host[i], sizeof(host[i]), "%s/%s", hostdir, n);
		snprintf(vpath[i], sizeof(vpath[i]), "%s/%s", vdir, n);
	}
	for (long it = 0; it < iterations; it++) {
		int i = (int)(rnd() % NSTRESS_FILES), op = (int)(rnd() % 100);
		fsc_ino ino;
		int verr = 0, herr = 0;

		if (op < 35) { /* write at random offset */
			uint64_t off = rnd() % (3u << 20);
			size_t len = (size_t)(rnd() % 300000) + 1;
			if (rnd() % 4 == 0)
				len = (size_t)(rnd() % 2000) + 1;
			for (size_t k = 0; k < len; k++)
				buf[k] = (char)(rnd() & 0xff);
			int fd = open(host[i], O_RDWR | O_CREAT, 0644);
			if (fd < 0 || pwrite(fd, buf, len, (off_t)off) != (ssize_t)len)
				herr = -errno;
			if (fd >= 0)
				close(fd);
			verr = open_or_create(vpath[i], &ino);
			if (!verr) {
				size_t w;
				verr = fsc_write(vol, ino, off, len, buf, &w);
				if (!verr && w != len)
					verr = -EIO;
			}
		} else if (op < 45) { /* truncate */
			uint64_t sz = rnd() % (4u << 20);
			herr = truncate(host[i], (off_t)sz) ? -errno : 0;
			verr = cmd_setattr(vpath[i], FSC_SET_SIZE, sz);
		} else if (op < 55) { /* unlink */
			herr = unlink(host[i]) ? -errno : 0;
			verr = cmd_rm(vpath[i], false);
		} else if (op < 65) { /* rename to another slot */
			int j = (int)(rnd() % NSTRESS_FILES);
			herr = rename(host[i], host[j]) ? -errno : 0;
			verr = cmd_mv(vpath[i], vpath[j]);
		} else if (op < 72) { /* hard link */
			int j = (int)(rnd() % NSTRESS_FILES);
			herr = link(host[i], host[j]) ? -errno : 0;
			verr = cmd_link(vpath[i], vpath[j]);
		} else if (op < 80) { /* xattr */
			char v[64];
			snprintf(v, sizeof(v), "value-%llu",
				 (unsigned long long)(rnd() % 1000));
			herr = access(host[i], F_OK) ? -errno : 0;
			verr = cmd_setxattr(vpath[i], "com.example.test", v);
			if (!verr) {
				fsc_ino xi;
				char g[64];
				size_t gl;
				resolve(vpath[i], &xi);
				if (fsc_getxattr(vol, xi, "com.example.test", g,
						 sizeof(g), &gl) ||
				    gl != strlen(v) || memcmp(g, v, gl)) {
					fprintf(stderr, "XATTR mismatch on %s\n",
						vpath[i]);
					mismatches++;
				}
			}
		} else if (op < 85) { /* remount */
			if (do_unmount() || do_mount()) {
				fprintf(stderr, "remount failed at %ld\n", it);
				return -EIO;
			}
			continue;
		} else if (op < 90) { /* sync */
			fsc_sync(vol, rnd() % 2 ? FSC_SYNC_FULL : FSC_SYNC_DATA);
			continue;
		} else { /* verify */
			if (verify_file(host[i], vpath[i]))
				mismatches++;
			continue;
		}
		/* Both sides must agree on success vs failure. */
		if ((herr == 0) != (verr == 0)) {
			fprintf(stderr, "MISMATCH it=%ld op=%d %s: host=%s volume=%s\n",
				it, op, vpath[i], strerror(-herr), strerror(-verr));
			mismatches++;
		}
	}
	for (int i = 0; i < NSTRESS_FILES; i++) {
		fsc_ino ino;
		bool hexists = access(host[i], F_OK) == 0;
		bool vexists = resolve(vpath[i], &ino) == 0;
		if (hexists != vexists) {
			fprintf(stderr, "FINAL: %s exists host=%d volume=%d\n",
				vpath[i], hexists, vexists);
			mismatches++;
		} else if (hexists && verify_file(host[i], vpath[i])) {
			mismatches++;
		}
	}
	printf("stress: %ld iterations, %d mismatches\n", iterations, mismatches);
	return mismatches ? -EIO : 0;
}

/* ---------------------------------------------------------------------- */

static void help(void)
{
	puts("commands:\n"
	     "  info | ls [path] | ll [path] | lspaged path n | stat path\n"
	     "  cat path | get path local | put local path | touch path\n"
	     "  mkdir path | rm path | rmdir path | mv from to\n"
	     "  link target path | symlink target path | readlink path\n"
	     "  truncate path size | chmod octal path | chown uid path\n"
	     "  mtime path epoch | flags path value\n"
	     "  lsxattr path | getxattr path name | setxattr path name value\n"
	     "  rmxattr path name | label name | sync | fullsync | remount\n"
	     "  tree [path] | digest [path] | import localdir path\n"
	     "  stress hostdir iterations [seed]");
}

static int run(int argc, char **argv)
{
	const char *c = argv[0];
#define NEED(n) do { if (argc < (n) + 1) { help(); return -EINVAL; } } while (0)

	if (!strcmp(c, "help")) { help(); return 0; }
	if (!strcmp(c, "info")) return cmd_info();
	if (!strcmp(c, "ls")) return cmd_ls(argc > 1 ? argv[1] : "/", false);
	if (!strcmp(c, "ll")) return cmd_ls(argc > 1 ? argv[1] : "/", true);
	if (!strcmp(c, "lspaged")) { NEED(2); return cmd_lspaged(argv[1], atoi(argv[2])); }
	if (!strcmp(c, "stat")) { NEED(1); return cmd_stat(argv[1]); }
	if (!strcmp(c, "cat")) { NEED(1); return cmd_cat(argv[1], stdout); }
	if (!strcmp(c, "get")) { NEED(2); return cmd_get(argv[1], argv[2]); }
	if (!strcmp(c, "put")) { NEED(2); return cmd_put(argv[1], argv[2]); }
	if (!strcmp(c, "touch")) { NEED(1); fsc_ino i; return open_or_create(argv[1], &i); }
	if (!strcmp(c, "mkdir")) { NEED(1); return cmd_mkdir(argv[1]); }
	if (!strcmp(c, "rm")) { NEED(1); return cmd_rm(argv[1], false); }
	if (!strcmp(c, "rmdir")) { NEED(1); return cmd_rm(argv[1], true); }
	if (!strcmp(c, "mv")) { NEED(2); return cmd_mv(argv[1], argv[2]); }
	if (!strcmp(c, "link")) { NEED(2); return cmd_link(argv[1], argv[2]); }
	if (!strcmp(c, "symlink")) { NEED(2); return cmd_symlink(argv[1], argv[2]); }
	if (!strcmp(c, "readlink")) { NEED(1); return cmd_readlink(argv[1]); }
	if (!strcmp(c, "truncate")) { NEED(2); return cmd_setattr(argv[1], FSC_SET_SIZE, strtoull(argv[2], NULL, 0)); }
	if (!strcmp(c, "chmod")) { NEED(2); return cmd_setattr(argv[2], FSC_SET_MODE, strtoull(argv[1], NULL, 8)); }
	if (!strcmp(c, "chown")) { NEED(2); return cmd_setattr(argv[2], FSC_SET_UID | FSC_SET_GID, strtoull(argv[1], NULL, 0)); }
	if (!strcmp(c, "mtime")) { NEED(2); return cmd_setattr(argv[1], FSC_SET_MTIME, strtoull(argv[2], NULL, 0)); }
	if (!strcmp(c, "flags")) { NEED(2); return cmd_setattr(argv[1], FSC_SET_FLAGS, strtoull(argv[2], NULL, 0)); }
	if (!strcmp(c, "lsxattr")) { NEED(1); return cmd_listxattr(argv[1]); }
	if (!strcmp(c, "getxattr")) { NEED(2); return cmd_getxattr(argv[1], argv[2]); }
	if (!strcmp(c, "setxattr")) { NEED(3); return cmd_setxattr(argv[1], argv[2], argv[3]); }
	if (!strcmp(c, "rmxattr")) { NEED(2); return cmd_rmxattr(argv[1], argv[2]); }
	if (!strcmp(c, "label")) { NEED(1); return fsc_set_label(vol, argv[1]); }
	if (!strcmp(c, "sync")) return fsc_sync(vol, FSC_SYNC_DATA);
	if (!strcmp(c, "fullsync")) return fsc_sync(vol, FSC_SYNC_FULL);
	if (!strcmp(c, "remount")) { int e = do_unmount(); return e ? e : do_mount(); }
	if (!strcmp(c, "tree")) return cmd_tree(argc > 1 ? argv[1] : "/", false);
	if (!strcmp(c, "digest")) return cmd_tree(argc > 1 ? argv[1] : "/", true);
	if (!strcmp(c, "import")) { NEED(2); return cmd_import(argv[1], argv[2]); }
	if (!strcmp(c, "stress")) { NEED(2); return cmd_stress(argv[1], atol(argv[2]), argc > 3 ? strtoull(argv[3], NULL, 0) : 0); }
	fprintf(stderr, "unknown command: %s\n", c);
	return -EINVAL;
}

static int run_line(char *line)
{
	char *argv[16];
	int argc = 0;
	char *p = line;

	while (*p && argc < 15) {
		while (*p == ' ' || *p == '\t' || *p == '\n')
			p++;
		if (!*p)
			break;
		if (*p == '"') {
			argv[argc++] = ++p;
			while (*p && *p != '"')
				p++;
		} else {
			argv[argc++] = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n')
				p++;
		}
		if (*p)
			*p++ = 0;
	}
	if (!argc || argv[0][0] == '#')
		return 0;
	return run(argc, argv);
}

int main(int argc, char **argv)
{
	int ch, err = 0;

	while ((ch = getopt(argc, argv, "rS:v")) != -1) {
		switch (ch) {
		case 'r': opt_readonly = true; break;
		case 'S': opt_sector = (uint32_t)atoi(optarg); break;
		case 'v': fsc_set_log_level(FSC_LOG_DEBUG); break;
		default:
			fprintf(stderr, "usage: %s [-r] [-S sector] image [cmd args...]\n",
				argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s [-r] [-S sector] image [cmd args...]\n",
			argv[0]);
		return 2;
	}
	image_path = argv[optind++];
	if (do_mount())
		return 1;
	if (optind < argc) {
		err = run(argc - optind, argv + optind);
		if (err) {
			fprintf(stderr, "error: %s\n", strerror(-err));
			failures++;
		}
	} else {
		char line[8192];
		while (fgets(line, sizeof(line), stdin)) {
			char copy[8192];
			snprintf(copy, sizeof(copy), "%s", line);
			err = run_line(line);
			if (err) {
				copy[strcspn(copy, "\n")] = 0;
				fprintf(stderr, "error: %s: %s\n", copy, strerror(-err));
				failures++;
			}
		}
	}
	if (vol && do_unmount())
		failures++;
	return failures ? 1 : 0;
}
