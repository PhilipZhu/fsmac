/*
 * Randomised test of the block device cache against a shadow copy.
 *
 *   blockdev-test <image> <sector-size> <cache-bytes> <iterations> [seed]
 *
 * Performs random reads/writes of random sizes and alignments (both cached
 * and direct paths), random flushes and invalidations, and checks every
 * read against an in-memory shadow. Finally the image file itself is
 * compared with the shadow.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fsmac/blockdev.h>

int fsc_dev_debug_check(fsc_dev *dev, const uint8_t *shadow);
void fsc_dev_debug_watch(long idx);
static const char *opname = "";

static uint64_t s;
static uint64_t rnd(void)
{
	s ^= s << 13;
	s ^= s >> 7;
	s ^= s << 17;
	return s;
}

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s image sector cache iterations [seed]\n",
			argv[0]);
		return 2;
	}
	const char *path = argv[1];
	fsc_dev_opts o = {0};
	o.sector_size = (uint32_t)atoi(argv[2]);
	o.cache_bytes = (size_t)atol(argv[3]);
	o.dirty_limit_bytes = o.cache_bytes / 4;
	long iters = atol(argv[4]);
	s = argc > 5 ? strtoull(argv[5], NULL, 0) : 88172645463325252ull;

	int err, fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror(path);
		return 2;
	}
	off_t size = lseek(fd, 0, SEEK_END);
	uint8_t *shadow = malloc((size_t)size);
	if (pread(fd, shadow, (size_t)size, 0) != size) {
		perror("pread");
		return 2;
	}
	close(fd);

	if (getenv("WATCH")) fsc_dev_debug_watch(atol(getenv("WATCH")));
	fsc_dev *dev = fsc_dev_open_path(path, false, &o, &err);
	if (!dev) {
		fprintf(stderr, "open: %s\n", strerror(-err));
		return 2;
	}
	uint8_t *buf = malloc(4 << 20);
	long bad = 0;
	for (long i = 0; i < iters; i++) {
		int op = (int)(rnd() % 100);
		size_t len;
		uint64_t off;

		switch (rnd() % 4) {
		case 0: len = (size_t)(rnd() % 600) + 1; break;
		case 1: len = (size_t)(rnd() % 20000) + 1; break;
		case 2: len = (size_t)(rnd() % (512 << 10)) + 1; break;
		default: len = ((size_t)(rnd() % 64) + 1) * 4096; break;
		}
		if (len > (size_t)size)
			len = (size_t)size;
		off = rnd() % ((uint64_t)size - len + 1);
		if (rnd() % 3 == 0)
			off &= ~4095ull;

		if (getenv("WATCH")) { long w = atol(getenv("WATCH")); if (off/4096 <= (uint64_t)w && (off+len-1)/4096 >= (uint64_t)w) fprintf(stderr, "iter %ld op %d covers watch (off=%llu len=%zu)\n", i, op, (unsigned long long)off, len); }
		if (op < 45) {
			for (size_t k = 0; k < len; k++)
				buf[k] = (uint8_t)rnd();
			err = fsc_dev_pwrite(dev, buf, len, off);
			if (err) {
				fprintf(stderr, "write err %d\n", err);
				return 1;
			}
			memcpy(shadow + off, buf, len);
		} else if (op < 90) {
			err = fsc_dev_pread(dev, buf, len, off);
			if (err) {
				fprintf(stderr, "read err %d\n", err);
				return 1;
			}
			if (memcmp(buf, shadow + off, len)) {
				size_t k = 0;
				while (buf[k] == shadow[off + k])
					k++;
				fprintf(stderr, "iter %ld: read mismatch off=%llu len=%zu "
					"first diff at +%zu (abs %llu)\n", i,
					(unsigned long long)off, len, k,
					(unsigned long long)(off + k));
				if (++bad > 5)
					return 1;
			}
		} else if (op < 95) {
			fsc_dev_flush(dev, rnd() % 2);
		} else {
			fsc_dev_invalidate(dev);
		}
		if (fsc_dev_debug_check(dev, shadow)) {
			fprintf(stderr, "iter %ld: cache inconsistent after op %d off=%llu len=%zu\n",
				i, op, (unsigned long long)off, len);
			return 1;
		}
	}
	fsc_dev_close(dev);

	fd = open(path, O_RDONLY);
	uint8_t *disk = malloc((size_t)size);
	if (pread(fd, disk, (size_t)size, 0) != size) {
		perror("pread");
		return 2;
	}
	close(fd);
	if (memcmp(disk, shadow, (size_t)size)) {
		size_t k = 0;
		while (disk[k] == shadow[k])
			k++;
		fprintf(stderr, "final image differs at %zu\n", k);
		return 1;
	}
	printf("blockdev: %ld iterations ok (sector %u, cache %zu)%s\n", iters,
	       o.sector_size, o.cache_bytes, bad ? " WITH READ ERRORS" : "");
	return bad ? 1 : 0;
}
