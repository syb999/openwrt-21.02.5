/* SPDX-License-Identifier: GPL-2.0-only */
#include "oam_ctc.h"

#include <stdio.h>
#include <string.h>

#define INFO_TYPE_ORGANIZATION_SPECIFIC 0xfe

#define OPCODE_GET_REQUEST	0x01
#define OPCODE_GET_RESPONSE	0x02
#define OPCODE_SET_REQUEST	0x03
#define OPCODE_SET_RESPONSE	0x04
#define OPCODE_AUTHENTICATION	0x05

#define AUTH_REQUEST		0x01
#define AUTH_RESPONSE		0x02
#define AUTH_SUCCESS		0x03
#define AUTH_FAILURE		0x04
#define AUTH_TYPE_LOID_PASSWORD	0x01
#define AUTH_TYPE_NAK		0x02

#define OBJECT_BRANCH_V1	0x36
#define OBJECT_BRANCH_V2	0x37
#define OBJECT_LEAF_PORT	0x0001

#define STANDARD_ATTRIBUTE	0x07
#define STANDARD_ACTION		0x09
#define EXTENDED_ATTRIBUTE	0xc7
#define EXTENDED_ACTION		0xc9

#define LEAF_ONU_SN		0x0001
#define LEAF_FIRMWARE_VERSION	0x0002
#define LEAF_CHIPSET_ID		0x0003
#define LEAF_ONU_CAPABILITIES_1	0x0004
#define LEAF_ONU_CAPABILITIES_2	0x0007
#define LEAF_ONU_CAPABILITIES_3	0x000c
#define LEAF_VLAN		0x0021

#define SET_OK			0x80
#define BAD_PARAMETERS		0x86
#define NO_RESOURCE		0x87

/* ---------- small byte writer ---------- */

struct wr {
	uint8_t *buf;
	size_t cap;
	size_t len;
	bool overflow;
};

static void wr_init(struct wr *w, uint8_t *buf, size_t cap)
{
	w->buf = buf;
	w->cap = cap;
	w->len = 0;
	w->overflow = false;
}

static void wr_u8(struct wr *w, uint8_t v)
{
	if (w->len + 1 > w->cap) {
		w->overflow = true;
		return;
	}
	w->buf[w->len++] = v;
}

static void wr_be16(struct wr *w, uint16_t v)
{
	wr_u8(w, (uint8_t)(v >> 8));
	wr_u8(w, (uint8_t)v);
}

static void wr_be32(struct wr *w, uint32_t v)
{
	wr_u8(w, (uint8_t)(v >> 24));
	wr_u8(w, (uint8_t)(v >> 16));
	wr_u8(w, (uint8_t)(v >> 8));
	wr_u8(w, (uint8_t)v);
}

static void wr_bytes(struct wr *w, const uint8_t *v, size_t len)
{
	if (w->len + len > w->cap) {
		w->overflow = true;
		return;
	}
	if (len)
		memcpy(w->buf + w->len, v, len);
	w->len += len;
}

/* ---------- byte reader ---------- */

struct rd {
	const uint8_t *buf;
	size_t len;
	size_t off;
};

static void rd_init(struct rd *r, const uint8_t *buf, size_t len)
{
	r->buf = buf;
	r->len = len;
	r->off = 0;
}

static bool rd_u8(struct rd *r, uint8_t *out)
{
	if (r->off + 1 > r->len)
		return false;
	*out = r->buf[r->off++];
	return true;
}

static bool rd_be16(struct rd *r, uint16_t *out)
{
	if (r->off + 2 > r->len)
		return false;
	*out = (uint16_t)((r->buf[r->off] << 8) | r->buf[r->off + 1]);
	r->off += 2;
	return true;
}

static bool rd_be32(struct rd *r, uint32_t *out)
{
	if (r->off + 4 > r->len)
		return false;
	*out = ((uint32_t)r->buf[r->off] << 24) | ((uint32_t)r->buf[r->off + 1] << 16) |
	       ((uint32_t)r->buf[r->off + 2] << 8) | r->buf[r->off + 3];
	r->off += 4;
	return true;
}

/* ---------- helpers ---------- */

static void hex_bytes(const uint8_t *value, size_t len, char *out, size_t out_cap)
{
	size_t o = 0;

	for (size_t i = 0; i < len && o + 3 < out_cap; i++)
		o += (size_t)snprintf(out + o, out_cap - o, "%02x", value[i]);
	out[o] = '\0';
}

static bool is_attribute_or_action(uint8_t branch)
{
	return branch == STANDARD_ATTRIBUTE || branch == STANDARD_ACTION ||
	       branch == EXTENDED_ATTRIBUTE || branch == EXTENDED_ACTION;
}

static void outcome_init(struct ctc_outcome *o)
{
	memset(o, 0, sizeof(*o));
}

static void outcome_event(struct ctc_outcome *o, const char *text)
{
	if (o->n_events >= CTC_EVENT_MAX)
		return;
	snprintf(o->events[o->n_events], CTC_EVENT_LEN, "%s", text);
	o->n_events++;
}

static void format_variable_event(struct ctc_outcome *o, const char *operation,
				  const struct ctc_object *object, uint8_t branch,
				  uint16_t leaf, size_t value_length, uint8_t result,
				  const uint8_t *value, size_t value_len)
{
	char obj[40], hex[2 * CTC_VLAN_RAW_MAX + 8], line[CTC_EVENT_LEN];

	if (object)
		snprintf(obj, sizeof(obj), "0x%02x:0x%04x:0x%08x", object->branch,
			 object->leaf, object->index);
	else
		snprintf(obj, sizeof(obj), "global");
	hex[0] = '\0';
	if (value)
		hex_bytes(value, value_len, hex, sizeof(hex));
	/*
	 * Branch, leaf, and object instance select the CTC node handler. The bounded
	 * event ring retains Set payloads for node implementation and payload decoding.
	 */
	snprintf(line, sizeof(line),
		 "CTC %s object=%s branch=0x%02x leaf=0x%04x length=%zu result=0x%02x value=%s",
		 operation, obj, branch, leaf, value_length, result, hex);
	outcome_event(o, line);
}

static int with_org_header(const uint8_t oui[3], uint8_t opcode,
			   const uint8_t *data, size_t data_len,
			   uint8_t *out, size_t out_cap, size_t *out_len)
{
	if (4 + data_len > out_cap)
		return -1;
	memcpy(out, oui, 3);
	out[3] = opcode;
	if (data_len)
		memcpy(out + 4, data, data_len);
	*out_len = 4 + data_len;
	return 0;
}

/* ---------- object TLV ---------- */

static int parse_object(const uint8_t *data, size_t len, struct ctc_object *out)
{
	struct rd r;
	uint8_t branch, width;

	rd_init(&r, data, len);
	if (!rd_u8(&r, &branch))
		return -1;
	if (!rd_be16(&r, &out->leaf))
		return -1;
	if (!rd_u8(&r, &width))
		return -1;
	out->branch = branch;
	if (branch == OBJECT_BRANCH_V1 && width == 1) {
		uint8_t idx;

		if (!rd_u8(&r, &idx))
			return -1;
		out->index = idx;
		out->encoded_len = r.off;
		return 0;
	}
	if (branch == OBJECT_BRANCH_V2 && width == 4) {
		if (!rd_be32(&r, &out->index))
			return -1;
		out->encoded_len = r.off;
		return 0;
	}
	return -1;
}

static void encode_object(struct wr *w, const struct ctc_object *object)
{
	wr_u8(w, object->branch);
	wr_be16(w, object->leaf);
	if (object->branch == OBJECT_BRANCH_V1) {
		wr_u8(w, 1);
		wr_u8(w, (uint8_t)object->index);
	} else {
		wr_u8(w, 4);
		wr_be32(w, object->index);
	}
}

static void encode_data_tlv(struct wr *w, uint8_t branch, uint16_t leaf,
			    const uint8_t *value, size_t len)
{
	size_t off = 0;

	do {
		size_t chunk = len - off;
		if (chunk > 128)
			chunk = 128;
		wr_u8(w, branch);
		wr_be16(w, leaf);
		wr_u8(w, chunk == 128 ? 0 : (uint8_t)chunk);
		wr_bytes(w, value + off, chunk);
		off += chunk;
	} while (off < len);
}

static void encode_result_tlv(struct wr *w, uint8_t branch, uint16_t leaf, uint8_t result)
{
	wr_u8(w, branch);
	wr_be16(w, leaf);
	wr_u8(w, result);
}

/* ---------- session ---------- */

void ctc_session_init(struct ctc_session *s)
{
	memset(s, 0, sizeof(*s));
	s->state = CTC_PASSIVE_WAIT;
	s->authentication = "not-requested";
}

const char *ctc_discovery_state_name(const struct ctc_session *s)
{
	switch (s->state) {
	case CTC_OFFERED: return "version-offered";
	case CTC_OPERATIONAL: return "operational";
	default: return "passive-wait";
	}
}

size_t ctc_vlan_ids(const struct ctc_session *s, uint16_t *out, size_t cap)
{
	size_t n = 0;

	if (!s->has_vlan || s->vlan.raw_len < 5 || s->vlan.mode == 0)
		return 0;
	for (size_t off = 1; off + 4 <= s->vlan.raw_len; off += 4) {
		uint32_t value = ((uint32_t)s->vlan.raw[off] << 24) |
				 ((uint32_t)s->vlan.raw[off + 1] << 16) |
				 ((uint32_t)s->vlan.raw[off + 2] << 8) |
				 s->vlan.raw[off + 3];
		uint16_t vid = (uint16_t)(value & 0x0fff);
		size_t pos = 0;

		while (pos < n && out[pos] < vid)
			pos++;
		if (pos < n && out[pos] == vid)
			continue;
		if (n >= cap)
			break;
		memmove(&out[pos + 1], &out[pos], (n - pos) * sizeof(uint16_t));
		out[pos] = vid;
		n++;
	}
	return n;
}

/* ---------- CTC discovery Information TLV ---------- */

int ctc_handle_information_tlv(struct ctc_session *s, const uint8_t *tlv, size_t tlv_len,
			       const struct oam_config *cfg,
			       uint8_t *response, size_t response_cap, size_t *response_len,
			       bool *has_response)
{
	struct wr w;
	size_t length;

	*has_response = false;
	*response_len = 0;
	if (tlv_len < 7 || tlv[0] != INFO_TYPE_ORGANIZATION_SPECIFIC)
		return -1;	/* CTC information TLV is truncated */
	length = tlv[1];
	if (length < 7 || length > tlv_len || (length - 7) % 4 != 0)
		return -2;	/* invalid length */

	wr_init(&w, response, response_cap);

	if (length > 7) {	/* version list offered */
		s->state = CTC_OFFERED;
		s->has_version = false;
		wr_u8(&w, INFO_TYPE_ORGANIZATION_SPECIFIC);
		wr_u8(&w, (uint8_t)(7 + cfg->n_ctc_versions * 4));
		wr_bytes(&w, cfg->ctc_oui, 3);
		wr_u8(&w, 1);
		wr_u8(&w, 0);
		for (size_t i = 0; i < cfg->n_ctc_versions; i++) {
			wr_bytes(&w, cfg->ctc_oui, 3);
			wr_u8(&w, cfg->ctc_versions[i]);
		}
		*has_response = true;
		*response_len = w.len;
		return 0;
	}

	/* version selected by the OLT */
	if (s->state == CTC_OFFERED && tlv[5] == 1) {
		int known = 0;

		for (size_t i = 0; i < cfg->n_ctc_versions; i++)
			if (cfg->ctc_versions[i] == tlv[6])
				known = 1;
		if (known) {
			s->state = CTC_OPERATIONAL;
			s->has_version = true;
			s->version = tlv[6];
			wr_u8(&w, INFO_TYPE_ORGANIZATION_SPECIFIC);
			wr_u8(&w, 7);
			wr_bytes(&w, cfg->ctc_oui, 3);
			wr_u8(&w, 1);
			wr_u8(&w, tlv[6]);
			*has_response = true;
			*response_len = w.len;
			return 0;
		}
	}

	if (s->state == CTC_OPERATIONAL && s->has_version && tlv[6] == s->version) {
		wr_u8(&w, INFO_TYPE_ORGANIZATION_SPECIFIC);
		wr_u8(&w, 7);
		wr_bytes(&w, cfg->ctc_oui, 3);
		wr_u8(&w, 1);
		wr_u8(&w, tlv[6]);
		*has_response = true;
		*response_len = w.len;
		return 0;
	}

	s->state = CTC_PASSIVE_WAIT;
	s->has_version = false;
	wr_u8(&w, INFO_TYPE_ORGANIZATION_SPECIFIC);
	wr_u8(&w, 7);
	wr_bytes(&w, cfg->ctc_oui, 3);
	wr_u8(&w, 0);
	wr_u8(&w, tlv[6]);
	*has_response = true;
	*response_len = w.len;
	return 0;
}

/* ---------- authentication ---------- */

static int handle_authentication(struct ctc_session *s, const uint8_t *data, size_t data_len,
				 const struct oam_config *cfg, struct ctc_outcome *out)
{
	uint8_t code, auth_type;
	uint16_t length;

	if (data_len < 3)
		return -1;	/* truncated */
	code = data[0];
	length = (uint16_t)((data[1] << 8) | data[2]);

	switch (code) {
	case AUTH_REQUEST:
		if (length != 1 && length != 0x0101)
			return -2;
		if (length == 0x0101) {
			auth_type = AUTH_TYPE_LOID_PASSWORD;
		} else {
			if (data_len < 4)
				return -1;
			auth_type = data[3];
		}
		s->authentication = "pending";
		s->has_failure = false;

		{
			uint8_t body[128];
			struct wr w;
			char ev[CTC_EVENT_LEN];

			wr_init(&w, body, sizeof(body));
			wr_u8(&w, AUTH_RESPONSE);
			if (auth_type == AUTH_TYPE_LOID_PASSWORD) {
				uint8_t loid[24], pass[12];

				wr_be16(&w, 0x0025);
				wr_u8(&w, AUTH_TYPE_LOID_PASSWORD);
				/* CTC stores the LOID right-aligned in a 24-byte field. */
				oam_fixed_width_right(cfg->loid, cfg->loid_len, loid, 24);
				oam_fixed_width(cfg->loid_password, cfg->loid_password_len,
						pass, 12);
				wr_bytes(&w, loid, 24);
				wr_bytes(&w, pass, 12);
			} else {
				wr_be16(&w, 2);
				wr_u8(&w, AUTH_TYPE_NAK);
				wr_u8(&w, AUTH_TYPE_LOID_PASSWORD);
			}
			if (with_org_header(cfg->ctc_oui, OPCODE_AUTHENTICATION, body, w.len,
					    out->response, sizeof(out->response),
					    &out->response_len) < 0)
				return -2;
			out->has_response = true;
			snprintf(ev, sizeof(ev), "CTC authentication request type=%u", auth_type);
			outcome_event(out, ev);
		}
		return 0;

	case AUTH_SUCCESS:
		s->authentication = "accepted";
		s->has_failure = false;
		outcome_event(out, "CTC authentication accepted");
		return 0;

	case AUTH_FAILURE:
		s->authentication = "rejected";
		if (data_len >= 4) {
			s->failure = data[3];
			s->has_failure = true;
		}
		{
			char ev[CTC_EVENT_LEN];

			if (s->has_failure)
				snprintf(ev, sizeof(ev), "CTC authentication rejected reason=0x%02x",
					 s->failure);
			else
				snprintf(ev, sizeof(ev), "CTC authentication rejected reason=missing");
			outcome_event(out, ev);
		}
		return 0;

	default:
		return -2;	/* invalid code or length */
	}
}

/* ---------- GET ---------- */

static int get_value(const struct ctc_session *s, uint8_t branch, uint16_t leaf,
		     const struct ctc_object *object, const struct oam_config *cfg,
		     const uint8_t source_mac[6], uint8_t *out, size_t out_cap,
		     size_t *out_len)
{
	*out_len = 0;
	if (branch != EXTENDED_ATTRIBUTE)
		return BAD_PARAMETERS;

	switch (leaf) {
	case LEAF_ONU_SN:
		if (object)
			break;
		{
			struct wr w;

			wr_init(&w, out, out_cap);
			{
				uint8_t tmp[16];

				oam_fixed_width(cfg->vendor_id, cfg->vendor_id_len, tmp, 4);
				wr_bytes(&w, tmp, 4);
				oam_fixed_width(cfg->model, cfg->model_len, tmp, 4);
				wr_bytes(&w, tmp, 4);
			}
			wr_bytes(&w, source_mac, 6);
			{
				uint8_t tmp[16];

				oam_fixed_width(cfg->hardware_version, cfg->hardware_version_len,
						tmp, 8);
				wr_bytes(&w, tmp, 8);
				oam_fixed_width(cfg->software_version, cfg->software_version_len,
						tmp, 16);
				wr_bytes(&w, tmp, 16);
				if (s->has_version && s->version >= 0x30) {
					oam_fixed_width(cfg->equipment_id,
							cfg->equipment_id_len, tmp, 16);
					wr_bytes(&w, tmp, 16);
				}
			}
			*out_len = w.len;
		}
		return 0;

	case LEAF_FIRMWARE_VERSION:
		if (object)
			break;
		{
			size_t n = cfg->firmware_version_len > 127 ? 127 : cfg->firmware_version_len;

			if (n > out_cap)
				return BAD_PARAMETERS;
			memcpy(out, cfg->firmware_version, n);
			*out_len = n;
		}
		return 0;

	case LEAF_CHIPSET_ID:
		if (object)
			break;
		if (out_cap < 8)
			return BAD_PARAMETERS;
		memcpy(out, cfg->chipset_id, 8);
		*out_len = 8;
		return 0;

	case LEAF_ONU_CAPABILITIES_1:
		if (object)
			break;
		if (out_cap < 26)
			return BAD_PARAMETERS;
		memset(out, 0, 26);
		out[0] = 0x01;
		out[1] = cfg->ge_ports;
		for (uint8_t port = 0; port < cfg->ge_ports && port < 64; port++)
			out[2 + 7 - (port / 8)] |= (uint8_t)(1 << (port % 8));
		out[21] = 8;
		out[22] = 8;
		out[23] = 8;
		out[24] = 8;
		*out_len = 26;
		return 0;

	case LEAF_ONU_CAPABILITIES_2:
		if (object)
			break;
		if (out_cap < 15)
			return BAD_PARAMETERS;
		/* HGU profile with one LLID, one PON port, and one GE UNI class. */
		out[0] = 0;
		out[1] = 0;
		out[2] = 0;
		out[3] = 1;
		out[4] = 0;
		out[5] = 0;
		out[6] = 1;
		out[7] = 0;
		out[8] = 1;
		out[9] = 0;
		out[10] = 0;
		out[11] = 0;
		out[12] = 0;
		out[13] = (uint8_t)(cfg->ge_ports >> 8);
		out[14] = (uint8_t)cfg->ge_ports;
		*out_len = 15;
		return 0;

	case LEAF_ONU_CAPABILITIES_3:
		if (object)
			break;
		if (out_cap < 3)
			return BAD_PARAMETERS;
		out[0] = 0;
		out[1] = 0;
		out[2] = 1;
		*out_len = 3;
		return 0;

	case LEAF_VLAN:
		if (!object || object->leaf != OBJECT_LEAF_PORT)
			return BAD_PARAMETERS;
		/* The HGU reports transparent mode and publishes Set rules for netifd. */
		if (out_cap < 1)
			return BAD_PARAMETERS;
		out[0] = 0;
		*out_len = 1;
		return 0;

	default:
		return leaf == 0x0005 ? NO_RESOURCE : BAD_PARAMETERS;
	}
	return BAD_PARAMETERS;
}

int ctc_handle_get(struct ctc_session *s, const uint8_t *data, size_t data_len,
		      const struct oam_config *cfg, const uint8_t source_mac[6],
		      struct ctc_outcome *out)
{
	size_t offset = 0;

	outcome_init(out);
	uint8_t body[1024];
	struct wr w;
	bool have_object = false;
	struct ctc_object object;
	bool object_written = false;

	wr_init(&w, body, sizeof(body));
	while (offset < data_len) {
		uint8_t branch = data[offset];
		uint16_t leaf;
		uint8_t value[256];
		size_t value_len = 0;
		int rc;

		if (branch == 0)
			break;
		if (branch == OBJECT_BRANCH_V1 || branch == OBJECT_BRANCH_V2) {
			if (parse_object(&data[offset], data_len - offset, &object) < 0)
				return -1;
			offset += object.encoded_len;
			have_object = true;
			object_written = false;
			continue;
		}
		if (!is_attribute_or_action(branch) || offset + 3 > data_len)
			return -1;
		leaf = (uint16_t)((data[offset + 1] << 8) | data[offset + 2]);

		rc = get_value(s, branch, leaf, have_object ? &object : NULL, cfg, source_mac,
			       value, sizeof(value), &value_len);
		if (have_object && !object_written) {
			encode_object(&w, &object);
			object_written = true;
		}
		if (rc == 0) {
			format_variable_event(out, "GET", have_object ? &object : NULL, branch,
					      leaf, value_len, SET_OK, NULL, 0);
			encode_data_tlv(&w, branch, leaf, value, value_len);
		} else {
			format_variable_event(out, "GET", have_object ? &object : NULL, branch,
					      leaf, 0, (uint8_t)rc, NULL, 0);
			encode_result_tlv(&w, branch, leaf, (uint8_t)rc);
		}
		offset += 3;
	}
	wr_u8(&w, 0);
	if (w.overflow)
		return -1;
	/* the body was built in a local buffer (bounded response assembly) */
	memcpy(out->response, body, w.len);
	out->response_len = w.len;
	out->has_response = true;
	return 0;
}

/* ---------- SET ---------- */

static uint8_t set_value(struct ctc_session *s, uint8_t branch, uint16_t leaf,
			 const struct ctc_object *object, const uint8_t *value, size_t value_len)
{
	uint8_t mode;

	if (branch != EXTENDED_ATTRIBUTE || leaf != LEAF_VLAN) {
		s->unsupported_requests++;
		return BAD_PARAMETERS;
	}
	if (!object || object->leaf != OBJECT_LEAF_PORT)
		return BAD_PARAMETERS;
	if (value_len < 1 || value[0] > 4)
		return BAD_PARAMETERS;
	mode = value[0];

	/* The HGU data path preserves OLT-provisioned tags for Linux VLAN and netifd. */
	s->has_vlan = true;
	s->vlan.object_index = object->index;
	s->vlan.mode = mode;
	s->vlan.raw_len = value_len > CTC_VLAN_RAW_MAX ? CTC_VLAN_RAW_MAX : value_len;
	memcpy(s->vlan.raw, value, s->vlan.raw_len);
	return SET_OK;
}

int ctc_handle_set(struct ctc_session *s, const uint8_t *data, size_t data_len,
		      struct ctc_outcome *out)
{
	size_t offset = 0;

	outcome_init(out);
	uint8_t body[1024];
	struct wr w;
	bool have_object = false;
	struct ctc_object object;
	bool object_written = false;

	wr_init(&w, body, sizeof(body));
	while (offset < data_len) {
		uint8_t branch = data[offset];
		uint16_t leaf;
		const uint8_t *value;
		size_t value_len, consumed;
		uint8_t result;

		if (branch == 0)
			break;
		if (branch == OBJECT_BRANCH_V1 || branch == OBJECT_BRANCH_V2) {
			if (parse_object(&data[offset], data_len - offset, &object) < 0)
				return -1;
			offset += object.encoded_len;
			have_object = true;
			object_written = false;
			continue;
		}
		if (!is_attribute_or_action(branch) || offset + 3 > data_len)
			return -1;
		leaf = (uint16_t)((data[offset + 1] << 8) | data[offset + 2]);

		if (branch == EXTENDED_ACTION && leaf == 0x0001) {
			value = NULL;
			value_len = 0;
			consumed = 3;
		} else {
			uint8_t width;

			if (offset + 4 > data_len)
				return -2;	/* value length missing */
			width = data[offset + 3] == 0 ? 128 : data[offset + 3];
			if (offset + 4 + width > data_len)
				return -3;	/* value truncated */
			value = data + offset + 4;
			value_len = width;
			consumed = 4 + width;
		}

		result = set_value(s, branch, leaf, have_object ? &object : NULL, value,
				   value_len);
		format_variable_event(out, "SET", have_object ? &object : NULL, branch, leaf,
				      value_len, result, value, value_len);
		if (have_object && !object_written) {
			encode_object(&w, &object);
			object_written = true;
		}
		encode_result_tlv(&w, branch, leaf, result);
		offset += consumed;
	}
	wr_u8(&w, 0);
	if (w.overflow)
		return -1;
	/* the body was built in a local buffer (bounded response assembly) */
	memcpy(out->response, body, w.len);
	out->response_len = w.len;
	out->has_response = true;
	return 0;
}

/* ---------- organization PDU dispatch ---------- */

int ctc_handle_organization_pdu(struct ctc_session *s, const uint8_t *payload,
				size_t payload_len, const struct oam_config *cfg,
				const uint8_t source_mac[6], struct ctc_outcome *out)
{
	uint8_t opcode, body[1024], inner[1024];
	size_t inner_len = 0;

	outcome_init(out);
	if (payload_len < 4)
		return -1;	/* truncated */
	if (memcmp(payload, cfg->ctc_oui, 3))
		return -2;	/* unexpected OUI */
	if (s->state != CTC_OPERATIONAL)
		return -3;	/* before version negotiation */

	opcode = payload[3];
	switch (opcode) {
	case OPCODE_AUTHENTICATION:
		return handle_authentication(s, payload + 4, payload_len - 4, cfg, out);

	case OPCODE_GET_REQUEST: {
		int rc;

		s->get_requests++;
		rc = ctc_handle_get(s, payload + 4, payload_len - 4, cfg, source_mac, out);
		if (rc < 0)
			return rc;
		if (with_org_header(cfg->ctc_oui, OPCODE_GET_RESPONSE, out->response,
				    out->response_len, body, sizeof(body), &inner_len) < 0)
			return -4;
		memcpy(out->response, body, inner_len);
		out->response_len = inner_len;
		out->has_response = true;
		return 0;
	}

	case OPCODE_SET_REQUEST: {
		int rc;

		s->set_requests++;
		rc = ctc_handle_set(s, payload + 4, payload_len - 4, out);
		if (rc < 0)
			return rc;
		if (with_org_header(cfg->ctc_oui, OPCODE_SET_RESPONSE, out->response,
				    out->response_len, body, sizeof(body), &inner_len) < 0)
			return -4;
		memcpy(out->response, body, inner_len);
		out->response_len = inner_len;
		out->has_response = true;
		return 0;
	}

	default: {
		char ev[CTC_EVENT_LEN];

		s->unsupported_requests++;
		snprintf(ev, sizeof(ev), "CTC opcode=0x%02x result=unsupported", opcode);
		outcome_event(out, ev);
		out->has_response = false;
		return 0;
	}
	}
	(void)inner;
}
