/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Class 332 (Enhanced security control) state machine.
 * Port of airoha-pon-daemons omci/mib.rs apply_enhanced_security_updates /
 * enhanced_security_step1 / enhanced_security_step2 / enhanced_security_challenges.
 */
#include "omci_security.h"
#include "omci_protocol.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define CLASS_ENHANCED_SECURITY_CONTROL 332
#define CLASS_ONU_G			256
#define ENTITY_ENHANCED_SECURITY	0

static struct mib_entity *security_entity(struct mib *m)
{
	return mib_lookup(m, CLASS_ENHANCED_SECURITY_CONTROL, ENTITY_ENHANCED_SECURITY);
}

static void queue_avc(struct mib *m, uint8_t attribute_index, const uint8_t *value, size_t len)
{
	mib_record_avc(m, CLASS_ENHANCED_SECURITY_CONTROL, ENTITY_ENHANCED_SECURITY,
		       omci_attribute_bit(attribute_index), value, len);
}

static void queue_table_avc(struct mib *m, uint8_t attribute_index, uint32_t byte_length)
{
	uint8_t v[4] = { (uint8_t)(byte_length >> 24), (uint8_t)(byte_length >> 16),
			 (uint8_t)(byte_length >> 8), (uint8_t)byte_length };

	queue_avc(m, attribute_index, v, 4);
}

static void set_enhanced_authentication_state(struct mib *m, uint8_t state)
{
	struct mib_entity *e = security_entity(m);
	struct mib_attr *a = e ? mib_entity_attr(e, 9) : NULL;

	if (a) {
		if (a->value && a->value_len)
			a->value[0] = state;
	}
	queue_avc(m, 9, &state, 1);
}

/*
 * Collect 16-byte challenges from a Class 332 table attribute.
 * `indexed` selects rows whose first byte is the row index (attributes 2 and 7),
 * otherwise the challenge starts at byte 0 (attribute 5).
 */
static int collect_challenges(struct mib *m, uint8_t attribute_index, int indexed,
			      uint8_t (*out)[16], size_t *n_out)
{
	struct mib_entity *e = security_entity(m);
	struct mib_attr *a = e ? mib_entity_attr(e, attribute_index) : NULL;
	size_t n = 0;

	if (!a)
		return -EINVAL;
	for (size_t i = 0; i < a->n_rows; i++) {
		const uint8_t *row = a->rows[i].data;
		size_t row_len = a->rows[i].data_len;
		size_t off = indexed ? 1 : 0;

		if (row_len < off + 16)
			return -EINVAL;
		memcpy(out[n], row + off, 16);
		n++;
	}
	if (!n)
		return -EINVAL;
	*n_out = n;
	return 0;
}

static int step1(struct mib *m)
{
	uint8_t olt[16][16], onu[16][16], result[16];
	size_t n_olt = 0, n_onu = 0;
	struct mib_entity *e;
	struct mib_attr *onu_table, *result_table;

	if (collect_challenges(m, 2, 1, olt, &n_olt) < 0)
		return -EINVAL;
	if (n_olt > 16)
		return -E2BIG;

	for (size_t i = 0; i < n_olt; i++) {
		uint8_t random[16];

		if (omci_security_fill_random(random, sizeof(random)) < 0)
			return -EIO;
		omci_security_onu_random_challenge(olt[i], random, onu[i]);
	}
	n_onu = n_olt;
	{
		static const uint8_t zero_identity[8] = { 0 };

		if (omci_security_authentication_result(olt, n_olt, onu, n_onu,
							zero_identity, true, result) < 0)
			return -EIO;
	}

	e = security_entity(m);
	if (!e)
		return -EINVAL;
	onu_table = mib_entity_attr(e, 5);
	result_table = mib_entity_attr(e, 6);
	if (!onu_table || !result_table)
		return -EINVAL;

	/* Attribute 5 rows are keyed by 1-based index. */
	for (size_t i = 0; i < onu_table->n_rows; i++)
		free(onu_table->rows[i].data);
	free(onu_table->rows);
	onu_table->rows = NULL;
	onu_table->n_rows = 0;
	for (size_t i = 0; i < n_onu; i++) {
		uint8_t key[1] = { (uint8_t)(i + 1) };

		mib_attr_row_insert(onu_table, key, 1, onu[i], 16);
	}
	for (size_t i = 0; i < result_table->n_rows; i++)
		free(result_table->rows[i].data);
	free(result_table->rows);
	result_table->rows = NULL;
	result_table->n_rows = 0;
	{
		uint8_t key[1] = { 1 };

		mib_attr_row_insert(result_table, key, 1, result, 16);
	}

	queue_table_avc(m, 5, (uint32_t)(n_onu * 16));
	queue_table_avc(m, 6, 16);
	return 0;
}

static int step2(struct mib *m)
{
	uint8_t olt[16][16], onu[16][16], expected[16];
	size_t n_olt = 0, n_onu = 0;
	struct mib_entity *e, *onu_g;
	struct mib_attr *a7, *a10;
	const uint8_t *received = NULL;
	size_t received_len = 0;

	if (collect_challenges(m, 2, 1, olt, &n_olt) < 0)
		return -EINVAL;
	if (collect_challenges(m, 5, 0, onu, &n_onu) < 0)
		return -EINVAL;
	if (n_olt > 16 || n_onu > 16)
		return -E2BIG;

	onu_g = mib_lookup(m, CLASS_ONU_G, 0);
	{
		struct mib_attr *a3 = onu_g ? mib_entity_attr(onu_g, 3) : NULL;
		uint8_t identity[8] = { 0 };

		if (a3 && a3->value_len >= 8)
			memcpy(identity, a3->value, 8);
		if (omci_security_authentication_result(olt, n_olt, onu, n_onu,
							identity, false, expected) < 0)
			return -EIO;
	}

	e = security_entity(m);
	if (!e)
		return -EINVAL;
	a7 = mib_entity_attr(e, 7);
	a10 = mib_entity_attr(e, 10);
	if (!a7 || !a10)
		return -EINVAL;

	if (a7->n_rows) {
		/* The expected value is stored after the 1-byte row key. */
		if (a7->rows[0].data_len >= 17) {
			received = a7->rows[0].data + 1;
			received_len = 16;
		}
	}

	if (!received || received_len != 16 || memcmp(received, expected, 16)) {
		set_enhanced_authentication_state(m, 4);
		return 0;
	}

	{
		uint8_t name[16], msk[16];

		if (omci_security_master_session_key_name(olt, n_olt, onu, n_onu, name) < 0)
			return -EIO;
		if (omci_security_master_session_key(olt, n_olt, onu, n_onu, msk) < 0)
			return -EIO;
		free(a10->value);
		a10->value = malloc(sizeof(name));
		if (!a10->value)
			return -ENOMEM;
		memcpy(a10->value, name, sizeof(name));
		a10->value_len = sizeof(name);
		/* State 3 is published after the driver installs the OIK. */
		memcpy(m->pending_msk, msk, sizeof(msk));
		m->has_pending_msk = true;
	}
	return 0;
}

int omci_enhanced_security_apply(struct mib *m, const struct sec_update *updates,
				 size_t n_updates)
{
	struct mib_entity *e = security_entity(m);
	int run_step1 = 0, run_step2 = 0;

	if (!e)
		return -EINVAL;

	for (size_t i = 0; i < n_updates; i++) {
		const struct sec_update *u = &updates[i];

		switch (u->index) {
		case 2:
		case 7:
			if (!u->is_table)
				return -EINVAL;
			{
				struct mib_attr *a = mib_entity_attr(e, u->index);
				uint8_t status_index = u->index == 2 ? 3 : 8;
				struct mib_attr *st;

				if (!a)
					return -EINVAL;
				/*
				 * Challenge rows use the first byte as their key, and row
				 * zero clears the table. Rewriting a key replaces its row.
				 */
				if (u->len >= 1) {
					if (u->value[0] == 0) {
						for (size_t r = 0; r < a->n_rows; r++)
							free(a->rows[r].data);
						free(a->rows);
						a->rows = NULL;
						a->n_rows = 0;
					} else {
						mib_attr_row_insert(a, u->value, 1, u->value, u->len);
					}
				}
				st = mib_entity_attr(e, status_index);
				if (st && st->value && st->value_len)
					st->value[0] = 0;
			}
			break;
		case 1:
			if (u->is_table)
				return -EINVAL;
			{
				struct mib_attr *a = mib_entity_attr(e, 1);
				struct mib_attr *st = mib_entity_attr(e, 3);

				if (!a)
					return -EINVAL;
				free(a->value);
				a->value = malloc(u->len ? u->len : 1);
				if (!a->value)
					return -ENOMEM;
				memcpy(a->value, u->value, u->len);
				a->value_len = u->len;
				if (st && st->value && st->value_len)
					st->value[0] = 0;
			}
			break;
		case 11:
			if (!u->is_table)
				return -EINVAL;
			{
				struct mib_attr *a = mib_entity_attr(e, 11);

				if (!a || u->len < 2)
					return -EINVAL;
				/*
				 * Broadcast key rows are keyed by the row identifier; row
				 * control bits 2..1 select set row, clear row, or clear table.
				 */
				switch (u->value[0] & 0x03) {
				case 0:
					mib_attr_row_insert(a, u->value + 1, 1, u->value, u->len);
					break;
				case 1:
					mib_attr_row_remove(a, u->value + 1, 1);
					break;
				case 2:
					for (size_t r = 0; r < a->n_rows; r++)
						free(a->rows[r].data);
					free(a->rows);
					a->rows = NULL;
					a->n_rows = 0;
					break;
				default:
					return -EINVAL;
				}
			}
			break;
		case 3:
		case 8:
			if (u->is_table || u->len < 1 || u->value[0] > 1)
				return -EINVAL;
			{
				struct mib_attr *a = mib_entity_attr(e, u->index);
				int rising_edge;

				if (!a || !a->value || !a->value_len)
					return -EINVAL;
				rising_edge = (a->value[0] == 0 && u->value[0] == 1);
				a->value[0] = u->value[0];
				if (u->index == 3 && rising_edge)
					run_step1 = 1;
				if (u->index == 8 && rising_edge)
					run_step2 = 1;
			}
			break;
		default:
			return -EINVAL;
		}
	}

	if (run_step1 && step1(m) < 0)
		return -EIO;
	if (run_step2 && step2(m) < 0)
		return -EIO;
	return 0;
}

void mib_complete_msk_install(struct mib *m, bool installed)
{
	set_enhanced_authentication_state(m, installed ? 3 : 4);
}
