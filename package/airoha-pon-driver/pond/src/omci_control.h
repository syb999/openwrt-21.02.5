/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OMCI status publication over a unix socket (pondctl / luci contract).
 * Port of airoha-pon-daemons omci/control.rs.
 *
 * Requests: "STATUS 1" | "DATAPATH 1" | "EVENTS 1 <after>"  (one JSON line back)
 */
#ifndef POND_OMCI_CONTROL_H
#define POND_OMCI_CONTROL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "omci_backend.h"
#include "omci_protocol.h"
#include "omci_provisioning.h"

#define CONTROL_PROTOCOL_VERSION 1
#define EVENT_CAPACITY 256

struct tx_record {
	bool valid;
	uint16_t tci;
	uint8_t action;
	uint16_t class_id;
	uint16_t entity_id;
	uint16_t attribute_mask;
	bool has_result;
	uint8_t result;
};

struct status_event {
	uint64_t sequence;
	uint64_t timestamp_ms;
	char kind[16];
	char message[192];
};

struct status_hub {
	pthread_mutex_t lock;
	char interface[64];
	bool loid_configured;
	uint64_t rx_messages;
	uint64_t tx_messages;
	uint64_t parse_errors;
	bool has_authentication;
	uint8_t authentication_status;
	char authentication_meaning[32];
	struct provisioning_snapshot provisioning;
	struct data_path_graph graph;
	struct backend_status backend;
	struct tx_record last_transaction;
	uint64_t event_sequence;
	struct status_event events[EVENT_CAPACITY];
	size_t n_events;
	size_t ev_next;		/* ring insertion index */
};

void status_hub_init(struct status_hub *hub, const char *interface, bool loid_configured);
void status_hub_free(struct status_hub *hub);

void status_record_rx(struct status_hub *hub, const struct omci_request *req);
void status_record_tx(struct status_hub *hub, const struct omci_request *req,
		      int has_result, uint8_t result, bool retransmission);
void status_record_parse_error(struct status_hub *hub, const char *message);
void status_record_transport_error(struct status_hub *hub, const char *message);
void status_record_authentication_status(struct status_hub *hub, uint8_t status,
					 const char *meaning);
void status_record_provisioning(struct status_hub *hub,
				const struct provisioning_snapshot *snap,
				const struct data_path_graph *graph,
				const struct backend_status *backend);
int status_hub_error(struct status_hub *hub, int err, const char *message);

/* Start the unix-socket server; returns 0 on success (socket path in hub). */
int status_server_start(struct status_hub *hub, const char *socket_path);
void status_server_stop(void);

#endif
