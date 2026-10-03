/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OMCI MIB: managed entity instances and their attributes.
 * Port of airoha-pon-daemons omci/mib.rs (data model + builders).
 *
 * The Rust implementation uses BTreeMap throughout, so iteration order is
 * significant (GET-next / MIB-upload walk in key order). The C port keeps
 * sorted arrays with the same ordering guarantee.
 */
#ifndef POND_OMCI_MIB_H
#define POND_OMCI_MIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omci_schema.h"
#include "omci_protocol.h"

#define MIB_MAX_VALUE		256
#define MIB_MAX_ROWS		256
#define MIB_ROW_KEY_MAX		64

/* One table row: the key is the row bytes used for ordering. */
struct mib_row {
	uint8_t key[MIB_ROW_KEY_MAX];
	size_t key_len;
	uint8_t *data;
	size_t data_len;
};

struct mib_attr {
	uint8_t index;
	uint8_t *value;
	size_t value_len;
	bool writable;
	bool is_table;
	size_t table_row_len;
	struct mib_row *rows;
	size_t n_rows;
	bool upload;
};

struct mib_entity {
	struct mib_attr *attrs;		/* sorted by index */
	size_t n_attrs;
};

struct mib_entry {
	uint16_t class_id;
	uint16_t entity_id;
	struct mib_entity entity;
};

struct avc {
	uint16_t class_id;
	uint16_t entity_id;
	uint16_t attribute_mask;
	uint8_t value[64];
	size_t value_len;
};

struct table_snapshot {
	uint16_t class_id;
	uint16_t entity_id;
	uint16_t attribute_mask;
	uint8_t *bytes;
	size_t bytes_len;
};

struct upload_record {
	uint16_t class_id;
	uint16_t entity_id;
	uint16_t attribute_mask;
	uint8_t *values;
	size_t values_len;
};

struct mib {
	struct mib_entry *entries;	/* sorted by (class_id, entity_id) */
	size_t n_entries;
	struct mib_entry *autonomous_defaults;
	size_t n_autonomous;
	uint16_t *known_classes;
	size_t n_known_classes;
	struct { uint16_t class_id, entity_id; } *olt_created;
	size_t n_olt_created;
	uint16_t *dynamic_classes;
	size_t n_dynamic_classes;
	bool has_table_snapshot;
	struct table_snapshot table_snapshot;
	struct upload_record *upload_snapshot;
	size_t n_upload;
	uint8_t onu_serial[8];
	bool has_pending_msk;
	uint8_t pending_msk[16];
	struct avc *avcs;
	size_t n_avcs;
	bool enhanced_security;
};

/* ---------- entity / attribute builders (mib.rs ManagedEntity::*) ---------- */

struct mib_entity *mib_entity_new(void);
void mib_entity_free(struct mib_entity *e);
/* Release contents only (entity embedded in an array). */
void mib_entity_clear(struct mib_entity *e);

void mib_entity_read_only(struct mib_entity *e, uint8_t index,
			  const uint8_t *value, size_t len);
void mib_entity_read_write(struct mib_entity *e, uint8_t index,
			   const uint8_t *value, size_t len);
void mib_entity_table(struct mib_entity *e, uint8_t index, size_t row_len);
void mib_entity_read_only_table(struct mib_entity *e, uint8_t index, size_t row_len);

struct mib_attr *mib_entity_attr(struct mib_entity *e, uint8_t index);
const struct mib_attr *mib_entity_attr_const(const struct mib_entity *e, uint8_t index);

/* Insert/replace a table row (keyed by key bytes, kept sorted). */
int mib_attr_row_insert(struct mib_attr *a, const uint8_t *key, size_t key_len,
			const uint8_t *data, size_t data_len);
size_t mib_attr_row_count(const struct mib_attr *a);
int mib_attr_row_remove(struct mib_attr *a, const uint8_t *key, size_t key_len);
void mib_attr_row_clear(struct mib_attr *a);

/* Deep copy (used by MIB Reset to restore the autonomous defaults). */
struct mib_entity *mib_entity_copy(const struct mib_entity *src);

/*
 * Build an entity instance from the schema definition.
 * `payload` may be NULL; when non-NULL the Create offsets are honoured
 * (Required offsets must be present or the call fails).
 * Returns NULL when the class is not registered or a required field is short.
 */
struct mib_entity *mib_entity_from_definition(uint16_t class_id,
					      const uint8_t *payload, size_t payload_len);

/* ---------- MIB container ---------- */

void mib_init(struct mib *m);
void mib_free(struct mib *m);

/* Insert or replace an entity instance. */
int mib_insert(struct mib *m, uint16_t class_id, uint16_t entity_id,
	       struct mib_entity *entity);

struct mib_entity *mib_lookup(struct mib *m, uint16_t class_id, uint16_t entity_id);
int mib_remove(struct mib *m, uint16_t class_id, uint16_t entity_id);
size_t mib_count(const struct mib *m);

/* Ordered accessors used by GET-next / MIB-upload walks. */
const struct mib_entry *mib_entry_at(const struct mib *m, size_t index);

void mib_record_avc(struct mib *m, uint16_t class_id, uint16_t entity_id,
		    uint16_t attribute_mask, const uint8_t *value, size_t len);
size_t mib_take_avcs(struct mib *m, struct avc **out);

/* ---------- sets used by the handlers ---------- */

void mib_known_class_add(struct mib *m, uint16_t class_id);
bool mib_known_class_has(const struct mib *m, uint16_t class_id);
void mib_dynamic_class_add(struct mib *m, uint16_t class_id);
bool mib_dynamic_class_has(const struct mib *m, uint16_t class_id);
void mib_olt_created_add(struct mib *m, uint16_t class_id, uint16_t entity_id);
bool mib_olt_created_remove(struct mib *m, uint16_t class_id, uint16_t entity_id);
void mib_olt_created_clear(struct mib *m);

/* MIB Data Sync counter on ONU-DATA attribute 1 (cycles 1..255). */
void mib_increment_sync(struct mib *m);

/* ---------- initial MIB from the identity (omci_mib_init.c) ---------- */

struct identity_config;
void mib_populate_from_identity(struct mib *m, const struct identity_config *id);
void mib_set_onu_serial(struct mib *m, const uint8_t serial[8]);
/* Publish state 3 (installed) or 4 (failed) after the driver takes the OIK. */
void mib_complete_msk_install(struct mib *m, bool installed);

/* ---------- request dispatch (mib.rs Mib::dispatch) ---------- */

int mib_dispatch(struct mib *m, const struct omci_request *req,
		 struct omci_response *out);

#endif
