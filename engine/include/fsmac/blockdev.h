/*
 * FSMac - block device access with a write-back page cache.
 *
 * Filesystem engines never touch the raw device directly; they go through
 * this layer, which:
 *   - accepts arbitrary (unaligned) offsets and lengths and converts them to
 *     sector-aligned I/O, which raw disk devices (/dev/rdiskN) require;
 *   - caches small (metadata-sized) I/O in a page cache with read-ahead;
 *   - absorbs repeated small writes (write-back) and coalesces them into
 *     large sequential writes on flush;
 *   - passes large transfers (file data) straight through to the device.
 *
 * The layer is not thread safe; the engine serialises all access.
 */
#ifndef FSMAC_BLOCKDEV_H
#define FSMAC_BLOCKDEV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fsc_dev fsc_dev;

typedef struct fsc_dev_stats {
	uint64_t bytes_read;      /* bytes transferred from the device */
	uint64_t bytes_written;   /* bytes transferred to the device */
	uint64_t cache_hits;      /* page lookups satisfied from cache */
	uint64_t cache_misses;    /* page lookups that required device I/O */
	uint64_t dirty_pages;     /* pages currently awaiting write-back */
	uint64_t cached_pages;
} fsc_dev_stats;

typedef struct fsc_dev_opts {
	size_t cache_bytes;       /* page cache capacity (0 = default 64 MiB) */
	size_t dirty_limit_bytes; /* flush when this much is dirty (0 = default) */
	uint32_t sector_size;     /* override detected sector size (testing) */
} fsc_dev_opts;

/*
 * Wraps an already-open file descriptor (a disk device or an image file).
 * The descriptor is owned by the returned object and closed by
 * fsc_dev_close(). Returns NULL and sets *err (negative errno) on failure.
 */
fsc_dev *fsc_dev_open_fd(int fd, bool readonly, const fsc_dev_opts *opts,
			 int *err);
/* Opens a path (device node or image file). */
fsc_dev *fsc_dev_open_path(const char *path, bool readonly,
			   const fsc_dev_opts *opts, int *err);
/* Writes back dirty pages (best effort) and releases everything. */
void fsc_dev_close(fsc_dev *dev);

uint64_t fsc_dev_size(const fsc_dev *dev);
uint32_t fsc_dev_sector_size(const fsc_dev *dev);
bool fsc_dev_is_readonly(const fsc_dev *dev);
/* True once an unrecoverable I/O error occurred (e.g. the disk vanished). */
bool fsc_dev_failed(const fsc_dev *dev);
const char *fsc_dev_name(const fsc_dev *dev);

/* Reads/writes exactly len bytes at off. Return 0 or a negative errno. */
int fsc_dev_pread(fsc_dev *dev, void *buf, size_t len, uint64_t off);
int fsc_dev_pwrite(fsc_dev *dev, const void *buf, size_t len, uint64_t off);

/*
 * Writes back all dirty pages. If barrier is true, additionally asks the
 * device to commit its volatile write cache to stable storage.
 */
int fsc_dev_flush(fsc_dev *dev, bool barrier);

/* Drops all clean cached pages (dirty pages are kept). */
void fsc_dev_invalidate(fsc_dev *dev);

/* Hints that a range is no longer in use (TRIM/UNMAP); may be a no-op. */
int fsc_dev_discard(fsc_dev *dev, uint64_t off, uint64_t len);

void fsc_dev_get_stats(const fsc_dev *dev, fsc_dev_stats *st);

#ifdef __cplusplus
}
#endif
#endif /* FSMAC_BLOCKDEV_H */
