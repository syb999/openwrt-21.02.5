/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EPON OAM identity configuration (UCI oam section).
 * Port of airoha-pon-daemons oam/config.rs.
 */
#ifndef POND_OAM_CONFIG_H
#define POND_OAM_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

#define OAM_TEXT_MAX	32
#define OAM_VERSIONS_MAX 16
#define OAM_CHIPSET_ID_LEN 8

struct oam_config {
	char operator[8];		/* "ieee" or "ctc" */
	uint8_t ctc_oui[3];
	uint8_t ctc_versions[OAM_VERSIONS_MAX];
	size_t n_ctc_versions;
	uint8_t vendor_id[OAM_TEXT_MAX];
	size_t vendor_id_len;
	uint8_t model[OAM_TEXT_MAX];
	size_t model_len;
	uint8_t equipment_id[OAM_TEXT_MAX];
	size_t equipment_id_len;
	uint8_t hardware_version[OAM_TEXT_MAX];
	size_t hardware_version_len;
	uint8_t software_version[OAM_TEXT_MAX];
	size_t software_version_len;
	uint8_t firmware_version[OAM_TEXT_MAX];
	size_t firmware_version_len;
	uint8_t chipset_id[OAM_CHIPSET_ID_LEN];
	uint8_t loid[OAM_TEXT_MAX];
	size_t loid_len;
	uint8_t loid_password[OAM_TEXT_MAX];
	size_t loid_password_len;
	uint8_t ge_ports;
};

void oam_config_defaults(struct oam_config *c);
/* 0 on success, <0 on invalid data (operator / ctc_versions / ge_ports / hex). */
int oam_config_from_section(struct oam_config *c, const struct pond_section *s,
			    char *err, size_t err_len);

bool oam_config_ctc_enabled(const struct oam_config *c);

/* Fixed-width copies (left-aligned / right-aligned, zero padded). */
void oam_fixed_width(const uint8_t *value, size_t len, uint8_t *out, size_t width);
void oam_fixed_width_right(const uint8_t *value, size_t len, uint8_t *out, size_t width);

#endif
