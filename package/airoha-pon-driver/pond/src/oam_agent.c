/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EPON OAM agent main loop.
 * Port of airoha-pon-daemons oam/mod.rs run_agent.
 */
#include "oam_agent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "oam_control.h"
#include "oam_ctc.h"
#include "oam_frame.h"
#include "oam_ieee.h"
#include "runtime.h"
#include "transport.h"

#define RECEIVE_BUFFER_LEN 2048
#define SEND_BUFFER_LEN 2048

/* Read the parent interface hardware address (the OAM source MAC). */
static int parent_interface_mac(const char *interface, uint8_t mac[6])
{
	char parent[64], path[300], text[64];
	int fd, n;

	if (pond_parent_interface(interface, parent, sizeof(parent)) < 0)
		snprintf(parent, sizeof(parent), "%s", interface);
	if (snprintf(path, sizeof(path), "/sys/class/net/%s/address", parent) >=
	    (int)sizeof(path))
		return -1;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	n = (int)read(fd, text, sizeof(text) - 1);
	close(fd);
	if (n <= 0)
		return -1;
	text[n] = '\0';
	if (sscanf(text, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
		   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6)
		return -1;
	return 0;
}

static void publish_service_ready(const char *path, bool has_path, bool ready)
{
	int fd;

	if (!has_path)
		return;
	fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, ready ? "1\n" : "0\n", 2) < 0)
		printf("PON service LED state update failed\n");
	close(fd);
}

int oam_run_agent(const char *interface, const struct oam_config *cfg, const char *socket_path)
{
	uint8_t source_mac[6];
	struct pond_sock sock;
	struct ieee_session ieee;
	struct ctc_session ctc;
	struct oam_status_hub hub;
	char service_ready[300];
	bool has_service_ready = false;
	bool published_service_ready = false;
	uint8_t buffer[RECEIVE_BUFFER_LEN];

	if (parent_interface_mac(interface, source_mac) < 0) {
		fprintf(stderr, "cannot read the parent interface MAC of %s\n", interface);
		return 1;
	}
	if (pond_sock_open(&sock, interface) < 0) {
		fprintf(stderr, "cannot open AF_PACKET socket on %s\n", interface);
		return 1;
	}
	if (pond_parent_xpon_attr(interface, "service_ready", service_ready,
				  sizeof(service_ready)) == 0)
		has_service_ready = true;
	publish_service_ready(service_ready, has_service_ready, false);

	ieee_session_init(&ieee);
	ctc_session_init(&ctc);
	oam_status_init(&hub, interface, cfg->operator, cfg->loid_len > 0, &ctc);

	if (oam_status_server_start(&hub, socket_path) < 0) {
		fprintf(stderr, "cannot start control socket %s\n", socket_path);
		pond_sock_close(&sock);
		return 1;
	}

	printf("OAM agent started: interface=%s operator=%s control_socket=%s\n",
	       interface, cfg->operator, socket_path);
	fflush(stdout);

	for (;;) {
		uint8_t pkt_type = 0;
		int n = pond_sock_recv(&sock, buffer, sizeof(buffer), &pkt_type);
		struct oam_pdu pdu;
		uint8_t response[SEND_BUFFER_LEN];
		int response_len = 0;

		if (n < 0) {
			if (-n == EINTR)
				continue;
			if (-n == ENETDOWN && pond_sock_present(&sock)) {
				oam_status_record_event(&hub, "line", "PON control netdev is down");
				continue;
			}
			break;
		}
		if (pkt_type == POND_PACKET_OUTGOING)
			continue;

		if (oam_pdu_parse(buffer, (size_t)n, &pdu) < 0) {
			oam_status_record_parse_error(&hub, "malformed OAMPDU");
			continue;
		}
		oam_status_record_rx(&hub, pdu.code);

		switch (pdu.code) {
		case OAM_CODE_INFORMATION: {
			uint8_t payload[1024];
			size_t payload_len = 0;

			if (ieee_handle_information(&ieee, pdu.payload, pdu.payload_len,
						    source_mac, cfg, &ctc, payload,
						    sizeof(payload), &payload_len) == 0) {
				oam_status_record_ieee_operational(&hub);
				response_len = oam_pdu_build(source_mac, OAM_FLAGS_STABLE,
							     OAM_CODE_INFORMATION,
							     payload, payload_len, response,
							     sizeof(response));
			} else {
				oam_status_record_parse_error(&hub, "invalid Information OAMPDU");
			}
			break;
		}
		case OAM_CODE_EVENT_NOTIFICATION:
			ieee.event_rx++;
			oam_status_record_event(&hub, "link-event", "OAM event notification received");
			break;
		case OAM_CODE_ORGANIZATION_SPECIFIC:
			if (oam_config_ctc_enabled(cfg)) {
				struct ctc_outcome outcome;
				int rc = ctc_handle_organization_pdu(&ctc, pdu.payload,
								     pdu.payload_len, cfg,
								     source_mac, &outcome);

				if (rc < 0) {
					oam_status_record_parse_error(&hub, "invalid CTC organization PDU");
				} else {
					for (size_t i = 0; i < outcome.n_events; i++)
						oam_status_record_event(&hub, "ctc", outcome.events[i]);
					if (outcome.has_response)
						response_len = oam_pdu_build(source_mac,
							OAM_FLAGS_STABLE,
							OAM_CODE_ORGANIZATION_SPECIFIC,
							outcome.response, outcome.response_len,
							response, sizeof(response));
				}
			}
			break;
		case OAM_CODE_VARIABLE_REQUEST:
			oam_status_record_event(&hub, "unsupported",
				"IEEE Variable Request received while capability is disabled");
			break;
		case OAM_CODE_LOOPBACK_CONTROL:
			oam_status_record_event(&hub, "unsupported",
				"IEEE remote loopback request received while capability is disabled");
			break;
		default: {
			char msg[64];

			snprintf(msg, sizeof(msg), "OAM code 0x%02x ignored", pdu.code);
			oam_status_record_event(&hub, "unsupported", msg);
			break;
		}
		}

		oam_status_update_ctc(&hub, &ctc);
		{
			bool ready = !strcmp(ctc.authentication, "accepted");

			if (ready != published_service_ready) {
				publish_service_ready(service_ready, has_service_ready, ready);
				published_service_ready = ready;
			}
		}

		if (response_len > 0) {
			if (pond_sock_send(&sock, response, (size_t)response_len) < 0) {
				if (errno == ENETDOWN) {
					oam_status_record_event(&hub, "line",
								"PON control netdev is down");
					continue;
				}
				break;
			}
			oam_status_record_tx(&hub);
		}
	}

	oam_status_server_stop();
	pond_sock_close(&sock);
	oam_status_free(&hub);
	return 0;
}
