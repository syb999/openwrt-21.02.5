/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OMCI frame decoding and response encoding (ITU-T G.988).
 * Port of airoha-pon-daemons omci/protocol.rs.
 *
 * The QDMA descriptor path appends the outbound MIC, so message contents are
 * encoded exactly as the Rust implementation produced them.
 */
#ifndef POND_OMCI_PROTOCOL_H
#define POND_OMCI_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OMCI_BASELINE_LEN		44
#define OMCI_BASELINE_WITH_MIC_LEN	48
#define OMCI_EXTENDED_HEADER_LEN	10
#define OMCI_EXTENDED_MAX_PDU_LEN	1980
#define OMCI_DEVICE_ID_BASELINE		0x0a
#define OMCI_DEVICE_ID_EXTENDED		0x0b
#define OMCI_BASELINE_VALUE_CAPACITY	29
#define OMCI_EXTENDED_GET_VALUE_CAPACITY \
	(OMCI_EXTENDED_MAX_PDU_LEN - OMCI_EXTENDED_HEADER_LEN - 7)
#define OMCI_EXTENDED_GET_NEXT_VALUE_CAPACITY \
	(OMCI_EXTENDED_MAX_PDU_LEN - OMCI_EXTENDED_HEADER_LEN - 3)

/* operation results */
#define OMCI_RESULT_SUCCESS		0x00
#define OMCI_RESULT_COMMAND_NOT_SUPPORTED 0x02
#define OMCI_RESULT_PARAMETER_ERROR	0x03
#define OMCI_RESULT_UNKNOWN_ME		0x04
#define OMCI_RESULT_UNKNOWN_INSTANCE	0x05
#define OMCI_RESULT_INSTANCE_EXISTS	0x07
#define OMCI_RESULT_ATTRIBUTE_FAILED	0x09

/* message types / actions (message_type & 0x1f) */
#define OMCI_ACTION_CREATE		0x04
#define OMCI_ACTION_DELETE		0x06
#define OMCI_ACTION_SET			0x08
#define OMCI_ACTION_GET			0x09
#define OMCI_ACTION_GET_ALL_ALARMS	0x0b
#define OMCI_ACTION_GET_ALL_ALARMS_NEXT	0x0c
#define OMCI_ACTION_MIB_UPLOAD		0x0d
#define OMCI_ACTION_MIB_UPLOAD_NEXT	0x0e
#define OMCI_ACTION_MIB_RESET		0x0f
#define OMCI_ACTION_ATTRIBUTE_VALUE_CHANGE 0x11
#define OMCI_ACTION_SYNCHRONIZE_TIME	0x18
#define OMCI_ACTION_GET_NEXT		0x1a
#define OMCI_ACTION_GET_CURRENT_DATA	0x1c
#define OMCI_ACTION_SET_TABLE		0x1d

enum omci_encoding {
	OMCI_BASELINE,
	OMCI_EXTENDED,
};

struct omci_request {
	enum omci_encoding enc;
	uint16_t tci;
	uint8_t message_type;
	uint8_t action;
	uint16_t class_id;
	uint16_t entity_id;
	uint16_t attribute_mask;
	const uint8_t *payload;		/* full contents (mask + content) */
	size_t payload_len;
	const uint8_t *content;		/* after the Set/Get mask */
	size_t content_len;
};

struct omci_response {
	uint8_t bytes[OMCI_EXTENDED_MAX_PDU_LEN];
	size_t len;
	int has_result;
	uint8_t result;
};

/* 0 on success, <0 on parse error */
int omci_request_parse(struct omci_request *req, const uint8_t *frame, size_t len);

const char *omci_selector_name(const struct omci_request *req);
size_t omci_get_value_capacity(const struct omci_request *req);
size_t omci_get_next_value_capacity(const struct omci_request *req);

int omci_response_new(const struct omci_request *req, uint8_t result,
		      struct omci_response *out);
int omci_response_get_success(const struct omci_request *req, uint16_t returned_mask,
			      const uint8_t *values, size_t n,
			      struct omci_response *out);
int omci_response_get_next_success(const struct omci_request *req, uint16_t returned_mask,
				   const uint8_t *values, size_t n,
				   struct omci_response *out);
int omci_response_mib_upload(const struct omci_request *req, uint16_t command_count,
			     struct omci_response *out);
int omci_response_get_all_alarms(const struct omci_request *req, uint16_t command_count,
				 struct omci_response *out);
int omci_response_get_all_alarms_next_empty(const struct omci_request *req,
					    struct omci_response *out);
int omci_response_mib_upload_next(const struct omci_request *req, uint16_t class_id,
				  uint16_t entity_id, uint16_t attribute_mask,
				  const uint8_t *values, size_t n,
				  struct omci_response *out);
int omci_response_sync_time(const struct omci_request *req, struct omci_response *out);
int omci_response_avc(uint16_t tci, uint16_t class_id, uint16_t entity_id,
		      uint16_t attribute_mask, const uint8_t *values, size_t n,
		      struct omci_response *out);

/* Bit for 1-based attribute index (0x8000 >> (index-1)); 0 when out of range. */
uint16_t omci_attribute_bit(uint8_t index);

#endif
