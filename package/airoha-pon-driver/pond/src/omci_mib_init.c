/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Initial MIB construction from the identity configuration.
 * Port of airoha-pon-daemons mib.rs Mib::from_identity.
 */
#include "omci_config.h"
#include "omci_mib.h"
#include "omci_schema.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TCONT_COUNT			16
#define TCONT_FIRST_ENTITY		0x8000
#define ETHERNET_UNI_COUNT		4
#define QUEUES_PER_TCONT		8
#define QUEUES_PER_UNI			8
#define UPSTREAM_PRIORITY_QUEUE_COUNT	(TCONT_COUNT * QUEUES_PER_TCONT)
#define DOWNSTREAM_PRIORITY_QUEUE_COUNT	(ETHERNET_UNI_COUNT * QUEUES_PER_UNI)
#define TOTAL_PRIORITY_QUEUE_COUNT	(UPSTREAM_PRIORITY_QUEUE_COUNT + \
					 DOWNSTREAM_PRIORITY_QUEUE_COUNT)
#define TOTAL_GEM_PORT_COUNT		256
#define VEIP_ENTITY			0x0a01

#define CLASS_PPTP_ETHERNET_UNI		11
#define CLASS_MAC_BRIDGE_PORT_FILTER_PREASSIGN_DATA 79
#define CLASS_MAC_BRIDGE_PORT_CONFIG_DATA 47
#define CLASS_OMCI			287

static void put16(uint8_t *buf, uint16_t v)
{
	buf[0] = (uint8_t)(v >> 8);
	buf[1] = (uint8_t)v;
}

/* ManagedEntity::read_only/read_write/table helpers over the raw builders. */
static void ro(struct mib_entity *e, uint8_t idx, const uint8_t *v, size_t len)
{
	mib_entity_read_only(e, idx, v, len);
}
static void rw(struct mib_entity *e, uint8_t idx, const uint8_t *v, size_t len)
{
	mib_entity_read_write(e, idx, v, len);
}
static void ro_u8(struct mib_entity *e, uint8_t idx, uint8_t v)
{
	mib_entity_read_only(e, idx, &v, 1);
}
static void rw_u8(struct mib_entity *e, uint8_t idx, uint8_t v)
{
	mib_entity_read_write(e, idx, &v, 1);
}
static void ro_u16(struct mib_entity *e, uint8_t idx, uint16_t v)
{
	uint8_t b[2];

	put16(b, v);
	mib_entity_read_only(e, idx, b, 2);
}
static void rw_u16(struct mib_entity *e, uint8_t idx, uint16_t v)
{
	uint8_t b[2];

	put16(b, v);
	mib_entity_read_write(e, idx, b, 2);
}
static void ro_zero(struct mib_entity *e, uint8_t idx, size_t len)
{
	uint8_t z[64];

	if (len > sizeof(z))
		len = sizeof(z);
	memset(z, 0, len);
	mib_entity_read_only(e, idx, z, len);
}
static void rw_zero(struct mib_entity *e, uint8_t idx, size_t len)
{
	uint8_t z[64];

	if (len > sizeof(z))
		len = sizeof(z);
	memset(z, 0, len);
	mib_entity_read_write(e, idx, z, len);
}

static void insert(struct mib *m, uint16_t class_id, uint16_t entity_id,
		   struct mib_entity *e)
{
	mib_known_class_add(m, class_id);
	mib_insert(m, class_id, entity_id, e);
}

/*
 * Airoha priority queue: attribute 6 packs the related port and priority.
 */
static struct mib_entity *priority_queue_entity(uint16_t related_port, uint16_t priority)
{
	struct mib_entity *e = mib_entity_new();
	uint8_t related[4];

	put16(related, related_port);
	put16(related + 2, priority);

	ro_u8(e, 1, 1);
	ro_u16(e, 2, 0xffff);
	rw_u16(e, 3, 4);
	rw(e, 6, related, 4);
	rw_u16(e, 7, 0);
	rw_u8(e, 8, 1);
	rw_u16(e, 9, 0);
	{
		uint8_t b[4] = { 0, 0, 0, 0 };

		rw(e, 10, b, 4);
	}
	rw_u16(e, 11, 0xffff);
	rw_u16(e, 12, 0);
	return e;
}

/*
 * Class 287 advertises the supported ME classes (attribute 1) and actions
 * (attribute 2) as table rows.
 */
static struct mib_entity *omci_capability_entity(bool enhanced_security)
{
	struct mib_entity *e = mib_entity_from_definition(CLASS_OMCI, NULL, 0);
	struct mib_attr *class_table, *message_table;

	if (!e)
		return NULL;
	class_table = mib_entity_attr(e, 1);
	message_table = mib_entity_attr(e, 2);
	if (!class_table || !message_table)
		return e;

	for (size_t i = 0; i < pond_me_table_len; i++) {
		const struct me_def *d = &pond_me_table[i];
		uint8_t row[2];

		/*
		 * Class 351 lies in the vendor-specific range; advertising it could
		 * invite another vendor's layout.
		 */
		if (d->class_id == 351)
			continue;
		if (!enhanced_security && d->class_id == 332)
			continue;
		put16(row, d->class_id);
		mib_attr_row_insert(class_table, row, 2, row, 2);
	}
	for (uint8_t action = 0; action < 32; action++) {
		int supported = 0;

		for (size_t i = 0; i < pond_me_table_len; i++)
			if (pond_me_supports_action(&pond_me_table[i], action))
				supported = 1;
		if (supported)
			mib_attr_row_insert(message_table, &action, 1, &action, 1);
	}
	return e;
}

static void fixed_width(const uint8_t *value, size_t len, uint8_t *out, size_t width)
{
	identity_fixed_width(value, len, out, width);
}

void mib_populate_from_identity(struct mib *m, const struct identity_config *id)
{
	uint8_t serial[8];
	uint8_t tmp[64];

	identity_onu_g_serial(id, serial);
	memcpy(m->onu_serial, serial, 8);
	m->enhanced_security = !id->disable_enhanced_security;

	/* ONU-DATA: the MIB Data Sync attribute. */
	{
		struct mib_entity *e = mib_entity_new();

		rw_u8(e, 1, 0);
		insert(m, 2, 0, e);
	}

	/* Software image 0 is active; image 1 is the valid standby slot. */
	fixed_width(id->software_version, id->software_version_len, tmp, 14);
	{
		struct mib_entity *e = mib_entity_new();

		ro(e, 1, tmp, 14);
		ro_u8(e, 2, 1);
		ro_u8(e, 3, 1);
		ro_u8(e, 4, 1);
		insert(m, 7, 0, e);
	}
	{
		struct mib_entity *e = mib_entity_new();

		ro(e, 1, tmp, 14);
		ro_u8(e, 2, 0);
		ro_u8(e, 3, 0);
		ro_u8(e, 4, 1);
		insert(m, 7, 1, e);
	}

	/* ONU-G */
	{
		struct mib_entity *e = mib_entity_new();

		fixed_width(id->vendor_id, id->vendor_id_len, tmp, 4);
		ro(e, 1, tmp, 4);
		fixed_width(id->hardware_version, id->hardware_version_len, tmp, 14);
		ro(e, 2, tmp, 14);
		ro(e, 3, serial, 8);
		/* Option 2 advertises priority-controlled scheduling with rate-controlled shaping. */
		ro_u8(e, 4, 0x02);
		ro_u8(e, 5, 0);
		rw_u8(e, 6, 0);
		rw_u8(e, 7, 0);
		ro_u8(e, 8, 0);
		ro_u8(e, 9, 0);
		fixed_width(id->loid, id->loid_len, tmp, 24);
		ro(e, 10, tmp, 24);
		fixed_width(id->loid_password, id->loid_password_len, tmp, 12);
		ro(e, 11, tmp, 12);
		rw_u8(e, 12, 0);
		ro_u8(e, 13, 0);
		insert(m, 256, 0, e);
	}

	/* ONU2-G */
	{
		struct mib_entity *e = mib_entity_new();

		fixed_width(id->equipment_id, id->equipment_id_len, tmp, 20);
		ro(e, 1, tmp, 20);
		ro_u8(e, 2, id->omcc_version);
		{
			uint8_t b[2] = { 0, 0 };

			ro(e, 3, b, 2);
		}
		ro_u8(e, 4, 1);
		rw_u8(e, 5, 1);
		ro_u16(e, 6, TOTAL_PRIORITY_QUEUE_COUNT);
		ro_u8(e, 7, TCONT_COUNT);
		ro_u8(e, 8, 1);
		ro_u16(e, 9, TOTAL_GEM_PORT_COUNT);
		ro_zero(e, 10, 4);
		{
			uint8_t b[2] = { 0, 0 };

			ro(e, 11, b, 2);
		}
		rw_u8(e, 12, 0);
		/* Airoha traffic management options occupy the low six bits. */
		ro_u16(e, 13, 0x003f);
		rw_u16(e, 14, 1);
		insert(m, 257, 0, e);
	}

	/* Class 11 entity IDs 1..4 are the four Ethernet UNIs. */
	for (uint16_t entity_id = 1; entity_id <= ETHERNET_UNI_COUNT; entity_id++) {
		struct mib_entity *e = mib_entity_new();

		rw_u8(e, 1, 0x2f);
		ro_u8(e, 2, 0x2f);
		rw_u8(e, 3, 0);
		rw_u8(e, 4, 0);
		rw_u8(e, 5, 0);
		ro_u8(e, 6, 1);
		ro_u8(e, 7, 0);
		rw_u16(e, 8, 0x05ee);
		rw_u8(e, 9, 0);
		rw_u16(e, 10, 0);
		rw_u8(e, 11, 0);
		rw_u8(e, 12, 0);
		rw_u8(e, 13, 0);
		rw_u8(e, 14, 0);
		rw_u8(e, 15, 0);
		insert(m, CLASS_PPTP_ETHERNET_UNI, entity_id, e);
	}

	/* ONU power shedding: autonomous singleton with zeroed timers. */
	{
		struct mib_entity *e = mib_entity_new();

		rw_u16(e, 1, 0);
		rw_u16(e, 2, 0);
		rw_u16(e, 4, 0);
		rw_u16(e, 5, 0);
		rw_u16(e, 6, 0);
		rw_u16(e, 7, 0);
		rw_u16(e, 8, 0);
		rw_u16(e, 9, 0);
		rw_u16(e, 10, 0);
		rw_u16(e, 11, 0);
		ro_u8(e, 12, 0);
		insert(m, 133, 0, e);
	}

	/* CTC LOID authentication */
	{
		struct mib_entity *e = mib_entity_new();

		fixed_width(id->operator_id, id->operator_id_len, tmp, 4);
		ro(e, 1, tmp, 4);
		fixed_width(id->loid, id->loid_len, tmp, 24);
		ro(e, 2, tmp, 24);
		fixed_width(id->loid_password, id->loid_password_len, tmp, 12);
		ro(e, 3, tmp, 12);
		rw_u8(e, 4, 0);
		insert(m, 0xfffa, 0, e);
	}

	if (!id->disable_enhanced_security) {
		/* Class 332 negotiates cryptographic capabilities and key length per O5 epoch. */
		struct mib_entity *e = mib_entity_new();

		rw_zero(e, 1, 16);
		mib_entity_table(e, 2, 17);
		rw_u8(e, 3, 0);
		ro_u8(e, 4, 1);
		mib_entity_read_only_table(e, 5, 16);
		mib_entity_read_only_table(e, 6, 16);
		mib_entity_table(e, 7, 17);
		rw_u8(e, 8, 0);
		ro_u8(e, 9, 0);
		rw_zero(e, 10, 16);
		mib_entity_table(e, 11, 18);
		ro_u16(e, 12, 128);
		insert(m, 332, 0, e);
	}

	/* Airoha maps the 16 onboard T-CONTs to entity IDs 0x8000..0x800f. */
	for (uint16_t off = 0; off < TCONT_COUNT; off++) {
		struct mib_entity *e = mib_entity_new();

		rw_u16(e, 1, 0xffff);	/* unassigned Alloc-ID */
		ro_u8(e, 2, 1);
		rw_u8(e, 3, 0);
		insert(m, 262, (uint16_t)(TCONT_FIRST_ENTITY + off), e);
	}

	/* Each T-CONT and Ethernet UNI owns eight priority queues. */
	for (uint16_t off = 0; off < UPSTREAM_PRIORITY_QUEUE_COUNT; off++) {
		uint16_t entity_id = (uint16_t)(TCONT_FIRST_ENTITY + off);
		uint16_t tcont_id = (uint16_t)(TCONT_FIRST_ENTITY + off / QUEUES_PER_TCONT);
		uint16_t priority = (uint16_t)(7 - (off % QUEUES_PER_TCONT));

		insert(m, 277, entity_id, priority_queue_entity(tcont_id, priority));
	}
	for (uint16_t off = 0; off < DOWNSTREAM_PRIORITY_QUEUE_COUNT; off++) {
		uint16_t uni_id = (uint16_t)(1 + off / QUEUES_PER_UNI);
		uint16_t priority = (uint16_t)(off % QUEUES_PER_UNI);

		insert(m, 277, off, priority_queue_entity((uint16_t)(0x0100 + uni_id), priority));
	}
	for (uint16_t off = 0; off < TCONT_COUNT; off++) {
		uint16_t entity_id = (uint16_t)(TCONT_FIRST_ENTITY + off);
		struct mib_entity *e = mib_entity_new();

		rw_u16(e, 1, entity_id);
		ro_u16(e, 2, 0);
		rw_u8(e, 3, 1);
		rw_u8(e, 4, 0);
		insert(m, 278, entity_id, e);
	}

	/* VEIP */
	{
		struct mib_entity *e = mib_entity_new();

		rw_u8(e, 1, 0);
		ro_u8(e, 2, 0);
		rw_zero(e, 3, 25);
		rw_u16(e, 4, 0xffff);
		{
			uint8_t b[2] = { 0, 0 };

			ro(e, 5, b, 2);
		}
		insert(m, 329, VEIP_ENTITY, e);
	}

	/* Schema-driven singletons */
	{
		struct mib_entity *e = mib_entity_from_definition(131, NULL, 0);

		if (e)
			insert(m, 131, 0, e);
	}
	for (uint16_t entity_id = 1; entity_id <= ETHERNET_UNI_COUNT; entity_id++) {
		struct mib_entity *e = mib_entity_from_definition(264, NULL, 0);

		if (e)
			insert(m, 264, entity_id, e);
	}
	{
		struct mib_entity *e = omci_capability_entity(!id->disable_enhanced_security);

		if (e)
			insert(m, CLASS_OMCI, 0, e);
	}

	/* known_classes / dynamic_classes follow the schema definition. */
	for (size_t i = 0; i < pond_me_table_len; i++) {
		mib_known_class_add(m, pond_me_table[i].class_id);
		if (pond_me_table[i].origin == ME_ORIGIN_OLT)
			mib_dynamic_class_add(m, pond_me_table[i].class_id);
	}

	/* The MIB Reset template preserves the initial attributes of autonomous entities. */
	{
		free(m->autonomous_defaults);
		m->autonomous_defaults = NULL;
		m->n_autonomous = 0;
		for (size_t i = 0; i < m->n_entries; i++) {
			struct mib_entity *copy = mib_entity_copy(&m->entries[i].entity);

			if (!copy)
				continue;
			m->autonomous_defaults = realloc(m->autonomous_defaults,
				(m->n_autonomous + 1) * sizeof(*m->autonomous_defaults));
			if (!m->autonomous_defaults)
				break;
			m->autonomous_defaults[m->n_autonomous].class_id = m->entries[i].class_id;
			m->autonomous_defaults[m->n_autonomous].entity_id = m->entries[i].entity_id;
			m->autonomous_defaults[m->n_autonomous].entity = *copy;
			free(copy);
			m->n_autonomous++;
		}
	}
}

void mib_set_onu_serial(struct mib *m, const uint8_t serial[8])
{
	struct mib_entity *e;

	memcpy(m->onu_serial, serial, 8);
	/* ONU-G attribute 3 and Class 332 use the same active PLOAM serial number. */
	e = mib_lookup(m, 256, 0);
	if (e) {
		struct mib_attr *a = mib_entity_attr(e, 3);

		if (a) {
			free(a->value);
			a->value = malloc(8);
			if (a->value) {
				memcpy(a->value, serial, 8);
				a->value_len = 8;
			}
		}
	}
	for (size_t i = 0; i < m->n_autonomous; i++) {
		if (m->autonomous_defaults[i].class_id != 256)
			continue;
		{
			struct mib_attr *a = mib_entity_attr(&m->autonomous_defaults[i].entity, 3);

			if (a) {
				free(a->value);
				a->value = malloc(8);
				if (a->value) {
					memcpy(a->value, serial, 8);
					a->value_len = 8;
				}
			}
		}
	}
}
