/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Service configuration model derived from the OMCI MIB.
 * Port of airoha-pon-daemons omci/provisioning.rs + Mib::provisioning_snapshot.
 *
 * This snapshot is the boundary between the MIB and the AN7581 kernel data
 * path: it is shared by the control interface and the kernel backend.
 */
#ifndef POND_OMCI_PROVISIONING_H
#define POND_OMCI_PROVISIONING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omci_mib.h"

/* Match value for untagged upstream frames and tagged frames after VID lookup. */
#define DATA_PATH_VLAN_ANY 0xffff

#define PROV_MAX_PATHS		256
#define PROV_MAX_VLANS		256
#define PROV_MAX_KEY_INDEXES	16
#define PROV_MAX_TAG_CONTROLS	4

struct data_path_candidate {
	uint16_t alloc_id;
	uint16_t gem_id;
	uint16_t vlan_id;
	uint8_t pbit_mask;
	/* A Class 281 GEM carries downstream multicast traffic. */
	bool multicast;
};

struct data_path_table_row {
	uint8_t *key;
	size_t key_len;
	uint8_t *value;
	size_t value_len;
};

struct data_path_attribute {
	uint8_t index;
	uint8_t *value;
	size_t value_len;
	struct data_path_table_row *rows;
	size_t n_rows;
};

struct data_path_entity {
	uint16_t class_id;
	uint16_t entity_id;
	struct data_path_attribute *attrs;
	size_t n_attrs;
};

struct provisioning_snapshot {
	/* OLT-G text fields after fixed-width wire padding is removed. */
	char olt_vendor_id[64];
	char olt_equipment_id[64];
	char olt_version[64];
	size_t configured_tconts;
	size_t gem_ports;
	size_t gem_interworking_tps;
	size_t vlan_rules;
	uint16_t vlan_ids[PROV_MAX_VLANS];		/* unicast service VLANs */
	size_t n_vlan_ids;
	uint16_t multicast_vlan_ids[PROV_MAX_VLANS];	/* Class 281 downstream */
	size_t n_multicast_vlan_ids;
	uint16_t igmp_upstream_vlan_ids[PROV_MAX_VLANS];/* Class 309 via Class 310 */
	size_t n_igmp_upstream_vlan_ids;
	uint8_t igmp_upstream_tag_controls[PROV_MAX_TAG_CONTROLS];
	size_t n_igmp_upstream_tag_controls;
	bool enhanced_security;
	uint8_t broadcast_key_indexes[PROV_MAX_KEY_INDEXES];
	size_t n_broadcast_key_indexes;
	struct data_path_candidate *data_paths;
	size_t n_data_paths;
};

struct data_path_graph {
	struct data_path_candidate *candidates;
	size_t n_candidates;
	struct data_path_entity *entities;
	size_t n_entities;
};

/* Build the provisioning snapshot from the current MIB. */
void omci_provisioning_snapshot(const struct mib *m, struct provisioning_snapshot *out);
void omci_provisioning_snapshot_free(struct provisioning_snapshot *s);

/* Export OMCI entities that determine GEM classification in MIB wire byte order. */
void omci_data_path_graph(const struct mib *m, struct data_path_graph *out);
void omci_data_path_graph_free(struct data_path_graph *g);

/* Deep copy (used by the status hub). */
void omci_data_path_graph_deep_copy(const struct data_path_graph *src,
				    struct data_path_graph *dst);

#endif
