/*
 * Minimal XDR (RFC 4506) encoding and decoding over memory buffers.
 */
#include <stdlib.h>
#include <string.h>

#include "xdr.h"

static inline size_t pad4(size_t n)
{
	return (n + 3) & ~(size_t)3;
}

uint32_t xin_u32(xdr_in *x)
{
	const uint8_t *p;

	if (x->err || x->len - x->pos < 4) {
		x->err = true;
		return 0;
	}
	p = x->buf + x->pos;
	x->pos += 4;
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

uint64_t xin_u64(xdr_in *x)
{
	uint64_t hi = xin_u32(x);

	return (hi << 32) | xin_u32(x);
}

const uint8_t *xin_fixed(xdr_in *x, size_t n)
{
	const uint8_t *p;
	size_t padded = pad4(n);

	if (x->err || padded < n || x->len - x->pos < padded) {
		x->err = true;
		return NULL;
	}
	p = x->buf + x->pos;
	x->pos += padded;
	return p;
}

const uint8_t *xin_var(xdr_in *x, size_t max, size_t *len)
{
	uint32_t n = xin_u32(x);

	*len = 0;
	if (x->err)
		return NULL;
	if (n > max) {
		x->err = true;
		return NULL;
	}
	*len = n;
	return xin_fixed(x, n);
}

void xin_skip_var(xdr_in *x, size_t max)
{
	size_t len;

	(void)xin_var(x, max, &len);
}

void xout_init(xdr_out *x, size_t initial)
{
	x->buf = malloc(initial ? initial : 256);
	x->cap = x->buf ? (initial ? initial : 256) : 0;
	x->len = 0;
	x->err = x->buf == NULL;
}

void xout_free(xdr_out *x)
{
	free(x->buf);
	x->buf = NULL;
	x->cap = x->len = 0;
}

void xout_reset(xdr_out *x)
{
	x->len = 0;
	x->err = x->buf == NULL;
}

uint8_t *xout_reserve(xdr_out *x, size_t n)
{
	if (x->err)
		return NULL;
	if (x->cap - x->len < n) {
		size_t ncap = x->cap ? x->cap : 256;
		uint8_t *nb;

		while (ncap - x->len < n)
			ncap *= 2;
		nb = realloc(x->buf, ncap);
		if (!nb) {
			x->err = true;
			return NULL;
		}
		x->buf = nb;
		x->cap = ncap;
	}
	return x->buf + x->len;
}

void xout_u32(xdr_out *x, uint32_t v)
{
	uint8_t *p = xout_reserve(x, 4);

	if (!p)
		return;
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
	x->len += 4;
}

void xout_u64(xdr_out *x, uint64_t v)
{
	xout_u32(x, (uint32_t)(v >> 32));
	xout_u32(x, (uint32_t)v);
}

void xout_fixed(xdr_out *x, const void *data, size_t n)
{
	size_t padded = pad4(n);
	uint8_t *p = xout_reserve(x, padded);

	if (!p)
		return;
	if (n)
		memcpy(p, data, n);
	memset(p + n, 0, padded - n);
	x->len += padded;
}

void xout_var(xdr_out *x, const void *data, size_t n)
{
	xout_u32(x, (uint32_t)n);
	xout_fixed(x, data, n);
}

void xout_put_u32_at(xdr_out *x, size_t at, uint32_t v)
{
	if (x->err || at + 4 > x->len)
		return;
	x->buf[at] = (uint8_t)(v >> 24);
	x->buf[at + 1] = (uint8_t)(v >> 16);
	x->buf[at + 2] = (uint8_t)(v >> 8);
	x->buf[at + 3] = (uint8_t)v;
}
