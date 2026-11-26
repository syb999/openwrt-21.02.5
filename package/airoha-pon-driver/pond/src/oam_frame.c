/* SPDX-License-Identifier: GPL-2.0-only */
#include "oam_frame.h"

#include <string.h>

const uint8_t oam_slow_protocols_multicast[6] = { 0x01, 0x80, 0xc2, 0x00, 0x00, 0x02 };

#define OAM_ETHERTYPE_OFF	12
#define OAM_ETHERTYPE_HI	0x88
#define OAM_ETHERTYPE_LO	0x09
#define OAM_SUBTYPE_OFF		14
#define OAM_SUBTYPE		0x03

size_t oam_frame_len(size_t payload_len)
{
	size_t len = OAM_HEADER_LEN + payload_len;

	return len < OAM_MIN_ETHERNET_FRAME_LEN ? OAM_MIN_ETHERNET_FRAME_LEN : len;
}

int oam_pdu_parse(const uint8_t *frame, size_t len, struct oam_pdu *out)
{
	if (len < OAM_HEADER_LEN)
		return -1;
	if (frame[OAM_ETHERTYPE_OFF] != OAM_ETHERTYPE_HI ||
	    frame[OAM_ETHERTYPE_OFF + 1] != OAM_ETHERTYPE_LO)
		return -1;
	if (frame[OAM_SUBTYPE_OFF] != OAM_SUBTYPE)
		return -1;

	/* DA(6) SA(6) ethertype(2) subtype(1) flags(2) code(1) */
	out->code = frame[OAM_HEADER_LEN - 1];
	out->payload = frame + OAM_HEADER_LEN;
	out->payload_len = len - OAM_HEADER_LEN;
	return 0;
}

int oam_pdu_build(const uint8_t source[6], uint16_t flags, uint8_t code,
		  const uint8_t *payload, size_t payload_len,
		  uint8_t *out, size_t out_cap)
{
	size_t total = oam_frame_len(payload_len);

	if (total > out_cap)
		return -1;
	memcpy(out, oam_slow_protocols_multicast, sizeof(oam_slow_protocols_multicast));
	memcpy(out + 6, source, 6);
	out[OAM_ETHERTYPE_OFF] = OAM_ETHERTYPE_HI;
	out[OAM_ETHERTYPE_OFF + 1] = OAM_ETHERTYPE_LO;
	out[OAM_SUBTYPE_OFF] = OAM_SUBTYPE;
	out[15] = (uint8_t)(flags >> 8);
	out[16] = (uint8_t)flags;
	out[17] = code;
	if (payload_len)
		memcpy(out + OAM_HEADER_LEN, payload, payload_len);
	if (total > OAM_HEADER_LEN + payload_len)
		memset(out + OAM_HEADER_LEN + payload_len, 0,
		       total - OAM_HEADER_LEN - payload_len);
	return (int)total;
}
