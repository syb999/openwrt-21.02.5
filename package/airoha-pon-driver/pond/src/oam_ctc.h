/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * CTC (China Telecom) EPON OAM organization-specific protocol.
 * Port of airoha-pon-daemons oam/ctc.rs.
 */
#ifndef POND_OAM_CTC_H
#define POND_OAM_CTC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "oam_config.h"

#define CTC_OUI_LEN			3
#define CTC_EVENT_MAX			8
#define CTC_EVENT_LEN			200
#define CTC_VLAN_RAW_MAX		64
#define CTC_VLANS_MAX			16

enum ctc_discovery {
	CTC_PASSIVE_WAIT,
	CTC_OFFERED,
	CTC_OPERATIONAL,
};

struct ctc_object {
	uint8_t branch;
	uint16_t leaf;
	uint32_t index;
	size_t encoded_len;
};

struct ctc_vlan {
	uint32_t object_index;
	uint8_t mode;
	uint8_t raw[CTC_VLAN_RAW_MAX];
	size_t raw_len;
};

struct ctc_session {
	enum ctc_discovery state;
	bool has_version;
	uint8_t version;
	const char *authentication;
	bool has_failure;
	uint8_t failure;
	uint64_t get_requests;
	uint64_t set_requests;
	uint64_t unsupported_requests;
	bool has_vlan;
	struct ctc_vlan vlan;
};

struct ctc_outcome {
	/* Response body (organization-specific data after the OUI+opcode header). */
	uint8_t response[1024];
	size_t response_len;
	bool has_response;
	char events[CTC_EVENT_MAX][CTC_EVENT_LEN];
	size_t n_events;
};

void ctc_session_init(struct ctc_session *s);

/*
 * Handle the organization-specific Information TLV.
 * 0 on success (the response TLV, when produced, lands in `response`).
 */
int ctc_handle_information_tlv(struct ctc_session *s, const uint8_t *tlv, size_t tlv_len,
			       const struct oam_config *cfg,
			       uint8_t *response, size_t response_cap, size_t *response_len,
			       bool *has_response);

/* Handle an organization-specific OAMPDU payload (OUI + opcode + data). */
int ctc_handle_organization_pdu(struct ctc_session *s, const uint8_t *payload,
				size_t payload_len, const struct oam_config *cfg,
				const uint8_t source_mac[6], struct ctc_outcome *out);

const char *ctc_discovery_state_name(const struct ctc_session *s);

/* Variable GET/SET over the organization-specific body (exposed for testing). */
int ctc_handle_get(struct ctc_session *s, const uint8_t *data, size_t data_len,
		   const struct oam_config *cfg, const uint8_t source_mac[6],
		   struct ctc_outcome *out);
int ctc_handle_set(struct ctc_session *s, const uint8_t *data, size_t data_len,
		   struct ctc_outcome *out);

/* VLAN ids decoded from the last recorded Set (sorted, de-duplicated). */
size_t ctc_vlan_ids(const struct ctc_session *s, uint16_t *out, size_t cap);

#endif
