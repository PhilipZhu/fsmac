/*
 * FSMac - block device access with a write-back page cache.
 *
 * Design notes
 * ------------
 * The cache works in pages of max(4 KiB, sector size). Small requests (the
 * typical metadata access pattern of libntfs-3g and libext2fs) are served
 * from the cache; misses read a whole aligned read-ahead window in one I/O.
 * Writes below the direct threshold land in the cache as dirty pages and are
 * written back sorted and coalesced, either when the dirty limit is reached
 * or on an explicit flush. Requests at or above the direct threshold (file
 * data) bypass the cache: aligned middle parts go straight to the device and
 * any cached copies are kept coherent.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fsmac/blockdev.h>

#define DEFAULT_CACHE_BYTES   (64u << 20)
#define DEFAULT_DIRTY_BYTES   (16u << 20)
#define DIRECT_THRESHOLD      (128u << 10) /* bypass cache at this size */
#define READAHEAD_BYTES       (64u << 10)  /* cache miss read window */
#define MAX_COALESCE_BYTES    (1u << 20)   /* largest write-back I/O */

typedef struct page {
	uint64_t idx;
	struct page *hnext;
	struct page *lprev, *lnext;
	bool dirty;
	uint8_t *data;
} page;

struct fsc_dev {
	int fd;
	bool readonly;
	bool is_device;
	bool failed;
	char name[256];
	uint64_t size;
	uint32_t sector;      /* device I/O alignment */
	uint32_t psize;       /* cache page size */
	uint32_t pshift;

	page **hash;
	size_t hmask;
	page *lru_head, *lru_tail; /* head = most recently used */
	size_t npages, max_pages;
	size_t ndirty, max_dirty;
	page *freelist;

	/* Separate bounce buffers: write-back can run in the middle of a
	 * read-ahead (when a page must be evicted), so they must not share. */
	uint8_t *rbounce, *wbounce;
	size_t rbounce_size, wbounce_size;

	fsc_dev_stats st;
};

/* ---------------------------------------------------------------------- */
/* Low-level device I/O                                                    */

static bool fatal_errno(int e)
{
	return e == ENXIO || e == ENODEV || e == EIO || e == EBADF;
}

static int raw_pread(fsc_dev *dev, void *buf, size_t len, uint64_t off)
{
	uint8_t *p = buf;

	if (dev->failed)
		return -EIO;
	while (len > 0) {
		ssize_t n = pread(dev->fd, p, len, (off_t)off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			if (fatal_errno(e))
				dev->failed = (e != EIO) ? true : dev->failed;
			return -e;
		}
		if (n == 0)
			return -EIO; /* unexpected end of device */
		p += n;
		len -= (size_t)n;
		off += (uint64_t)n;
		dev->st.bytes_read += (uint64_t)n;
	}
	return 0;
}

static int raw_pwrite(fsc_dev *dev, const void *buf, size_t len, uint64_t off)
{
	const uint8_t *p = buf;

	if (dev->failed)
		return -EIO;
	if (dev->readonly)
		return -EROFS;
	while (len > 0) {
		ssize_t n = pwrite(dev->fd, p, len, (off_t)off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			if (fatal_errno(e) && e != EIO)
				dev->failed = true;
			return -e;
		}
		if (n == 0)
			return -EIO;
		p += n;
		len -= (size_t)n;
		off += (uint64_t)n;
		dev->st.bytes_written += (uint64_t)n;
	}
	return 0;
}

static int ensure_buf(uint8_t **buf, size_t *size, size_t len)
{
	void *nb;

	if (*size >= len)
		return 0;
	if (posix_memalign(&nb, 4096, len))
		return -ENOMEM;
	free(*buf);
	*buf = nb;
	*size = len;
	return 0;
}
#define ensure_rbounce(dev, len) ensure_buf(&(dev)->rbounce, &(dev)->rbounce_size, (len))
#define ensure_wbounce(dev, len) ensure_buf(&(dev)->wbounce, &(dev)->wbounce_size, (len))

static int device_barrier(fsc_dev *dev)
{
	if (dev->readonly || dev->failed)
		return dev->failed ? -EIO : 0;
	if (dev->is_device) {
		dk_synchronize_t sync;

		memset(&sync, 0, sizeof(sync));
		if (ioctl(dev->fd, DKIOCSYNCHRONIZE, &sync) == 0)
			return 0;
		if (fsync(dev->fd) == 0)
			return 0;
		return -errno;
	}
	if (fcntl(dev->fd, F_FULLFSYNC) == 0)
		return 0;
	if (fsync(dev->fd) == 0)
		return 0;
	return -errno;
}

/* ---------------------------------------------------------------------- */
/* Page cache bookkeeping                                                  */

static inline size_t hslot(const fsc_dev *dev, uint64_t idx)
{
	uint64_t h = idx * 0x9E3779B97F4A7C15ull;
	return (size_t)(h >> 20) & dev->hmask;
}

static page *lookup(fsc_dev *dev, uint64_t idx)
{
	page *p;

	for (p = dev->hash[hslot(dev, idx)]; p; p = p->hnext)
		if (p->idx == idx)
			return p;
	return NULL;
}

static void lru_unlink(fsc_dev *dev, page *p)
{
	if (p->lprev)
		p->lprev->lnext = p->lnext;
	else
		dev->lru_head = p->lnext;
	if (p->lnext)
		p->lnext->lprev = p->lprev;
	else
		dev->lru_tail = p->lprev;
	p->lprev = p->lnext = NULL;
}

static void lru_push_head(fsc_dev *dev, page *p)
{
	p->lprev = NULL;
	p->lnext = dev->lru_head;
	if (dev->lru_head)
		dev->lru_head->lprev = p;
	dev->lru_head = p;
	if (!dev->lru_tail)
		dev->lru_tail = p;
}

static void touch(fsc_dev *dev, page *p)
{
	if (dev->lru_head != p) {
		lru_unlink(dev, p);
		lru_push_head(dev, p);
	}
}

static void hash_remove(fsc_dev *dev, page *p)
{
	page **pp = &dev->hash[hslot(dev, p->idx)];

	while (*pp && *pp != p)
		pp = &(*pp)->hnext;
	if (*pp)
		*pp = p->hnext;
	p->hnext = NULL;
}

#ifdef FSC_BLOCKDEV_DEBUG
static long watch_idx = -1;
#define WATCH(p, what) do { if ((long)(p)->idx == watch_idx) fprintf(stderr, "  watch %ld: %s (dirty=%d)\n", watch_idx, what, (p)->dirty); } while (0)
#else
#define WATCH(p, what) do {} while (0)
#endif

static void mark_dirty(fsc_dev *dev, page *p)
{
	WATCH(p, "mark_dirty");
	if (!p->dirty) {
		p->dirty = true;
		dev->ndirty++;
	}
}

static void mark_clean(fsc_dev *dev, page *p)
{
	WATCH(p, "mark_clean");
	if (p->dirty) {
		p->dirty = false;
		dev->ndirty--;
	}
}

static void release_page(fsc_dev *dev, page *p)
{
	WATCH(p, "release");
	mark_clean(dev, p);
	hash_remove(dev, p);
	lru_unlink(dev, p);
	dev->npages--;
	p->hnext = dev->freelist;
	dev->freelist = p;
}

static int page_cmp(const void *a, const void *b)
{
	const page *pa = *(page * const *)a, *pb = *(page * const *)b;

	return pa->idx < pb->idx ? -1 : pa->idx > pb->idx ? 1 : 0;
}

/* Bytes of page idx that lie within the device. */
static size_t page_valid_len(const fsc_dev *dev, uint64_t idx)
{
	uint64_t start = idx << dev->pshift;

	if (start >= dev->size)
		return 0;
	if (dev->size - start < dev->psize)
		return (size_t)(dev->size - start);
	return dev->psize;
}

/* Writes back all dirty pages, sorted and coalesced. */
static int writeback(fsc_dev *dev)
{
	page **v, *p;
	size_t n = 0, i, j;
	int err = 0;

	if (dev->ndirty == 0)
		return 0;
	v = malloc(dev->ndirty * sizeof(*v));
	if (!v)
		return -ENOMEM;
	for (p = dev->lru_head; p; p = p->lnext)
		if (p->dirty)
			v[n++] = p;
	qsort(v, n, sizeof(*v), page_cmp);

	for (i = 0; i < n; i = j) {
		size_t run = 1, bytes, off;
		uint64_t start = v[i]->idx << dev->pshift;

		while (i + run < n && v[i + run]->idx == v[i]->idx + run &&
		       (run + 1) * dev->psize <= MAX_COALESCE_BYTES)
			run++;
		j = i + run;
		bytes = 0;
		if (run == 1) {
			bytes = page_valid_len(dev, v[i]->idx);
			err = raw_pwrite(dev, v[i]->data, bytes, start);
		} else {
			err = ensure_wbounce(dev, run * dev->psize);
			if (err)
				break;
			for (off = 0; off < run; off++) {
				size_t vl = page_valid_len(dev, v[i + off]->idx);
				memcpy(dev->wbounce + bytes, v[i + off]->data, vl);
				bytes += vl;
			}
			err = raw_pwrite(dev, dev->wbounce, bytes, start);
		}
		if (err)
			break;
		for (off = 0; off < run; off++)
			mark_clean(dev, v[i + off]);
	}
	free(v);
	return err;
}

/* Makes room for at least `need` more pages. */
static int make_room(fsc_dev *dev, size_t need)
{
	while (dev->npages + need > dev->max_pages && dev->npages) {
		page *victim = dev->lru_tail;

		/* Prefer evicting clean pages near the tail. */
		int scanned = 0;
		while (victim && victim->dirty && scanned < 64) {
			victim = victim->lprev;
			scanned++;
		}
		if (!victim || victim->dirty) {
			int err = writeback(dev);
			if (err)
				return err;
			victim = dev->lru_tail;
		}
		if (!victim)
			break;
		release_page(dev, victim);
	}
	return 0;
}

/*
 * Allocates a page for idx. With evict=false the caller must already have
 * made room (so that pages allocated earlier in the same operation cannot
 * be chosen as eviction victims and recycled under the caller's feet).
 */
static page *alloc_page(fsc_dev *dev, uint64_t idx, bool evict, int *err)
{
	page *p;

	*err = evict ? make_room(dev, 1) : 0;
	if (*err)
		return NULL;
	p = dev->freelist;
	if (p) {
		dev->freelist = p->hnext;
	} else {
		void *mem;

		p = calloc(1, sizeof(*p));
		if (!p || posix_memalign(&mem, 4096, dev->psize)) {
			free(p);
			*err = -ENOMEM;
			return NULL;
		}
		p->data = mem;
	}
	p->idx = idx;
	p->dirty = false;
	WATCH(p, "alloc");
	p->hnext = dev->hash[hslot(dev, idx)];
	dev->hash[hslot(dev, idx)] = p;
	lru_push_head(dev, p);
	dev->npages++;
	return p;
}

/*
 * Returns the cached page idx, reading it (plus neighbouring uncached pages
 * inside the read-ahead window) from the device on a miss.
 */
static page *get_page(fsc_dev *dev, uint64_t idx, bool need_data, int *err)
{
	page *p = lookup(dev, idx);
	uint64_t last_idx, ra, wstart, wend, lo, hi, i;
	size_t bytes;

	*err = 0;
	if (p) {
		dev->st.cache_hits++;
		touch(dev, p);
		return p;
	}
	dev->st.cache_misses++;
	if (!need_data) {
		p = alloc_page(dev, idx, true, err);
		if (p)
			memset(p->data, 0, dev->psize);
		return p;
	}
	if ((idx << dev->pshift) >= dev->size) {
		*err = -EIO;
		return NULL;
	}

	last_idx = (dev->size - 1) >> dev->pshift;
	ra = READAHEAD_BYTES >> dev->pshift;
	if (ra == 0)
		ra = 1;
	wstart = idx - (idx % ra);
	wend = wstart + ra - 1;
	if (wend > last_idx)
		wend = last_idx;
	/* Extend over the contiguous run of uncached pages around idx. */
	lo = idx;
	while (lo > wstart && !lookup(dev, lo - 1))
		lo--;
	hi = idx;
	while (hi < wend && !lookup(dev, hi + 1))
		hi++;
	if (hi - lo + 1 > dev->max_pages / 4) /* tiny caches: no read-ahead */
		lo = hi = idx;

	/* Reserve room for the whole window first: evicting while we allocate
	 * could recycle a page allocated earlier in this loop. */
	*err = make_room(dev, (size_t)(hi - lo + 1));
	if (*err)
		return NULL;
	bytes = (size_t)((hi - lo) << dev->pshift) + page_valid_len(dev, hi);
	*err = ensure_rbounce(dev, (size_t)(hi - lo + 1) << dev->pshift);
	if (*err)
		return NULL;
	*err = raw_pread(dev, dev->rbounce, bytes, lo << dev->pshift);
	if (*err)
		return NULL;

	for (i = lo; i <= hi; i++) {
		page *np = alloc_page(dev, i, false, err);
		size_t vl;

		if (!np)
			return NULL;
		vl = page_valid_len(dev, i);
		memcpy(np->data, dev->rbounce + ((i - lo) << dev->pshift), vl);
		if (vl < dev->psize)
			memset(np->data + vl, 0, dev->psize - vl);
		if (i == idx)
			p = np;
	}
	/* alloc_page() pushed pages at the head in order; make idx MRU. */
	touch(dev, p);
	return p;
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */

fsc_dev *fsc_dev_open_fd(int fd, bool readonly, const fsc_dev_opts *opts,
			 int *err)
{
	struct stat sb;
	fsc_dev *dev;
	size_t cache_bytes, dirty_bytes, buckets;

	*err = 0;
	if (fstat(fd, &sb) < 0) {
		*err = -errno;
		return NULL;
	}
	dev = calloc(1, sizeof(*dev));
	if (!dev) {
		*err = -ENOMEM;
		return NULL;
	}
	dev->fd = fd;
	dev->readonly = readonly;
	snprintf(dev->name, sizeof(dev->name), "fd%d", fd);

	if (S_ISCHR(sb.st_mode) || S_ISBLK(sb.st_mode)) {
		uint32_t bs = 0;
		uint64_t bc = 0;

		dev->is_device = true;
		if (ioctl(fd, DKIOCGETBLOCKSIZE, &bs) < 0 ||
		    ioctl(fd, DKIOCGETBLOCKCOUNT, &bc) < 0) {
			*err = -errno;
			free(dev);
			return NULL;
		}
		dev->sector = bs;
		dev->size = (uint64_t)bs * bc;
	} else if (S_ISREG(sb.st_mode)) {
		dev->sector = 512;
		dev->size = (uint64_t)sb.st_size;
	} else {
		*err = -ENODEV;
		free(dev);
		return NULL;
	}
	if (opts && opts->sector_size)
		dev->sector = opts->sector_size;
	if (dev->sector < 512 || (dev->sector & (dev->sector - 1))) {
		*err = -EINVAL;
		free(dev);
		return NULL;
	}

	dev->psize = dev->sector > 4096 ? dev->sector : 4096;
	dev->pshift = (uint32_t)__builtin_ctz(dev->psize);

	cache_bytes = (opts && opts->cache_bytes) ? opts->cache_bytes
						  : DEFAULT_CACHE_BYTES;
	dirty_bytes = (opts && opts->dirty_limit_bytes) ? opts->dirty_limit_bytes
							: DEFAULT_DIRTY_BYTES;
	dev->max_pages = cache_bytes / dev->psize;
	if (dev->max_pages < 16)
		dev->max_pages = 16;
	dev->max_dirty = dirty_bytes / dev->psize;
	if (dev->max_dirty < 4)
		dev->max_dirty = 4;
	if (dev->max_dirty > dev->max_pages / 2)
		dev->max_dirty = dev->max_pages / 2;

	buckets = 1;
	while (buckets < dev->max_pages * 2)
		buckets <<= 1;
	dev->hash = calloc(buckets, sizeof(page *));
	if (!dev->hash) {
		*err = -ENOMEM;
		free(dev);
		return NULL;
	}
	dev->hmask = buckets - 1;
	return dev;
}

fsc_dev *fsc_dev_open_path(const char *path, bool readonly,
			   const fsc_dev_opts *opts, int *err)
{
	int fd = open(path, readonly ? O_RDONLY : O_RDWR);
	fsc_dev *dev;

	if (fd < 0) {
		*err = -errno;
		return NULL;
	}
	dev = fsc_dev_open_fd(fd, readonly, opts, err);
	if (!dev) {
		close(fd);
		return NULL;
	}
	snprintf(dev->name, sizeof(dev->name), "%s", path);
	return dev;
}

void fsc_dev_close(fsc_dev *dev)
{
	page *p, *n;

	if (!dev)
		return;
	if (!dev->readonly && !dev->failed) {
		writeback(dev);
		device_barrier(dev);
	}
	for (p = dev->lru_head; p; p = n) {
		n = p->lnext;
		free(p->data);
		free(p);
	}
	for (p = dev->freelist; p; p = n) {
		n = p->hnext;
		free(p->data);
		free(p);
	}
	free(dev->hash);
	free(dev->rbounce);
	free(dev->wbounce);
	close(dev->fd);
	free(dev);
}

uint64_t fsc_dev_size(const fsc_dev *dev) { return dev->size; }
uint32_t fsc_dev_sector_size(const fsc_dev *dev) { return dev->sector; }
bool fsc_dev_is_readonly(const fsc_dev *dev) { return dev->readonly; }
bool fsc_dev_failed(const fsc_dev *dev) { return dev->failed; }
const char *fsc_dev_name(const fsc_dev *dev) { return dev->name; }

static bool range_ok(const fsc_dev *dev, size_t len, uint64_t off)
{
	return off <= dev->size && len <= dev->size - off;
}

/* Copies dirty cached pages over a buffer read directly from the device. */
static void overlay_dirty(fsc_dev *dev, uint8_t *buf, size_t len, uint64_t off)
{
	uint64_t first, last, idx;

	if (dev->ndirty == 0)
		return;
	first = off >> dev->pshift;
	last = (off + len - 1) >> dev->pshift;
	for (idx = first; idx <= last; idx++) {
		page *p = lookup(dev, idx);
		uint64_t pstart, s, e;

		if (!p || !p->dirty)
			continue;
		pstart = idx << dev->pshift;
		s = pstart > off ? pstart : off;
		e = pstart + dev->psize;
		if (e > off + len)
			e = off + len;
		memcpy(buf + (s - off), p->data + (s - pstart), (size_t)(e - s));
	}
}

static int direct_read(fsc_dev *dev, uint8_t *buf, size_t len, uint64_t off)
{
	uint64_t mask = dev->sector - 1;
	uint64_t a0 = off & ~mask;
	uint64_t a1 = (off + len + mask) & ~mask;
	int err;

	if (a1 > dev->size)
		a1 = dev->size;
	if (a0 == off && a1 == off + len) {
		err = raw_pread(dev, buf, len, off);
	} else {
		err = ensure_rbounce(dev, (size_t)(a1 - a0));
		if (!err)
			err = raw_pread(dev, dev->rbounce, (size_t)(a1 - a0), a0);
		if (!err)
			memcpy(buf, dev->rbounce + (off - a0), len);
	}
	if (!err)
		overlay_dirty(dev, buf, len, off);
	return err;
}

int fsc_dev_pread(fsc_dev *dev, void *vbuf, size_t len, uint64_t off)
{
	uint8_t *buf = vbuf;
	int err = 0;

	if (len == 0)
		return 0;
	if (!range_ok(dev, len, off))
		return -EIO;
	if (len >= DIRECT_THRESHOLD)
		return direct_read(dev, buf, len, off);

	while (len > 0) {
		uint64_t idx = off >> dev->pshift;
		size_t po = (size_t)(off & (dev->psize - 1));
		size_t n = dev->psize - po;
		page *p;

		if (n > len)
			n = len;
		p = get_page(dev, idx, true, &err);
		if (!p)
			return err;
		memcpy(buf, p->data + po, n);
		buf += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int cached_write(fsc_dev *dev, const uint8_t *buf, size_t len,
			uint64_t off)
{
	int err = 0;

	while (len > 0) {
		uint64_t idx = off >> dev->pshift;
		size_t po = (size_t)(off & (dev->psize - 1));
		size_t n = dev->psize - po;
		bool full;
		page *p;

		if (n > len)
			n = len;
		/* A page is fully overwritten if we cover all its valid bytes. */
		full = (po == 0) && (n >= page_valid_len(dev, idx));
		p = get_page(dev, idx, !full, &err);
		if (!p)
			return err;
		memcpy(p->data + po, buf, n);
		mark_dirty(dev, p);
		buf += n;
		off += n;
		len -= n;
	}
	if (dev->ndirty > dev->max_dirty)
		err = writeback(dev);
	return err;
}

int fsc_dev_pwrite(fsc_dev *dev, const void *vbuf, size_t len, uint64_t off)
{
	const uint8_t *buf = vbuf;
	uint64_t pmask = dev->psize - 1;
	uint64_t m0, m1, idx;
	int err;

	if (len == 0)
		return 0;
	if (dev->readonly)
		return -EROFS;
	if (dev->failed)
		return -EIO;
	if (!range_ok(dev, len, off))
		return -EIO;
	if (len < DIRECT_THRESHOLD)
		return cached_write(dev, buf, len, off);

	/* Page-aligned middle goes straight to the device. */
	m0 = (off + pmask) & ~pmask;
	m1 = (off + len) & ~pmask;
	if (off + len == dev->size)
		m1 = dev->size; /* device end needs no page alignment */
	if (m1 <= m0)
		return cached_write(dev, buf, len, off);

	if (m0 > off) {
		err = cached_write(dev, buf, (size_t)(m0 - off), off);
		if (err)
			return err;
	}
	/* Keep cached copies of the middle range coherent. */
	for (idx = m0 >> dev->pshift; (idx << dev->pshift) < m1; idx++) {
		page *p = lookup(dev, idx);
		if (p) {
			size_t vl = page_valid_len(dev, idx);
			memcpy(p->data, buf + ((idx << dev->pshift) - off), vl);
			mark_clean(dev, p);
		}
	}
	err = raw_pwrite(dev, buf + (m0 - off), (size_t)(m1 - m0), m0);
	if (err)
		return err;
	if (off + len > m1)
		err = cached_write(dev, buf + (m1 - off), (size_t)(off + len - m1),
				   m1);
	return err;
}

int fsc_dev_flush(fsc_dev *dev, bool barrier)
{
	int err;

	if (dev->readonly)
		return 0;
	err = writeback(dev);
	if (!err && barrier)
		err = device_barrier(dev);
	return err;
}

void fsc_dev_invalidate(fsc_dev *dev)
{
	page *p, *n;

	for (p = dev->lru_head; p; p = n) {
		n = p->lnext;
		if (!p->dirty)
			release_page(dev, p);
	}
}

int fsc_dev_discard(fsc_dev *dev, uint64_t off, uint64_t len)
{
	dk_extent_t ext;
	dk_unmap_t um;
	uint64_t mask = dev->sector - 1;
	uint64_t a0 = (off + mask) & ~mask, a1 = (off + len) & ~mask;

	if (!dev->is_device || dev->readonly || a1 <= a0)
		return 0;
	memset(&um, 0, sizeof(um));
	ext.offset = a0;
	ext.length = a1 - a0;
	um.extents = &ext;
	um.extentsCount = 1;
	(void)ioctl(dev->fd, DKIOCUNMAP, &um); /* advisory */
	return 0;
}

void fsc_dev_get_stats(const fsc_dev *dev, fsc_dev_stats *st)
{
	*st = dev->st;
	st->dirty_pages = dev->ndirty;
	st->cached_pages = dev->npages;
}

#ifdef FSC_BLOCKDEV_DEBUG
/* Test hook: every cached page must match the logical contents (shadow)
 * and every clean page must also match the device. Returns 0 if OK. */
void fsc_dev_debug_watch(long idx) { watch_idx = idx; }
int fsc_dev_debug_check(fsc_dev *dev, const uint8_t *shadow)
{
	uint8_t *tmp = malloc(dev->psize);
	int bad = 0;

	for (page *p = dev->lru_head; p; p = p->lnext) {
		size_t vl = page_valid_len(dev, p->idx);
		uint64_t off = p->idx << dev->pshift;

		if (memcmp(p->data, shadow + off, vl)) {
			fprintf(stderr, "page %llu (%s) differs from shadow\n",
				(unsigned long long)p->idx,
				p->dirty ? "dirty" : "clean");
			bad = 1;
		}
		if (!p->dirty && pread(dev->fd, tmp, vl, (off_t)off) == (ssize_t)vl &&
		    memcmp(tmp, p->data, vl)) {
			fprintf(stderr, "clean page %llu differs from device\n",
				(unsigned long long)p->idx);
			bad = 1;
		}
	}
	free(tmp);
	return bad;
}
#endif
