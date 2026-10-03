/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci_protocol.h"
#include "wire.h"
#include <errno.h>
#include <string.h>

static int parse_baseline(struct omci_request *req, const uint8_t *frame, size_t len)
{
	if (len != OMCI_BASELINE_LEN && len != OMCI_BASELINE_WITH_MIC_LEN)
		return -EINVAL;
	req->payload = frame + 8;
	req->payload_len = 32;			/* frame[8..40] */
	req->attribute_mask = (uint16_t)((frame[8] << 8) | frame[9]);
	req->content = frame + 10;
	req->content_len = 30;
	return 0;
}

static int parse_extended(struct omci_request *req, const uint8_t *frame, size_t len)
{
	size_t declared, pdu_length;

	if (len < OMCI_EXTENDED_HEADER_LEN)
		return -EINVAL;
	declared = (size_t)((frame[8] << 8) | frame[9]);
	pdu_length = OMCI_EXTENDED_HEADER_LEN + declared;
	if (pdu_length > OMCI_EXTENDED_MAX_PDU_LEN ||
	    (len != pdu_length && len != pdu_length + 4))
		return -EINVAL;
	req->payload = frame + OMCI_EXTENDED_HEADER_LEN;
	req->payload_len = declared;
	req->attribute_mask = declared >= 2 ?
		(uint16_t)((req->payload[0] << 8) | req->payload[1]) : 0;
	req->content = declared >= 2 ? req->payload + 2 : req->payload;
	req->content_len = declared >= 2 ? declared - 2 : 0;
	return 0;
}

int omci_request_parse(struct omci_request *req, const uint8_t *frame, size_t len)
{
	int rc;

	memset(req, 0, sizeof(*req));
	if (len < 4)
		return -EINVAL;

	req->tci = (uint16_t)((frame[0] << 8) | frame[1]);
	req->message_type = frame[2];
	req->action = frame[2] & 0x1f;
	req->class_id = (uint16_t)((frame[4] << 8) | frame[5]);
	req->entity_id = (uint16_t)((frame[6] << 8) | frame[7]);

	switch (frame[3]) {
	case OMCI_DEVICE_ID_BASELINE:
		req->enc = OMCI_BASELINE;
		rc = parse_baseline(req, frame, len);
		break;
	case OMCI_DEVICE_ID_EXTENDED:
		req->enc = OMCI_EXTENDED;
		rc = parse_extended(req, frame, len);
		break;
	default:
		return -EPROTONOSUPPORT;
	}
	return rc;
}

const char *omci_selector_name(const struct omci_request *req)
{
	switch (req->action) {
	case OMCI_ACTION_GET_ALL_ALARMS_NEXT:
	case OMCI_ACTION_MIB_UPLOAD_NEXT:
		return "sequence";
	default:
		return "mask";
	}
}

size_t omci_get_value_capacity(const struct omci_request *req)
{
	return req->enc == OMCI_BASELINE ? OMCI_BASELINE_VALUE_CAPACITY :
					   OMCI_EXTENDED_GET_VALUE_CAPACITY;
}

size_t omci_get_next_value_capacity(const struct omci_request *req)
{
	return req->enc == OMCI_BASELINE ? OMCI_BASELINE_VALUE_CAPACITY :
					   OMCI_EXTENDED_GET_NEXT_VALUE_CAPACITY;
}

/* ---------- response construction ---------- */

static void baseline_header(struct omci_response *out, const struct omci_request *req)
{
	memset(out->bytes, 0, OMCI_BASELINE_LEN);
	out->bytes[0] = (uint8_t)(req->tci >> 8);
	out->bytes[1] = (uint8_t)req->tci;
	out->bytes[2] = (uint8_t)((req->message_type & 0x1f) | 0x20);
	out->bytes[3] = OMCI_DEVICE_ID_BASELINE;
	out->bytes[4] = (uint8_t)(req->class_id >> 8);
	out->bytes[5] = (uint8_t)req->class_id;
	out->bytes[6] = (uint8_t)(req->entity_id >> 8);
	out->bytes[7] = (uint8_t)req->entity_id;
	/* The baseline trailer carries the fixed message length 0x0028. */
	out->bytes[42] = 0x00;
	out->bytes[43] = 0x28;
	out->len = OMCI_BASELINE_LEN;
	out->has_result = 0;
	out->result = 0;
}

static int extended_response(struct omci_response *out, const struct omci_request *req,
			     const uint8_t *content, size_t content_len, int has_result,
			     uint8_t result)
{
	struct wr w;

	if (OMCI_EXTENDED_HEADER_LEN + content_len > sizeof(out->bytes))
		return -EMSGSIZE;
	wr_init(&w, out->bytes, sizeof(out->bytes));
	wr_be16(&w, req->tci);
	wr_u8(&w, (uint8_t)((req->message_type & 0x1f) | 0x20));
	wr_u8(&w, OMCI_DEVICE_ID_EXTENDED);
	wr_be16(&w, req->class_id);
	wr_be16(&w, req->entity_id);
	wr_be16(&w, (uint16_t)content_len);
	wr_bytes(&w, content, content_len);
	if (w.err)
		return -EMSGSIZE;
	out->len = w.len;
	out->has_result = has_result;
	out->result = result;
	return 0;
}

static void baseline_result(struct omci_response *out, const struct omci_request *req,
			    uint8_t result)
{
	baseline_header(out, req);
	out->bytes[8] = result;
	out->has_result = 1;
	out->result = result;
}

int omci_response_new(const struct omci_request *req, uint8_t result,
		      struct omci_response *out)
{
	uint8_t content[8];

	memset(out, 0, sizeof(*out));
	if (req->enc == OMCI_BASELINE) {
		baseline_result(out, req, result);
		return 0;
	}

	switch (req->action) {
	case OMCI_ACTION_CREATE:
		/* Create responses reserve the two-byte attribute execution mask. */
		content[0] = result; content[1] = 0; content[2] = 0;
		return extended_response(out, req, content, 3, 1, result);
	case OMCI_ACTION_GET:
	case OMCI_ACTION_GET_CURRENT_DATA:
		/* Get responses carry returned, optional, and execution masks. */
		memset(content, 0, 7);
		content[0] = result;
		return extended_response(out, req, content, 7, 1, result);
	case OMCI_ACTION_SET:
		if (result == OMCI_RESULT_ATTRIBUTE_FAILED) {
			content[0] = result; content[1] = 0; content[2] = 0;
			content[3] = (uint8_t)(req->attribute_mask >> 8);
			content[4] = (uint8_t)req->attribute_mask;
			return extended_response(out, req, content, 5, 1, result);
		}
		/* fallthrough */
	default:
		content[0] = result;
		return extended_response(out, req, content, 1, 1, result);
	}
}

int omci_response_get_success(const struct omci_request *req, uint16_t returned_mask,
			      const uint8_t *values, size_t n,
			      struct omci_response *out)
{
	uint8_t content[OMCI_EXTENDED_MAX_PDU_LEN];

	memset(out, 0, sizeof(*out));
	if (n > omci_get_value_capacity(req))
		return -EMSGSIZE;

	if (req->enc == OMCI_BASELINE) {
		baseline_result(out, req, OMCI_RESULT_SUCCESS);
		out->bytes[9] = (uint8_t)(returned_mask >> 8);
		out->bytes[10] = (uint8_t)returned_mask;
		memcpy(out->bytes + 11, values, n);
		return 0;
	}
	content[0] = OMCI_RESULT_SUCCESS;
	content[1] = (uint8_t)(returned_mask >> 8);
	content[2] = (uint8_t)returned_mask;
	/* G.988 reserves four bytes for optional-attribute and execution masks. */
	memset(content + 3, 0, 4);
	memcpy(content + 7, values, n);
	return extended_response(out, req, content, 7 + n, 1, OMCI_RESULT_SUCCESS);
}

int omci_response_get_next_success(const struct omci_request *req, uint16_t returned_mask,
				   const uint8_t *values, size_t n,
				   struct omci_response *out)
{
	uint8_t content[OMCI_EXTENDED_MAX_PDU_LEN];

	memset(out, 0, sizeof(*out));
	if (n > omci_get_next_value_capacity(req))
		return -EMSGSIZE;

	if (req->enc == OMCI_BASELINE) {
		baseline_result(out, req, OMCI_RESULT_SUCCESS);
		out->bytes[9] = (uint8_t)(returned_mask >> 8);
		out->bytes[10] = (uint8_t)returned_mask;
		memcpy(out->bytes + 11, values, n);
		return 0;
	}
	content[0] = OMCI_RESULT_SUCCESS;
	content[1] = (uint8_t)(returned_mask >> 8);
	content[2] = (uint8_t)returned_mask;
	memcpy(content + 3, values, n);
	return extended_response(out, req, content, 3 + n, 1, OMCI_RESULT_SUCCESS);
}

static void baseline_without_result(struct omci_response *out, const struct omci_request *req)
{
	baseline_header(out, req);
}

int omci_response_mib_upload(const struct omci_request *req, uint16_t command_count,
			     struct omci_response *out)
{
	uint8_t content[2];

	memset(out, 0, sizeof(*out));
	if (req->enc == OMCI_BASELINE) {
		baseline_without_result(out, req);
		out->bytes[8] = (uint8_t)(command_count >> 8);
		out->bytes[9] = (uint8_t)command_count;
		return 0;
	}
	content[0] = (uint8_t)(command_count >> 8);
	content[1] = (uint8_t)command_count;
	return extended_response(out, req, content, 2, 0, 0);
}

int omci_response_get_all_alarms(const struct omci_request *req, uint16_t command_count,
				 struct omci_response *out)
{
	return omci_response_mib_upload(req, command_count, out);
}

int omci_response_get_all_alarms_next_empty(const struct omci_request *req,
					    struct omci_response *out)
{
	memset(out, 0, sizeof(*out));
	if (req->enc == OMCI_BASELINE) {
		baseline_without_result(out, req);
		return 0;
	}
	return extended_response(out, req, NULL, 0, 0, 0);
}

int omci_response_mib_upload_next(const struct omci_request *req, uint16_t class_id,
				  uint16_t entity_id, uint16_t attribute_mask,
				  const uint8_t *values, size_t n,
				  struct omci_response *out)
{
	uint8_t content[OMCI_EXTENDED_MAX_PDU_LEN];

	memset(out, 0, sizeof(*out));
	if (req->enc == OMCI_BASELINE) {
		if (n > 26)
			return -EMSGSIZE;
		baseline_without_result(out, req);
		out->bytes[8] = (uint8_t)(class_id >> 8);
		out->bytes[9] = (uint8_t)class_id;
		out->bytes[10] = (uint8_t)(entity_id >> 8);
		out->bytes[11] = (uint8_t)entity_id;
		out->bytes[12] = (uint8_t)(attribute_mask >> 8);
		out->bytes[13] = (uint8_t)attribute_mask;
		memcpy(out->bytes + 14, values, n);
		return 0;
	}
	if (!class_id && !entity_id && !attribute_mask && !n)
		return extended_response(out, req, NULL, 0, 0, 0);
	content[0] = (uint8_t)(n >> 8);
	content[1] = (uint8_t)n;
	content[2] = (uint8_t)(class_id >> 8);
	content[3] = (uint8_t)class_id;
	content[4] = (uint8_t)(entity_id >> 8);
	content[5] = (uint8_t)entity_id;
	content[6] = (uint8_t)(attribute_mask >> 8);
	content[7] = (uint8_t)attribute_mask;
	memcpy(content + 8, values, n);
	return extended_response(out, req, content, 8 + n, 0, 0);
}

int omci_response_sync_time(const struct omci_request *req, struct omci_response *out)
{
	uint8_t content[2];

	memset(out, 0, sizeof(*out));
	if (req->enc == OMCI_BASELINE)
		return omci_response_new(req, OMCI_RESULT_SUCCESS, out);
	/* Result information zero confirms sync to the 15-minute counter boundary. */
	content[0] = OMCI_RESULT_SUCCESS;
	content[1] = 0;
	return extended_response(out, req, content, 2, 1, OMCI_RESULT_SUCCESS);
}

int omci_response_avc(uint16_t tci, uint16_t class_id, uint16_t entity_id,
		      uint16_t attribute_mask, const uint8_t *values, size_t n,
		      struct omci_response *out)
{
	if (n > 30)
		return -EMSGSIZE;
	memset(out, 0, sizeof(*out));
	out->bytes[0] = (uint8_t)(tci >> 8);
	out->bytes[1] = (uint8_t)tci;
	out->bytes[2] = OMCI_ACTION_ATTRIBUTE_VALUE_CHANGE;
	out->bytes[3] = OMCI_DEVICE_ID_BASELINE;
	out->bytes[4] = (uint8_t)(class_id >> 8);
	out->bytes[5] = (uint8_t)class_id;
	out->bytes[6] = (uint8_t)(entity_id >> 8);
	out->bytes[7] = (uint8_t)entity_id;
	out->bytes[8] = (uint8_t)(attribute_mask >> 8);
	out->bytes[9] = (uint8_t)attribute_mask;
	memcpy(out->bytes + 10, values, n);
	/* Autonomous baseline messages use the same 0x0028 trailer as responses. */
	out->bytes[42] = 0x00;
	out->bytes[43] = 0x28;
	out->len = OMCI_BASELINE_LEN;
	out->has_result = 0;
	out->result = 0;
	return 0;
}

uint16_t omci_attribute_bit(uint8_t index)
{
	if (index >= 1 && index <= 16)
		return (uint16_t)(0x8000u >> (index - 1));
	return 0;
}
