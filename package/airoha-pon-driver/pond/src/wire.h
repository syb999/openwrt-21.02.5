/* SPDX-License-Identifier: GPL-2.0-only */
/* Network-order byte reader/writer (port of airoha-pon-daemons wire.rs). */
#ifndef POND_WIRE_H
#define POND_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rd {
	const uint8_t *buf;
	size_t len;
	size_t off;
};

struct wr {
	uint8_t *buf;
	size_t len;	/* used bytes */
	size_t cap;
	int err;
};

void rd_init(struct rd *r, const uint8_t *buf, size_t len);
bool rd_u8(struct rd *r, uint8_t *out);
bool rd_be16(struct rd *r, uint16_t *out);
bool rd_be32(struct rd *r, uint32_t *out);
bool rd_bytes(struct rd *r, size_t n, const uint8_t **out);
size_t rd_remaining(const struct rd *r);

void wr_init(struct wr *w, uint8_t *buf, size_t cap);
void wr_u8(struct wr *w, uint8_t v);
void wr_be16(struct wr *w, uint16_t v);
void wr_be32(struct wr *w, uint32_t v);
void wr_bytes(struct wr *w, const uint8_t *v, size_t n);
void wr_zeros(struct wr *w, size_t n);

#endif
