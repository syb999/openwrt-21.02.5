/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void identity_fixed_width(const uint8_t *value, size_t len, uint8_t *out, size_t width)
{
	size_t copied = len < width ? len : width;

	memset(out, 0, width);
	if (copied)
		memcpy(out, value, copied);
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int identity_decode_hex(const char *input, uint8_t *out, size_t out_cap, size_t *out_len)
{
	size_t n = 0;

	*out_len = 0;
	if (strlen(input) % 2)
		return -1;
	for (size_t i = 0; input[i]; i += 2) {
		int hi = hex_nibble(input[i]), lo = hex_nibble(input[i + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		if (n >= out_cap)
			return -1;
		out[n++] = (uint8_t)((hi << 4) | lo);
	}
	*out_len = n;
	return 0;
}

void identity_defaults(struct identity_config *c)
{
	memset(c, 0, sizeof(*c));
	c->omcc_version = 0xb0;
	c->disable_enhanced_security = false;
	c->alloc_id_timeout = 30;
	memcpy(c->vendor_id, "OWRT", 4);
	c->vendor_id_len = 4;
}

static void set_text(uint8_t *dst, size_t *dst_len, const char *src)
{
	size_t n = strlen(src);

	if (n >= IDENTITY_TEXT_MAX)
		n = IDENTITY_TEXT_MAX - 1;
	memcpy(dst, src, n);
	*dst_len = n;
}

void identity_from_section(struct identity_config *c, const struct pond_section *s)
{
	const char *v;

	identity_defaults(c);
	if (!s)
		return;
	v = pond_section_option(s, "omcc_version");
	if (v && !strcmp(v, "0x86"))
		c->omcc_version = 0x86;
	v = pond_section_option(s, "disable_enhanced_security");
	if (v && !strcmp(v, "1"))
		c->disable_enhanced_security = true;
	v = pond_section_option(s, "alloc_id_timeout");
	if (v && *v) {
		char *end;
		unsigned long secs = strtoul(v, &end, 10);

		if (!*end)
			c->alloc_id_timeout = (uint32_t)secs;
		else
			fprintf(stderr, "Invalid OMCI alloc_id_timeout '%s'; using %u s\n",
				v, c->alloc_id_timeout);
	}

	v = pond_section_option(s, "vendor_id");
	if (v && *v) set_text(c->vendor_id, &c->vendor_id_len, v);
	v = pond_section_option(s, "equipment_id");
	if (v && *v) set_text(c->equipment_id, &c->equipment_id_len, v);
	v = pond_section_option(s, "hardware_version");
	if (v && *v) set_text(c->hardware_version, &c->hardware_version_len, v);
	v = pond_section_option(s, "software_version");
	if (v && *v) set_text(c->software_version, &c->software_version_len, v);
	v = pond_section_option(s, "loid");
	if (v && *v) set_text(c->loid, &c->loid_len, v);
	v = pond_section_option(s, "loid_password");
	if (v && *v) set_text(c->loid_password, &c->loid_password_len, v);
	v = pond_section_option(s, "operator_id");
	if (v && *v) set_text(c->operator_id, &c->operator_id_len, v);
	v = pond_section_option(s, "serial_number");
	if (v && *v) set_text(c->serial_number, &c->serial_number_len, v);
}

void identity_onu_g_serial(const struct identity_config *c, uint8_t out[8])
{
	uint8_t vendor4[4];

	/* fallback: vendor id padded to 4 bytes followed by a zero VSSN */
	identity_fixed_width(c->vendor_id, c->vendor_id_len, vendor4, 4);
	memcpy(out, vendor4, 4);
	memset(out + 4, 0, 4);

	if (!c->serial_number_len)
		return;
	if (c->serial_number_len == 8) {
		memcpy(out, c->serial_number, 8);
		return;
	}

	{
		char text[IDENTITY_TEXT_MAX + 1];
		size_t n = c->serial_number_len;
		const char *raw;
		uint8_t decoded[8];
		size_t dlen = 0;

		if (n > IDENTITY_TEXT_MAX)
			n = IDENTITY_TEXT_MAX;
		memcpy(text, c->serial_number, n);
		text[n] = '\0';
		while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' ||
			     text[n - 1] == '\n' || text[n - 1] == '\r'))
			text[--n] = '\0';
		raw = !strncmp(text, "hex:", 4) ? text + 4 : text;
		if (n == 16 && !identity_decode_hex(raw, decoded, 8, &dlen) && dlen == 8) {
			memcpy(out, decoded, 8);
			return;
		}
		if (n == 12 && !identity_decode_hex(text + 4, decoded, 8, &dlen) && dlen == 4) {
			memcpy(out, text, 4);
			memcpy(out + 4, decoded, 4);
			return;
		}
	}
	fprintf(stderr, "Invalid OMCI serial number; using the vendor ID with a zero VSSN\n");
}
