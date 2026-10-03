/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OMCI request dispatch and handlers.
 * Port of airoha-pon-daemons omci/mib.rs Mib::{dispatch,handle_*}.
 */
#include "omci_mib.h"
#include "omci_protocol.h"
#include "omci_security.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---------- set helpers ---------- */

void mib_known_class_add(struct mib *m, uint16_t class_id)
{
	for (size_t i = 0; i < m->n_known_classes; i++)
		if (m->known_classes[i] == class_id)
			return;
	m->known_classes = realloc(m->known_classes,
				   (m->n_known_classes + 1) * sizeof(uint16_t));
	if (!m->known_classes)
		return;
	m->known_classes[m->n_known_classes++] = class_id;
}

bool mib_known_class_has(const struct mib *m, uint16_t class_id)
{
	for (size_t i = 0; i < m->n_known_classes; i++)
		if (m->known_classes[i] == class_id)
			return true;
	return false;
}

void mib_dynamic_class_add(struct mib *m, uint16_t class_id)
{
	for (size_t i = 0; i < m->n_dynamic_classes; i++)
		if (m->dynamic_classes[i] == class_id)
			return;
	m->dynamic_classes = realloc(m->dynamic_classes,
				     (m->n_dynamic_classes + 1) * sizeof(uint16_t));
	if (!m->dynamic_classes)
		return;
	m->dynamic_classes[m->n_dynamic_classes++] = class_id;
}

bool mib_dynamic_class_has(const struct mib *m, uint16_t class_id)
{
	for (size_t i = 0; i < m->n_dynamic_classes; i++)
		if (m->dynamic_classes[i] == class_id)
			return true;
	return false;
}

void mib_olt_created_add(struct mib *m, uint16_t class_id, uint16_t entity_id)
{
	for (size_t i = 0; i < m->n_olt_created; i++)
		if (m->olt_created[i].class_id == class_id &&
		    m->olt_created[i].entity_id == entity_id)
			return;
	m->olt_created = realloc(m->olt_created,
				 (m->n_olt_created + 1) * sizeof(*m->olt_created));
	if (!m->olt_created)
		return;
	m->olt_created[m->n_olt_created].class_id = class_id;
	m->olt_created[m->n_olt_created].entity_id = entity_id;
	m->n_olt_created++;
}

bool mib_olt_created_remove(struct mib *m, uint16_t class_id, uint16_t entity_id)
{
	for (size_t i = 0; i < m->n_olt_created; i++) {
		if (m->olt_created[i].class_id != class_id ||
		    m->olt_created[i].entity_id != entity_id)
			continue;
		memmove(&m->olt_created[i], &m->olt_created[i + 1],
			(m->n_olt_created - i - 1) * sizeof(*m->olt_created));
		m->n_olt_created--;
		return true;
	}
	return false;
}

void mib_increment_sync(struct mib *m)
{
	struct mib_entity *e = mib_lookup(m, 0 /*CLASS_ONU_DATA*/, 0);
	struct mib_attr *a;

	if (!e)
		return;
	a = mib_entity_attr(e, 1);
	if (!a || a->value_len < 1)
		return;
	/* G.988 MIB Data Sync cycles through 1..255; zero represents reset state. */
	a->value[0] = a->value[0] == 255 ? 1 : (uint8_t)(a->value[0] + 1);
}

/* ---------- helpers ---------- */

static int entity_error(const struct mib *m, const struct omci_request *req,
			struct omci_response *out)
{
	uint8_t result = mib_known_class_has(m, req->class_id) ?
		OMCI_RESULT_UNKNOWN_INSTANCE : OMCI_RESULT_UNKNOWN_ME;

	return omci_response_new(req, result, out);
}

/* dynamic_entity: Olt-origin classes only, built from the Create payload. */
static struct mib_entity *dynamic_entity(uint16_t class_id, const uint8_t *payload,
					 size_t payload_len)
{
	const struct me_def *def = pond_me_lookup(class_id);

	if (!def || def->origin != ME_ORIGIN_OLT)
		return NULL;
	return mib_entity_from_definition(class_id, payload, payload_len);
}

static int entities_equal(const struct mib_entity *a, const struct mib_entity *b)
{
	if (a->n_attrs != b->n_attrs)
		return 0;
	for (size_t i = 0; i < a->n_attrs; i++) {
		const struct mib_attr *x = &a->attrs[i], *y = &b->attrs[i];

		if (x->index != y->index || x->value_len != y->value_len ||
		    x->n_rows != y->n_rows || x->writable != y->writable)
			return 0;
		if (x->value_len && memcmp(x->value, y->value, x->value_len))
			return 0;
		for (size_t j = 0; j < x->n_rows; j++) {
			if (x->rows[j].key_len != y->rows[j].key_len ||
			    x->rows[j].data_len != y->rows[j].data_len)
				return 0;
			if (memcmp(x->rows[j].key, y->rows[j].key, x->rows[j].key_len))
				return 0;
			if (x->rows[j].data_len &&
			    memcmp(x->rows[j].data, y->rows[j].data, x->rows[j].data_len))
				return 0;
		}
	}
	return 1;
}

/* ---------- individual handlers ---------- */

static int handle_create(struct mib *m, const struct omci_request *req,
			 struct omci_response *out)
{
	struct mib_entity *entity, *existing;

	if (!mib_dynamic_class_has(m, req->class_id))
		return omci_response_new(req, mib_known_class_has(m, req->class_id) ?
					 OMCI_RESULT_COMMAND_NOT_SUPPORTED :
					 OMCI_RESULT_UNKNOWN_ME, out);

	entity = dynamic_entity(req->class_id, req->payload, req->payload_len);
	if (!entity)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);

	existing = mib_lookup(m, req->class_id, req->entity_id);
	if (existing) {
		/*
		 * Repeated Create requests recover a lost response; identical
		 * content reuses the instance and the MIB Data Sync value.
		 */
		uint8_t result = entities_equal(existing, entity) ?
			OMCI_RESULT_SUCCESS : OMCI_RESULT_INSTANCE_EXISTS;

		mib_entity_free(entity);
		return omci_response_new(req, result, out);
	}

	if (mib_insert(m, req->class_id, req->entity_id, entity) < 0)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);

	if (req->class_id == 47 /* CLASS_MAC_BRIDGE_PORT_CONFIG_DATA */) {
		/* Creating Class 47 also creates the Class 79 entity used by the following Set. */
		struct mib_entity *f = mib_entity_from_definition(79, NULL, 0);

		if (f)
			mib_insert(m, 79, req->entity_id, f);
	}
	mib_olt_created_add(m, req->class_id, req->entity_id);
	mib_increment_sync(m);
	return omci_response_new(req, OMCI_RESULT_SUCCESS, out);
}

static int handle_delete(struct mib *m, const struct omci_request *req,
			 struct omci_response *out)
{
	if (!mib_lookup(m, req->class_id, req->entity_id))
		return entity_error(m, req, out);
	if (!mib_olt_created_remove(m, req->class_id, req->entity_id))
		return omci_response_new(req, OMCI_RESULT_COMMAND_NOT_SUPPORTED, out);

	mib_remove(m, req->class_id, req->entity_id);
	if (req->class_id == 47)
		mib_remove(m, 79, req->entity_id);
	mib_increment_sync(m);
	return omci_response_new(req, OMCI_RESULT_SUCCESS, out);
}

static int handle_get(struct mib *m, const struct omci_request *req,
		      struct omci_response *out)
{
	uint8_t values[OMCI_EXTENDED_MAX_PDU_LEN];
	size_t values_len = 0;
	uint16_t returned_mask = 0;
	struct table_snapshot snapshot;
	struct mib_entity *entity = mib_lookup(m, req->class_id, req->entity_id);
	size_t capacity = omci_get_value_capacity(req);
	int have_table = 0;

	if (!entity)
		return entity_error(m, req, out);
	if (req->attribute_mask == 0)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);

	memset(&snapshot, 0, sizeof(snapshot));
	for (uint8_t index = 1; index <= 16; index++) {
		uint16_t bit = omci_attribute_bit(index);
		struct mib_attr *a;

		if (!(req->attribute_mask & bit))
			continue;
		a = mib_entity_attr(entity, index);
		if (!a)
			return omci_response_new(req, OMCI_RESULT_ATTRIBUTE_FAILED, out);

		if (a->is_table) {
			/* A table attribute must be requested on its own. */
			if (__builtin_popcount((unsigned)req->attribute_mask) != 1)
				return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);
			{
				size_t total = 0;

				for (size_t r = 0; r < a->n_rows; r++)
					total += a->rows[r].data_len;
				if (values_len + 4 + total > sizeof(values))
					return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);
				values[values_len++] = (uint8_t)(total >> 24);
				values[values_len++] = (uint8_t)(total >> 16);
				values[values_len++] = (uint8_t)(total >> 8);
				values[values_len++] = (uint8_t)total;
				for (size_t r = 0; r < a->n_rows; r++) {
					memcpy(values + values_len, a->rows[r].data,
					       a->rows[r].data_len);
					values_len += a->rows[r].data_len;
				}
				snapshot.class_id = req->class_id;
				snapshot.entity_id = req->entity_id;
				snapshot.attribute_mask = bit;
				snapshot.bytes = malloc(total ? total : 1);
				if (snapshot.bytes) {
					size_t off = 0;

					for (size_t r = 0; r < a->n_rows; r++) {
						memcpy(snapshot.bytes + off, a->rows[r].data,
						       a->rows[r].data_len);
						off += a->rows[r].data_len;
					}
				}
				snapshot.bytes_len = total;
				have_table = 1;
				returned_mask |= bit;
				continue;
			}
		}

		if (values_len + a->value_len > capacity)
			return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);
		returned_mask |= bit;
		if (a->value_len) {
			memcpy(values + values_len, a->value, a->value_len);
			values_len += a->value_len;
		}
	}

	free(m->table_snapshot.bytes);
	m->table_snapshot = snapshot;
	m->has_table_snapshot = have_table;
	return omci_response_get_success(req, returned_mask, values, values_len, out);
}

static int handle_get_next(struct mib *m, const struct omci_request *req,
			   struct omci_response *out)
{
	size_t sequence, capacity, start, end;

	if (!m->has_table_snapshot)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);
	if (m->table_snapshot.class_id != req->class_id ||
	    m->table_snapshot.entity_id != req->entity_id ||
	    m->table_snapshot.attribute_mask != req->attribute_mask)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);

	sequence = req->content_len >= 2 ?
		(size_t)((req->content[0] << 8) | req->content[1]) : 0;
	capacity = omci_get_next_value_capacity(req);
	start = sequence * capacity;
	if (m->table_snapshot.bytes_len && start >= m->table_snapshot.bytes_len)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);
	end = start + capacity;
	if (end > m->table_snapshot.bytes_len)
		end = m->table_snapshot.bytes_len;
	return omci_response_get_next_success(req, m->table_snapshot.attribute_mask,
					      m->table_snapshot.bytes + start,
					      end - start, out);
}

static void rebuild_upload_snapshot(struct mib *m)
{
	free(m->upload_snapshot);
	m->upload_snapshot = NULL;
	m->n_upload = 0;

	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];
		uint16_t mask = 0;
		uint8_t values[256];
		size_t values_len = 0;

		/* CTC requires LOID authentication attributes to be read with GET, not MIB Upload. */
		if (e->class_id == 0xfffa)
			continue;

		for (size_t j = 0; j < e->entity.n_attrs; j++) {
			const struct mib_attr *a = &e->entity.attrs[j];
			uint16_t bit;

			/* MIB Upload carries persistent scalar state; PM counters and tables use GET. */
			if (!a->upload || a->is_table)
				continue;
			bit = omci_attribute_bit(a->index);
			if (!bit)
				continue;
			if (values_len && values_len + a->value_len > 26) {
				m->upload_snapshot = realloc(m->upload_snapshot,
					(m->n_upload + 1) * sizeof(*m->upload_snapshot));
				if (!m->upload_snapshot)
					return;
				m->upload_snapshot[m->n_upload].class_id = e->class_id;
				m->upload_snapshot[m->n_upload].entity_id = e->entity_id;
				m->upload_snapshot[m->n_upload].attribute_mask = mask;
				m->upload_snapshot[m->n_upload].values = malloc(values_len ? values_len : 1);
				if (m->upload_snapshot[m->n_upload].values)
					memcpy(m->upload_snapshot[m->n_upload].values, values, values_len);
				m->upload_snapshot[m->n_upload].values_len = values_len;
				m->n_upload++;
				mask = 0;
				values_len = 0;
			}
			if (a->value_len <= 26) {
				mask |= bit;
				memcpy(values + values_len, a->value, a->value_len);
				values_len += a->value_len;
			}
		}
		if (mask) {
			m->upload_snapshot = realloc(m->upload_snapshot,
				(m->n_upload + 1) * sizeof(*m->upload_snapshot));
			if (!m->upload_snapshot)
				return;
			m->upload_snapshot[m->n_upload].class_id = e->class_id;
			m->upload_snapshot[m->n_upload].entity_id = e->entity_id;
			m->upload_snapshot[m->n_upload].attribute_mask = mask;
			m->upload_snapshot[m->n_upload].values = malloc(values_len ? values_len : 1);
			if (m->upload_snapshot[m->n_upload].values)
				memcpy(m->upload_snapshot[m->n_upload].values, values, values_len);
			m->upload_snapshot[m->n_upload].values_len = values_len;
			m->n_upload++;
		}
	}
}

static int handle_mib_upload(struct mib *m, const struct omci_request *req,
			     struct omci_response *out)
{
	if (req->class_id != 0 /*CLASS_ONU_DATA*/ || req->entity_id != 0)
		return entity_error(m, req, out);
	rebuild_upload_snapshot(m);
	return omci_response_mib_upload(req, (uint16_t)m->n_upload, out);
}

static int handle_mib_upload_next(struct mib *m, const struct omci_request *req,
				  struct omci_response *out)
{
	size_t sequence = req->attribute_mask;

	if (req->class_id != 0 || req->entity_id != 0)
		return entity_error(m, req, out);
	if (sequence >= m->n_upload)
		return omci_response_mib_upload_next(req, 0, 0, 0, NULL, 0, out);
	return omci_response_mib_upload_next(req,
					     m->upload_snapshot[sequence].class_id,
					     m->upload_snapshot[sequence].entity_id,
					     m->upload_snapshot[sequence].attribute_mask,
					     m->upload_snapshot[sequence].values,
					     m->upload_snapshot[sequence].values_len, out);
}

static int handle_set(struct mib *m, const struct omci_request *req,
		      struct omci_response *out)
{
	struct mib_entity *entity = mib_lookup(m, req->class_id, req->entity_id);
	struct {
		uint8_t index;
		const uint8_t *value;
		size_t len;
		int is_table;
	} updates[16];
	size_t n_updates = 0, cursor = 0;

	if (!entity)
		return entity_error(m, req, out);
	if (req->attribute_mask == 0)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);

	/* Validate every attribute and length before applying the Set atomically. */
	for (uint8_t index = 1; index <= 16; index++) {
		uint16_t bit = omci_attribute_bit(index);
		struct mib_attr *a;
		size_t length, end;

		if (!(req->attribute_mask & bit))
			continue;
		a = mib_entity_attr(entity, index);
		if (!a || !a->writable)
			return omci_response_new(req, OMCI_RESULT_ATTRIBUTE_FAILED, out);
		length = a->is_table ? a->table_row_len : a->value_len;
		if (a->is_table &&
		    __builtin_popcount((unsigned)req->attribute_mask) != 1)
			return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);
		end = cursor + length;
		if (end > req->content_len)
			return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);
		updates[n_updates].index = index;
		updates[n_updates].value = req->content + cursor;
		updates[n_updates].len = length;
		updates[n_updates].is_table = a->is_table;
		n_updates++;
		cursor = end;
	}

	if (req->class_id == 332 /*CLASS_ENHANCED_SECURITY_CONTROL*/ && req->entity_id == 0) {
		struct sec_update su[16];

		for (size_t i = 0; i < n_updates; i++) {
			su[i].index = updates[i].index;
			su[i].value = updates[i].value;
			su[i].len = updates[i].len;
			su[i].is_table = updates[i].is_table;
		}
		if (omci_enhanced_security_apply(m, su, n_updates) < 0)
			return omci_response_new(req, OMCI_RESULT_ATTRIBUTE_FAILED, out);
		mib_increment_sync(m);
		return omci_response_new(req, OMCI_RESULT_SUCCESS, out);
	}

	for (size_t i = 0; i < n_updates; i++) {
		struct mib_attr *a = mib_entity_attr(entity, updates[i].index);

		if (!a)
			continue;
		if (updates[i].is_table) {
			/* key = first 8 bytes; an all-0xff tail deletes the row. */
			const uint8_t *v = updates[i].value;
			size_t rl = updates[i].len;
			int del = 1;

			for (size_t k = 8; k < rl; k++)
				if (v[k] != 0xff)
					del = 0;
			if (rl >= 8) {
				if (del)
					mib_attr_row_remove(a, v, 8);
				else
					mib_attr_row_insert(a, v, 8, v, rl);
			}
		} else {
			free(a->value);
			a->value = malloc(updates[i].len ? updates[i].len : 1);
			if (a->value) {
				memcpy(a->value, updates[i].value, updates[i].len);
				a->value_len = updates[i].len;
			}
		}
	}
	mib_increment_sync(m);
	return omci_response_new(req, OMCI_RESULT_SUCCESS, out);
}

static int handle_set_table(struct mib *m, const struct omci_request *req,
			    struct omci_response *out)
{
	struct mib_entity *entity = mib_lookup(m, req->class_id, req->entity_id);
	struct mib_attr *a;
	uint8_t index;
	size_t row_len, off;

	if (!entity)
		return entity_error(m, req, out);
	if (__builtin_popcount((unsigned)req->attribute_mask) != 1)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);

	index = (uint8_t)(__builtin_clz((unsigned)req->attribute_mask) + 1);
	a = mib_entity_attr(entity, index);
	if (!a)
		return omci_response_new(req, OMCI_RESULT_ATTRIBUTE_FAILED, out);
	if (!a->is_table)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);
	row_len = a->table_row_len;
	if (!a->writable || row_len < 8 || req->content_len == 0 ||
	    req->content_len % row_len)
		return omci_response_new(req, OMCI_RESULT_PARAMETER_ERROR, out);

	/* Set-table applies rows in wire order using each table's add/delete control field. */
	for (off = 0; off + row_len <= req->content_len; off += row_len) {
		const uint8_t *row = req->content + off;
		int del = 1;

		for (size_t k = 8; k < row_len; k++)
			if (row[k] != 0xff)
				del = 0;
		if (del)
			mib_attr_row_remove(a, row, 8);
		else
			mib_attr_row_insert(a, row, 8, row, row_len);
	}
	mib_increment_sync(m);
	return omci_response_new(req, OMCI_RESULT_SUCCESS, out);
}

static int handle_mib_reset(struct mib *m, const struct omci_request *req,
			    struct omci_response *out)
{
	struct mib_entity *saved_loid = NULL;

	if (req->class_id != 0 || req->entity_id != 0)
		return entity_error(m, req, out);

	/* The CTC LOID Authentication ME survives MIB Reset with every attribute unchanged. */
	{
		struct mib_entity *e = mib_lookup(m, 0xfffa, 0);

		if (e)
			saved_loid = mib_entity_copy(e);
	}

	/* Restore the autonomous defaults, then re-attach the preserved LOID ME. */
	for (size_t i = 0; i < m->n_entries; i++) {
		struct mib_entity *x = mib_lookup(m, m->entries[i].class_id, m->entries[i].entity_id);
		if (x)
			mib_entity_clear(x);
	}
	free(m->entries);
	m->entries = NULL;
	m->n_entries = 0;
	for (size_t i = 0; i < m->n_autonomous; i++) {
		struct mib_entity *copy = mib_entity_copy(&m->autonomous_defaults[i].entity);

		if (copy)
			mib_insert(m, m->autonomous_defaults[i].class_id,
				   m->autonomous_defaults[i].entity_id, copy);
	}
	if (saved_loid)
		mib_insert(m, 0xfffa, 0, saved_loid);

	mib_olt_created_clear(m);
	free(m->table_snapshot.bytes);
	m->table_snapshot.bytes = NULL;
	m->has_table_snapshot = false;
	for (size_t i = 0; i < m->n_upload; i++)
		free(m->upload_snapshot[i].values);
	free(m->upload_snapshot);
	m->upload_snapshot = NULL;
	m->n_upload = 0;
	m->has_pending_msk = false;
	free(m->avcs);
	m->avcs = NULL;
	m->n_avcs = 0;
	return omci_response_new(req, 0, out);
}

static int handle_synchronize_time(struct mib *m, const struct omci_request *req,
				   struct omci_response *out)
{
	if (req->class_id != 256 /*CLASS_ONU_G*/ || req->entity_id != 0)
		return entity_error(m, req, out);
	return omci_response_sync_time(req, out);
}

/* ---------- dispatch ---------- */

int mib_dispatch(struct mib *m, const struct omci_request *req,
		 struct omci_response *out)
{
	const struct me_def *def;

	if (req->class_id == 332 && !m->enhanced_security) {
		/* The disabled class is absent from the MIB and the OMCI class table. */
		return omci_response_new(req, OMCI_RESULT_UNKNOWN_ME, out);
	}
	def = pond_me_lookup(req->class_id);
	if (!def)
		return omci_response_new(req, OMCI_RESULT_UNKNOWN_ME, out);
	if (!pond_me_supports_action(def, req->action))
		return omci_response_new(req, OMCI_RESULT_COMMAND_NOT_SUPPORTED, out);

	switch (req->action) {
	case OMCI_ACTION_CREATE:
		return handle_create(m, req, out);
	case OMCI_ACTION_DELETE:
		return handle_delete(m, req, out);
	case OMCI_ACTION_GET:
	case OMCI_ACTION_GET_CURRENT_DATA:
		return handle_get(m, req, out);
	case OMCI_ACTION_GET_ALL_ALARMS:
		return omci_response_get_all_alarms(req, 0, out);
	case OMCI_ACTION_GET_ALL_ALARMS_NEXT:
		return omci_response_get_all_alarms_next_empty(req, out);
	case OMCI_ACTION_GET_NEXT:
		return handle_get_next(m, req, out);
	case OMCI_ACTION_SET:
		return handle_set(m, req, out);
	case OMCI_ACTION_SET_TABLE:
		return handle_set_table(m, req, out);
	case OMCI_ACTION_MIB_UPLOAD:
		return handle_mib_upload(m, req, out);
	case OMCI_ACTION_MIB_UPLOAD_NEXT:
		return handle_mib_upload_next(m, req, out);
	case OMCI_ACTION_MIB_RESET:
		return handle_mib_reset(m, req, out);
	case OMCI_ACTION_SYNCHRONIZE_TIME:
		return handle_synchronize_time(m, req, out);
	default:
		return omci_response_new(req, OMCI_RESULT_COMMAND_NOT_SUPPORTED, out);
	}
}
