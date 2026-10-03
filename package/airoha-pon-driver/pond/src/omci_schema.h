/* SPDX-License-Identifier: GPL-2.0-only */
/* Generated from airoha-pon-daemons omci/schema.rs — do not edit by hand. */
#ifndef POND_OMCI_SCHEMA_H
#define POND_OMCI_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum me_origin { ME_ORIGIN_ONU, ME_ORIGIN_OLT };
enum attr_access { ATTR_RO, ATTR_RW };
enum attr_kind { ATTR_SCALAR, ATTR_TABLE };
enum create_source { CREATE_NONE, CREATE_REQUIRED, CREATE_OPTIONAL };

struct me_attr {
	uint8_t index;
	enum attr_access access;
	enum attr_kind kind;
	uint16_t length;		/* scalar length or table row length */
	enum create_source create;
	uint16_t create_offset;
	const uint8_t *def;
	uint8_t def_len;
	bool upload;
};

struct me_def {
	uint16_t class_id;
	const char *name;
	enum me_origin origin;
	uint32_t actions;
	const struct me_attr *attrs;
	size_t n_attrs;
};

#define CLASS_ONU_DATA 2
#define CLASS_CARDHOLDER 5
#define CLASS_CIRCUIT_PACK 6
#define CLASS_SOFTWARE_IMAGE 7
#define CLASS_PPTP_ETHERNET_UNI 11
#define CLASS_ETHERNET_PM_HISTORY_DATA 24
#define CLASS_MAC_BRIDGE_SERVICE_PROFILE 45
#define CLASS_MAC_BRIDGE_PORT_CONFIG_DATA 47
#define CLASS_MAC_BRIDGE_PORT_PM_HISTORY_DATA 52
#define CLASS_MAC_BRIDGE_PORT_FILTER_PREASSIGN_DATA 79
#define CLASS_VLAN_TAGGING_FILTER 84
#define CLASS_ETHERNET_PM_HISTORY_DATA_2 89
#define CLASS_IEEE_8021P_MAPPER 130
#define CLASS_OLT_G 131
#define CLASS_ONU_POWER_SHEDDING 133
#define CLASS_EXTENDED_VLAN_TAGGING 171
#define CLASS_VENDOR_247 247
#define CLASS_ONU_G 256
#define CLASS_ONU2_G 257
#define CLASS_TCONT 262
#define CLASS_ANI_G 263
#define CLASS_UNI_G 264
#define CLASS_GEM_INTERWORKING_TP 266
#define CLASS_GEM_PORT_PM_HISTORY_DATA 267
#define CLASS_GEM_PORT_NETWORK_CTP 268
#define CLASS_GAL_ETHERNET_PROFILE 272
#define CLASS_THRESHOLD_DATA_1 273
#define CLASS_THRESHOLD_DATA_2 274
#define CLASS_PRIORITY_QUEUE 277
#define CLASS_TRAFFIC_SCHEDULER 278
#define CLASS_TRAFFIC_DESCRIPTOR 280
#define CLASS_MULTICAST_GEM_INTERWORKING_TP 281
#define CLASS_OMCI 287
#define CLASS_ETHERNET_PM_HISTORY_DATA_3 296
#define CLASS_MULTICAST_OPERATIONS_PROFILE 309
#define CLASS_MULTICAST_SUBSCRIBER_CONFIG 310
#define CLASS_MULTICAST_SUBSCRIBER_MONITOR 311
#define CLASS_FEC_PM_HISTORY_DATA 312
#define CLASS_VEIP 329
#define CLASS_ENHANCED_SECURITY_CONTROL 332
#define CLASS_ETHERNET_FRAME_EXTENDED_PM 334
#define CLASS_VENDOR_351 351
#define CLASS_CTC_LOID_AUTH 65530

extern const struct me_def pond_me_table[];
extern const size_t pond_me_table_len;

/* Lookup by class id (NULL when absent). */
const struct me_def *pond_me_lookup(uint16_t class_id);

/* action_bit(action) == 1u << action */
static inline uint32_t pond_action_bit(uint8_t action) { return 1u << action; }

static inline bool pond_me_supports_action(const struct me_def *me, uint8_t action)
{
	return (me->actions & pond_action_bit(action)) != 0;
}

#endif
