/* SPDX-License-Identifier: GPL-2.0-only */
#include "oam_control.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static pthread_t server_thread;
static int server_fd = -1;
static volatile int server_stop;

void oam_status_init(struct oam_status_hub *hub, const char *interface,
		     const char *operator_name, bool loid_configured,
		     const struct ctc_session *ctc)
{
	memset(hub, 0, sizeof(*hub));
	pthread_mutex_init(&hub->lock, NULL);
	snprintf(hub->interface, sizeof(hub->interface), "%s", interface);
	snprintf(hub->operator_name, sizeof(hub->operator_name), "%s", operator_name);
	hub->loid_configured = loid_configured;
	hub->ctc = *ctc;
	oam_status_record_event(hub, "lifecycle", "OAM daemon started");
}

void oam_status_free(struct oam_status_hub *hub)
{
	pthread_mutex_destroy(&hub->lock);
}

static void push_event_locked(struct oam_status_hub *hub, const char *kind, const char *message)
{
	struct oam_event *e = &hub->events[hub->ev_next];
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	hub->event_sequence++;
	e->sequence = hub->event_sequence;
	e->timestamp_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
	snprintf(e->kind, sizeof(e->kind), "%s", kind);
	snprintf(e->message, sizeof(e->message), "%s", message);
	hub->ev_next = (hub->ev_next + 1) % OAM_EVENT_CAPACITY;
	if (hub->n_events < OAM_EVENT_CAPACITY)
		hub->n_events++;
}

void oam_status_record_event(struct oam_status_hub *hub, const char *kind, const char *message)
{
	pthread_mutex_lock(&hub->lock);
	push_event_locked(hub, kind, message);
	pthread_mutex_unlock(&hub->lock);
}

void oam_status_record_rx(struct oam_status_hub *hub, uint8_t code)
{
	pthread_mutex_lock(&hub->lock);
	hub->rx_messages++;
	hub->has_last_code = true;
	hub->last_code = code;
	{
		char msg[64];
		char kind[16];

		switch (code) {
		case 0x00: snprintf(kind, sizeof(kind), "information"); break;
		case 0x01: snprintf(kind, sizeof(kind), "event"); hub->event_rx++; break;
		case 0xfe: snprintf(kind, sizeof(kind), "organization"); hub->organization_rx++; break;
		default: snprintf(kind, sizeof(kind), "code"); break;
		}
		snprintf(msg, sizeof(msg), "OAMPDU code=0x%02x", code);
		push_event_locked(hub, kind, msg);
	}
	pthread_mutex_unlock(&hub->lock);
}

void oam_status_record_tx(struct oam_status_hub *hub)
{
	pthread_mutex_lock(&hub->lock);
	hub->tx_messages++;
	pthread_mutex_unlock(&hub->lock);
}

void oam_status_record_parse_error(struct oam_status_hub *hub, const char *message)
{
	pthread_mutex_lock(&hub->lock);
	hub->parse_errors++;
	push_event_locked(hub, "parse-error", message);
	pthread_mutex_unlock(&hub->lock);
}

void oam_status_record_ieee_operational(struct oam_status_hub *hub)
{
	pthread_mutex_lock(&hub->lock);
	hub->ieee_operational = true;
	hub->information_rx++;
	pthread_mutex_unlock(&hub->lock);
}

void oam_status_update_ctc(struct oam_status_hub *hub, const struct ctc_session *ctc)
{
	pthread_mutex_lock(&hub->lock);
	hub->ctc = *ctc;
	pthread_mutex_unlock(&hub->lock);
}

/* ---------- JSON ---------- */

static void json_string_into(char *out, size_t cap, const char *value)
{
	size_t o = 0;

	if (cap < 3)
		return;
	out[o++] = '"';
	for (const char *p = value; *p && o + 8 < cap; p++) {
		unsigned char c = (unsigned char)*p;

		switch (c) {
		case '"': out[o++] = '\\'; out[o++] = '"'; break;
		case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
		case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
		case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
		case '\t': out[o++] = '\\'; out[o++] = 't'; break;
		default:
			if (c < 0x20)
				o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
			else
				out[o++] = (char)c;
		}
	}
	out[o++] = '"';
	out[o] = '\0';
}

static void status_json(char *out, size_t cap, struct oam_status_hub *hub)
{
	char iface[96], op[24], disc[32], auth[24];
	char last_code[16], version[16], failure[16], vlan_mode[16], vlan_obj[16];
	char vlan_list[512];
	uint16_t ids[CTC_VLANS_MAX];
	size_t n_ids = ctc_vlan_ids(&hub->ctc, ids, CTC_VLANS_MAX);
	size_t o = 0;

	json_string_into(iface, sizeof(iface), hub->interface);
	json_string_into(op, sizeof(op), hub->operator_name);
	json_string_into(disc, sizeof(disc), ctc_discovery_state_name(&hub->ctc));
	json_string_into(auth, sizeof(auth), hub->ctc.authentication);
	snprintf(last_code, sizeof(last_code), hub->has_last_code ? "0x%02x" : "null",
		 hub->last_code);
	if (hub->ctc.has_version)
		snprintf(version, sizeof(version), "0x%02x", hub->ctc.version);
	else
		snprintf(version, sizeof(version), "null");
	if (hub->ctc.has_failure)
		snprintf(failure, sizeof(failure), "0x%02x", hub->ctc.failure);
	else
		snprintf(failure, sizeof(failure), "null");
	if (hub->ctc.has_vlan) {
		snprintf(vlan_obj, sizeof(vlan_obj), "%u", hub->ctc.vlan.object_index);
		snprintf(vlan_mode, sizeof(vlan_mode), "%u", hub->ctc.vlan.mode);
	} else {
		snprintf(vlan_obj, sizeof(vlan_obj), "null");
		snprintf(vlan_mode, sizeof(vlan_mode), "null");
	}
	vlan_list[0] = '\0';
	for (size_t i = 0; i < n_ids; i++)
		o += (size_t)snprintf(vlan_list + o, sizeof(vlan_list) - o, i ? ",%u" : "%u", ids[i]);

	o = 0;
	o += (size_t)snprintf(out + o, cap - o,
		"{\"protocol_version\":%d,\"interface\":%s,\"channel_available\":true,"
		"\"operator\":%s,\"ieee_discovery_completed\":%s,\"loid_configured\":%s,"
		"\"rx_messages\":%llu,"
		"\"tx_messages\":%llu,\"parse_errors\":%llu,\"information_rx\":%llu,"
		"\"event_rx\":%llu,\"organization_rx\":%llu,\"last_code\":%s,"
		"\"ctc_discovery_state\":%s,\"ctc_version\":%s,"
		"\"authentication_status\":%s,\"authentication_failure\":%s,"
		"\"ctc_get_requests\":%llu,\"ctc_set_requests\":%llu,"
		"\"ctc_unsupported_requests\":%llu,\"vlan_object\":%s,\"vlan_mode\":%s,"
		"\"vlan_ids\":[%s],\"event_sequence\":%llu}\n",
		OAM_CONTROL_PROTOCOL_VERSION, iface,
		op, hub->ieee_operational ? "true" : "false",
		hub->loid_configured ? "true" : "false",
		(unsigned long long)hub->rx_messages,
		(unsigned long long)hub->tx_messages,
		(unsigned long long)hub->parse_errors,
		(unsigned long long)hub->information_rx,
		(unsigned long long)hub->event_rx,
		(unsigned long long)hub->organization_rx,
		last_code, disc, version, auth, failure,
		(unsigned long long)hub->ctc.get_requests,
		(unsigned long long)hub->ctc.set_requests,
		(unsigned long long)hub->ctc.unsupported_requests,
		vlan_obj, vlan_mode, vlan_list,
		(unsigned long long)hub->event_sequence);
	(void)o;
}

static void serve_connection(struct oam_status_hub *hub, int fd)
{
	char request[256], out[8192];
	ssize_t n = read(fd, request, sizeof(request) - 1);

	if (n <= 0)
		return;
	request[n] = '\0';

	pthread_mutex_lock(&hub->lock);
	if (!strncmp(request, "STATUS", 6)) {
		status_json(out, sizeof(out), hub);
		write(fd, out, strlen(out));
	} else if (!strncmp(request, "EVENTS", 6)) {
		unsigned long long after = 0;
		const char *sp = strchr(request, ' ');

		if (sp) {
			const char *sp2 = strchr(sp + 1, ' ');

			if (sp2)
				after = strtoull(sp2 + 1, NULL, 10);
		}
		for (size_t i = 0; i < hub->n_events; i++) {
			size_t idx = (hub->ev_next + OAM_EVENT_CAPACITY - hub->n_events + i) %
				     OAM_EVENT_CAPACITY;
			const struct oam_event *e = &hub->events[idx];
			char kind[32], msg[400], line[600];

			if (e->sequence <= after)
				continue;
			json_string_into(kind, sizeof(kind), e->kind);
			json_string_into(msg, sizeof(msg), e->message);
			snprintf(line, sizeof(line),
				 "{\"sequence\":%llu,\"timestamp_ms\":%llu,\"kind\":%s,"
				 "\"message\":%s}\n",
				 (unsigned long long)e->sequence,
				 (unsigned long long)e->timestamp_ms, kind, msg);
			write(fd, line, strlen(line));
		}
	} else {
		snprintf(out, sizeof(out),
			 "{\"error\":\"unsupported control request\",\"protocol_version\":%d}\n",
			 OAM_CONTROL_PROTOCOL_VERSION);
		write(fd, out, strlen(out));
	}
	pthread_mutex_unlock(&hub->lock);
}

static void *server_main(void *arg)
{
	struct oam_status_hub *hub = arg;

	while (!server_stop) {
		int fd = accept(server_fd, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		serve_connection(hub, fd);
		close(fd);
	}
	return NULL;
}

int oam_status_server_start(struct oam_status_hub *hub, const char *socket_path)
{
	struct sockaddr_un addr;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);
	unlink(socket_path);
	server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (server_fd < 0)
		return -errno;
	if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		return -errno;
	if (listen(server_fd, 4) < 0)
		return -errno;
	if (pthread_create(&server_thread, NULL, server_main, hub) != 0)
		return -errno;
	return 0;
}

void oam_status_server_stop(void)
{
	server_stop = 1;
	if (server_fd >= 0) {
		shutdown(server_fd, SHUT_RDWR);
		close(server_fd);
		server_fd = -1;
	}
}
