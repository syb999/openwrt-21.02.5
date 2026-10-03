/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IEEE 802.3ah OAM discovery session.
 * Port of airoha-pon-daemons oam/ieee.rs.
 */
#ifndef POND_OAM_IEEE_H
#define POND_OAM_IEEE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "oam_config.h"
#include "oam_ctc.h"

#define OAM_INFO_END			0x00
#define OAM_INFO_LOCAL			0x01
#define OAM_INFO_REMOTE			0x02
#define OAM_INFO_ORGANIZATION_SPECIFIC	0xfe
#define OAM_INFO_LENGTH			16

struct ieee_session {
	uint16_t revision;
	uint64_t information_rx;
	uint64_t event_rx;
};

void ieee_session_init(struct ieee_session *s);

/*
 * Handle an Information OAMPDU payload: pick the Local Information TLV, feed any
 * organization-specific TLV to the CTC session, and build our Information reply
 * (Local Information + echoed Remote Information + optional CTC response + END).
 * Returns 0 on success; <0 when the payload is malformed.
 */
int ieee_handle_information(struct ieee_session *s, const uint8_t *payload, size_t len,
			    const uint8_t source_mac[6], const struct oam_config *cfg,
			    struct ctc_session *ctc,
			    uint8_t *out, size_t out_cap, size_t *out_len);

#endif
