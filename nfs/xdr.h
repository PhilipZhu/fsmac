/*
 * Minimal XDR (RFC 4506) encoding and decoding over memory buffers.
 *
 * Decoders never read past the end of the input; on malformed input they
 * set the error flag and return zero values, so callers can decode a whole
 * structure and check the flag once at the end.
 */
#ifndef FSMAC_XDR_H
#define FSMAC_XDR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct xdr_in {
	const uint8_t *buf;
	size_t len;
	size_t pos;
	bool err;
} xdr_in;

typedef struct xdr_out {
	uint8_t *buf;
	size_t len;   /* bytes written */
	size_t cap;
	bool err;     /* allocation failure */
} xdr_out;

static inline void xin_init(xdr_in *x, const void *buf, size_t len)
{
	x->buf = buf;
	x->len = len;
	x->pos = 0;
	x->err = false;
}

static inline size_t xin_remaining(const xdr_in *x)
{
	return x->len - x->pos;
}

uint32_t xin_u32(xdr_in *x);
uint64_t xin_u64(xdr_in *x);
static inline bool xin_bool(xdr_in *x) { return xin_u32(x) != 0; }
/* Fixed-length opaque data (padded to 4 bytes). Returns a pointer into the
 * input buffer, or NULL on error. */
const uint8_t *xin_fixed(xdr_in *x, size_t n);
/* Variable-length opaque/string: returns pointer and length (<= max). */
const uint8_t *xin_var(xdr_in *x, size_t max, size_t *len);
void xin_skip_var(xdr_in *x, size_t max);

void xout_init(xdr_out *x, size_t initial);
void xout_free(xdr_out *x);
void xout_reset(xdr_out *x);
/* Ensures n more bytes can be written; returns a pointer to them. */
uint8_t *xout_reserve(xdr_out *x, size_t n);
void xout_u32(xdr_out *x, uint32_t v);
void xout_u64(xdr_out *x, uint64_t v);
static inline void xout_bool(xdr_out *x, bool v) { xout_u32(x, v ? 1 : 0); }
void xout_fixed(xdr_out *x, const void *data, size_t n);
void xout_var(xdr_out *x, const void *data, size_t n);
/* Overwrites a previously written u32 at byte offset `at`. */
void xout_put_u32_at(xdr_out *x, size_t at, uint32_t v);

#endif /* FSMAC_XDR_H */
