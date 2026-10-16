/*
 * AppleDouble translation.
 *
 * Over NFSv3 the macOS client stores extended attributes, Finder info and
 * resource forks in "._name" AppleDouble companion files. Instead of
 * littering the volume with them, the server presents such a file for any
 * object that has native extended attributes (NTFS named streams, ext4
 * user.* xattrs), synthesising its contents on the fly, and turns what the
 * client writes into native extended attributes. Hidden NTFS files get the
 * Finder "invisible" bit so Finder hides them as Windows does.
 *
 * Virtual companion files are not listed by READDIR. A real "._name" file
 * on disk always takes precedence over a virtual one.
 */
#ifndef FSMAC_APPLEDOUBLE_H
#define FSMAC_APPLEDOUBLE_H

#include "nfsd.h"

/* Is this an AppleDouble companion name ("._x")? */
static inline bool ad_is_name(const char *name, size_t len)
{
	return len > 2 && name[0] == '.' && name[1] == '_';
}

/* Synthetic fileid for the companion of ino (distinct from real inodes). */
#define AD_FILEID(ino) ((ino) | (1ull << 63))

int ad_init(nfs_server *srv);
void ad_shutdown(nfs_server *srv);

/*
 * Looks up a virtual companion "._name" in dir. Returns 0 and fills fh and
 * attr if the base object exists and has (or may get) attributes,
 * -ENOENT otherwise. `for_create` presents a companion even if the base has
 * no attributes yet.
 */
int ad_lookup(nfs_server *srv, fsc_ino dir, const char *name, size_t len,
	      bool for_create, nfs_fh *fh, fsc_attr *attr);
int ad_getattr(nfs_server *srv, const nfs_fh *fh, fsc_attr *attr);
int ad_read(nfs_server *srv, const nfs_fh *fh, uint64_t off, size_t len,
	    void *buf, size_t *got);
int ad_write(nfs_server *srv, const nfs_fh *fh, uint64_t off, size_t len,
	     const void *buf);
int ad_truncate(nfs_server *srv, const nfs_fh *fh, uint64_t size);
/* Removes all extended attributes of the base object. */
int ad_remove(nfs_server *srv, const nfs_fh *fh);
/* Applies pending client writes to native attributes. */
int ad_commit(nfs_server *srv, const nfs_fh *fh);
/* Applies companions idle for a while; returns ms until next call. */
int ad_tick(nfs_server *srv);
int ad_flush_all(nfs_server *srv);
/* Forgets cached state for ino (e.g. after the base object is removed). */
void ad_forget(nfs_server *srv, fsc_ino ino);

#endif /* FSMAC_APPLEDOUBLE_H */
