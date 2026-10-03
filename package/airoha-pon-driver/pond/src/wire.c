/* SPDX-License-Identifier: GPL-2.0-only */
#include "wire.h"
#include <string.h>

void rd_init(struct rd *r, const uint8_t *buf, size_t len)
{
	r->buf = buf;
	r->len = len;
	r->off = 0;
}

size_t rd_remaining(const struct rd *r)
{
	return r->len - r->off;
}

bool rd_bytes(struct rd *r, size_t n, const uint8_t **out)
{
	if (r->off + n > r->len)
		return false;
	*out = r->buf + r->off;
	r->off += n;
	return true;
}

bool rd_u8(struct rd *r, uint8_t *out)
{
	const uint8_t *p;

	if (!rd_bytes(r, 1, &p))
		return false;
	*out = p[0];
	return true;
}

bool rd_be16(struct rd *r, uint16_t *out)
{
	const uint8_t *p;

	if (!rd_bytes(r, 2, &p))
		return false;
	*out = (uint16_t)((p[0] << 8) | p[1]);
	return true;
}

bool rd_be32(struct rd *r, uint32_t *out)
{
	const uint8_t *p;

	if (!rd_bytes(r, 4, &p))
		return false;
	*out = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
	return true;
}

void wr_init(struct wr *w, uint8_t *buf, size_t cap)
{
	w->buf = buf;
	w->len = 0;
	w->cap = cap;
	w->err = 0;
}

void wr_bytes(struct wr *w, const uint8_t *v, size_t n)
{
	if (w->err)
		return;
	if (w->len + n > w->cap) {
		w->err = 1;
		return;
	}
	memcpy(w->buf + w->len, v, n);
	w->len += n;
}

void wr_u8(struct wr *w, uint8_t v)
{
	wr_bytes(w, &v, 1);
}

void wr_be16(struct wr *w, uint16_t v)
{
	uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };

	wr_bytes(w, b, 2);
}

void wr_be32(struct wr *w, uint32_t v)
{
	uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
			 (uint8_t)(v >> 8), (uint8_t)v };

	wr_bytes(w, b, 4);
}

void wr_zeros(struct wr *w, size_t n)
{
	static const uint8_t zero[64];

	while (n) {
		size_t chunk = n > sizeof(zero) ? sizeof(zero) : n;

		wr_bytes(w, zero, chunk);
		n -= chunk;
	}
}
