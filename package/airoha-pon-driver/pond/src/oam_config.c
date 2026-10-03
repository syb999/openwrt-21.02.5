/* SPDX-License-Identifier: GPL-2.0-only */
#include "oam_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oam_fixed_width(const uint8_t *value, size_t len, uint8_t *out, size_t width)
{
	size_t copied = len < width ? len : width;

	memset(out, 0, width);
	if (copied)
		memcpy(out, value, copied);
}

void oam_fixed_width_right(const uint8_t *value, size_t len, uint8_t *out, size_t width)
{
	size_t copied = len < width ? len : width;

	memset(out, 0, width);
	if (copied)
		memcpy(out + width - copied, value, copied);
}

void oam_config_defaults(struct oam_config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->operator, sizeof(c->operator), "ctc");
	c->ctc_oui[0] = 0x11;
	c->ctc_oui[1] = 0x11;
	c->ctc_oui[2] = 0x11;
	c->ctc_versions[0] = 0x21;
	c->ctc_versions[1] = 0x30;
	c->n_ctc_versions = 2;
	memcpy(c->vendor_id, "OWRT", 4);
	c->vendor_id_len = 4;
	memcpy(c->model, "AN75", 4);
	c->model_len = 4;
	memcpy(c->equipment_id, "AN75", 4);
	c->equipment_id_len = 4;
	memcpy(c->hardware_version, "AN7581", 6);
	c->hardware_version_len = 6;
	memcpy(c->software_version, "OpenWrt", 7);
	c->software_version_len = 7;
	memcpy(c->firmware_version, "OpenWrt", 7);
	c->firmware_version_len = 7;
	c->chipset_id[2] = 0x75;
	c->chipset_id[3] = 0x81;
	c->ge_ports = 1;
}

bool oam_config_ctc_enabled(const struct oam_config *c)
{
	return !strcmp(c->operator, "ctc");
}

static void set_text(uint8_t *dst, size_t *dst_len, const char *src, size_t max)
{
	size_t n = strlen(src);

	if (n >= max)
		n = max - 1;
	memcpy(dst, src, n);
	*dst_len = n;
}

static int hex_nibble(char ch)
{
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

/* "aa:bb:cc" or "aabbcc" */
static int parse_oui(const char *text, uint8_t out[3])
{
	size_t n = 0;

	for (const char *p = text; *p && n < 3; ) {
		int hi, lo;

		if (*p == ':' || *p == '-' || *p == '.') {
			p++;
			continue;
		}
		hi = hex_nibble(p[0]);
		lo = p[1] ? hex_nibble(p[1]) : -1;
		if (hi < 0 || lo < 0)
			return -1;
		out[n++] = (uint8_t)((hi << 4) | lo);
		p += 2;
	}
	return n == 3 ? 0 : -1;
}

/* 16 hex digits */
static int parse_hex_array(const char *text, uint8_t *out, size_t n)
{
	size_t k = 0;

	for (const char *p = text; *p; ) {
		int hi, lo;

		if (*p == ':' || *p == '-' || *p == ' ') {
			p++;
			continue;
		}
		hi = hex_nibble(p[0]);
		lo = p[1] ? hex_nibble(p[1]) : -1;
		if (hi < 0 || lo < 0 || k >= n)
			return -1;
		out[k++] = (uint8_t)((hi << 4) | lo);
		p += 2;
	}
	return k == n ? 0 : -1;
}

/* whitespace/comma separated hex bytes, de-duplicated */
static int parse_versions(const char *text, uint8_t *out, size_t cap, size_t *n_out)
{
	const char *p = text;
	size_t n = 0;

	*n_out = 0;
	while (*p) {
		char field[16];
		size_t flen = 0;
		unsigned long v;

		while (*p && (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n'))
			p++;
		if (!*p)
			break;
		while (*p && !(*p == ' ' || *p == '\t' || *p == ',' || *p == '\n')) {
			if (flen + 1 < sizeof(field))
				field[flen++] = *p;
			p++;
		}
		field[flen] = '\0';
		{
			const char *f = field;
			char *end;
			int i;

			if (!strncmp(f, "0x", 2))
				f += 2;
			/* all digits must be hex */
			for (i = 0; f[i]; i++)
				if (hex_nibble(f[i]) < 0)
					return -1;
			v = strtoul(f, &end, 16);
			if (*end || v > 0xff)
				return -1;
		}
		{
			int dup = 0;

			for (size_t k = 0; k < n; k++)
				if (out[k] == (uint8_t)v)
					dup = 1;
			if (!dup && n < cap)
				out[n++] = (uint8_t)v;
		}
	}
	*n_out = n;
	return 0;
}

int oam_config_from_section(struct oam_config *c, const struct pond_section *s,
			    char *err, size_t err_len)
{
	const char *v;

	oam_config_defaults(c);
	if (!s)
		return 0;

	v = pond_section_option(s, "operator");
	if (v && *v)
		snprintf(c->operator, sizeof(c->operator), "%s", v);

	if (oam_config_ctc_enabled(c)) {
		v = pond_section_option(s, "ctc_oui");
		if (v && *v && parse_oui(v, c->ctc_oui) < 0) {
			snprintf(err, err_len, "ctc_oui is not a three-byte hex value");
			return -1;
		}
		v = pond_section_option(s, "ctc_versions");
		if (v && *v &&
		    parse_versions(v, c->ctc_versions, OAM_VERSIONS_MAX, &c->n_ctc_versions) < 0) {
			snprintf(err, err_len, "ctc_versions contains a non-hex value");
			return -1;
		}
	}

	v = pond_section_option(s, "vendor_id");
	if (v && *v) set_text(c->vendor_id, &c->vendor_id_len, v, OAM_TEXT_MAX);
	v = pond_section_option(s, "model");
	if (v && *v) set_text(c->model, &c->model_len, v, OAM_TEXT_MAX);
	v = pond_section_option(s, "equipment_id");
	if (v && *v) set_text(c->equipment_id, &c->equipment_id_len, v, OAM_TEXT_MAX);
	v = pond_section_option(s, "hardware_version");
	if (v && *v) set_text(c->hardware_version, &c->hardware_version_len, v, OAM_TEXT_MAX);
	v = pond_section_option(s, "software_version");
	if (v && *v) set_text(c->software_version, &c->software_version_len, v, OAM_TEXT_MAX);
	v = pond_section_option(s, "firmware_version");
	if (v && *v) set_text(c->firmware_version, &c->firmware_version_len, v, OAM_TEXT_MAX);
	v = pond_section_option(s, "loid");
	if (v && *v) set_text(c->loid, &c->loid_len, v, OAM_TEXT_MAX);
	v = pond_section_option(s, "loid_password");
	if (v && *v) set_text(c->loid_password, &c->loid_password_len, v, OAM_TEXT_MAX);

	v = pond_section_option(s, "chipset_id");
	if (v && *v && parse_hex_array(v, c->chipset_id, OAM_CHIPSET_ID_LEN) < 0) {
		snprintf(err, err_len, "chipset_id must be eight hex bytes");
		return -1;
	}
	v = pond_section_option(s, "ge_ports");
	if (v && *v) {
		char *end;
		unsigned long n = strtoul(v, &end, 10);

		if (*end || n > 0xff) {
			snprintf(err, err_len, "ge_ports must be an integer");
			return -1;
		}
		c->ge_ports = (uint8_t)n;
	}

	if (strcmp(c->operator, "ieee") && strcmp(c->operator, "ctc")) {
		snprintf(err, err_len, "operator must be ieee or ctc");
		return -1;
	}
	if (oam_config_ctc_enabled(c) && !c->n_ctc_versions) {
		snprintf(err, err_len, "ctc_versions is empty");
		return -1;
	}
	return 0;
}
