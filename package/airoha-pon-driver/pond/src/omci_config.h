/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OMCI identity configuration (UCI omci section).
 * Port of airoha-pon-daemons omci/config.rs.
 */
#ifndef POND_OMCI_CONFIG_H
#define POND_OMCI_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define IDENTITY_TEXT_MAX 64

struct identity_config {
	uint8_t omcc_version;
	bool disable_enhanced_security;
	/* Seconds a data path may wait for PLOAM Alloc-IDs before failing; 0 never fails. */
	uint32_t alloc_id_timeout;
	uint8_t vendor_id[IDENTITY_TEXT_MAX];
	size_t vendor_id_len;
	uint8_t equipment_id[IDENTITY_TEXT_MAX];
	size_t equipment_id_len;
	uint8_t hardware_version[IDENTITY_TEXT_MAX];
	size_t hardware_version_len;
	uint8_t software_version[IDENTITY_TEXT_MAX];
	size_t software_version_len;
	uint8_t loid[IDENTITY_TEXT_MAX];
	size_t loid_len;
	uint8_t loid_password[IDENTITY_TEXT_MAX];
	size_t loid_password_len;
	uint8_t operator_id[IDENTITY_TEXT_MAX];
	size_t operator_id_len;
	uint8_t serial_number[IDENTITY_TEXT_MAX];
	size_t serial_number_len;
};

void identity_defaults(struct identity_config *c);
void identity_from_section(struct identity_config *c, const struct pond_section *s);

/* The ONU-G serial number (8 bytes) derived from serial_number or vendor_id. */
void identity_onu_g_serial(const struct identity_config *c, uint8_t out[8]);

/* Copy at most `width` bytes, zero padded. */
void identity_fixed_width(const uint8_t *value, size_t len, uint8_t *out, size_t width);

/* Hex decoding: 0 on success, <0 on odd length or non-hex digit. */
int identity_decode_hex(const char *input, uint8_t *out, size_t out_cap, size_t *out_len);

#endif
