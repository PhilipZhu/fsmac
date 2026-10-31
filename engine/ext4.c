/*
 * FSMac ext2/ext3/ext4 backend, built on libext2fs (e2fsprogs).
 *
 * Operation logic follows e2fsprogs' fuse2fs (misc/fuse2fs.c), converted
 * from path-based FUSE calls to the inode-based engine API, plus:
 *   - a custom libext2fs I/O manager on top of the FSMac block device cache;
 *   - write elision for bitmap and group descriptor blocks whose content did
 *     not change (libext2fs rewrites all of them on every flush);
 *   - resumable, position-based directory enumeration (NFS cookies);
 *   - atomic replacement in rename (the target entry is repointed in place,
 *     the way the kernel does it, instead of unlink + link);
 *   - deferred superblock/bitmap flushing (see ext4_sync_op).
 */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ext2fs/ext2_fs.h"
#include "ext2fs/ext2fs.h"

#include "fsc_internal.h"

errcode_t ext2fs_run_ext3_journal(ext2_filsys *fs);
errcode_t ext2fs_check_ext3_journal(ext2_filsys fs);

/* ---------------------------------------------------------------------- */
/* Timestamp helpers (from fuse2fs)                                        */

#define EXT4_EPOCH_BITS 2
#define EXT4_EPOCH_MASK ((1 << EXT4_EPOCH_BITS) - 1)
#define EXT4_NSEC_MASK (~0UL << EXT4_EPOCH_BITS)

#define EXT4_FITS_IN_INODE(ext4_inode, field)                                 \
	((offsetof(typeof(*ext4_inode), field) + sizeof((ext4_inode)->field)) <= \
	 ((size_t)EXT2_GOOD_OLD_INODE_SIZE + (ext4_inode)->i_extra_isize))

static inline __u32 ext4_encode_extra_time(const struct timespec *time)
{
	__u32 extra = sizeof(time->tv_sec) > 4
			      ? ((time->tv_sec - (__s32)time->tv_sec) >> 32) &
					EXT4_EPOCH_MASK
			      : 0;
	return extra | (__u32)(time->tv_nsec << EXT4_EPOCH_BITS);
}

static inline void ext4_decode_extra_time(struct timespec *time, __u32 extra)
{
	if (sizeof(time->tv_sec) > 4 && (extra & EXT4_EPOCH_MASK)) {
		__u64 extra_bits = extra & EXT4_EPOCH_MASK;
		time->tv_sec += extra_bits << 32;
	}
	time->tv_nsec = ((extra)&EXT4_NSEC_MASK) >> EXT4_EPOCH_BITS;
}

#define EXT4_CLAMP_TIMESTAMP(xtime, tsp, raw_inode)                            \
	do {                                                                    \
		if ((tsp)->tv_sec < EXT4_TIMESTAMP_MIN)                         \
			(tsp)->tv_sec = EXT4_TIMESTAMP_MIN;                     \
		if (EXT4_FITS_IN_INODE(raw_inode, xtime##_extra)) {             \
			if ((tsp)->tv_sec > EXT4_EXTRA_TIMESTAMP_MAX)           \
				(tsp)->tv_sec = EXT4_EXTRA_TIMESTAMP_MAX;       \
		} else {                                                        \
			if ((tsp)->tv_sec > EXT4_NON_EXTRA_TIMESTAMP_MAX)       \
				(tsp)->tv_sec = EXT4_NON_EXTRA_TIMESTAMP_MAX;   \
		}                                                               \
	} while (0)

#define EXT4_INODE_SET_XTIME(xtime, tsp, raw_inode)                            \
	do {                                                                    \
		struct timespec _ts = *(tsp);                                   \
		EXT4_CLAMP_TIMESTAMP(xtime, &_ts, raw_inode);                   \
		(raw_inode)->xtime = (__u32)_ts.tv_sec;                         \
		if (EXT4_FITS_IN_INODE(raw_inode, xtime##_extra))               \
			(raw_inode)->xtime##_extra = ext4_encode_extra_time(&_ts); \
	} while (0)

#define EXT4_EINODE_SET_XTIME(xtime, tsp, raw_inode)                           \
	do {                                                                    \
		struct timespec _ts = *(tsp);                                   \
		EXT4_CLAMP_TIMESTAMP(xtime, &_ts, raw_inode);                   \
		if (EXT4_FITS_IN_INODE(raw_inode, xtime))                       \
			(raw_inode)->xtime = (__u32)_ts.tv_sec;                 \
		if (EXT4_FITS_IN_INODE(raw_inode, xtime##_extra))               \
			(raw_inode)->xtime##_extra = ext4_encode_extra_time(&_ts); \
	} while (0)

#define EXT4_INODE_GET_XTIME(xtime, tsp, raw_inode)                            \
	do {                                                                    \
		(tsp)->tv_sec = (signed)((raw_inode)->xtime);                   \
		if (EXT4_FITS_IN_INODE(raw_inode, xtime##_extra))               \
			ext4_decode_extra_time((tsp), (raw_inode)->xtime##_extra); \
		else                                                            \
			(tsp)->tv_nsec = 0;                                     \
	} while (0)

#define EXT4_EINODE_GET_XTIME(xtime, tsp, raw_inode)                           \
	do {                                                                    \
		if (EXT4_FITS_IN_INODE(raw_inode, xtime))                       \
			(tsp)->tv_sec = (signed)((raw_inode)->xtime);           \
		if (EXT4_FITS_IN_INODE(raw_inode, xtime##_extra))               \
			ext4_decode_extra_time((tsp), raw_inode->xtime##_extra); \
		else                                                            \
			(tsp)->tv_nsec = 0;                                     \
	} while (0)

/* ---------------------------------------------------------------------- */
/* Volume state                                                            */

typedef struct dedup_ent {
	blk64_t blk;
	uint64_t hash;
	bool valid;
} dedup_ent;

typedef struct ext4_vol {
	fsc_volume base;
	ext2_filsys fs;
	uint32_t next_generation;
	bool dirty;          /* modified since the last full sync */
	char devname[64];
	char notice[512];
	/* Blocks libext2fs rewrites unconditionally (bitmaps, descriptors),
	 * with the hash of their last known on-disk contents. Open hashing. */
	dedup_ent *dedup;
	size_t dedup_size;   /* power of two */
} ext4_vol;

static inline ext4_vol *EV(fsc_volume *v) { return (ext4_vol *)v; }

/* The I/O manager is opened by name; the name carries the device. */
static ext4_vol *io_owner; /* volume being opened (single threaded) */

/* ---------------------------------------------------------------------- */
/* Error translation (from fuse2fs)                                        */

static int xlate(ext2_filsys fs, ext2_ino_t ino, errcode_t err)
{
	(void)fs;
	switch (err) {
	case 0:
		return 0;
	case EXT2_ET_NO_MEMORY:
	case EXT2_ET_TDB_ERR_OOM:
		return -ENOMEM;
	case EXT2_ET_INVALID_ARGUMENT:
	case EXT2_ET_LLSEEK_FAILED:
		return -EINVAL;
	case EXT2_ET_NO_DIRECTORY:
		return -ENOTDIR;
	case EXT2_ET_FILE_NOT_FOUND:
		return -ENOENT;
	case EXT2_ET_DIR_NO_SPACE:
	case EXT2_ET_TOOSMALL:
	case EXT2_ET_BLOCK_ALLOC_FAIL:
	case EXT2_ET_INODE_ALLOC_FAIL:
	case EXT2_ET_EA_NO_SPACE:
		return -ENOSPC;
	case EXT2_ET_SYMLINK_LOOP:
		return -EMLINK;
	case EXT2_ET_FILE_TOO_BIG:
		return -EFBIG;
	case EXT2_ET_TDB_ERR_EXISTS:
	case EXT2_ET_FILE_EXISTS:
		return -EEXIST;
	case EXT2_ET_MMP_FAILED:
	case EXT2_ET_MMP_FSCK_ON:
		return -EBUSY;
	case EXT2_ET_EA_KEY_NOT_FOUND:
		return -ENOATTR;
	case EXT2_ET_UNIMPLEMENTED:
	case EXT2_ET_OP_NOT_SUPPORTED:
		return -ENOTSUP;
	case EXT2_ET_RO_FILSYS:
	case EXT2_ET_FILE_RO:
		return -EROFS;
	case EXT2_ET_SHORT_READ:
	case EXT2_ET_SHORT_WRITE:
		return -EIO;
	case EXT2_ET_DIR_CORRUPTED:
	case EXT2_ET_CORRUPT_SUPERBLOCK:
	case EXT2_ET_FILESYSTEM_CORRUPTED:
	case EXT2_ET_CORRUPT_JOURNAL_SB:
	case EXT2_ET_INODE_CORRUPTED:
	case EXT2_ET_EA_INODE_CORRUPTED:
	case EXT2_ET_DIR_CSUM_INVALID:
	case EXT2_ET_INODE_CSUM_INVALID:
	case EXT2_ET_EXTENT_CSUM_INVALID:
	case EXT2_ET_EXT_ATTR_CSUM_INVALID:
		fsc_error("ext4: filesystem corruption (inode %u): %s", ino,
			  error_message(err));
		return -EIO;
	default:
		if (err > 0 && err < 256)
			return -(int)err;
		fsc_error("ext4: error (inode %u): %s", ino, error_message(err));
		return -EIO;
	}
}

/* ---------------------------------------------------------------------- */
/* Write elision for unconditionally rewritten metadata blocks            */

static uint64_t hash_block(const void *data, size_t len)
{
	const uint8_t *p = data;
	uint64_t h = 0x9E3779B97F4A7C15ull ^ len;
	size_t i;

	for (i = 0; i + 8 <= len; i += 8) {
		uint64_t w;
		memcpy(&w, p + i, 8);
		h ^= w;
		h *= 0xBF58476D1CE4E5B9ull;
		h ^= h >> 31;
	}
	for (; i < len; i++) {
		h ^= p[i];
		h *= 0x94D049BB133111EBull;
	}
	h ^= h >> 29;
	h *= 0x94D049BB133111EBull;
	h ^= h >> 32;
	return h;
}

static dedup_ent *dedup_find(ext4_vol *ev, blk64_t blk)
{
	size_t mask, i;

	if (!ev || !ev->dedup)
		return NULL;
	mask = ev->dedup_size - 1;
	i = (size_t)(blk * 0x9E3779B97F4A7C15ull >> 17) & mask;
	while (ev->dedup[i].blk) {
		if (ev->dedup[i].blk == blk)
			return &ev->dedup[i];
		i = (i + 1) & mask;
	}
	return NULL;
}

static void dedup_add(ext4_vol *ev, blk64_t blk)
{
	size_t mask = ev->dedup_size - 1;
	size_t i = (size_t)(blk * 0x9E3779B97F4A7C15ull >> 17) & mask;

	if (!blk)
		return;
	while (ev->dedup[i].blk) {
		if (ev->dedup[i].blk == blk)
			return;
		i = (i + 1) & mask;
	}
	ev->dedup[i].blk = blk;
}

/* Registers block/inode bitmap and group descriptor block locations. */
static void dedup_setup(ext4_vol *ev)
{
	ext2_filsys fs = ev->fs;
	size_t want = (size_t)fs->group_desc_count * 2 + fs->desc_blocks + 16;
	size_t size = 64;
	dgrp_t g;

	while (size < want * 2)
		size <<= 1;
	free(ev->dedup);
	ev->dedup = calloc(size, sizeof(dedup_ent));
	if (!ev->dedup)
		return;
	ev->dedup_size = size;
	for (g = 0; g < fs->group_desc_count; g++) {
		dedup_add(ev, ext2fs_block_bitmap_loc(fs, g));
		dedup_add(ev, ext2fs_inode_bitmap_loc(fs, g));
	}
	for (blk64_t i = 0; i < fs->desc_blocks; i++)
		dedup_add(ev, ext2fs_descriptor_block_loc2(
				      fs, fs->super->s_first_data_block, (dgrp_t)i));
}

/* ---------------------------------------------------------------------- */
/* libext2fs I/O manager over fsc_dev                                      */

struct fio_priv {
	errcode_t magic;
	fsc_dev *dev;
	ext4_vol *ev;
	struct struct_io_stats stats;
};

#define FIO_MAGIC 0x46534d43 /* "FSMC" */

static struct struct_io_manager fsmac_io_manager;

static errcode_t fio_open(const char *name, int flags, io_channel *channel)
{
	io_channel io = NULL;
	struct fio_priv *priv = NULL;
	errcode_t ret;
	void *ptr = NULL;

	if (!name || sscanf(name, "fsmac:%p", &ptr) != 1 || !ptr)
		return EXT2_ET_BAD_DEVICE_NAME;
	if ((flags & IO_FLAG_RW) && fsc_dev_is_readonly((fsc_dev *)ptr))
		return EROFS;
	ret = ext2fs_get_memzero(sizeof(*io), &io);
	if (ret)
		return ret;
	ret = ext2fs_get_memzero(sizeof(*priv), &priv);
	if (ret)
		goto fail;
	ret = ext2fs_get_mem(strlen(name) + 1, &io->name);
	if (ret)
		goto fail;
	strcpy(io->name, name);
	io->magic = EXT2_ET_MAGIC_IO_CHANNEL;
	io->manager = &fsmac_io_manager;
	io->block_size = 1024;
	io->refcount = 1;
	io->flags = CHANNEL_FLAGS_BLOCK_DEVICE;
	priv->magic = FIO_MAGIC;
	priv->dev = ptr;
	priv->ev = io_owner;
	priv->stats.num_fields = 2;
	io->private_data = priv;
	*channel = io;
	return 0;
fail:
	if (io)
		ext2fs_free_mem(&io->name);
	ext2fs_free_mem(&priv);
	ext2fs_free_mem(&io);
	return ret;
}

static errcode_t fio_close(io_channel io)
{
	struct fio_priv *priv = io->private_data;

	if (--io->refcount > 0)
		return 0;
	if (!fsc_dev_is_readonly(priv->dev))
		fsc_dev_flush(priv->dev, false);
	ext2fs_free_mem(&io->private_data);
	ext2fs_free_mem(&io->name);
	ext2fs_free_mem(&io);
	return 0;
}

static errcode_t fio_set_blksize(io_channel io, int blksize)
{
	io->block_size = blksize;
	return 0;
}

static errcode_t fio_err(int err)
{
	if (err == -EIO)
		return EXT2_ET_SHORT_READ;
	return (errcode_t)-err;
}

static errcode_t fio_read_blk64(io_channel io, unsigned long long block,
				int count, void *data)
{
	struct fio_priv *priv = io->private_data;
	size_t size = count < 0 ? (size_t)-count
				: (size_t)count * (size_t)io->block_size;
	uint64_t off = block * (uint64_t)io->block_size;
	int err = fsc_dev_pread(priv->dev, data, size, off);

	if (err) {
		if (io->read_error)
			return io->read_error(io, (unsigned long)block, count,
					      data, size, 0, fio_err(err));
		return fio_err(err);
	}
	priv->stats.bytes_read += size;
	if (count == 1) {
		dedup_ent *d = dedup_find(priv->ev, block);
		if (d) {
			d->hash = hash_block(data, size);
			d->valid = true;
		}
	}
	return 0;
}

static errcode_t fio_write_blk64(io_channel io, unsigned long long block,
				 int count, const void *data)
{
	struct fio_priv *priv = io->private_data;
	size_t size = count < 0 ? (size_t)-count
				: (size_t)count * (size_t)io->block_size;
	uint64_t off = block * (uint64_t)io->block_size;
	dedup_ent *d = count == 1 ? dedup_find(priv->ev, block) : NULL;
	uint64_t h = 0;
	int err;

	if (d) {
		h = hash_block(data, size);
		if (d->valid && d->hash == h)
			return 0; /* unchanged on disk */
	}
	err = fsc_dev_pwrite(priv->dev, data, size, off);
	if (err) {
		if (d)
			d->valid = false;
		if (io->write_error)
			return io->write_error(io, (unsigned long)block, count,
					       data, size, 0, fio_err(err));
		return err == -EIO ? EXT2_ET_SHORT_WRITE : (errcode_t)-err;
	}
	if (d) {
		d->hash = h;
		d->valid = true;
	}
	priv->stats.bytes_written += size;
	return 0;
}

static errcode_t fio_read_blk(io_channel io, unsigned long block, int count,
			      void *data)
{
	return fio_read_blk64(io, block, count, data);
}

static errcode_t fio_write_blk(io_channel io, unsigned long block, int count,
			       const void *data)
{
	return fio_write_blk64(io, block, count, data);
}

static errcode_t fio_write_byte(io_channel io, unsigned long offset, int size,
				const void *data)
{
	struct fio_priv *priv = io->private_data;
	int err = fsc_dev_pwrite(priv->dev, data, (size_t)size, offset);

	return err ? (errcode_t)-err : 0;
}

static errcode_t fio_flush(io_channel io)
{
	struct fio_priv *priv = io->private_data;
	int err = fsc_dev_flush(priv->dev, true);

	return err ? (errcode_t)-err : 0;
}

static errcode_t fio_set_option(io_channel io, const char *option,
				const char *arg)
{
	(void)io; (void)option; (void)arg;
	return EXT2_ET_INVALID_ARGUMENT;
}

static errcode_t fio_get_stats(io_channel io, io_stats *stats)
{
	struct fio_priv *priv = io->private_data;

	*stats = &priv->stats;
	return 0;
}

static errcode_t fio_discard(io_channel io, unsigned long long block,
			     unsigned long long count)
{
	struct fio_priv *priv = io->private_data;

	fsc_dev_discard(priv->dev, block * (uint64_t)io->block_size,
			count * (uint64_t)io->block_size);
	return 0;
}

static errcode_t fio_cache_readahead(io_channel io, unsigned long long block,
				     unsigned long long count)
{
	(void)io; (void)block; (void)count;
	return 0;
}

static errcode_t fio_zeroout(io_channel io, unsigned long long block,
			     unsigned long long count)
{
	(void)io; (void)block; (void)count;
	return EXT2_ET_UNIMPLEMENTED; /* library falls back to writing zeros */
}

static struct struct_io_manager fsmac_io_manager = {
	.magic = EXT2_ET_MAGIC_IO_MANAGER,
	.name = "FSMac I/O Manager",
	.open = fio_open,
	.close = fio_close,
	.set_blksize = fio_set_blksize,
	.read_blk = fio_read_blk,
	.write_blk = fio_write_blk,
	.flush = fio_flush,
	.write_byte = fio_write_byte,
	.set_option = fio_set_option,
	.get_stats = fio_get_stats,
	.read_blk64 = fio_read_blk64,
	.write_blk64 = fio_write_blk64,
	.discard = fio_discard,
	.cache_readahead = fio_cache_readahead,
	.zeroout = fio_zeroout,
};

/* ---------------------------------------------------------------------- */
/* Inode helpers                                                           */

static inline errcode_t read_inode(ext2_filsys fs, ext2_ino_t ino,
				   struct ext2_inode_large *inode)
{
	memset(inode, 0, sizeof(*inode));
	return ext2fs_read_inode_full(fs, ino, EXT2_INODE(inode),
				      sizeof(*inode));
}

static inline errcode_t write_inode(ext2_filsys fs, ext2_ino_t ino,
				    struct ext2_inode_large *inode)
{
	return ext2fs_write_inode_full(fs, ino, EXT2_INODE(inode),
				       sizeof(*inode));
}

static void increment_version(struct ext2_inode_large *inode)
{
	__u64 ver = inode->osd1.linux1.l_i_version;

	if (EXT4_FITS_IN_INODE(inode, i_version_hi))
		ver |= (__u64)inode->i_version_hi << 32;
	ver++;
	inode->osd1.linux1.l_i_version = (__u32)ver;
	if (EXT4_FITS_IN_INODE(inode, i_version_hi))
		inode->i_version_hi = (__u32)(ver >> 32);
}

static void touch_ctime(struct ext2_inode_large *inode)
{
	struct timespec now = fsc_now();

	increment_version(inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, inode);
}

static void touch_mtime(struct ext2_inode_large *inode)
{
	struct timespec now = fsc_now();

	EXT4_INODE_SET_XTIME(i_mtime, &now, inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, inode);
	increment_version(inode);
}

static void init_times(struct ext2_inode_large *inode)
{
	struct timespec now = fsc_now();

	EXT4_INODE_SET_XTIME(i_atime, &now, inode);
	EXT4_INODE_SET_XTIME(i_ctime, &now, inode);
	EXT4_INODE_SET_XTIME(i_mtime, &now, inode);
	EXT4_EINODE_SET_XTIME(i_crtime, &now, inode);
	increment_version(inode);
}

/* Read-modify-write helpers for timestamps of another inode. */
static int update_mtime(ext2_filsys fs, ext2_ino_t ino)
{
	struct ext2_inode_large inode;
	errcode_t err = read_inode(fs, ino, &inode);

	if (err)
		return xlate(fs, ino, err);
	touch_mtime(&inode);
	return xlate(fs, ino, write_inode(fs, ino, &inode));
}

static int update_ctime(ext2_filsys fs, ext2_ino_t ino)
{
	struct ext2_inode_large inode;
	errcode_t err = read_inode(fs, ino, &inode);

	if (err)
		return xlate(fs, ino, err);
	touch_ctime(&inode);
	return xlate(fs, ino, write_inode(fs, ino, &inode));
}

static inline void set_uid(struct ext2_inode_large *inode, uint32_t uid)
{
	inode->i_uid = (__u16)uid;
	ext2fs_set_i_uid_high(*inode, uid >> 16);
}

static inline void set_gid(struct ext2_inode_large *inode, uint32_t gid)
{
	inode->i_gid = (__u16)gid;
	ext2fs_set_i_gid_high(*inode, gid >> 16);
}

static fsc_type mode_type(uint32_t mode)
{
	switch (mode & LINUX_S_IFMT) {
	case LINUX_S_IFREG: return FSC_TYPE_REG;
	case LINUX_S_IFDIR: return FSC_TYPE_DIR;
	case LINUX_S_IFLNK: return FSC_TYPE_LNK;
	case LINUX_S_IFCHR: return FSC_TYPE_CHR;
	case LINUX_S_IFBLK: return FSC_TYPE_BLK;
	case LINUX_S_IFIFO: return FSC_TYPE_FIFO;
	case LINUX_S_IFSOCK: return FSC_TYPE_SOCK;
	default: return FSC_TYPE_NONE;
	}
}

static int ext2_file_type(unsigned int mode)
{
	switch (mode & LINUX_S_IFMT) {
	case LINUX_S_IFREG: return EXT2_FT_REG_FILE;
	case LINUX_S_IFDIR: return EXT2_FT_DIR;
	case LINUX_S_IFCHR: return EXT2_FT_CHRDEV;
	case LINUX_S_IFBLK: return EXT2_FT_BLKDEV;
	case LINUX_S_IFLNK: return EXT2_FT_SYMLINK;
	case LINUX_S_IFIFO: return EXT2_FT_FIFO;
	case LINUX_S_IFSOCK: return EXT2_FT_SOCK;
	default: return 0;
	}
}

static fsc_type ft_type(int ft)
{
	switch (ft) {
	case EXT2_FT_REG_FILE: return FSC_TYPE_REG;
	case EXT2_FT_DIR: return FSC_TYPE_DIR;
	case EXT2_FT_CHRDEV: return FSC_TYPE_CHR;
	case EXT2_FT_BLKDEV: return FSC_TYPE_BLK;
	case EXT2_FT_FIFO: return FSC_TYPE_FIFO;
	case EXT2_FT_SOCK: return FSC_TYPE_SOCK;
	case EXT2_FT_SYMLINK: return FSC_TYPE_LNK;
	default: return FSC_TYPE_NONE;
	}
}

/* Checks an inode number from a client-supplied handle. */
static int check_ino(ext4_vol *ev, fsc_ino ino)
{
	ext2_filsys fs = ev->fs;

	if (ino == 0 || ino > fs->super->s_inodes_count)
		return -ESTALE;
	if (ino != EXT2_ROOT_INO && ino < EXT2_FIRST_INODE(fs->super))
		return -ESTALE; /* reserved inodes are not user visible */
	return 0;
}

/* Reads an inode that must currently be in use. */
static int get_live_inode(ext4_vol *ev, fsc_ino ino,
			  struct ext2_inode_large *inode)
{
	ext2_filsys fs = ev->fs;
	errcode_t err;
	int r = check_ino(ev, ino);

	if (r)
		return r;
	err = read_inode(fs, (ext2_ino_t)ino, inode);
	if (err)
		return xlate(fs, (ext2_ino_t)ino, err);
	if (inode->i_links_count == 0 || inode->i_mode == 0)
		return -ESTALE;
	if (fs->inode_map &&
	    !ext2fs_test_inode_bitmap2(fs->inode_map, (ext2_ino_t)ino))
		return -ESTALE;
	return 0;
}

static bool fs_can_allocate(ext2_filsys fs, blk64_t num)
{
	blk64_t reserved = ext2fs_r_blocks_count(fs->super);
	blk64_t total = ext2fs_blocks_count(fs->super);

	/* libext2fs can corrupt the filesystem if it runs completely out of
	 * blocks; always keep a small safety margin even with no reserve. */
	if (reserved < total / 200)
		reserved = total / 200;
	if (num > total)
		return false;
	return ext2fs_free_blocks_count(fs->super) > reserved + num;
}

static int check_name(const char *name, size_t len)
{
	return fsc_check_name(name, len, EXT2_NAME_LEN);
}

/* Copies a counted name into a NUL-terminated buffer. */
static void cname(char *buf, const char *name, size_t len)
{
	memcpy(buf, name, len);
	buf[len] = 0;
}

/* ---------------------------------------------------------------------- */
/* Probe / mount                                                           */

static bool ext4_probe(fsc_dev *dev, char *label, size_t labelsz,
		       uint8_t uuid[16])
{
	struct ext2_super_block sb;

	if (fsc_dev_size(dev) < 2048 + sizeof(sb))
		return false;
	if (fsc_dev_pread(dev, &sb, sizeof(sb), 1024))
		return false;
	if (ext2fs_le16_to_cpu(sb.s_magic) != EXT2_SUPER_MAGIC)
		return false;
	if (ext2fs_le32_to_cpu(sb.s_log_block_size) > 6 ||
	    ext2fs_le32_to_cpu(sb.s_blocks_per_group) == 0 ||
	    ext2fs_le32_to_cpu(sb.s_inodes_per_group) == 0)
		return false;
	if (label && labelsz) {
		size_t n = strnlen((const char *)sb.s_volume_name,
				   sizeof(sb.s_volume_name));
		if (n >= labelsz)
			n = labelsz - 1;
		memcpy(label, sb.s_volume_name, n);
		label[n] = 0;
	}
	memcpy(uuid, sb.s_uuid, 16);
	return true;
}

static const char *fs_flavor(ext2_filsys fs)
{
	struct ext2_super_block *s = fs->super;

	if (ext2fs_has_feature_extents(s) || ext2fs_has_feature_64bit(s) ||
	    ext2fs_has_feature_flex_bg(s) || ext2fs_has_feature_huge_file(s) ||
	    ext2fs_has_feature_dir_nlink(s) || ext2fs_has_feature_extra_isize(s) ||
	    ext2fs_has_feature_metadata_csum(s) || ext2fs_has_feature_gdt_csum(s) ||
	    ext2fs_has_feature_inline_data(s))
		return "ext4";
	if (ext2fs_has_feature_journal(s))
		return "ext3";
	return "ext2";
}

/* Features that force a read-only mount (support in libext2fs or in our
 * write paths is missing or incomplete). Returns a reason or NULL. */
static const char *ro_reason(ext2_filsys fs)
{
	struct ext2_super_block *s = fs->super;

	if (s->s_state & EXT2_ERROR_FS)
		return "has errors (use Check Disk)";
	if (ext2fs_has_feature_quota(s))
		return "quotas";
	if (ext2fs_has_feature_verity(s))
		return "fs-verity";
	if (ext2fs_has_feature_casefold(s))
		return "case-folding";
	if (ext2fs_has_feature_shared_blocks(s))
		return "shared blocks";
	if (ext2fs_has_feature_journal_dev(s))
		return "journal device";
	if (ext2fs_has_feature_journal(s) && s->s_journal_inum == 0)
		return "external journal";
	return NULL;
}

static int ext4_mount_vol(fsc_dev *dev, const fsc_mount_opts *o,
			  fsc_volume **out)
{
	ext4_vol *ev;
	ext2_filsys fs = NULL;
	errcode_t err;
	int flags = EXT2_FLAG_64BITS;
	bool rw = !o->readonly && !fsc_dev_is_readonly(dev);
	const char *why;

	ev = calloc(1, sizeof(*ev));
	if (!ev)
		return -ENOMEM;
	ev->base.ops = &fsc_ext4_ops;
	ev->base.dev = dev;
	ev->base.opts = *o;
	snprintf(ev->devname, sizeof(ev->devname), "fsmac:%p", (void *)dev);
	io_owner = ev;

	/* The journal may need replay even for a read-only mount, as the
	 * kernel does; that requires opening the device read-write. */
	err = ext2fs_open2(ev->devname, NULL,
			   flags | (fsc_dev_is_readonly(dev) ? 0 : EXT2_FLAG_RW),
			   0, 0, &fsmac_io_manager, &fs);
	if (err == EXT2_ET_RO_UNSUPP_FEATURE) {
		snprintf(ev->notice, sizeof(ev->notice),
			 "Uses features FSMac can't write; mounted read-only.");
		rw = false;
		err = ext2fs_open2(ev->devname, NULL, flags, 0, 0,
				   &fsmac_io_manager, &fs);
	}
	if (err) {
		fsc_error("ext4: cannot open filesystem: %s", error_message(err));
		io_owner = NULL;
		free(ev);
		return err == EXT2_ET_UNSUPP_FEATURE ? -ENOTSUP : xlate(NULL, 0, err);
	}
	ev->fs = fs;

	if (ext2fs_has_feature_journal_needs_recovery(fs->super)) {
		if (fs->flags & EXT2_FLAG_RW) {
			fsc_info("ext4: replaying journal");
			err = ext2fs_run_ext3_journal(&fs);
			ev->fs = fs;
			if (err) {
				fsc_error("ext4: journal replay failed: %s",
					  error_message(err));
				if (!fs) {
					io_owner = NULL;
					free(ev);
					return -EIO;
				}
				rw = false;
				snprintf(ev->notice, sizeof(ev->notice),
					 "Journal not replayed; mounted read-only. "
					 "Use Check Disk.");
			}
		} else {
			rw = false;
			snprintf(ev->notice, sizeof(ev->notice),
				 "Journal not replayed; mounted read-only.");
		}
	} else if (ext2fs_has_feature_journal(fs->super)) {
		err = ext2fs_check_ext3_journal(fs);
		if (err)
			fsc_warn("ext4: journal check: %s", error_message(err));
	}

	why = ro_reason(fs);
	if (why && rw) {
		rw = false;
		snprintf(ev->notice, sizeof(ev->notice),
			 "Mounted read-only: %s.", why);
	}
	if (!rw)
		fs->flags &= ~EXT2_FLAG_RW;
	ev->base.readonly = !rw;

	if (rw) {
		err = ext2fs_read_bitmaps(fs);
		if (err) {
			fsc_error("ext4: reading bitmaps: %s", error_message(err));
			goto fail;
		}
		/* Only maintain the primary superblock and descriptors, like
		 * the kernel; e2fsck refreshes the backups. */
		fs->flags |= EXT2_FLAG_MASTER_SB_ONLY;
		dedup_setup(ev);
		if (!(fs->super->s_state & EXT2_VALID_FS))
			snprintf(ev->notice, sizeof(ev->notice),
				 "Not unmounted cleanly.");
		/* Clear the valid flag so an unclean detach forces a check. */
		fs->super->s_mnt_count++;
		ext2fs_set_tstamp(fs->super, s_mtime, time(NULL));
		fs->super->s_state &= ~EXT2_VALID_FS;
		ext2fs_mark_super_dirty(fs);
		err = ext2fs_flush2(fs, 0);
		if (err) {
			fsc_error("ext4: marking filesystem in use: %s",
				  error_message(err));
			goto fail;
		}
	}
	arc4random_buf(&ev->next_generation, sizeof(ev->next_generation));
	io_owner = NULL;

	fsc_info("ext4: mounted '%.16s' (%s, %u-byte blocks, %s)",
		 (char *)fs->super->s_volume_name, fs_flavor(fs), fs->blocksize,
		 rw ? "read-write" : "read-only");
	*out = &ev->base;
	return 0;
fail:
	io_owner = NULL;
	ext2fs_free(fs);
	free(ev->dedup);
	free(ev);
	return xlate(NULL, 0, err);
}

static int ext4_unmount_vol(fsc_volume *v)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	errcode_t err = 0;
	int ret = 0, e;

	if (fs->flags & EXT2_FLAG_RW) {
		fs->super->s_state |= EXT2_VALID_FS;
		if (fs->super->s_error_count)
			fs->super->s_state |= EXT2_ERROR_FS;
		ext2fs_mark_super_dirty(fs);
		err = ext2fs_set_gdt_csum(fs);
		if (!err)
			err = ext2fs_flush2(fs, 0);
		if (err) {
			fsc_error("ext4: final flush failed: %s", error_message(err));
			ret = xlate(fs, 0, err);
		}
	}
	err = ext2fs_close_free(&ev->fs);
	if (err && !ret)
		ret = xlate(NULL, 0, err);
	e = fsc_dev_flush(v->dev, true);
	if (e && !ret)
		ret = e;
	fsc_dev_close(v->dev);
	free(ev->dedup);
	free(ev);
	return ret;
}

static void ext4_volinfo(fsc_volume *v, fsc_volinfo *info)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_super_block *s = fs->super;
	size_t n;

	snprintf(info->fstype, sizeof(info->fstype), "%s", fs_flavor(fs));
	n = strnlen((const char *)s->s_volume_name, sizeof(s->s_volume_name));
	memcpy(info->label, s->s_volume_name, n);
	info->label[n] = 0;
	memcpy(info->uuid, s->s_uuid, 16);
	info->case_insensitive = false;
	info->case_preserving = true;
	info->supports_symlinks = true;
	info->supports_hardlinks = true;
	info->supports_xattrs = ext2fs_has_feature_xattr(s);
	info->supports_unix_perms = true;
	info->name_max = EXT2_NAME_LEN;
	info->link_max = EXT2_LINK_MAX;
	info->max_file_size = ext2fs_has_feature_huge_file(s) ||
				      ext2fs_has_feature_extents(s)
				      ? (uint64_t)fs->blocksize << 32
				      : (uint64_t)1 << 41;
	info->time_gran_ns = EXT2_INODE_SIZE(s) > EXT2_GOOD_OLD_INODE_SIZE
				     ? 1
				     : 1000000000;
	info->root_ino = EXT2_ROOT_INO;
	snprintf(info->details, sizeof(info->details),
		 "%s, %u-byte blocks, %u-byte inodes%s%s%s", fs_flavor(fs),
		 fs->blocksize, EXT2_INODE_SIZE(s),
		 ext2fs_has_feature_journal(s) ? ", journal" : "",
		 ext2fs_has_feature_metadata_csum(s) ? ", metadata_csum" : "",
		 ext2fs_has_feature_64bit(s) ? ", 64bit" : "");
	snprintf(info->notice, sizeof(info->notice), "%s", ev->notice);
}

static int ext4_statfs_vol(fsc_volume *v, fsc_fsstat *st)
{
	ext2_filsys fs = EV(v)->fs;
	blk64_t overhead, reserved, free_blocks, total;

	overhead = fs->desc_blocks +
		   (blk64_t)fs->group_desc_count * (fs->inode_blocks_per_group + 2);
	total = ext2fs_blocks_count(fs->super);
	if (overhead > total)
		overhead = 0;
	reserved = ext2fs_r_blocks_count(fs->super);
	if (reserved < total / 200)
		reserved = total / 200;
	free_blocks = ext2fs_free_blocks_count(fs->super);
	st->block_size = fs->blocksize;
	st->total_bytes = (total - overhead) * fs->blocksize;
	st->free_bytes = free_blocks * fs->blocksize;
	st->avail_bytes = free_blocks > reserved
				  ? (free_blocks - reserved) * fs->blocksize
				  : 0;
	st->total_files = fs->super->s_inodes_count;
	st->free_files = fs->super->s_free_inodes_count;
	st->name_max = EXT2_NAME_LEN;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Attributes                                                              */

static void fill_attr(ext2_filsys fs, ext2_ino_t ino,
		      struct ext2_inode_large *inode, fsc_attr *a)
{
	a->ino = ino;
	a->gen = inode->i_generation;
	a->type = mode_type(inode->i_mode);
	a->mode = inode->i_mode & 07777;
	a->nlink = inode->i_links_count;
	a->uid = inode_uid(*inode);
	a->gid = inode_gid(*inode);
	a->size = EXT2_I_SIZE(inode);
	a->used = ext2fs_get_stat_i_blocks(fs, EXT2_INODE(inode)) * 512;
	EXT4_INODE_GET_XTIME(i_atime, &a->atime, inode);
	EXT4_INODE_GET_XTIME(i_mtime, &a->mtime, inode);
	EXT4_INODE_GET_XTIME(i_ctime, &a->ctime, inode);
	if (EXT4_FITS_IN_INODE(inode, i_crtime)) {
		EXT4_EINODE_GET_XTIME(i_crtime, &a->btime, inode);
	} else {
		a->btime = a->ctime; /* best available approximation */
	}
	if (a->type == FSC_TYPE_CHR || a->type == FSC_TYPE_BLK) {
		uint32_t dev = inode->i_block[0] ? inode->i_block[0]
						 : inode->i_block[1];
		if (inode->i_block[0]) { /* old 8:8 encoding */
			a->rdev_major = (dev >> 8) & 0xFF;
			a->rdev_minor = dev & 0xFF;
		} else { /* new 12:20 encoding */
			a->rdev_major = (dev & 0xFFF00) >> 8;
			a->rdev_minor = (dev & 0xFF) | ((dev >> 12) & 0xFFF00);
		}
	}
	if (a->type == FSC_TYPE_DIR && a->nlink == 1)
		a->nlink = 1; /* dir_nlink: "many" */
	if (inode->i_flags & EXT2_IMMUTABLE_FL)
		a->flags |= FSC_FLAG_IMMUTABLE;
	if (inode->i_flags & EXT2_APPEND_FL)
		a->flags |= FSC_FLAG_APPEND;
	if (inode->i_flags & EXT2_NODUMP_FL)
		a->flags |= FSC_FLAG_NODUMP;
	if (inode->i_flags & EXT4_ENCRYPT_FL)
		a->flags |= FSC_FLAG_ENCRYPTED;
}

static int ext4_getattr_op(fsc_volume *v, fsc_ino ino, fsc_attr *a)
{
	ext4_vol *ev = EV(v);
	struct ext2_inode_large inode;
	int err = get_live_inode(ev, ino, &inode);

	if (err)
		return err;
	fill_attr(ev->fs, (ext2_ino_t)ino, &inode, a);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Lookup and directories                                                  */

static int lookup_raw(ext2_filsys fs, ext2_ino_t dir, const char *name,
		      size_t len, ext2_ino_t *ino)
{
	errcode_t err = ext2fs_lookup(fs, dir, name, (int)len, NULL, ino);

	if (err)
		return xlate(fs, dir, err);
	if (*ino == 0)
		return -ENOENT;
	return 0;
}

static int check_dir(ext4_vol *ev, fsc_ino dir, struct ext2_inode_large *inode)
{
	int err = get_live_inode(ev, dir, inode);

	if (err)
		return err;
	if (!LINUX_S_ISDIR(inode->i_mode))
		return -ENOTDIR;
	if (inode->i_flags & EXT4_ENCRYPT_FL)
		return -EACCES; /* names are ciphertext */
	return 0;
}

static int ext4_lookup_op(fsc_volume *v, fsc_ino dir, const char *name,
			  size_t len, fsc_ino *ino)
{
	ext4_vol *ev = EV(v);
	struct ext2_inode_large dinode;
	ext2_ino_t found;
	int err;

	err = check_name(name, len);
	if (err)
		return err == -ENAMETOOLONG ? err : -ENOENT;
	err = check_dir(ev, dir, &dinode);
	if (err)
		return err;
	err = lookup_raw(ev->fs, (ext2_ino_t)dir, name, len, &found);
	if (err)
		return err;
	if (check_ino(ev, found))
		return -ENOENT;
	*ino = found;
	return 0;
}

static int ext4_parent_op(fsc_volume *v, fsc_ino dir, fsc_ino *parent)
{
	ext4_vol *ev = EV(v);
	struct ext2_inode_large dinode;
	ext2_ino_t found;
	int err;

	if (dir == EXT2_ROOT_INO) {
		*parent = EXT2_ROOT_INO;
		return 0;
	}
	err = get_live_inode(ev, dir, &dinode);
	if (err)
		return err;
	if (!LINUX_S_ISDIR(dinode.i_mode))
		return -ENOTDIR;
	err = lookup_raw(ev->fs, (ext2_ino_t)dir, "..", 2, &found);
	if (err)
		return err;
	*parent = found;
	return 0;
}

/* Fallback enumeration (inline-data directories): entry-index cookies. */
struct idx_iter {
	fsc_filldir_fn fn;
	void *ctx;
	uint64_t skip;   /* entries to skip */
	uint64_t index;  /* current entry index */
	ext2_filsys fs;
	bool stopped;
};

static int idx_iter_proc(ext2_ino_t dir, int entry,
			 struct ext2_dir_entry *dirent, int offset,
			 int blocksize, char *buf, void *priv)
{
	struct idx_iter *it = priv;
	int nlen = ext2fs_dirent_name_len(dirent);

	(void)dir; (void)offset; (void)blocksize; (void)buf;
	if (entry == DIRENT_DOT_FILE || entry == DIRENT_DOT_DOT_FILE)
		return 0;
	if (!dirent->inode || entry == DIRENT_CHECKSUM)
		return 0;
	if ((nlen == 1 && dirent->name[0] == '.') ||
	    (nlen == 2 && dirent->name[0] == '.' && dirent->name[1] == '.'))
		return 0;
	it->index++;
	if (it->index <= it->skip)
		return 0;
	if (it->fn(it->ctx, dirent->name, (size_t)nlen, dirent->inode,
		   ft_type(ext2fs_dirent_file_type(dirent)), it->index)) {
		it->stopped = true;
		return DIRENT_ABORT;
	}
	return 0;
}

/*
 * Enumerates a block-based directory starting at a (logical block, offset)
 * position encoded in the cookie: cookie = (lblk << 32) | byte offset.
 */
static int ext4_readdir_op(fsc_volume *v, fsc_ino dir, uint64_t cookie,
			   fsc_filldir_fn fn, void *ctx)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large dinode;
	blk64_t lblk, nblocks;
	unsigned int off, bsize = fs->blocksize;
	char *buf = NULL;
	errcode_t err;
	int ret;

	ret = check_dir(ev, dir, &dinode);
	if (ret)
		return ret;

	if (dinode.i_flags & EXT4_INLINE_DATA_FL) {
		struct idx_iter it = {fn, ctx, cookie, 0, fs, false};

		err = ext2fs_dir_iterate2(fs, (ext2_ino_t)dir,
					  DIRENT_FLAG_INCLUDE_INLINE_DATA, NULL,
					  idx_iter_proc, &it);
		return err ? xlate(fs, (ext2_ino_t)dir, err) : 0;
	}

	err = ext2fs_get_mem(bsize, &buf);
	if (err)
		return -ENOMEM;
	nblocks = (EXT2_I_SIZE(&dinode) + bsize - 1) / bsize;
	lblk = cookie >> 32;
	off = (unsigned int)(cookie & 0xFFFFFFFFu);
	if (off >= bsize) {
		lblk++;
		off = 0;
	}
	ret = 0;
	for (; lblk < nblocks; lblk++, off = 0) {
		blk64_t pblk = 0;
		unsigned int pos;

		err = ext2fs_bmap2(fs, (ext2_ino_t)dir, EXT2_INODE(&dinode), NULL,
				   0, lblk, NULL, &pblk);
		if (err) {
			ret = xlate(fs, (ext2_ino_t)dir, err);
			break;
		}
		if (!pblk)
			continue; /* hole */
		err = ext2fs_read_dir_block4(fs, pblk, buf, 0, (ext2_ino_t)dir);
		if (err == EXT2_ET_DIR_CSUM_INVALID) {
			fsc_warn("ext4: directory %llu block %llu has a bad "
				 "checksum", (unsigned long long)dir,
				 (unsigned long long)lblk);
		} else if (err) {
			ret = xlate(fs, (ext2_ino_t)dir, err);
			break;
		}
		/* Walk entries from the start of the block so that a resume
		 * offset always lands on an entry boundary. */
		for (pos = 0; pos < bsize;) {
			struct ext2_dir_entry *de =
				(struct ext2_dir_entry *)(buf + pos);
			unsigned int rec_len;
			int nlen;

			if (pos + 8 > bsize ||
			    ext2fs_get_rec_len(fs, de, &rec_len) ||
			    rec_len < 8 || (rec_len & 3) ||
			    pos + rec_len > bsize) {
				fsc_warn("ext4: corrupt entry in directory %llu",
					 (unsigned long long)dir);
				break;
			}
			nlen = ext2fs_dirent_name_len(de);
			if (pos >= off && de->inode && nlen &&
			    (unsigned int)nlen + 8 <= rec_len &&
			    !(nlen == 1 && de->name[0] == '.') &&
			    !(nlen == 2 && de->name[0] == '.' &&
			      de->name[1] == '.') &&
			    de->inode <= fs->super->s_inodes_count) {
				uint64_t next = pos + rec_len >= bsize
					? (lblk + 1) << 32
					: (lblk << 32) | (pos + rec_len);
				if (fn(ctx, de->name, (size_t)nlen, de->inode,
				       ft_type(ext2fs_dirent_file_type(de)),
				       next))
					goto done;
			}
			pos += rec_len;
		}
	}
done:
	ext2fs_free_mem(&buf);
	return ret;
}

static int ext4_dir_version_op(fsc_volume *v, fsc_ino dir, uint64_t *version)
{
	struct ext2_inode_large inode;
	int err = get_live_inode(EV(v), dir, &inode);

	if (err)
		return err;
	*version = ((uint64_t)inode.i_mtime << 32) ^ inode.i_mtime_extra ^
		   EXT2_I_SIZE(&inode);
	return 0;
}

/* ---------------------------------------------------------------------- */
/* File data                                                               */

/*
 * Fast paths for extent-mapped files. libext2fs's file API moves data one
 * block at a time and allocates block by block (each allocation walking the
 * extent tree and re-verifying checksums), which caps throughput far below
 * what USB SSDs deliver. For extent files we instead look up the mapping
 * once per request, allocate appended ranges with a single fallocate, and
 * transfer data with large direct device I/O.
 */

#define MAX_SEGS 128

typedef struct ext_seg {
	blk64_t lblk, pblk, len;
	bool uninit;
} ext_seg;

typedef struct ext_map {
	ext_seg seg[MAX_SEGS];
	int n;
	bool overflow;
} ext_map;

/* Collects the mapped extents overlapping [start, start + count). */
static errcode_t map_range(ext2_filsys fs, ext2_ino_t ino,
			   struct ext2_inode *inode, blk64_t start,
			   blk64_t count, ext_map *m)
{
	ext2_extent_handle_t h;
	struct ext2fs_extent e;
	blk64_t end = start + count;
	errcode_t err;
	int op = EXT2_EXTENT_CURRENT;
	bool first = true;

	m->n = 0;
	m->overflow = false;
	err = ext2fs_extent_open2(fs, ino, inode, &h);
	if (err)
		return err;
	err = ext2fs_extent_goto2(h, 0, start);
	if (err && err != EXT2_ET_EXTENT_NOT_FOUND)
		goto out;
	for (;;) {
		err = ext2fs_extent_get(h, op, &e);
		op = EXT2_EXTENT_NEXT_LEAF;
		if (err == EXT2_ET_EXTENT_NO_NEXT ||
		    err == EXT2_ET_NO_CURRENT_NODE || (err && first)) {
			err = 0; /* end of map (or empty tree) */
			break;
		}
		first = false;
		if (err)
			break;
		if (!(e.e_flags & EXT2_EXTENT_FLAGS_LEAF))
			continue;
		if (e.e_lblk >= end)
			break;
		if (e.e_lblk + e.e_len <= start)
			continue;
		if (m->n == MAX_SEGS) {
			m->overflow = true;
			break;
		}
		blk64_t s = e.e_lblk > start ? e.e_lblk : start;
		blk64_t t = e.e_lblk + e.e_len < end ? e.e_lblk + e.e_len : end;
		m->seg[m->n].lblk = s;
		m->seg[m->n].pblk = e.e_pblk + (s - e.e_lblk);
		m->seg[m->n].len = t - s;
		m->seg[m->n].uninit = (e.e_flags & EXT2_EXTENT_FLAGS_UNINIT) != 0;
		m->n++;
	}
out:
	ext2fs_extent_free(h);
	return err;
}

static bool fast_path_ok(ext2_filsys fs, struct ext2_inode_large *inode)
{
	return (inode->i_flags & EXT4_EXTENTS_FL) &&
	       !(inode->i_flags & (EXT4_INLINE_DATA_FL | EXT4_ENCRYPT_FL |
				   EXT4_VERITY_FL)) &&
	       !ext2fs_has_feature_bigalloc(fs->super);
}

/* Reads [off, off + len) (already clamped to the file size). */
static int read_fast(ext4_vol *ev, ext2_ino_t ino,
		     struct ext2_inode_large *inode, uint64_t off, size_t len,
		     uint8_t *buf, bool *handled)
{
	ext2_filsys fs = ev->fs;
	uint64_t bs = fs->blocksize;
	blk64_t first = off / bs, last = (off + len - 1) / bs;
	ext_map m;
	errcode_t err;

	*handled = false;
	err = map_range(fs, ino, EXT2_INODE(inode), first, last - first + 1, &m);
	if (err || m.overflow)
		return 0;
	*handled = true;
	memset(buf, 0, len); /* holes and unwritten extents read as zeros */
	for (int i = 0; i < m.n; i++) {
		uint64_t ls, le, s, e;
		int r;

		if (m.seg[i].uninit)
			continue;
		ls = m.seg[i].lblk * bs;
		le = ls + m.seg[i].len * bs;
		s = ls > off ? ls : off;
		e = le < off + len ? le : off + len;
		if (s >= e)
			continue;
		r = fsc_dev_pread(ev->base.dev, buf + (s - off), (size_t)(e - s),
				  m.seg[i].pblk * bs + (s - ls));
		if (r)
			return r;
	}
	return 0;
}

/* Writes a byte range of the file whose blocks are known to be mapped. */
static int put_mapped(ext4_vol *ev, const ext_map *m, uint64_t bs,
		      uint64_t foff, size_t len, const uint8_t *data)
{
	for (int i = 0; i < m->n && len; i++) {
		uint64_t ls = m->seg[i].lblk * bs, le = ls + m->seg[i].len * bs;
		uint64_t s = ls > foff ? ls : foff;
		uint64_t e = le < foff + len ? le : foff + len;
		int r;

		if (s >= e)
			continue;
		r = fsc_dev_pwrite(ev->base.dev, data ? data + (s - foff) : NULL,
				   (size_t)(e - s), m->seg[i].pblk * bs + (s - ls));
		if (r)
			return r;
	}
	return 0;
}

static int zero_mapped(ext4_vol *ev, const ext_map *m, uint64_t bs,
		       uint64_t foff, size_t len)
{
	static const uint8_t zeros[65536];

	while (len) {
		size_t n = len > sizeof(zeros) ? sizeof(zeros) : len;
		int r = put_mapped(ev, m, bs, foff, n, zeros);
		if (r)
			return r;
		foff += n;
		len -= n;
	}
	return 0;
}

/*
 * Overwrites and appends. Holes below the current end of file and unwritten
 * extents are left to the library (which handles them with the required
 * zeroing); everything else is done here. Newly allocated blocks are fully
 * written (data or zeros) before the size is advanced, so a crash can never
 * expose stale disk contents.
 */
static int write_fast(ext4_vol *ev, ext2_ino_t ino,
		      struct ext2_inode_large *inode, uint64_t off, size_t len,
		      const uint8_t *buf, bool *handled)
{
	ext2_filsys fs = ev->fs;
	uint64_t bs = fs->blocksize, old_size = EXT2_I_SIZE(inode);
	uint64_t end = off + len;
	blk64_t first = off / bs, last = (end - 1) / bs;
	blk64_t eof_blk = (old_size + bs - 1) / bs; /* first block past EOF */
	blk64_t mstart, covered, gap;
	ext_map m;
	errcode_t err;
	int r;

	*handled = false;
	if (len < 16384 || !fast_path_ok(fs, inode))
		return 0;
	/* Map from the old EOF if the write starts beyond it, so that blocks
	 * past EOF (which would be exposed by growing the file) are seen. */
	mstart = first < eof_blk ? first : eof_blk;
	err = map_range(fs, ino, EXT2_INODE(inode), mstart, last - mstart + 1, &m);
	if (err || m.overflow)
		return 0;
	gap = (blk64_t)-1;
	covered = first;
	for (int i = 0; i < m.n; i++) {
		if (m.seg[i].uninit || m.seg[i].lblk < first)
			return 0; /* unwritten extent, or stale blocks past EOF */
		if (m.seg[i].lblk > covered) {
			if (covered < eof_blk)
				return 0; /* hole inside the file */
			if (gap == (blk64_t)-1)
				gap = covered;
		}
		covered = m.seg[i].lblk + m.seg[i].len;
	}
	if (covered <= last) {
		if (covered < eof_blk)
			return 0;
		if (gap == (blk64_t)-1)
			gap = covered;
	}
	*handled = true;

	/* Allocate all unmapped blocks of the range (past EOF) in one go. */
	if (gap != (blk64_t)-1) {
		blk64_t goal, nb = last - gap + 1;

		if (!fs_can_allocate(fs, nb + 16))
			return -ENOSPC;
		goal = m.n ? m.seg[m.n - 1].pblk + m.seg[m.n - 1].len
			   : ext2fs_find_inode_goal(fs, ino, EXT2_INODE(inode), gap);
		err = ext2fs_fallocate(fs, EXT2_FALLOCATE_FORCE_INIT |
				       EXT2_FALLOCATE_INIT_BEYOND_EOF, ino,
				       EXT2_INODE(inode), goal, gap, nb);
		if (!err)
			err = map_range(fs, ino, EXT2_INODE(inode), first,
					last - first + 1, &m);
		/* The whole range must now be mapped, initialised and gapless. */
		covered = first;
		for (int i = 0; !err && !m.overflow && i < m.n; i++) {
			if (m.seg[i].uninit || m.seg[i].lblk != covered)
				break;
			covered += m.seg[i].len;
		}
		if (err || m.overflow || covered != last + 1) {
			/* Give back anything allocated past EOF. */
			ext2fs_punch(fs, ino, EXT2_INODE(inode), NULL, eof_blk, ~0ULL);
			write_inode(fs, ino, inode);
			return err ? xlate(fs, ino, err) : -EIO;
		}
	}

	/* Bytes between the old EOF and the write start must read as zero,
	 * including the stale tail of the old last block. */
	if (off > old_size && (old_size % bs)) {
		uint64_t zend = (old_size / bs + 1) * bs;
		if (zend > off)
			zend = off;
		if (old_size / bs >= first) {
			r = zero_mapped(ev, &m, bs, old_size, (size_t)(zend - old_size));
		} else {
			ext_map tm;
			blk64_t tb = old_size / bs;
			err = map_range(fs, ino, EXT2_INODE(inode), tb, 1, &tm);
			r = err ? xlate(fs, ino, err)
				: zero_mapped(ev, &tm, bs, old_size,
					      (size_t)(zend - old_size));
		}
		if (r)
			return r;
	}
	/* Zero the unwritten head of a newly allocated first block. */
	if (first >= eof_blk && off % bs) {
		r = zero_mapped(ev, &m, bs, first * bs, (size_t)(off % bs));
		if (r)
			return r;
	}
	/* ... and the tail of a newly allocated last block. */
	if (last >= eof_blk && end % bs) {
		r = zero_mapped(ev, &m, bs, end, (size_t)(bs - end % bs));
		if (r)
			return r;
	}
	r = put_mapped(ev, &m, bs, off, len, buf);
	if (r)
		return r;

	if (end > old_size) {
		err = ext2fs_inode_size_set(fs, EXT2_INODE(inode), end);
		if (err)
			return xlate(fs, ino, err);
	}
	touch_mtime(inode);
	err = write_inode(fs, ino, inode);
	return err ? xlate(fs, ino, err) : 0;
}

static int ext4_read_op(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len,
			void *buf, size_t *got)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large inode;
	ext2_file_t efp;
	errcode_t err;
	size_t total = 0;
	int ret;

	ret = get_live_inode(ev, ino, &inode);
	if (ret)
		return ret;
	if (LINUX_S_ISDIR(inode.i_mode))
		return -EISDIR;
	if (!LINUX_S_ISREG(inode.i_mode))
		return -EINVAL;
	if (inode.i_flags & EXT4_ENCRYPT_FL)
		return -EACCES;
	if (off >= EXT2_I_SIZE(&inode))
		return 0;
	if (len > EXT2_I_SIZE(&inode) - off)
		len = (size_t)(EXT2_I_SIZE(&inode) - off);
	if (fast_path_ok(fs, &inode)) {
		bool handled;
		ret = read_fast(ev, (ext2_ino_t)ino, &inode, off, len, buf, &handled);
		if (handled) {
			*got = ret ? 0 : len;
			return ret;
		}
	}

	err = ext2fs_file_open(fs, (ext2_ino_t)ino, 0, &efp);
	if (err)
		return xlate(fs, (ext2_ino_t)ino, err);
	err = ext2fs_file_llseek(efp, off, SEEK_SET, NULL);
	while (!err && total < len) {
		unsigned int chunk, n = 0;

		chunk = len - total > (1u << 30) ? (1u << 30)
						  : (unsigned int)(len - total);
		err = ext2fs_file_read(efp, (char *)buf + total, chunk, &n);
		if (err || n == 0)
			break;
		total += n;
	}
	ret = err ? xlate(fs, (ext2_ino_t)ino, err) : 0;
	err = ext2fs_file_close(efp);
	if (err && !ret)
		ret = xlate(fs, (ext2_ino_t)ino, err);
	*got = total;
	return total ? 0 : ret;
}

static int ext4_write_op(fsc_volume *v, fsc_ino ino, uint64_t off, size_t len,
			 const void *buf, size_t *written)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large inode;
	ext2_file_t efp;
	errcode_t err;
	size_t total = 0;
	int ret;

	ret = get_live_inode(ev, ino, &inode);
	if (ret)
		return ret;
	if (LINUX_S_ISDIR(inode.i_mode))
		return -EISDIR;
	if (!LINUX_S_ISREG(inode.i_mode))
		return -EINVAL;
	if (inode.i_flags & (EXT2_IMMUTABLE_FL | EXT4_VERITY_FL))
		return -EPERM;
	if ((inode.i_flags & EXT2_APPEND_FL) && off < EXT2_I_SIZE(&inode))
		return -EPERM;
	if (inode.i_flags & EXT4_ENCRYPT_FL)
		return -EACCES;
	if (!fs_can_allocate(fs, (len + fs->blocksize - 1) / fs->blocksize + 8))
		return -ENOSPC;
	{
		bool handled;
		ret = write_fast(ev, (ext2_ino_t)ino, &inode, off, len, buf, &handled);
		if (handled) {
			if (!ret) {
				*written = len;
				ev->dirty = true;
			}
			return ret;
		}
	}

	err = ext2fs_file_open(fs, (ext2_ino_t)ino, EXT2_FILE_WRITE, &efp);
	if (err)
		return xlate(fs, (ext2_ino_t)ino, err);
	err = ext2fs_file_llseek(efp, off, SEEK_SET, NULL);
	while (!err && total < len) {
		unsigned int chunk, n = 0;

		chunk = len - total > (1u << 30) ? (1u << 30)
						  : (unsigned int)(len - total);
		err = ext2fs_file_write(efp, (const char *)buf + total, chunk, &n);
		total += n;
		if (!err && n == 0)
			err = EXT2_ET_SHORT_WRITE;
	}
	ret = err ? xlate(fs, (ext2_ino_t)ino, err) : 0;
	err = ext2fs_file_close(efp); /* flushes data and the inode */
	if (err && !ret) {
		ret = xlate(fs, (ext2_ino_t)ino, err);
		total = 0;
	}
	if (total) {
		int e = update_mtime(fs, (ext2_ino_t)ino);
		if (e && !ret)
			ret = e;
		ev->dirty = true;
	}
	*written = total;
	return total ? 0 : ret;
}

/* ---------------------------------------------------------------------- */
/* Creation                                                                */

static int new_child_gid(ext2_filsys fs, struct ext2_inode_large *parent,
			 const fsc_cred *cred, bool *parent_sgid)
{
	(void)fs;
	*parent_sgid = (parent->i_mode & LINUX_S_ISGID) != 0;
	return *parent_sgid ? (int)inode_gid(*parent) : (int)cred->gid;
}

static int prep_create(ext4_vol *ev, fsc_ino dir, const char *name, size_t len,
		       struct ext2_inode_large *dinode, char *nbuf)
{
	ext2_ino_t existing;
	int err;

	err = check_name(name, len);
	if (err)
		return err;
	err = check_dir(ev, dir, dinode);
	if (err)
		return err;
	if (dinode->i_flags & (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL))
		return -EPERM;
	cname(nbuf, name, len);
	err = lookup_raw(ev->fs, (ext2_ino_t)dir, nbuf, len, &existing);
	if (err == 0)
		return -EEXIST;
	if (err != -ENOENT)
		return err;
	if (!fs_can_allocate(ev->fs, 4))
		return -ENOSPC;
	return 0;
}

static errcode_t link_name(ext2_filsys fs, ext2_ino_t dir, const char *name,
			   ext2_ino_t ino, int filetype)
{
	return ext2fs_link(fs, dir, name, ino, filetype | EXT2FS_LINK_EXPAND);
}

static int ext4_create_op(fsc_volume *v, fsc_ino dir, const char *name,
			  size_t len, fsc_type type, uint32_t mode,
			  uint32_t major, uint32_t minor, const fsc_cred *cred,
			  fsc_ino *ino)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large dinode, inode;
	char nbuf[EXT2_NAME_LEN + 1];
	ext2_ino_t child;
	errcode_t err;
	bool sgid;
	uint32_t gid, ftype_mode;
	int ret;

	ret = prep_create(ev, dir, name, len, &dinode, nbuf);
	if (ret)
		return ret;
	gid = (uint32_t)new_child_gid(fs, &dinode, cred, &sgid);

	if (type == FSC_TYPE_DIR) {
		err = ext2fs_mkdir2(fs, (ext2_ino_t)dir, 0, 0, EXT2FS_LINK_EXPAND,
				    nbuf, &child);
		if (err)
			return xlate(fs, (ext2_ino_t)dir, err);
		ret = update_mtime(fs, (ext2_ino_t)dir);
		if (ret)
			return ret;
		err = read_inode(fs, child, &inode);
		if (err)
			return xlate(fs, child, err);
		set_uid(&inode, cred->uid);
		set_gid(&inode, gid);
		inode.i_mode = LINUX_S_IFDIR | (mode & 07777 & ~LINUX_S_ISUID);
		if (sgid)
			inode.i_mode |= LINUX_S_ISGID;
		inode.i_generation = ev->next_generation++;
		init_times(&inode);
		err = write_inode(fs, child, &inode);
		if (err)
			return xlate(fs, child, err);
		/* The directory block checksum covers i_generation; rewrite
		 * it now that the generation is final (as fuse2fs does). */
		if (!(inode.i_flags & EXT4_INLINE_DATA_FL) &&
		    ext2fs_has_feature_metadata_csum(fs->super)) {
			char *block;
			blk64_t blk;

			err = ext2fs_new_dir_block(fs, child, (ext2_ino_t)dir, &block);
			if (err)
				return xlate(fs, child, err);
			err = ext2fs_bmap2(fs, child, EXT2_INODE(&inode), NULL, 0, 0,
					   NULL, &blk);
			if (!err)
				err = ext2fs_write_dir_block4(fs, blk, block, 0, child);
			ext2fs_free_mem(&block);
			if (err)
				return xlate(fs, child, err);
		}
		*ino = child;
		ev->dirty = true;
		return 0;
	}

	switch (type) {
	case FSC_TYPE_REG: ftype_mode = LINUX_S_IFREG; break;
	case FSC_TYPE_CHR: ftype_mode = LINUX_S_IFCHR; break;
	case FSC_TYPE_BLK: ftype_mode = LINUX_S_IFBLK; break;
	case FSC_TYPE_FIFO: ftype_mode = LINUX_S_IFIFO; break;
	case FSC_TYPE_SOCK: ftype_mode = LINUX_S_IFSOCK; break;
	default: return -EINVAL;
	}
	mode = ftype_mode | (mode & 07777);

	err = ext2fs_new_inode(fs, (ext2_ino_t)dir, (int)mode, 0, &child);
	if (err)
		return xlate(fs, (ext2_ino_t)dir, err);
	err = link_name(fs, (ext2_ino_t)dir, nbuf, child, ext2_file_type(mode));
	if (err)
		return xlate(fs, (ext2_ino_t)dir, err);
	ret = update_mtime(fs, (ext2_ino_t)dir);
	if (ret)
		return ret;

	memset(&inode, 0, sizeof(inode));
	inode.i_mode = (__u16)mode;
	inode.i_links_count = 1;
	inode.i_extra_isize = sizeof(struct ext2_inode_large) -
			      EXT2_GOOD_OLD_INODE_SIZE;
	set_uid(&inode, cred->uid);
	set_gid(&inode, gid);
	if (type == FSC_TYPE_CHR || type == FSC_TYPE_BLK) {
		if (major < 256 && minor < 256)
			inode.i_block[0] = (major << 8) | minor;
		else
			inode.i_block[1] = (minor & 0xFF) | (major << 8) |
					   ((minor & ~0xFFu) << 12);
	} else if (type == FSC_TYPE_REG &&
		   ext2fs_has_feature_extents(fs->super)) {
		ext2_extent_handle_t handle;

		inode.i_flags &= ~EXT4_EXTENTS_FL;
		err = ext2fs_extent_open2(fs, child, EXT2_INODE(&inode), &handle);
		if (err)
			return xlate(fs, child, err);
		ext2fs_extent_free(handle);
	}
	err = ext2fs_write_new_inode(fs, child, EXT2_INODE(&inode));
	if (err)
		return xlate(fs, child, err);
	inode.i_generation = ev->next_generation++;
	init_times(&inode);
	err = write_inode(fs, child, &inode);
	if (err)
		return xlate(fs, child, err);
	ext2fs_inode_alloc_stats2(fs, child, 1, 0);
	*ino = child;
	ev->dirty = true;
	return 0;
}

static int ext4_symlink_op(fsc_volume *v, fsc_ino dir, const char *name,
			   size_t len, const char *target, size_t tlen,
			   const fsc_cred *cred, fsc_ino *ino)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large dinode, inode;
	char nbuf[EXT2_NAME_LEN + 1], *tbuf;
	ext2_ino_t child;
	errcode_t err;
	bool sgid;
	uint32_t gid;
	int ret;

	if (tlen >= fs->blocksize)
		return -ENAMETOOLONG;
	ret = prep_create(ev, dir, name, len, &dinode, nbuf);
	if (ret)
		return ret;
	gid = (uint32_t)new_child_gid(fs, &dinode, cred, &sgid);
	tbuf = malloc(tlen + 1);
	if (!tbuf)
		return -ENOMEM;
	cname(tbuf, target, tlen);
	err = ext2fs_symlink(fs, (ext2_ino_t)dir, 0, nbuf, tbuf);
	if (err == EXT2_ET_DIR_NO_SPACE) {
		err = ext2fs_expand_dir(fs, (ext2_ino_t)dir);
		if (!err)
			err = ext2fs_symlink(fs, (ext2_ino_t)dir, 0, nbuf, tbuf);
	}
	free(tbuf);
	if (err)
		return xlate(fs, (ext2_ino_t)dir, err);
	ret = update_mtime(fs, (ext2_ino_t)dir);
	if (ret)
		return ret;
	ret = lookup_raw(fs, (ext2_ino_t)dir, nbuf, len, &child);
	if (ret)
		return ret;
	err = read_inode(fs, child, &inode);
	if (err)
		return xlate(fs, child, err);
	set_uid(&inode, cred->uid);
	set_gid(&inode, gid);
	inode.i_generation = ev->next_generation++;
	init_times(&inode);
	err = write_inode(fs, child, &inode);
	if (err)
		return xlate(fs, child, err);
	*ino = child;
	ev->dirty = true;
	return 0;
}

static int ext4_readlink_op(fsc_volume *v, fsc_ino ino, char *buf,
			    size_t bufsz, size_t *len)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large inode;
	errcode_t err;
	size_t n;
	int ret;

	ret = get_live_inode(ev, ino, &inode);
	if (ret)
		return ret;
	if (!LINUX_S_ISLNK(inode.i_mode))
		return -EINVAL;
	if (inode.i_flags & EXT4_ENCRYPT_FL)
		return -EACCES;
	n = EXT2_I_SIZE(&inode);
	if (n > bufsz)
		n = bufsz;
	if (ext2fs_is_fast_symlink(EXT2_INODE(&inode))) {
		memcpy(buf, (char *)inode.i_block, n);
	} else {
		ext2_file_t file;
		unsigned int got = 0;

		err = ext2fs_file_open(fs, (ext2_ino_t)ino, 0, &file);
		if (err)
			return xlate(fs, (ext2_ino_t)ino, err);
		err = ext2fs_file_read(file, buf, (unsigned int)n, &got);
		if (!err && got != n)
			err = EXT2_ET_INODE_CORRUPTED;
		errcode_t e2 = ext2fs_file_close(file);
		if (err || e2)
			return xlate(fs, (ext2_ino_t)ino, err ? err : e2);
	}
	*len = n;
	return 0;
}

static int ext4_link_op(fsc_volume *v, fsc_ino ino, fsc_ino dir,
			const char *name, size_t len)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large dinode, inode;
	char nbuf[EXT2_NAME_LEN + 1];
	errcode_t err;
	int ret;

	ret = prep_create(ev, dir, name, len, &dinode, nbuf);
	if (ret)
		return ret;
	ret = get_live_inode(ev, ino, &inode);
	if (ret)
		return ret;
	if (LINUX_S_ISDIR(inode.i_mode))
		return -EPERM;
	if (inode.i_flags & (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL))
		return -EPERM;
	if (inode.i_links_count >= EXT2_LINK_MAX)
		return -EMLINK;
	inode.i_links_count++;
	touch_ctime(&inode);
	err = write_inode(fs, (ext2_ino_t)ino, &inode);
	if (err)
		return xlate(fs, (ext2_ino_t)ino, err);
	err = link_name(fs, (ext2_ino_t)dir, nbuf, (ext2_ino_t)ino,
			ext2_file_type(inode.i_mode));
	if (err) {
		/* Undo the link count increment. */
		inode.i_links_count--;
		write_inode(fs, (ext2_ino_t)ino, &inode);
		return xlate(fs, (ext2_ino_t)dir, err);
	}
	ev->dirty = true;
	return update_mtime(fs, (ext2_ino_t)dir);
}

/* ---------------------------------------------------------------------- */
/* Removal                                                                 */

static int remove_ea_inodes(ext2_filsys fs, ext2_ino_t ino,
			    struct ext2_inode_large *inode)
{
	struct ext2_xattr_handle *h;
	errcode_t err;

	err = write_inode(fs, ino, inode);
	if (err)
		return xlate(fs, ino, err);
	err = ext2fs_xattrs_open(fs, ino, &h);
	if (err)
		return xlate(fs, ino, err);
	err = ext2fs_xattrs_read(h);
	if (!err)
		err = ext2fs_xattr_remove_all(h);
	ext2fs_xattrs_close(&h);
	if (err)
		return xlate(fs, ino, err);
	err = read_inode(fs, ino, inode);
	return err ? xlate(fs, ino, err) : 0;
}

/* Drops one link from ino and frees it when no links remain. */
static int put_inode(ext2_filsys fs, ext2_ino_t ino, int drop)
{
	struct ext2_inode_large inode;
	errcode_t err;
	int ret;

	err = read_inode(fs, ino, &inode);
	if (err)
		return xlate(fs, ino, err);
	if (inode.i_links_count == 0)
		return 0;
	if (inode.i_links_count <= drop)
		inode.i_links_count = 0;
	else
		inode.i_links_count -= drop;
	touch_ctime(&inode);
	if (inode.i_links_count) {
		err = write_inode(fs, ino, &inode);
		return err ? xlate(fs, ino, err) : 0;
	}
	ext2fs_set_dtime(fs, EXT2_INODE(&inode));
	if (ext2fs_has_feature_ea_inode(fs->super)) {
		ret = remove_ea_inodes(fs, ino, &inode);
		if (ret)
			return ret;
	}
	err = ext2fs_free_ext_attr(fs, ino, &inode);
	if (err)
		return xlate(fs, ino, err);
	if (ext2fs_inode_has_valid_blocks2(fs, EXT2_INODE(&inode))) {
		err = ext2fs_punch(fs, ino, EXT2_INODE(&inode), NULL, 0, ~0ULL);
		if (err)
			return xlate(fs, ino, err);
	}
	ext2fs_inode_alloc_stats2(fs, ino, -1, LINUX_S_ISDIR(inode.i_mode));
	err = write_inode(fs, ino, &inode);
	return err ? xlate(fs, ino, err) : 0;
}

struct empty_check {
	bool empty;
};

static int empty_proc(ext2_ino_t dir, int entry, struct ext2_dir_entry *dirent,
		      int offset, int blocksize, char *buf, void *priv)
{
	struct empty_check *ec = priv;
	int nlen = ext2fs_dirent_name_len(dirent);

	(void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
	if (!dirent->inode)
		return 0;
	if ((nlen == 1 && dirent->name[0] == '.') ||
	    (nlen == 2 && dirent->name[0] == '.' && dirent->name[1] == '.'))
		return 0;
	ec->empty = false;
	return DIRENT_ABORT;
}

static int dir_is_empty(ext2_filsys fs, ext2_ino_t dir)
{
	struct empty_check ec = {true};
	errcode_t err = ext2fs_dir_iterate2(fs, dir, 0, NULL, empty_proc, &ec);

	if (err)
		return xlate(fs, dir, err);
	return ec.empty ? 1 : 0;
}

/* Decrements a directory's link count (one subdirectory went away). */
static int dir_unref(ext2_filsys fs, ext2_ino_t dir)
{
	struct ext2_inode_large inode;
	errcode_t err = read_inode(fs, dir, &inode);

	if (err)
		return xlate(fs, dir, err);
	/* With dir_nlink, a count of 1 means "too many to count". */
	if (inode.i_links_count > 2)
		inode.i_links_count--;
	touch_mtime(&inode);
	err = write_inode(fs, dir, &inode);
	return err ? xlate(fs, dir, err) : 0;
}

static int dir_ref(ext2_filsys fs, ext2_ino_t dir)
{
	struct ext2_inode_large inode;
	errcode_t err = read_inode(fs, dir, &inode);

	if (err)
		return xlate(fs, dir, err);
	if (inode.i_links_count > 1) {
		if (inode.i_links_count < EXT2_LINK_MAX)
			inode.i_links_count++;
		else if (ext2fs_has_feature_dir_nlink(fs->super))
			inode.i_links_count = 1;
	}
	touch_mtime(&inode);
	err = write_inode(fs, dir, &inode);
	return err ? xlate(fs, dir, err) : 0;
}

/* Removes the entry and the object it names; the caller validated it. */
static int remove_entry(ext2_filsys fs, ext2_ino_t dir, const char *nbuf,
			ext2_ino_t ino, bool isdir)
{
	errcode_t err;
	int ret;

	err = ext2fs_unlink(fs, dir, nbuf, ino, 0);
	if (err)
		return xlate(fs, dir, err);
	if (isdir) {
		/* Directories have two links (name and "."). */
		ret = put_inode(fs, ino, 2);
		if (!ret)
			ret = dir_unref(fs, dir);
	} else {
		ret = put_inode(fs, ino, 1);
		if (!ret)
			ret = update_mtime(fs, dir);
	}
	return ret;
}

static int ext4_unlink_op(fsc_volume *v, fsc_ino dir, const char *name,
			  size_t len, bool isdir)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large dinode, inode;
	char nbuf[EXT2_NAME_LEN + 1];
	ext2_ino_t ino;
	int ret;

	ret = check_name(name, len);
	if (ret)
		return ret == -ENAMETOOLONG ? ret : -ENOENT;
	ret = check_dir(ev, dir, &dinode);
	if (ret)
		return ret;
	if (dinode.i_flags & (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL))
		return -EPERM;
	cname(nbuf, name, len);
	ret = lookup_raw(fs, (ext2_ino_t)dir, nbuf, len, &ino);
	if (ret)
		return ret;
	ret = get_live_inode(ev, ino, &inode);
	if (ret)
		return ret == -ESTALE ? -ENOENT : ret;
	if (inode.i_flags & (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL))
		return -EPERM;
	if (isdir) {
		if (!LINUX_S_ISDIR(inode.i_mode))
			return -ENOTDIR;
		ret = dir_is_empty(fs, ino);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ENOTEMPTY;
	} else if (LINUX_S_ISDIR(inode.i_mode)) {
		return -EPERM;
	}
	ret = remove_entry(fs, (ext2_ino_t)dir, nbuf, ino, isdir);
	ev->dirty = true;
	return ret;
}

/* ---------------------------------------------------------------------- */
/* Rename                                                                  */

struct repoint_ctx {
	const char *name;
	int len;
	ext2_ino_t old_ino, new_ino;
	int new_ft;
	bool done;
	ext2_filsys fs;
};

static int repoint_proc(ext2_ino_t dir, int entry, struct ext2_dir_entry *de,
			int offset, int blocksize, char *buf, void *priv)
{
	struct repoint_ctx *rc = priv;

	(void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
	if (de->inode != rc->old_ino || ext2fs_dirent_name_len(de) != rc->len ||
	    memcmp(de->name, rc->name, (size_t)rc->len))
		return 0;
	de->inode = rc->new_ino;
	if (ext2fs_has_feature_filetype(rc->fs->super))
		ext2fs_dirent_set_file_type(de, rc->new_ft);
	rc->done = true;
	return DIRENT_CHANGED | DIRENT_ABORT;
}

struct dotdot_ctx {
	ext2_ino_t new_parent;
};

static int dotdot_proc(ext2_ino_t dir, int entry, struct ext2_dir_entry *de,
		       int offset, int blocksize, char *buf, void *priv)
{
	struct dotdot_ctx *dc = priv;

	(void)dir; (void)entry; (void)offset; (void)blocksize; (void)buf;
	if (ext2fs_dirent_name_len(de) == 2 && de->name[0] == '.' &&
	    de->name[1] == '.') {
		de->inode = dc->new_parent;
		return DIRENT_CHANGED | DIRENT_ABORT;
	}
	return 0;
}

static int ext4_rename_op(fsc_volume *v, fsc_ino sdir, const char *sname,
			  size_t slen, fsc_ino ddir, const char *dname,
			  size_t dlen)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large sdinode, ddinode, sinode, tinode;
	char sbuf[EXT2_NAME_LEN + 1], dbuf[EXT2_NAME_LEN + 1];
	ext2_ino_t src, tgt = 0;
	bool sdir_flag, tdir_flag = false;
	errcode_t err;
	int ret;

	if ((ret = check_name(sname, slen)) || (ret = check_name(dname, dlen)))
		return ret;
	if ((ret = check_dir(ev, sdir, &sdinode)) ||
	    (ret = check_dir(ev, ddir, &ddinode)))
		return ret;
	if ((sdinode.i_flags | ddinode.i_flags) &
	    (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL))
		return -EPERM;
	if (!fs_can_allocate(fs, 5))
		return -ENOSPC;
	cname(sbuf, sname, slen);
	cname(dbuf, dname, dlen);

	ret = lookup_raw(fs, (ext2_ino_t)sdir, sbuf, slen, &src);
	if (ret)
		return ret;
	ret = get_live_inode(ev, src, &sinode);
	if (ret)
		return ret == -ESTALE ? -ENOENT : ret;
	if (sinode.i_flags & (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL))
		return -EPERM;
	sdir_flag = LINUX_S_ISDIR(sinode.i_mode);

	ret = lookup_raw(fs, (ext2_ino_t)ddir, dbuf, dlen, &tgt);
	if (ret == -ENOENT)
		tgt = 0;
	else if (ret)
		return ret;
	if (tgt == src)
		return 0; /* same file: POSIX says do nothing */
	if (sdir == ddir && slen == dlen && !memcmp(sname, dname, slen))
		return 0;

	if (tgt) {
		ret = get_live_inode(ev, tgt, &tinode);
		if (ret)
			return ret;
		if (tinode.i_flags & (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL))
			return -EPERM;
		tdir_flag = LINUX_S_ISDIR(tinode.i_mode);
		if (sdir_flag && !tdir_flag)
			return -ENOTDIR;
		if (!sdir_flag && tdir_flag)
			return -EISDIR;
		if (tdir_flag) {
			ret = dir_is_empty(fs, tgt);
			if (ret < 0)
				return ret;
			if (!ret)
				return -ENOTEMPTY;
		}
		/* Atomically repoint the existing entry at the source. */
		struct repoint_ctx rc = {dbuf, (int)dlen, tgt, src,
					 ext2_file_type(sinode.i_mode), false, fs};
		err = ext2fs_dir_iterate2(fs, (ext2_ino_t)ddir, 0, NULL,
					  repoint_proc, &rc);
		if (err)
			return xlate(fs, (ext2_ino_t)ddir, err);
		if (!rc.done)
			return -EIO;
		/* Drop the old target. */
		ret = put_inode(fs, tgt, tdir_flag ? 2 : 1);
		if (ret)
			return ret;
		if (tdir_flag) {
			ret = dir_unref(fs, (ext2_ino_t)ddir);
			if (ret)
				return ret;
		}
	} else {
		err = link_name(fs, (ext2_ino_t)ddir, dbuf, src,
				ext2_file_type(sinode.i_mode));
		if (err)
			return xlate(fs, (ext2_ino_t)ddir, err);
	}

	if (sdir_flag && sdir != ddir) {
		struct dotdot_ctx dc = {(ext2_ino_t)ddir};

		err = ext2fs_dir_iterate2(fs, src, 0, NULL, dotdot_proc, &dc);
		if (err)
			return xlate(fs, src, err);
		ret = dir_unref(fs, (ext2_ino_t)sdir);
		if (!ret)
			ret = dir_ref(fs, (ext2_ino_t)ddir);
		if (ret)
			return ret;
	}

	/* Remove the old name. */
	err = ext2fs_unlink(fs, (ext2_ino_t)sdir, sbuf, src, 0);
	if (err)
		return xlate(fs, (ext2_ino_t)sdir, err);

	ret = update_ctime(fs, src);
	if (!ret)
		ret = update_mtime(fs, (ext2_ino_t)sdir);
	if (!ret && ddir != sdir)
		ret = update_mtime(fs, (ext2_ino_t)ddir);
	ev->dirty = true;
	return ret;
}

/* ---------------------------------------------------------------------- */
/* Attributes                                                              */

static int truncate_ino(ext2_filsys fs, ext2_ino_t ino, uint64_t size)
{
	ext2_file_t file;
	errcode_t err, e2;

	err = ext2fs_file_open(fs, ino, EXT2_FILE_WRITE, &file);
	if (err)
		return xlate(fs, ino, err);
	err = ext2fs_file_set_size2(file, size);
	e2 = ext2fs_file_close(file);
	if (err || e2)
		return xlate(fs, ino, err ? err : e2);
	return 0;
}

static int ext4_setattr_op(fsc_volume *v, fsc_ino ino, const fsc_sattr *sa)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large inode;
	errcode_t err;
	int ret;

	ret = get_live_inode(ev, ino, &inode);
	if (ret)
		return ret;
	if (inode.i_flags & EXT4_ENCRYPT_FL)
		return -EACCES;
	/* Changing anything but flags on an immutable inode is refused;
	 * clearing the immutable flag itself is allowed. */
	if ((inode.i_flags & EXT2_IMMUTABLE_FL) &&
	    (sa->mask & ~(FSC_SET_FLAGS | FSC_SET_ATIME | FSC_SET_ATIME_NOW)))
		return -EPERM;

	if (sa->mask & FSC_SET_SIZE) {
		if (LINUX_S_ISDIR(inode.i_mode))
			return -EISDIR;
		if (!LINUX_S_ISREG(inode.i_mode))
			return -EINVAL;
		if (inode.i_flags & EXT2_APPEND_FL)
			return -EPERM;
		if (sa->size != EXT2_I_SIZE(&inode)) {
			ret = truncate_ino(fs, (ext2_ino_t)ino, sa->size);
			if (ret)
				return ret;
			err = read_inode(fs, (ext2_ino_t)ino, &inode);
			if (err)
				return xlate(fs, (ext2_ino_t)ino, err);
		}
		touch_mtime(&inode);
	}
	if (sa->mask & FSC_SET_MODE)
		inode.i_mode = (__u16)((inode.i_mode & ~07777u) | (sa->mode & 07777));
	if (sa->mask & FSC_SET_UID)
		set_uid(&inode, sa->uid);
	if (sa->mask & FSC_SET_GID)
		set_gid(&inode, sa->gid);
	if (sa->mask & FSC_SET_ATIME_NOW) {
		struct timespec now = fsc_now();
		EXT4_INODE_SET_XTIME(i_atime, &now, &inode);
	} else if (sa->mask & FSC_SET_ATIME) {
		EXT4_INODE_SET_XTIME(i_atime, &sa->atime, &inode);
	}
	if (sa->mask & FSC_SET_MTIME_NOW) {
		struct timespec now = fsc_now();
		EXT4_INODE_SET_XTIME(i_mtime, &now, &inode);
	} else if (sa->mask & FSC_SET_MTIME) {
		EXT4_INODE_SET_XTIME(i_mtime, &sa->mtime, &inode);
	}
	if (sa->mask & FSC_SET_BTIME)
		EXT4_EINODE_SET_XTIME(i_crtime, &sa->btime, &inode);
	if (sa->mask & FSC_SET_FLAGS) {
		__u32 f = inode.i_flags &
			  ~(EXT2_IMMUTABLE_FL | EXT2_APPEND_FL | EXT2_NODUMP_FL);
		if (sa->flags & FSC_FLAG_IMMUTABLE)
			f |= EXT2_IMMUTABLE_FL;
		if (sa->flags & FSC_FLAG_APPEND)
			f |= EXT2_APPEND_FL;
		if (sa->flags & FSC_FLAG_NODUMP)
			f |= EXT2_NODUMP_FL;
		inode.i_flags = f;
	}
	touch_ctime(&inode);
	err = write_inode(fs, (ext2_ino_t)ino, &inode);
	if (err)
		return xlate(fs, (ext2_ino_t)ino, err);
	ev->dirty = true;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Extended attributes: macOS names live in the Linux "user." namespace   */

#define XATTR_USER_PREFIX "user."
#define XATTR_USER_PREFIX_LEN 5

static int xattr_key(const char *name, char *key, size_t keysz)
{
	size_t n = strlen(name);

	if (n == 0 || n + XATTR_USER_PREFIX_LEN >= keysz)
		return n ? -ENAMETOOLONG : -EINVAL;
	memcpy(key, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN);
	memcpy(key + XATTR_USER_PREFIX_LEN, name, n + 1);
	return 0;
}

static int xattr_open(ext4_vol *ev, fsc_ino ino, struct ext2_xattr_handle **h)
{
	ext2_filsys fs = ev->fs;
	struct ext2_inode_large inode;
	errcode_t err;
	int ret = get_live_inode(ev, ino, &inode);

	if (ret)
		return ret;
	if (!ext2fs_has_feature_xattr(fs->super))
		return -ENOTSUP;
	if (LINUX_S_ISLNK(inode.i_mode))
		return -EPERM;
	err = ext2fs_xattrs_open(fs, (ext2_ino_t)ino, h);
	if (err)
		return xlate(fs, (ext2_ino_t)ino, err);
	err = ext2fs_xattrs_read(*h);
	if (err) {
		ext2fs_xattrs_close(h);
		return xlate(fs, (ext2_ino_t)ino, err);
	}
	return 0;
}

struct list_ctx {
	char *buf;
	size_t bufsz, total;
	int err;
};

static int list_proc(char *name, char *value, size_t value_len, void *data)
{
	struct list_ctx *lc = data;
	size_t n;

	(void)value; (void)value_len;
	if (strncmp(name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN))
		return 0;
	name += XATTR_USER_PREFIX_LEN;
	n = strlen(name) + 1;
	if (lc->buf) {
		if (lc->total + n > lc->bufsz) {
			lc->err = -ERANGE;
			return XATTR_ABORT;
		}
		memcpy(lc->buf + lc->total, name, n);
	}
	lc->total += n;
	return 0;
}

static int ext4_listxattr_op(fsc_volume *v, fsc_ino ino, char *buf,
			     size_t bufsz, size_t *len)
{
	ext4_vol *ev = EV(v);
	struct ext2_xattr_handle *h;
	struct list_ctx lc = {buf, bufsz, 0, 0};
	errcode_t err;
	int ret = xattr_open(ev, ino, &h);

	if (ret == -ENOTSUP || ret == -EPERM)
		return 0;
	if (ret)
		return ret;
	err = ext2fs_xattrs_iterate(h, list_proc, &lc);
	ext2fs_xattrs_close(&h);
	if (err)
		return xlate(ev->fs, (ext2_ino_t)ino, err);
	*len = lc.total;
	return lc.err;
}

static int ext4_getxattr_op(fsc_volume *v, fsc_ino ino, const char *name,
			    void *buf, size_t bufsz, size_t *len)
{
	ext4_vol *ev = EV(v);
	struct ext2_xattr_handle *h;
	char key[512];
	void *val = NULL;
	size_t vlen = 0;
	errcode_t err;
	int ret;

	ret = xattr_key(name, key, sizeof(key));
	if (ret)
		return ret;
	ret = xattr_open(ev, ino, &h);
	if (ret == -ENOTSUP || ret == -EPERM)
		return -ENOATTR;
	if (ret)
		return ret;
	err = ext2fs_xattr_get(h, key, &val, &vlen);
	ext2fs_xattrs_close(&h);
	if (err)
		return xlate(ev->fs, (ext2_ino_t)ino, err);
	*len = vlen;
	if (buf) {
		if (vlen > bufsz)
			ret = -ERANGE;
		else
			memcpy(buf, val, vlen);
	}
	ext2fs_free_mem(&val);
	return ret;
}

static int ext4_setxattr_op(fsc_volume *v, fsc_ino ino, const char *name,
			    const void *val, size_t len, unsigned flags)
{
	ext4_vol *ev = EV(v);
	struct ext2_xattr_handle *h;
	char key[512];
	errcode_t err;
	int ret;

	ret = xattr_key(name, key, sizeof(key));
	if (ret)
		return ret;
	ret = xattr_open(ev, ino, &h);
	if (ret)
		return ret;
	if (flags & (FSC_XATTR_CREATE | FSC_XATTR_REPLACE)) {
		void *old = NULL;
		size_t olen;

		err = ext2fs_xattr_get(h, key, &old, &olen);
		if (!err) {
			ext2fs_free_mem(&old);
			if (flags & FSC_XATTR_CREATE) {
				ext2fs_xattrs_close(&h);
				return -EEXIST;
			}
		} else if (err == EXT2_ET_EA_KEY_NOT_FOUND) {
			if (flags & FSC_XATTR_REPLACE) {
				ext2fs_xattrs_close(&h);
				return -ENOATTR;
			}
		} else {
			ext2fs_xattrs_close(&h);
			return xlate(ev->fs, (ext2_ino_t)ino, err);
		}
	}
	err = ext2fs_xattr_set(h, key, val, len);
	errcode_t e2 = ext2fs_xattrs_close(&h);
	if (err == EXT2_ET_EA_NO_SPACE || err == EXT2_ET_EA_INODE_CORRUPTED)
		return -E2BIG;
	if (err || e2)
		return xlate(ev->fs, (ext2_ino_t)ino, err ? err : e2);
	ev->dirty = true;
	return update_ctime(ev->fs, (ext2_ino_t)ino);
}

static int ext4_removexattr_op(fsc_volume *v, fsc_ino ino, const char *name)
{
	ext4_vol *ev = EV(v);
	struct ext2_xattr_handle *h;
	char key[512];
	void *old = NULL;
	size_t olen;
	errcode_t err;
	int ret;

	ret = xattr_key(name, key, sizeof(key));
	if (ret)
		return ret;
	ret = xattr_open(ev, ino, &h);
	if (ret == -ENOTSUP || ret == -EPERM)
		return -ENOATTR;
	if (ret)
		return ret;
	err = ext2fs_xattr_get(h, key, &old, &olen);
	if (err) {
		ext2fs_xattrs_close(&h);
		return xlate(ev->fs, (ext2_ino_t)ino, err);
	}
	ext2fs_free_mem(&old);
	err = ext2fs_xattr_remove(h, key);
	errcode_t e2 = ext2fs_xattrs_close(&h);
	if (err || e2)
		return xlate(ev->fs, (ext2_ino_t)ino, err ? err : e2);
	ev->dirty = true;
	return update_ctime(ev->fs, (ext2_ino_t)ino);
}

/* ---------------------------------------------------------------------- */
/* Maintenance                                                             */

/*
 * FSC_SYNC_DATA: everything libext2fs has written so far (file data,
 * inodes, directory blocks, extent blocks) is pushed to the device. The
 * superblock counters and the allocation bitmaps stay in memory.
 * FSC_SYNC_FULL: additionally writes superblock, descriptors and bitmaps
 * (unchanged blocks are elided) and flushes the device cache.
 */
static int ext4_sync_op(fsc_volume *v, int level)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	errcode_t err;

	if (level == FSC_SYNC_FULL) {
		if ((fs->flags & (EXT2_FLAG_DIRTY | EXT2_FLAG_BB_DIRTY |
				  EXT2_FLAG_IB_DIRTY)) ||
		    ev->dirty) {
			ext2fs_mark_super_dirty(fs);
			err = ext2fs_flush2(fs, 0); /* includes device flush */
			if (err)
				return xlate(fs, 0, err);
		} else {
			int e = fsc_dev_flush(v->dev, true);
			if (e)
				return e;
		}
		ev->dirty = false;
		return 0;
	}
	return fsc_dev_flush(v->dev, false);
}

static bool ext4_is_dirty_op(fsc_volume *v)
{
	ext4_vol *ev = EV(v);
	fsc_dev_stats st;

	fsc_dev_get_stats(v->dev, &st);
	return ev->dirty || st.dirty_pages ||
	       (ev->fs->flags & (EXT2_FLAG_DIRTY | EXT2_FLAG_BB_DIRTY |
				 EXT2_FLAG_IB_DIRTY));
}

static int ext4_set_label_op(fsc_volume *v, const char *label)
{
	ext4_vol *ev = EV(v);
	ext2_filsys fs = ev->fs;
	size_t n = strlen(label);

	if (n > sizeof(fs->super->s_volume_name))
		return -ENAMETOOLONG;
	memset(fs->super->s_volume_name, 0, sizeof(fs->super->s_volume_name));
	memcpy(fs->super->s_volume_name, label, n);
	ext2fs_mark_super_dirty(fs);
	ev->dirty = true;
	return 0;
}

const fsc_ops fsc_ext4_ops = {
	.name = "ext4",
	.probe = ext4_probe,
	.mount = ext4_mount_vol,
	.unmount = ext4_unmount_vol,
	.volinfo = ext4_volinfo,
	.statfs = ext4_statfs_vol,
	.getattr = ext4_getattr_op,
	.lookup = ext4_lookup_op,
	.parent = ext4_parent_op,
	.readdir = ext4_readdir_op,
	.dir_version = ext4_dir_version_op,
	.read = ext4_read_op,
	.write = ext4_write_op,
	.create = ext4_create_op,
	.symlink = ext4_symlink_op,
	.readlink = ext4_readlink_op,
	.link = ext4_link_op,
	.unlink = ext4_unlink_op,
	.rename = ext4_rename_op,
	.setattr = ext4_setattr_op,
	.listxattr = ext4_listxattr_op,
	.getxattr = ext4_getxattr_op,
	.setxattr = ext4_setxattr_op,
	.removexattr = ext4_removexattr_op,
	.sync = ext4_sync_op,
	.is_dirty = ext4_is_dirty_op,
	.set_label = ext4_set_label_op,
};
