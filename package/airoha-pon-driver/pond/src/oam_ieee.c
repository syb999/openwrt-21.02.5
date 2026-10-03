/* SPDX-License-Identifier: GPL-2.0-only */
#include "oam_ieee.h"

#include <string.h>

void ieee_session_init(struct ieee_session *s)
{
	memset(s, 0, sizeof(*s));
	s->revision = 1;
}

/* Local Information TLV: type/revision/flags/MTU/source-MAC-prefix padding. */
static size_t local_information(uint16_t revision, const uint8_t source_mac[6],
				uint8_t *out, size_t out_cap)
{
	if (out_cap < OAM_INFO_LENGTH)
		return 0;
	out[0] = OAM_INFO_LOCAL;
	out[1] = OAM_INFO_LENGTH;
	out[2] = 1;
	out[3] = (uint8_t)(revision >> 8);
	out[4] = (uint8_t)revision;
	out[5] = 0;
	/* Passive mode advertises Link Event support. */
	out[6] = 0x08;
	out[7] = (uint8_t)(1500 >> 8);
	out[8] = (uint8_t)1500;
	memcpy(out + 9, source_mac, 3);
	memset(out + 12, 0, 4);
	return OAM_INFO_LENGTH;
}

int ieee_handle_information(struct ieee_session *s, const uint8_t *payload, size_t len,
			    const uint8_t source_mac[6], const struct oam_config *cfg,
			    struct ctc_session *ctc,
			    uint8_t *out, size_t out_cap, size_t *out_len)
{
	size_t offset = 0, w = 0;
	uint8_t remote[OAM_INFO_LENGTH];
	bool have_remote = false;
	uint8_t ctc_response[512];
	size_t ctc_len = 0;
	bool have_ctc = false;

	*out_len = 0;
	s->information_rx += 1;

	while (offset < len) {
		uint8_t info_type = payload[offset];
		size_t length;

		if (info_type == OAM_INFO_END)
			break;
		if (offset + 2 > len)
			return -1;	/* TLV header truncated */
		length = payload[offset + 1];
		if (length < 2 || offset + length > len)
			return -2;	/* invalid length */
		if (info_type == OAM_INFO_LOCAL && length == OAM_INFO_LENGTH) {
			memcpy(remote, payload + offset, OAM_INFO_LENGTH);
			have_remote = true;
		} else if (info_type == OAM_INFO_ORGANIZATION_SPECIFIC && oam_config_ctc_enabled(cfg)) {
			if (ctc_handle_information_tlv(ctc, payload + offset, length, cfg,
						       ctc_response, sizeof(ctc_response),
						       &ctc_len, &have_ctc) < 0)
				return -3;
		}
		offset += length;
	}

	if (!have_remote)
		return -4;	/* no Local Information TLV */

	w += local_information(s->revision, source_mac, out + w, out_cap - w);
	if (!w)
		return -5;
	out[w++] = OAM_INFO_REMOTE;
	if (w + (OAM_INFO_LENGTH - 1) > out_cap)
		return -6;
	memcpy(out + w, remote + 1, OAM_INFO_LENGTH - 1);
	w += OAM_INFO_LENGTH - 1;
	if (have_ctc) {
		if (w + ctc_len + 1 > out_cap)
			return -7;
		memcpy(out + w, ctc_response, ctc_len);
		w += ctc_len;
	}
	out[w++] = OAM_INFO_END;
	*out_len = w;
	return 0;
}
