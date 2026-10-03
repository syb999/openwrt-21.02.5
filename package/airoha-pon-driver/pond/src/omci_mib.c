/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci_mib.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ---------- small helpers ---------- */

static void *xcalloc(size_t n, size_t sz)
{
	return calloc(n ? n : 1, sz ? sz : 1);
}

static int cmp_bytes(const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
	size_t n = alen < blen ? alen : blen;
	int rc = n ? memcmp(a, b, n) : 0;

	if (rc)
		return rc;
	if (alen == blen)
		return 0;
	return alen < blen ? -1 : 1;
}

/* grow a void** array of n elements of size sz by one slot */
static void *grow(void *base, size_t *n, size_t sz)
{
	void *p = realloc(base, (*n + 1) * sz);

	if (!p)
		return NULL;
	*n += 1;
	return p;
}

/* ---------- entity / attribute builders ---------- */

struct mib_entity *mib_entity_new(void)
{
	return xcalloc(1, sizeof(struct mib_entity));
}

/* Release the contents of an entity (which may be embedded in an array). */
void mib_entity_clear(struct mib_entity *e)
{
	if (!e)
		return;
	for (size_t i = 0; i < e->n_attrs; i++) {
		struct mib_attr *a = &e->attrs[i];

		free(a->value);
		for (size_t j = 0; j < a->n_rows; j++)
			free(a->rows[j].data);
		free(a->rows);
	}
	free(e->attrs);
	memset(e, 0, sizeof(*e));
}

void mib_entity_free(struct mib_entity *e)
{
	if (!e)
		return;
	mib_entity_clear(e);
	free(e);
}

struct mib_attr *mib_entity_attr(struct mib_entity *e, uint8_t index)
{
	for (size_t i = 0; i < e->n_attrs; i++)
		if (e->attrs[i].index == index)
			return &e->attrs[i];
	return NULL;
}

const struct mib_attr *mib_entity_attr_const(const struct mib_entity *e, uint8_t index)
{
	for (size_t i = 0; i < e->n_attrs; i++)
		if (e->attrs[i].index == index)
			return &e->attrs[i];
	return NULL;
}

/*
 * Insert (or replace) an attribute, keeping the array sorted by index —
 * the Rust BTreeMap<u8, Attribute> iteration order.
 */
static struct mib_attr *attr_upsert(struct mib_entity *e, uint8_t index)
{
	struct mib_attr *a = mib_entity_attr(e, index);
	size_t pos;

	if (a)
		return a;

	for (pos = 0; pos < e->n_attrs && e->attrs[pos].index < index; pos++)
		;
	e->attrs = grow(e->attrs, &e->n_attrs, sizeof(struct mib_attr));
	if (!e->attrs)
		return NULL;
	memmove(&e->attrs[pos + 1], &e->attrs[pos],
		(e->n_attrs - 1 - pos) * sizeof(struct mib_attr));
	memset(&e->attrs[pos], 0, sizeof(struct mib_attr));
	e->attrs[pos].index = index;
	return &e->attrs[pos];
}

static void attr_set_value(struct mib_attr *a, const uint8_t *value, size_t len)
{
	free(a->value);
	a->value = xcalloc(len ? len : 1, 1);
	if (value && len)
		memcpy(a->value, value, len);
	a->value_len = len;
}

void mib_entity_read_only(struct mib_entity *e, uint8_t index,
			  const uint8_t *value, size_t len)
{
	struct mib_attr *a = attr_upsert(e, index);

	if (!a)
		return;
	attr_set_value(a, value, len);
	a->writable = false;
	a->is_table = false;
	a->upload = true;
}

void mib_entity_read_write(struct mib_entity *e, uint8_t index,
			   const uint8_t *value, size_t len)
{
	struct mib_attr *a = attr_upsert(e, index);

	if (!a)
		return;
	attr_set_value(a, value, len);
	a->writable = true;
	a->is_table = false;
	a->upload = true;
}

void mib_entity_table(struct mib_entity *e, uint8_t index, size_t row_len)
{
	struct mib_attr *a = attr_upsert(e, index);

	if (!a)
		return;
	attr_set_value(a, NULL, 0);
	a->writable = true;
	a->is_table = true;
	a->table_row_len = row_len;
	a->upload = false;
}

void mib_entity_read_only_table(struct mib_entity *e, uint8_t index, size_t row_len)
{
	struct mib_attr *a = attr_upsert(e, index);

	if (!a)
		return;
	attr_set_value(a, NULL, 0);
	a->writable = false;
	a->is_table = true;
	a->table_row_len = row_len;
	a->upload = false;
}

int mib_attr_row_insert(struct mib_attr *a, const uint8_t *key, size_t key_len,
			const uint8_t *data, size_t data_len)
{
	size_t pos, old;

	if (key_len > MIB_ROW_KEY_MAX || a->n_rows >= MIB_MAX_ROWS)
		return -E2BIG;

	for (pos = 0; pos < a->n_rows; pos++) {
		int rc = cmp_bytes(a->rows[pos].key, a->rows[pos].key_len, key, key_len);

		if (rc == 0) {
			/* replace the row payload, keep the key */
			uint8_t *nd = xcalloc(data_len ? data_len : 1, 1);

			if (!nd)
				return -ENOMEM;
			if (data && data_len)
				memcpy(nd, data, data_len);
			free(a->rows[pos].data);
			a->rows[pos].data = nd;
			a->rows[pos].data_len = data_len;
			return 0;
		}
		if (rc > 0)
			break;
	}

	old = a->n_rows;
	a->rows = grow(a->rows, &a->n_rows, sizeof(struct mib_row));
	if (!a->rows)
		return -ENOMEM;
	memmove(&a->rows[pos + 1], &a->rows[pos], (old - pos) * sizeof(struct mib_row));
	memset(&a->rows[pos], 0, sizeof(struct mib_row));
	memcpy(a->rows[pos].key, key, key_len);
	a->rows[pos].key_len = key_len;
	a->rows[pos].data = xcalloc(data_len ? data_len : 1, 1);
	if (!a->rows[pos].data)
		return -ENOMEM;
	if (data && data_len)
		memcpy(a->rows[pos].data, data, data_len);
	a->rows[pos].data_len = data_len;
	return 0;
}

size_t mib_attr_row_count(const struct mib_attr *a)
{
	return a ? a->n_rows : 0;
}

/*
 * Build an instance from the schema definition, honouring the Create payload
 * offsets exactly like mib.rs entity_from_definition().
 */
struct mib_entity *mib_entity_from_definition(uint16_t class_id,
					      const uint8_t *payload, size_t payload_len)
{
	const struct me_def *def = pond_me_lookup(class_id);
	struct mib_entity *e;

	if (!def)
		return NULL;
	e = mib_entity_new();
	if (!e)
		return NULL;

	for (size_t i = 0; i < def->n_attrs; i++) {
		const struct me_attr *ad = &def->attrs[i];
		struct mib_attr *a;

		if (ad->kind == ATTR_TABLE) {
			if (ad->access == ATTR_RW)
				mib_entity_table(e, ad->index, ad->length);
			else
				mib_entity_read_only_table(e, ad->index, ad->length);
			continue;
		}

		/* scalar */
		{
			uint8_t buf[MIB_MAX_VALUE];
			const uint8_t *value = buf;
			size_t len = ad->length;
			bool have = false;

			if (len > sizeof(buf)) {
				mib_entity_free(e);
				return NULL;
			}
			if (payload && ad->create == CREATE_REQUIRED) {
				if (payload_len < (size_t)ad->create_offset + len) {
					/* Required offset must be fully present. */
					mib_entity_free(e);
					return NULL;
				}
				memcpy(buf, payload + ad->create_offset, len);
				have = true;
			} else if (payload && ad->create == CREATE_OPTIONAL &&
				   payload_len >= (size_t)ad->create_offset + len) {
				memcpy(buf, payload + ad->create_offset, len);
				have = true;
			}
			if (!have) {
				if (ad->def && ad->def_len) {
					memset(buf, 0, len);
					memcpy(buf, ad->def, ad->def_len < len ? ad->def_len : len);
				} else {
					memset(buf, 0, len);
				}
			}
			if (ad->access == ATTR_RW)
				mib_entity_read_write(e, ad->index, value, len);
			else
				mib_entity_read_only(e, ad->index, value, len);
			/* the schema upload flag overrides the default (true) */
			a = mib_entity_attr(e, ad->index);
			if (a)
				a->upload = ad->upload;
		}
	}
	return e;
}

/* ---------- MIB container ---------- */

void mib_init(struct mib *m)
{
	memset(m, 0, sizeof(*m));
}

void mib_free(struct mib *m)
{
	if (!m)
		return;
	for (size_t i = 0; i < m->n_entries; i++)
		mib_entity_clear(&m->entries[i].entity);
	free(m->entries);
	for (size_t i = 0; i < m->n_autonomous; i++)
		mib_entity_clear(&m->autonomous_defaults[i].entity);
	free(m->autonomous_defaults);
	free(m->known_classes);
	free(m->olt_created);
	free(m->dynamic_classes);
	free(m->table_snapshot.bytes);
	for (size_t i = 0; i < m->n_upload; i++)
		free(m->upload_snapshot[i].values);
	free(m->upload_snapshot);
	free(m->avcs);
	memset(m, 0, sizeof(*m));
}

static int entry_cmp(uint16_t ac, uint16_t ae, uint16_t bc, uint16_t be)
{
	if (ac != bc)
		return ac < bc ? -1 : 1;
	if (ae != be)
		return ae < be ? -1 : 1;
	return 0;
}

static size_t entry_find(const struct mib *m, uint16_t class_id, uint16_t entity_id)
{
	size_t lo = 0, hi = m->n_entries;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int rc = entry_cmp(m->entries[mid].class_id, m->entries[mid].entity_id,
				   class_id, entity_id);

		if (rc == 0)
			return mid;
		if (rc < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;	/* insertion point */
}

int mib_insert(struct mib *m, uint16_t class_id, uint16_t entity_id,
	       struct mib_entity *entity)
{
	size_t pos = entry_find(m, class_id, entity_id);
	size_t old;

	if (pos < m->n_entries &&
	    m->entries[pos].class_id == class_id && m->entries[pos].entity_id == entity_id) {
		mib_entity_clear(&m->entries[pos].entity);
		m->entries[pos].entity = *entity;
		free(entity);
		return 0;
	}
	old = m->n_entries;
	m->entries = grow(m->entries, &m->n_entries, sizeof(struct mib_entry));
	if (!m->entries)
		return -ENOMEM;
	memmove(&m->entries[pos + 1], &m->entries[pos], (old - pos) * sizeof(struct mib_entry));
	memset(&m->entries[pos], 0, sizeof(struct mib_entry));
	m->entries[pos].class_id = class_id;
	m->entries[pos].entity_id = entity_id;
	m->entries[pos].entity = *entity;
	free(entity);
	return 0;
}

struct mib_entity *mib_lookup(struct mib *m, uint16_t class_id, uint16_t entity_id)
{
	size_t pos = entry_find(m, class_id, entity_id);

	if (pos < m->n_entries &&
	    m->entries[pos].class_id == class_id && m->entries[pos].entity_id == entity_id)
		return &m->entries[pos].entity;
	return NULL;
}

int mib_remove(struct mib *m, uint16_t class_id, uint16_t entity_id)
{
	size_t pos = entry_find(m, class_id, entity_id);

	if (pos >= m->n_entries || m->entries[pos].class_id != class_id ||
	    m->entries[pos].entity_id != entity_id)
		return -ENOENT;
	mib_entity_clear(&m->entries[pos].entity);
	memmove(&m->entries[pos], &m->entries[pos + 1],
		(m->n_entries - pos - 1) * sizeof(struct mib_entry));
	m->n_entries--;
	return 0;
}

size_t mib_count(const struct mib *m)
{
	return m->n_entries;
}

const struct mib_entry *mib_entry_at(const struct mib *m, size_t index)
{
	return index < m->n_entries ? &m->entries[index] : NULL;
}

void mib_record_avc(struct mib *m, uint16_t class_id, uint16_t entity_id,
		    uint16_t attribute_mask, const uint8_t *value, size_t len)
{
	struct avc *a;

	if (len > sizeof(m->avcs[0].value))
		len = sizeof(m->avcs[0].value);
	m->avcs = grow(m->avcs, &m->n_avcs, sizeof(struct avc));
	if (!m->avcs)
		return;
	a = &m->avcs[m->n_avcs - 1];
	memset(a, 0, sizeof(*a));
	a->class_id = class_id;
	a->entity_id = entity_id;
	a->attribute_mask = attribute_mask;
	if (value && len)
		memcpy(a->value, value, len);
	a->value_len = len;
}

size_t mib_take_avcs(struct mib *m, struct avc **out)
{
	size_t n = m->n_avcs;

	*out = m->avcs;
	m->avcs = NULL;
	m->n_avcs = 0;
	return n;
}


int mib_attr_row_remove(struct mib_attr *a, const uint8_t *key, size_t key_len)
{
	for (size_t i = 0; i < a->n_rows; i++) {
		if (a->rows[i].key_len != key_len ||
		    (key_len && memcmp(a->rows[i].key, key, key_len)))
			continue;
		free(a->rows[i].data);
		memmove(&a->rows[i], &a->rows[i + 1],
			(a->n_rows - i - 1) * sizeof(struct mib_row));
		a->n_rows--;
		return 0;
	}
	return -ENOENT;
}

void mib_attr_row_clear(struct mib_attr *a)
{
	if (!a)
		return;
	for (size_t i = 0; i < a->n_rows; i++)
		free(a->rows[i].data);
	free(a->rows);
	a->rows = NULL;
	a->n_rows = 0;
}

static void attr_copy(struct mib_attr *dst, const struct mib_attr *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->index = src->index;
	dst->writable = src->writable;
	dst->is_table = src->is_table;
	dst->table_row_len = src->table_row_len;
	dst->upload = src->upload;
	if (src->value_len) {
		dst->value = malloc(src->value_len);
		if (dst->value) {
			memcpy(dst->value, src->value, src->value_len);
			dst->value_len = src->value_len;
		}
	}
	for (size_t i = 0; i < src->n_rows; i++) {
		struct mib_row row;

		memset(&row, 0, sizeof(row));
		memcpy(row.key, src->rows[i].key, src->rows[i].key_len);
		row.key_len = src->rows[i].key_len;
		row.data = malloc(src->rows[i].data_len ? src->rows[i].data_len : 1);
		if (!row.data)
			break;
		memcpy(row.data, src->rows[i].data, src->rows[i].data_len);
		row.data_len = src->rows[i].data_len;
		dst->rows = realloc(dst->rows, (dst->n_rows + 1) * sizeof(row));
		if (!dst->rows)
			break;
		dst->rows[dst->n_rows++] = row;
	}
}

struct mib_entity *mib_entity_copy(const struct mib_entity *src)
{
	struct mib_entity *e;

	if (!src)
		return NULL;
	e = xcalloc(1, sizeof(*e));
	if (!e)
		return NULL;
	for (size_t i = 0; i < src->n_attrs; i++) {
		e->attrs = realloc(e->attrs, (e->n_attrs + 1) * sizeof(struct mib_attr));
		if (!e->attrs)
			break;
		attr_copy(&e->attrs[e->n_attrs], &src->attrs[i]);
		e->n_attrs++;
	}
	return e;
}

void mib_olt_created_clear(struct mib *m)
{
	free(m->olt_created);
	m->olt_created = NULL;
	m->n_olt_created = 0;
}
