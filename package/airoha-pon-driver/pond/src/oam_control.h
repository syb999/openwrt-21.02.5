/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EPON OAM status publication over a unix socket (pondctl contract).
 * Port of airoha-pon-daemons oam/control.rs.
 */
#ifndef POND_OAM_CONTROL_H
#define POND_OAM_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "oam_ctc.h"

#define OAM_CONTROL_PROTOCOL_VERSION 1
#define OAM_EVENT_CAPACITY 256

struct oam_event {
	uint64_t sequence;
	uint64_t timestamp_ms;
	char kind[16];
	char message[192];
};

struct oam_status_hub {
	pthread_mutex_t lock;
	char interface[64];
	char operator_name[8];
	bool loid_configured;
	bool ieee_operational;
	uint64_t rx_messages;
	uint64_t tx_messages;
	uint64_t parse_errors;
	uint64_t information_rx;
	uint64_t event_rx;
	uint64_t organization_rx;
	bool has_last_code;
	uint8_t last_code;
	struct ctc_session ctc;
	uint64_t event_sequence;
	struct oam_event events[OAM_EVENT_CAPACITY];
	size_t n_events;
	size_t ev_next;
};

void oam_status_init(struct oam_status_hub *hub, const char *interface,
		     const char *operator_name, bool loid_configured,
		     const struct ctc_session *ctc);
void oam_status_free(struct oam_status_hub *hub);
void oam_status_record_rx(struct oam_status_hub *hub, uint8_t code);
void oam_status_record_tx(struct oam_status_hub *hub);
void oam_status_record_parse_error(struct oam_status_hub *hub, const char *message);
void oam_status_record_ieee_operational(struct oam_status_hub *hub);
void oam_status_record_event(struct oam_status_hub *hub, const char *kind, const char *message);
void oam_status_update_ctc(struct oam_status_hub *hub, const struct ctc_session *ctc);

int oam_status_server_start(struct oam_status_hub *hub, const char *socket_path);
void oam_status_server_stop(void);

#endif
