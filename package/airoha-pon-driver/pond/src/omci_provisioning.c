/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci_provisioning.h"
#include "omci_schema.h"

#include <stdlib.h>
#include <string.h>

/* Class IDs used by the snapshot (from omci_schema.h). */
#define CLS_MAC_BRIDGE_PORT_CONFIG_DATA		47
#define CLS_MAC_BRIDGE_PORT_FILTER_PREASSIGN	79
#define CLS_VLAN_TAGGING_FILTER			84
#define CLS_IEEE_8021P_MAPPER			130
#define CLS_OLT_G				131
#define CLS_EXTENDED_VLAN_TAGGING		171
#define CLS_TCONT				262
#define CLS_GEM_PORT_NETWORK_CTP		268
#define CLS_GEM_INTERWORKING_TP			266
#define CLS_MULTICAST_GEM_INTERWORKING_TP	281
#define CLS_MULTICAST_OPERATIONS_PROFILE	309
#define CLS_MULTICAST_SUBSCRIBER_CONFIG		310
#define CLS_ENHANCED_SECURITY_CONTROL		332

/* ---------- helpers ---------- */

static int be_u16(const uint8_t *v, size_t len, uint16_t *out)
{
	if (len < 2)
		return -1;
	*out = (uint16_t)((v[0] << 8) | v[1]);
	return 0;
}

static void omci_text(const uint8_t *v, size_t len, char *out, size_t out_len)
{
	size_t end = len;

	for (size_t i = 0; i < len; i++) {
		if (v[i] == 0 || v[i] == 0xff) {
			end = i;
			break;
		}
	}
	while (end && (v[end - 1] == ' ' || v[end - 1] == '\t'))
		end--;
	if (end >= out_len)
		end = out_len - 1;
	memcpy(out, v, end);
	out[end] = '\0';
}

static void set_insert_u16(uint16_t *set, size_t *n, size_t cap, uint16_t v)
{
	size_t pos = 0;

	while (pos < *n && set[pos] < v)
		pos++;
	if (pos < *n && set[pos] == v)
		return;
	if (*n >= cap)
		return;
	memmove(&set[pos + 1], &set[pos], (*n - pos) * sizeof(uint16_t));
	set[pos] = v;
	(*n)++;
}

static void set_insert_u8(uint8_t *set, size_t *n, size_t cap, uint8_t v)
{
	size_t pos = 0;

	while (pos < *n && set[pos] < v)
		pos++;
	if (pos < *n && set[pos] == v)
		return;
	if (*n >= cap)
		return;
	memmove(&set[pos + 1], &set[pos], (*n - pos));
	set[pos] = v;
	(*n)++;
}

/*
 * Class 171 rows carry four 32-bit words. Fields at offsets 0 and 4 place the
 * VID in bits 27..15; treatment fields at 8 and 12 use bits 15..3.
 * Values 4096 and 4097 encode copy/don't-care and are filtered out.
 */
static void collect_extended_vlan_ids(const uint8_t *row, size_t len,
				      uint16_t *set, size_t *n, size_t cap)
{
	static const int offsets[4] = { 0, 4, 8, 12 };

	if (len != 16)
		return;
	for (int i = 0; i < 4; i++) {
		int off = offsets[i];
		uint32_t word = ((uint32_t)row[off] << 24) | ((uint32_t)row[off + 1] << 16) |
				((uint32_t)row[off + 2] << 8) | row[off + 3];
		uint16_t vid = off < 8 ? (uint16_t)((word >> 15) & 0x1fff) :
					 (uint16_t)((word >> 3) & 0x1fff);

		if (vid >= 1 && vid <= 4094)
			set_insert_u16(set, n, cap, vid);
	}
}

/*
 * G.988 Class 309 static and dynamic ACL rows carry the two-byte VLAN TCI at
 * offset 4; the low 12 bits contain VID (0 and 4095 reserved).
 */
static void collect_multicast_acl_vlan_id(const uint8_t *row, size_t len,
					  uint16_t *set, size_t *n, size_t cap)
{
	uint16_t vlan_id;

	if (len != 24)
		return;
	vlan_id = (uint16_t)(((row[4] << 8) | row[5]) & 0x0fff);
	if (vlan_id >= 1 && vlan_id <= 4094)
		set_insert_u16(set, n, cap, vlan_id);
}

/* ---------- simple ordered maps used by the snapshot ---------- */

struct u16map16 {
	uint16_t key;
	uint16_t value;
};
struct u16mapcand {
	uint16_t key;
	struct data_path_candidate value;
};
struct pairmapcand {
	uint16_t a, b;
	struct data_path_candidate value;
};

static struct u16map16 *map16_put(struct u16map16 *m, size_t *n, uint16_t key, uint16_t value)
{
	struct u16map16 *p = realloc(m, (*n + 1) * sizeof(*p));

	if (!p)
		return m;
	m = p;
	m[*n].key = key;
	m[*n].value = value;
	(*n)++;
	return m;
}

static int map16_get(const struct u16map16 *m, size_t n, uint16_t key, uint16_t *out)
{
	for (size_t i = 0; i < n; i++)
		if (m[i].key == key) {
			*out = m[i].value;
			return 0;
		}
	return -1;
}

static struct u16mapcand *cand_put(struct u16mapcand *m, size_t *n, uint16_t key,
				   const struct data_path_candidate *v)
{
	struct u16mapcand *p = realloc(m, (*n + 1) * sizeof(*p));

	if (!p)
		return m;
	m = p;
	m[*n].key = key;
	m[*n].value = *v;
	(*n)++;
	return m;
}

static struct data_path_candidate *cand_get(struct u16mapcand *m, size_t n, uint16_t key)
{
	for (size_t i = 0; i < n; i++)
		if (m[i].key == key)
			return &m[i].value;
	return NULL;
}

static struct pairmapcand *pair_put(struct pairmapcand *m, size_t *n, uint16_t a, uint16_t b,
				    const struct data_path_candidate *v)
{
	struct pairmapcand *p = realloc(m, (*n + 1) * sizeof(*p));

	if (!p)
		return m;
	m = p;
	m[*n].a = a;
	m[*n].b = b;
	m[*n].value = *v;
	(*n)++;
	return m;
}

/* ---------- candidate list ---------- */

static void paths_push(struct data_path_candidate **v, size_t *n, size_t *cap,
		       const struct data_path_candidate *c)
{
	if (*n >= *cap) {
		size_t ncap = *cap ? *cap * 2 : 16;
		struct data_path_candidate *p = realloc(*v, ncap * sizeof(**v));

		if (!p)
			return;
		*v = p;
		*cap = ncap;
	}
	(*v)[(*n)++] = *c;
}

static int candidate_cmp(const void *x, const void *y)
{
	const struct data_path_candidate *a = x, *b = y;

	if (a->multicast != b->multicast)
		return a->multicast ? 1 : -1;
	if (a->vlan_id != b->vlan_id)
		return a->vlan_id < b->vlan_id ? -1 : 1;
	if (a->alloc_id != b->alloc_id)
		return a->alloc_id < b->alloc_id ? -1 : 1;
	if (a->gem_id != b->gem_id)
		return a->gem_id < b->gem_id ? -1 : 1;
	if (a->pbit_mask != b->pbit_mask)
		return a->pbit_mask < b->pbit_mask ? -1 : 1;
	return 0;
}

static int candidate_eq(const struct data_path_candidate *a, const struct data_path_candidate *b)
{
	return a->multicast == b->multicast && a->vlan_id == b->vlan_id &&
	       a->alloc_id == b->alloc_id && a->gem_id == b->gem_id &&
	       a->pbit_mask == b->pbit_mask;
}

/* ---------- bridge path lists ---------- */

struct bridge_paths {
	uint16_t entity_id;
	struct data_path_candidate *paths;
	size_t n_paths;
};

static void bridge_add(struct bridge_paths **bp, size_t *n, uint16_t entity_id,
		       const struct data_path_candidate *path)
{
	size_t i;

	for (i = 0; i < *n; i++)
		if ((*bp)[i].entity_id == entity_id)
			break;
	if (i == *n) {
		struct bridge_paths *p = realloc(*bp, (*n + 1) * sizeof(**bp));

		if (!p)
			return;
		*bp = p;
		memset(&(*bp)[*n], 0, sizeof(**bp));
		(*bp)[*n].entity_id = entity_id;
		(*n)++;
	}
	{
		size_t c = (*bp)[i].n_paths;
		struct data_path_candidate *p =
			realloc((*bp)[i].paths, (c + 1) * sizeof(*p));

		if (!p)
			return;
		(*bp)[i].paths = p;
		(*bp)[i].paths[c] = *path;
		(*bp)[i].n_paths = c + 1;
	}
}

/* ---------- snapshot ---------- */

void omci_provisioning_snapshot(const struct mib *m, struct provisioning_snapshot *out)
{
	struct u16map16 *tcont_alloc = NULL;
	size_t n_tcont = 0;
	struct u16map16 *gem_id_by_entity = NULL;
	size_t n_gem_ent = 0;
	struct u16mapcand *unicast_by_entity = NULL;
	size_t n_unicast = 0;
	struct u16mapcand *multicast_by_iwtp = NULL;
	size_t n_mcast = 0;
	struct pairmapcand *mapper_paths = NULL;
	size_t n_mapper = 0;
	struct bridge_paths *bridges = NULL;
	size_t n_bridges = 0;
	size_t paths_cap = 0;

	memset(out, 0, sizeof(*out));

	/* OLT-G text fields */
	{
		struct mib_entity *olt = mib_lookup((struct mib *)m, CLS_OLT_G, 0);

		if (olt) {
			const struct mib_attr *a;

			a = mib_entity_attr_const(olt, 1);
			if (a)
				omci_text(a->value, a->value_len, out->olt_vendor_id,
					  sizeof(out->olt_vendor_id));
			a = mib_entity_attr_const(olt, 2);
			if (a)
				omci_text(a->value, a->value_len, out->olt_equipment_id,
					  sizeof(out->olt_equipment_id));
			a = mib_entity_attr_const(olt, 3);
			if (a)
				omci_text(a->value, a->value_len, out->olt_version,
					  sizeof(out->olt_version));
		}
	}

	/* Enhanced security presence and broadcast key indexes */
	{
		struct mib_entity *sec = mib_lookup((struct mib *)m, CLS_ENHANCED_SECURITY_CONTROL, 0);

		if (sec) {
			const struct mib_attr *a11 = mib_entity_attr_const(sec, 11);

			out->enhanced_security = true;
			if (a11)
				for (size_t r = 0; r < a11->n_rows; r++)
					if (a11->rows[r].key_len)
						set_insert_u8(out->broadcast_key_indexes,
							      &out->n_broadcast_key_indexes,
							      PROV_MAX_KEY_INDEXES,
							      (uint8_t)(a11->rows[r].key[0] >> 6));
		}
	}

	/* T-CONT alloc-ids (attribute 1 != 0xffff) */
	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];
		const struct mib_attr *a;
		uint16_t alloc;

		if (e->class_id != CLS_TCONT)
			continue;
		a = mib_entity_attr_const(&e->entity, 1);
		if (!a || be_u16(a->value, a->value_len, &alloc))
			continue;
		if (alloc != 0xffff)
			tcont_alloc = map16_put(tcont_alloc, &n_tcont, e->entity_id, alloc);
	}
	out->configured_tconts = n_tcont;

	/* GEM Port Network CTP: the entity id is the pointer stored by mappers and IWTPs */
	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];
		const struct mib_attr *a1, *a2, *a3;
		uint16_t gem_id, tcont;
		uint8_t direction = 0;
		int have_gem = 0, have_tcont = 0, have_dir = 0;

		if (e->class_id != CLS_GEM_PORT_NETWORK_CTP)
			continue;
		a1 = mib_entity_attr_const(&e->entity, 1);
		a2 = mib_entity_attr_const(&e->entity, 2);
		a3 = mib_entity_attr_const(&e->entity, 3);
		have_gem = a1 && !be_u16(a1->value, a1->value_len, &gem_id);
		have_tcont = a2 && !be_u16(a2->value, a2->value_len, &tcont);
		if (a3 && a3->value_len) {
			direction = a3->value[0];
			have_dir = 1;
		}
		if (have_gem)
			gem_id_by_entity = map16_put(gem_id_by_entity, &n_gem_ent,
						     e->entity_id, gem_id);
		if (have_gem && have_tcont && have_dir && direction == 3) {
			uint16_t alloc;

			if (!map16_get(tcont_alloc, n_tcont, tcont, &alloc)) {
				struct data_path_candidate c = {
					.alloc_id = alloc, .gem_id = gem_id,
					.vlan_id = DATA_PATH_VLAN_ANY, .pbit_mask = 0,
					.multicast = false,
				};

				unicast_by_entity = cand_put(unicast_by_entity, &n_unicast,
							     e->entity_id, &c);
			}
		}
	}

	/* Class 281: attribute 1 points to a GEM CTP; membership derives from the CTP */
	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];
		const struct mib_attr *a1;
		uint16_t ctp_entity, gem_id;

		if (e->class_id != CLS_MULTICAST_GEM_INTERWORKING_TP)
			continue;
		a1 = mib_entity_attr_const(&e->entity, 1);
		if (!a1 || be_u16(a1->value, a1->value_len, &ctp_entity))
			continue;
		if (map16_get(gem_id_by_entity, n_gem_ent, ctp_entity, &gem_id))
			continue;
		{
			struct data_path_candidate c = {
				.alloc_id = 0xffff, .gem_id = gem_id,
				.vlan_id = DATA_PATH_VLAN_ANY, .pbit_mask = 0xff,
				.multicast = true,
			};

			multicast_by_iwtp = cand_put(multicast_by_iwtp, &n_mcast,
						     e->entity_id, &c);
		}
	}

	/* 802.1p mapper attributes 2..9 point to GEM IWTPs for P-bits 0..7 */
	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];

		if (e->class_id != CLS_IEEE_8021P_MAPPER)
			continue;
		for (uint8_t pbit = 0; pbit < 8; pbit++) {
			const struct mib_attr *a = mib_entity_attr_const(&e->entity, (uint8_t)(pbit + 2));
			uint16_t pointer, ctp_entity;
			struct data_path_candidate *path;
			int found = 0;

			if (!a || be_u16(a->value, a->value_len, &pointer))
				continue;
			if (pointer == 0xffff)
				continue;
			ctp_entity = pointer;
			{
				struct mib_entity *iwtp =
					mib_lookup((struct mib *)m, CLS_GEM_INTERWORKING_TP, pointer);

				if (iwtp) {
					const struct mib_attr *p1 = mib_entity_attr_const(iwtp, 1);
					uint16_t v;

					if (p1 && !be_u16(p1->value, p1->value_len, &v))
						ctp_entity = v;
				}
			}
			path = cand_get(unicast_by_entity, n_unicast, ctp_entity);
			if (!path)
				continue;
			for (size_t k = 0; k < n_mapper; k++)
				if (mapper_paths[k].a == e->entity_id &&
				    mapper_paths[k].b == path->gem_id) {
					mapper_paths[k].value.pbit_mask |= (uint8_t)(1 << pbit);
					found = 1;
					break;
				}
			if (!found) {
				mapper_paths = pair_put(mapper_paths, &n_mapper, e->entity_id,
							path->gem_id, path);
				/* A newly inserted mapper entry receives its P-bit too
				 * (the Rust code applies the mask after or_insert_with). */
				if (mapper_paths)
					mapper_paths[n_mapper - 1].value.pbit_mask |= (uint8_t)(1 << pbit);
			}
		}
	}

	/* MAC bridge port: type 3 -> mapper, type 5 -> GEM IWTP, type 6 -> multicast IWTP */
	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];
		const struct mib_attr *a3, *a4;
		uint8_t tp_type = 0;
		uint16_t pointer;
		int have_type = 0;

		if (e->class_id != CLS_MAC_BRIDGE_PORT_CONFIG_DATA)
			continue;
		a3 = mib_entity_attr_const(&e->entity, 3);
		a4 = mib_entity_attr_const(&e->entity, 4);
		if (a3 && a3->value_len) {
			tp_type = a3->value[0];
			have_type = 1;
		}
		if (!a4 || be_u16(a4->value, a4->value_len, &pointer))
			continue;
		if (!have_type)
			continue;

		switch (tp_type) {
		case 3:
			for (size_t k = 0; k < n_mapper; k++)
				if (mapper_paths[k].a == pointer)
					bridge_add(&bridges, &n_bridges, e->entity_id,
						   &mapper_paths[k].value);
			break;
		case 5: {
			struct mib_entity *iwtp =
				mib_lookup((struct mib *)m, CLS_GEM_INTERWORKING_TP, pointer);
			uint16_t ctp = 0;
			struct data_path_candidate *path;

			if (iwtp) {
				const struct mib_attr *p1 = mib_entity_attr_const(iwtp, 1);

				if (p1 && !be_u16(p1->value, p1->value_len, &ctp))
					path = cand_get(unicast_by_entity, n_unicast, ctp);
				else
					path = NULL;
				if (path)
					bridge_add(&bridges, &n_bridges, e->entity_id, path);
			}
			break;
		}
		case 6: {
			struct data_path_candidate *path =
				cand_get(multicast_by_iwtp, n_mcast, pointer);

			if (path)
				bridge_add(&bridges, &n_bridges, e->entity_id, path);
			break;
		}
		default:
			break;
		}
	}

	/* VLAN tagging filter refines each bridge port's paths */
	for (size_t b = 0; b < n_bridges; b++) {
		size_t entity_id = bridges[b].entity_id;
		struct mib_entity *filter = mib_lookup((struct mib *)m, CLS_VLAN_TAGGING_FILTER,
						       (uint16_t)entity_id);
		uint8_t operation = 0, count = 0;
		const struct mib_attr *list;
		size_t before;

		if (!filter) {
			for (size_t k = 0; k < bridges[b].n_paths; k++)
				paths_push(&out->data_paths, &out->n_data_paths, &paths_cap,
					   &bridges[b].paths[k]);
			continue;
		}
		{
			const struct mib_attr *a2 = mib_entity_attr_const(filter, 2);
			const struct mib_attr *a3 = mib_entity_attr_const(filter, 3);

			if (a2 && a2->value_len)
				operation = a2->value[0];
			if (a3 && a3->value_len) {
				count = a3->value[0];
				if (count > 12)
					count = 12;
			}
		}
		list = mib_entity_attr_const(filter, 1);
		if (!list) {
			for (size_t k = 0; k < bridges[b].n_paths; k++)
				paths_push(&out->data_paths, &out->n_data_paths, &paths_cap,
					   &bridges[b].paths[k]);
			continue;
		}
		before = out->n_data_paths;
		for (size_t off = 0; off + 2 <= list->value_len && off / 2 < count; off += 2) {
			uint16_t tci = (uint16_t)((list->value[off] << 8) | list->value[off + 1]);

			switch (operation) {
			case 0x03: case 0x04: case 0x0f: case 0x10: case 0x16: {
				uint16_t vlan_id = (uint16_t)(tci & 0x0fff);

				if (vlan_id < 1 || vlan_id > 4094)
					continue;
				for (size_t k = 0; k < bridges[b].n_paths; k++) {
					struct data_path_candidate c = bridges[b].paths[k];

					c.vlan_id = vlan_id;
					if (c.pbit_mask == 0)
						c.pbit_mask = 0xff;
					paths_push(&out->data_paths, &out->n_data_paths, &paths_cap, &c);
				}
				break;
			}
			case 0x07: case 0x08: case 0x11: case 0x12: {
				uint8_t pbit = (uint8_t)((tci >> 13) & 7);

				for (size_t k = 0; k < bridges[b].n_paths; k++) {
					struct data_path_candidate c = bridges[b].paths[k];

					if (c.pbit_mask == 0 || (c.pbit_mask & (1 << pbit))) {
						c.pbit_mask = (uint8_t)(1 << pbit);
						paths_push(&out->data_paths, &out->n_data_paths,
							   &paths_cap, &c);
					}
				}
				break;
			}
			default:
				break;
			}
		}
		if (out->n_data_paths == before)
			for (size_t k = 0; k < bridges[b].n_paths; k++)
				paths_push(&out->data_paths, &out->n_data_paths, &paths_cap,
					   &bridges[b].paths[k]);
	}

	/* Fallbacks: mapper paths when no unicast survived, then Class 281 GEMs */
	{
		int have_unicast = 0;

		for (size_t i = 0; i < out->n_data_paths; i++)
			if (!out->data_paths[i].multicast)
				have_unicast = 1;
		if (!have_unicast)
			for (size_t k = 0; k < n_mapper; k++)
				paths_push(&out->data_paths, &out->n_data_paths, &paths_cap,
					   &mapper_paths[k].value);
	}
	for (size_t i = 0; i < n_mcast; i++) {
		int present = 0;

		for (size_t k = 0; k < out->n_data_paths; k++)
			if (out->data_paths[k].multicast &&
			    out->data_paths[k].gem_id == multicast_by_iwtp[i].value.gem_id)
				present = 1;
		if (!present)
			paths_push(&out->data_paths, &out->n_data_paths, &paths_cap,
				   &multicast_by_iwtp[i].value);
	}

	/* Counters over the remaining classes */
	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];

		switch (e->class_id) {
		case CLS_GEM_PORT_NETWORK_CTP:
			out->gem_ports++;
			break;
		case CLS_GEM_INTERWORKING_TP:
		case CLS_MULTICAST_GEM_INTERWORKING_TP:
			out->gem_interworking_tps++;
			break;
		case CLS_EXTENDED_VLAN_TAGGING: {
			const struct mib_attr *t = mib_entity_attr_const(&e->entity, 6);

			if (t) {
				out->vlan_rules += t->n_rows;
				for (size_t r = 0; r < t->n_rows; r++)
					collect_extended_vlan_ids(t->rows[r].data,
								  t->rows[r].data_len,
								  out->vlan_ids, &out->n_vlan_ids,
								  PROV_MAX_VLANS);
			}
			break;
		}
		case CLS_VLAN_TAGGING_FILTER: {
			const struct mib_attr *a3 = mib_entity_attr_const(&e->entity, 3);
			uint8_t count = 0;

			if (a3 && a3->value_len) {
				count = a3->value[0];
				if (count > 12)
					count = 12;
			}
			out->vlan_rules += count;
			break;
		}
		default:
			break;
		}
	}

	/* Sort and deduplicate */
	if (out->n_data_paths > 1)
		qsort(out->data_paths, out->n_data_paths, sizeof(out->data_paths[0]),
		      candidate_cmp);
	{
		size_t w = 0;

		for (size_t r = 0; r < out->n_data_paths; r++) {
			if (w && candidate_eq(&out->data_paths[w - 1], &out->data_paths[r]))
				continue;
			out->data_paths[w++] = out->data_paths[r];
		}
		out->n_data_paths = w;
	}

	/* Unicast vs multicast VLAN classification */
	{
		size_t saved_unicast = 0;

		/* collect_extended_vlan_ids above filled vlan_ids; keep both sources. */
		for (size_t i = 0; i < out->n_data_paths; i++) {
			uint16_t vid = out->data_paths[i].vlan_id;

			if (vid < 1 || vid > 4094)
				continue;
			if (out->data_paths[i].multicast) {
				size_t n_before = out->n_multicast_vlan_ids;

				set_insert_u16(out->multicast_vlan_ids,
					       &out->n_multicast_vlan_ids,
					       PROV_MAX_VLANS, vid);
				if (out->n_multicast_vlan_ids != n_before)
					saved_unicast = saved_unicast;	/* no-op, keeps intent clear */
			} else {
				set_insert_u16(out->vlan_ids, &out->n_vlan_ids,
					       PROV_MAX_VLANS, vid);
			}
		}
	}

	/* Class 309/310 multicast subscriber IGMP tagging */
	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];
		const struct mib_attr *a2;
		uint16_t profile_id;
		struct mib_entity *profile;

		if (e->class_id != CLS_MULTICAST_SUBSCRIBER_CONFIG)
			continue;
		a2 = mib_entity_attr_const(&e->entity, 2);
		if (!a2 || be_u16(a2->value, a2->value_len, &profile_id))
			continue;
		profile = mib_lookup((struct mib *)m, CLS_MULTICAST_OPERATIONS_PROFILE, profile_id);
		if (!profile)
			continue;
		for (int idx = 7; idx <= 8; idx++) {
			const struct mib_attr *t = mib_entity_attr_const(profile, (uint8_t)idx);

			if (!t)
				continue;
			for (size_t r = 0; r < t->n_rows; r++)
				collect_multicast_acl_vlan_id(t->rows[r].data, t->rows[r].data_len,
							      out->multicast_vlan_ids,
							      &out->n_multicast_vlan_ids,
							      PROV_MAX_VLANS);
		}
		{
			const struct mib_attr *a5 = mib_entity_attr_const(profile, 5);
			uint8_t tag_control;

			if (!a5 || !a5->value_len)
				continue;
			tag_control = a5->value[0];
			if (tag_control > 3)
				continue;
			set_insert_u8(out->igmp_upstream_tag_controls,
				      &out->n_igmp_upstream_tag_controls,
				      PROV_MAX_TAG_CONTROLS, tag_control);
			if (tag_control == 0)
				continue;
			{
				const struct mib_attr *a4 = mib_entity_attr_const(profile, 4);
				uint16_t tci;

				if (!a4 || be_u16(a4->value, a4->value_len, &tci))
					continue;
				{
					uint16_t vlan_id = (uint16_t)(tci & 0x0fff);

					if (vlan_id >= 1 && vlan_id <= 4094)
						set_insert_u16(out->igmp_upstream_vlan_ids,
							       &out->n_igmp_upstream_vlan_ids,
							       PROV_MAX_VLANS, vlan_id);
				}
			}
		}
	}

	free(tcont_alloc);
	free(gem_id_by_entity);
	free(unicast_by_entity);
	free(multicast_by_iwtp);
	free(mapper_paths);
	for (size_t i = 0; i < n_bridges; i++)
		free(bridges[i].paths);
	free(bridges);
}

void omci_provisioning_snapshot_free(struct provisioning_snapshot *s)
{
	free(s->data_paths);
	s->data_paths = NULL;
	s->n_data_paths = 0;
}

/* ---------- data path graph ---------- */

static const uint16_t graph_classes[] = {
	47, 84, 130, 171, 266, 281, 309, 310, 268, 280,
};

void omci_data_path_graph(const struct mib *m, struct data_path_graph *out)
{
	memset(out, 0, sizeof(*out));

	/* candidates = provisioning_snapshot().data_paths */
	{
		struct provisioning_snapshot snap;

		omci_provisioning_snapshot(m, &snap);
		out->candidates = snap.data_paths;
		out->n_candidates = snap.n_data_paths;
		/* ownership transferred; avoid the snapshot free path */
		snap.data_paths = NULL;
		snap.n_data_paths = 0;
	}

	for (size_t i = 0; i < m->n_entries; i++) {
		const struct mib_entry *e = &m->entries[i];
		int wanted = 0;

		for (size_t k = 0; k < sizeof(graph_classes) / sizeof(graph_classes[0]); k++)
			if (graph_classes[k] == e->class_id)
				wanted = 1;
		if (!wanted)
			continue;

		{
			struct data_path_entity *ent = realloc(out->entities,
				(out->n_entities + 1) * sizeof(*out->entities));

			if (!ent)
				return;
			out->entities = ent;
			ent = &out->entities[out->n_entities];
			memset(ent, 0, sizeof(*ent));
			ent->class_id = e->class_id;
			ent->entity_id = e->entity_id;
			for (size_t j = 0; j < e->entity.n_attrs; j++) {
				const struct mib_attr *a = &e->entity.attrs[j];
				struct data_path_attribute *da;

				ent->attrs = realloc(ent->attrs,
					(ent->n_attrs + 1) * sizeof(*ent->attrs));
				if (!ent->attrs)
					return;
				da = &ent->attrs[ent->n_attrs];
				memset(da, 0, sizeof(*da));
				da->index = a->index;
				if (a->value_len) {
					da->value = malloc(a->value_len);
					if (da->value) {
						memcpy(da->value, a->value, a->value_len);
						da->value_len = a->value_len;
					}
				}
				for (size_t r = 0; r < a->n_rows; r++) {
					struct data_path_table_row *row;

					da->rows = realloc(da->rows,
						(da->n_rows + 1) * sizeof(*da->rows));
					if (!da->rows)
						break;
					row = &da->rows[da->n_rows];
					memset(row, 0, sizeof(*row));
					row->key = malloc(a->rows[r].key_len ? a->rows[r].key_len : 1);
					row->value = malloc(a->rows[r].data_len ? a->rows[r].data_len : 1);
					if (row->key && a->rows[r].key_len) {
						memcpy(row->key, a->rows[r].key, a->rows[r].key_len);
						row->key_len = a->rows[r].key_len;
					}
					if (row->value && a->rows[r].data_len) {
						memcpy(row->value, a->rows[r].data,
						       a->rows[r].data_len);
						row->value_len = a->rows[r].data_len;
					}
					da->n_rows++;
				}
				ent->n_attrs++;
			}
			out->n_entities++;
		}
	}
}

void omci_data_path_graph_free(struct data_path_graph *g)
{
	for (size_t i = 0; i < g->n_entities; i++) {
		for (size_t j = 0; j < g->entities[i].n_attrs; j++) {
			struct data_path_attribute *a = &g->entities[i].attrs[j];

			free(a->value);
			for (size_t r = 0; r < a->n_rows; r++) {
				free(a->rows[r].key);
				free(a->rows[r].value);
			}
			free(a->rows);
		}
		free(g->entities[i].attrs);
	}
	free(g->entities);
	free(g->candidates);
	memset(g, 0, sizeof(*g));
}

void omci_data_path_graph_deep_copy(const struct data_path_graph *src,
				    struct data_path_graph *dst)
{
	memset(dst, 0, sizeof(*dst));
	if (src->n_candidates) {
		dst->candidates = malloc(src->n_candidates * sizeof(*src->candidates));
		if (dst->candidates) {
			memcpy(dst->candidates, src->candidates,
			       src->n_candidates * sizeof(*src->candidates));
			dst->n_candidates = src->n_candidates;
		}
	}
	for (size_t i = 0; i < src->n_entities; i++) {
		const struct data_path_entity *se = &src->entities[i];
		struct data_path_entity *de;

		dst->entities = realloc(dst->entities, (dst->n_entities + 1) * sizeof(*dst->entities));
		if (!dst->entities)
			return;
		de = &dst->entities[dst->n_entities];
		memset(de, 0, sizeof(*de));
		de->class_id = se->class_id;
		de->entity_id = se->entity_id;
		for (size_t j = 0; j < se->n_attrs; j++) {
			const struct data_path_attribute *sa = &se->attrs[j];
			struct data_path_attribute *da;

			de->attrs = realloc(de->attrs, (de->n_attrs + 1) * sizeof(*de->attrs));
			if (!de->attrs)
				return;
			da = &de->attrs[de->n_attrs];
			memset(da, 0, sizeof(*da));
			da->index = sa->index;
			if (sa->value_len) {
				da->value = malloc(sa->value_len);
				if (da->value) {
					memcpy(da->value, sa->value, sa->value_len);
					da->value_len = sa->value_len;
				}
			}
			for (size_t r = 0; r < sa->n_rows; r++) {
				struct data_path_table_row *row;

				da->rows = realloc(da->rows, (da->n_rows + 1) * sizeof(*da->rows));
				if (!da->rows)
					break;
				row = &da->rows[da->n_rows];
				memset(row, 0, sizeof(*row));
				row->key = malloc(sa->rows[r].key_len ? sa->rows[r].key_len : 1);
				row->value = malloc(sa->rows[r].value_len ? sa->rows[r].value_len : 1);
				if (row->key && sa->rows[r].key_len) {
					memcpy(row->key, sa->rows[r].key, sa->rows[r].key_len);
					row->key_len = sa->rows[r].key_len;
				}
				if (row->value && sa->rows[r].value_len) {
					memcpy(row->value, sa->rows[r].value, sa->rows[r].value_len);
					row->value_len = sa->rows[r].value_len;
				}
				da->n_rows++;
			}
			de->n_attrs++;
		}
		dst->n_entities++;
	}
}
