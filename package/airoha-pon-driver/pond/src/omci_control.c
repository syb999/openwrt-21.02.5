/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci_control.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

static pthread_t server_thread;
static int server_fd = -1;
static volatile int server_stop;

/* ---------- hub ---------- */

void status_hub_init(struct status_hub *hub, const char *interface, bool loid_configured)
{
	memset(hub, 0, sizeof(*hub));
	pthread_mutex_init(&hub->lock, NULL);
	snprintf(hub->interface, sizeof(hub->interface), "%s", interface);
	hub->loid_configured = loid_configured;
	snprintf(hub->authentication_meaning, sizeof(hub->authentication_meaning),
		 "not-reported");
	snprintf(hub->backend.state, sizeof(hub->backend.state), "waiting-for-gem");
	status_hub_error(hub, 0, "lifecycle");
}

void status_hub_free(struct status_hub *hub)
{
	omci_provisioning_snapshot_free(&hub->provisioning);
	omci_data_path_graph_free(&hub->graph);
	pthread_mutex_destroy(&hub->lock);
}

/* Append an event to the ring (internal, lock held). */
static void push_event_locked(struct status_hub *hub, const char *kind, const char *message)
{
	struct status_event *e = &hub->events[hub->ev_next];
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	hub->event_sequence++;
	e->sequence = hub->event_sequence;
	e->timestamp_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
	snprintf(e->kind, sizeof(e->kind), "%s", kind);
	snprintf(e->message, sizeof(e->message), "%s", message);
	hub->ev_next = (hub->ev_next + 1) % EVENT_CAPACITY;
	if (hub->n_events < EVENT_CAPACITY)
		hub->n_events++;
}

int status_hub_error(struct status_hub *hub, int err, const char *message)
{
	pthread_mutex_lock(&hub->lock);
	push_event_locked(hub, err ? "error" : "lifecycle", message);
	pthread_mutex_unlock(&hub->lock);
	return err;
}

void status_record_rx(struct status_hub *hub, const struct omci_request *req)
{
	char msg[192];

	pthread_mutex_lock(&hub->lock);
	hub->rx_messages++;
	hub->last_transaction.valid = true;
	hub->last_transaction.tci = req->tci;
	hub->last_transaction.action = req->action;
	hub->last_transaction.class_id = req->class_id;
	hub->last_transaction.entity_id = req->entity_id;
	hub->last_transaction.attribute_mask = req->attribute_mask;
	hub->last_transaction.has_result = false;
	snprintf(msg, sizeof(msg),
		 "tci=0x%04x action=0x%02x class=%u entity=%u %s=0x%04x",
		 req->tci, req->action, req->class_id, req->entity_id,
		 omci_selector_name(req), req->attribute_mask);
	push_event_locked(hub, "rx", msg);
	pthread_mutex_unlock(&hub->lock);
}

void status_record_tx(struct status_hub *hub, const struct omci_request *req,
		      int has_result, uint8_t result, bool retransmission)
{
	char msg[192];

	pthread_mutex_lock(&hub->lock);
	hub->tx_messages++;
	hub->last_transaction.valid = true;
	hub->last_transaction.tci = req->tci;
	hub->last_transaction.action = req->action;
	hub->last_transaction.class_id = req->class_id;
	hub->last_transaction.entity_id = req->entity_id;
	hub->last_transaction.attribute_mask = req->attribute_mask;
	hub->last_transaction.has_result = has_result ? true : false;
	hub->last_transaction.result = result;
	snprintf(msg, sizeof(msg),
		 "tci=0x%04x action=0x%02x class=%u entity=%u result=%s%s",
		 req->tci, req->action, req->class_id, req->entity_id,
		 has_result ? "0x" : "none", "");
	if (has_result)
		snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), "%02x", result);
	if (retransmission)
		snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), " (retransmission)");
	push_event_locked(hub, "tx", msg);
	pthread_mutex_unlock(&hub->lock);
}

void status_record_parse_error(struct status_hub *hub, const char *message)
{
	pthread_mutex_lock(&hub->lock);
	hub->parse_errors++;
	push_event_locked(hub, "parse-error", message);
	pthread_mutex_unlock(&hub->lock);
}

void status_record_transport_error(struct status_hub *hub, const char *message)
{
	pthread_mutex_lock(&hub->lock);
	push_event_locked(hub, "transport-error", message);
	pthread_mutex_unlock(&hub->lock);
}

void status_record_authentication_status(struct status_hub *hub, uint8_t status,
					 const char *meaning)
{
	pthread_mutex_lock(&hub->lock);
	hub->has_authentication = true;
	hub->authentication_status = status;
	snprintf(hub->authentication_meaning, sizeof(hub->authentication_meaning),
		 "%s", meaning);
	pthread_mutex_unlock(&hub->lock);
}

void status_record_provisioning(struct status_hub *hub,
				const struct provisioning_snapshot *snap,
				const struct data_path_graph *graph,
				const struct backend_status *backend)
{
	pthread_mutex_lock(&hub->lock);
	omci_provisioning_snapshot_free(&hub->provisioning);
	omci_data_path_graph_free(&hub->graph);
	memset(&hub->provisioning, 0, sizeof(hub->provisioning));
	memset(&hub->graph, 0, sizeof(hub->graph));

	/* shallow copy of the scalar fields, deep copy of the path vector */
	hub->provisioning = *snap;
	if (snap->n_data_paths) {
		hub->provisioning.data_paths = malloc(snap->n_data_paths * sizeof(*snap->data_paths));
		if (hub->provisioning.data_paths)
			memcpy(hub->provisioning.data_paths, snap->data_paths,
			       snap->n_data_paths * sizeof(*snap->data_paths));
		else
			hub->provisioning.n_data_paths = 0;
	}
	/* the graph owns allocations; deep copy candidates + entities */
	{
		struct data_path_graph copy;

		omci_data_path_graph_deep_copy(graph, &copy);
		hub->graph = copy;
	}
	hub->backend = *backend;
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

static void hex_into(char *out, size_t cap, const uint8_t *bytes, size_t len)
{
	size_t o = 0;

	for (size_t i = 0; i < len && o + 3 < cap; i++)
		o += (size_t)snprintf(out + o, cap - o, "%02x", bytes[i]);
	out[o] = '\0';
}

static void u16_list(char *out, size_t cap, const uint16_t *v, size_t n)
{
	size_t o = 0;

	for (size_t i = 0; i < n && o + 8 < cap; i++)
		o += (size_t)snprintf(out + o, cap - o, i ? ",%u" : "%u", v[i]);
	out[o] = '\0';
}

static void u8_list(char *out, size_t cap, const uint8_t *v, size_t n)
{
	size_t o = 0;

	for (size_t i = 0; i < n && o + 6 < cap; i++)
		o += (size_t)snprintf(out + o, cap - o, i ? ",%u" : "%u", v[i]);
	out[o] = '\0';
}

static const char *data_path_class_name(uint16_t class_id)
{
	switch (class_id) {
	case 47: return "MAC Bridge Port Configuration Data";
	case 84: return "VLAN Tagging Filter Data";
	case 130: return "IEEE 802.1p Mapper Service Profile";
	case 171: return "Extended VLAN Tagging Operation Configuration Data";
	case 266: return "GEM Interworking Termination Point";
	case 268: return "GEM Port Network CTP";
	case 280: return "Traffic Descriptor";
	case 281: return "Multicast GEM Interworking Termination Point";
	case 309: return "Multicast Operations Profile";
	case 310: return "Multicast Subscriber Configuration Info";
	default: return "Unknown";
	}
}

static void transaction_json(char *out, size_t cap, const struct tx_record *t)
{
	char result[16];

	if (!t->valid) {
		snprintf(out, cap, "null");
		return;
	}
	if (t->has_result)
		snprintf(result, sizeof(result), "%u", t->result);
	else
		snprintf(result, sizeof(result), "null");
	snprintf(out, cap,
		 "{\"tci\":%u,\"action\":%u,\"class_id\":%u,\"entity_id\":%u,"
		 "\"attribute_mask\":%u,\"result\":%s}",
		 t->tci, t->action, t->class_id, t->entity_id, t->attribute_mask, result);
}

/* Serializes the whole status. Caller must hold the lock. */
static void status_json(char *out, size_t cap, struct status_hub *hub)
{
	char iface[96], meaning[48], vendor[80], equip[80], version[80];
	char vid_list[1024], mvid_list[1024], igmp_list[1024], tag_list[64], key_list[64];
	char backend_state[48], backend_error[200], last[200];
	bool data_path_active;
	bool all_vlans;
	const struct backend_status *be = &hub->backend;

	json_string_into(iface, sizeof(iface), hub->interface);
	json_string_into(meaning, sizeof(meaning), hub->authentication_meaning);
	json_string_into(vendor, sizeof(vendor), hub->provisioning.olt_vendor_id);
	json_string_into(equip, sizeof(equip), hub->provisioning.olt_equipment_id);
	json_string_into(version, sizeof(version), hub->provisioning.olt_version);
	u16_list(vid_list, sizeof(vid_list), hub->provisioning.vlan_ids,
		 hub->provisioning.n_vlan_ids);
	u16_list(mvid_list, sizeof(mvid_list), hub->provisioning.multicast_vlan_ids,
		 hub->provisioning.n_multicast_vlan_ids);
	u16_list(igmp_list, sizeof(igmp_list), hub->provisioning.igmp_upstream_vlan_ids,
		 hub->provisioning.n_igmp_upstream_vlan_ids);
	u8_list(tag_list, sizeof(tag_list), hub->provisioning.igmp_upstream_tag_controls,
		hub->provisioning.n_igmp_upstream_tag_controls);
	u8_list(key_list, sizeof(key_list), hub->provisioning.broadcast_key_indexes,
		hub->provisioning.n_broadcast_key_indexes);

	/* Carried-over semantics: the data path is inactive only on a known link-down. */
	data_path_active = true;
	all_vlans = data_path_active && be->all_vlans;
	json_string_into(backend_state, sizeof(backend_state),
			 data_path_active ? be->state : "inactive");
	if (be->has_error && data_path_active)
		json_string_into(backend_error, sizeof(backend_error), be->error);
	else
		snprintf(backend_error, sizeof(backend_error), "null");
	transaction_json(last, sizeof(last), &hub->last_transaction);

	snprintf(out, cap,
		 "{\"protocol_version\":%d,\"interface\":%s,"
		 "\"channel_available\":true,\"loid_configured\":%s,"
		 "\"authentication_status\":%s,\"authentication_meaning\":%s,"
		 "\"olt_vendor_id\":%s,\"olt_equipment_id\":%s,\"olt_version\":%s,"
		 "\"rx_messages\":%llu,\"tx_messages\":%llu,\"parse_errors\":%llu,"
		 "\"tcont_count\":%zu,\"gem_port_count\":%zu,"
		 "\"gem_iwtp_count\":%zu,\"vlan_rule_count\":%zu,"
		 "\"vlan_ids\":[%s],\"multicast_vlan_ids\":[%s],"
		 "\"igmp_upstream_vlan_ids\":[%s],"
		 "\"igmp_upstream_tag_controls\":[%s],"
		 "\"enhanced_security\":%s,\"broadcast_key_indexes\":[%s],"
		 "\"data_path_candidate_count\":%zu,"
		 "\"data_path_all_vlans\":%s,"
		 "\"backend_state\":%s,\"active_alloc_id\":%s,"
		 "\"active_gem_id\":%s,\"backend_error\":%s,"
		 "\"event_sequence\":%llu,\"last_transaction\":%s}",
		 CONTROL_PROTOCOL_VERSION, iface,
		 hub->loid_configured ? "true" : "false",
		 hub->has_authentication ? "0" : "null",
		 meaning, vendor, equip, version,
		 (unsigned long long)hub->rx_messages, (unsigned long long)hub->tx_messages,
		 (unsigned long long)hub->parse_errors,
		 hub->provisioning.configured_tconts, hub->provisioning.gem_ports,
		 hub->provisioning.gem_interworking_tps, hub->provisioning.vlan_rules,
		 vid_list, mvid_list, igmp_list, tag_list,
		 hub->provisioning.enhanced_security ? "true" : "false", key_list,
		 hub->provisioning.n_data_paths,
		 all_vlans ? "true" : "false",
		 backend_state,
		 (data_path_active && be->has_alloc_id) ? "0" : "null",
		 (data_path_active && be->has_gem_id) ? "0" : "null",
		 backend_error,
		 (unsigned long long)hub->event_sequence, last);
	/* active ids need their numeric form; patch the two placeholders cleanly */
	{
		char buf[32];
		char *p;

		p = strstr(out, "\"active_alloc_id\":0");
		if (p && !(data_path_active && be->has_alloc_id))
			memmove(p + 18, p + 19, strlen(p + 19) + 1);
		if (data_path_active && be->has_alloc_id) {
			snprintf(buf, sizeof(buf), "%u", be->active_alloc_id);
			p = strstr(out, "\"active_alloc_id\":0");
			if (p) {
				char tail[256];
				size_t tail_len;

				snprintf(tail, sizeof(tail), "%s", p + 18);
				tail_len = strlen(tail);
				memmove(p + 18, buf, strlen(buf));
				memmove(p + 18 + strlen(buf), tail, tail_len + 1);
			}
		}
		p = strstr(out, "\"active_gem_id\":0");
		if (p && !(data_path_active && be->has_gem_id))
			memmove(p + 15, p + 16, strlen(p + 16) + 1);
		if (data_path_active && be->has_gem_id) {
			snprintf(buf, sizeof(buf), "%u", be->active_gem_id);
			p = strstr(out, "\"active_gem_id\":0");
			if (p) {
				char tail[256];
				size_t tail_len;

				snprintf(tail, sizeof(tail), "%s", p + 15);
				tail_len = strlen(tail);
				memmove(p + 15, buf, strlen(buf));
				memmove(p + 15 + strlen(buf), tail, tail_len + 1);
			}
		}
	}
}

static void data_path_json(char *out, size_t cap, struct status_hub *hub)
{
	size_t off = 0;
	char state[48], backend_error[200];

	json_string_into(state, sizeof(state), hub->backend.state);
	if (hub->backend.has_error)
		json_string_into(backend_error, sizeof(backend_error), hub->backend.error);
	else
		snprintf(backend_error, sizeof(backend_error), "null");

	off += (size_t)snprintf(out + off, off < cap ? cap - off : 0,
				"{\"protocol_version\":%d,\"backend_state\":%s,"
				"\"backend_error\":%s,\"data_path_all_vlans\":%s,\"candidates\":[",
				CONTROL_PROTOCOL_VERSION, state, backend_error,
				hub->backend.all_vlans ? "true" : "false");
	for (size_t i = 0; i < hub->graph.n_candidates && off + 96 < cap; i++) {
		const struct data_path_candidate *c = &hub->graph.candidates[i];

		off += (size_t)snprintf(out + off, cap - off,
					"%s{\"alloc_id\":%u,\"gem_id\":%u,\"vlan_id\":%u,"
					"\"pbit_mask\":%u,\"multicast\":%s}",
					i ? "," : "", c->alloc_id, c->gem_id, c->vlan_id,
					c->pbit_mask, c->multicast ? "true" : "false");
	}
	off += (size_t)snprintf(out + off, cap - off, "],\"entities\":[");
	for (size_t i = 0; i < hub->graph.n_entities && off + 128 < cap; i++) {
		const struct data_path_entity *e = &hub->graph.entities[i];
		char cls[80];

		json_string_into(cls, sizeof(cls), data_path_class_name(e->class_id));
		off += (size_t)snprintf(out + off, cap - off,
					"%s{\"class_id\":%u,\"class_name\":%s,\"entity_id\":%u,"
					"\"attributes\":[",
					i ? "," : "", e->class_id, cls, e->entity_id);
		for (size_t j = 0; j < e->n_attrs && off + 200 < cap; j++) {
			const struct data_path_attribute *a = &e->attrs[j];
			char hex[600], rows[4096];
			size_t ro = 0;

			hex_into(hex, sizeof(hex), a->value, a->value_len);
			rows[0] = '\0';
			for (size_t r = 0; r < a->n_rows && ro + 200 < sizeof(rows); r++) {
				char k[260], v[260];

				hex_into(k, sizeof(k), a->rows[r].key, a->rows[r].key_len);
				hex_into(v, sizeof(v), a->rows[r].value, a->rows[r].value_len);
				ro += (size_t)snprintf(rows + ro, sizeof(rows) - ro,
						       "%s{\"key\":\"%s\",\"value\":\"%s\"}",
						       r ? "," : "", k, v);
			}
			off += (size_t)snprintf(out + off, cap - off,
						"%s{\"index\":%u,\"value\":\"%s\","
						"\"table_rows\":[%s]}",
						j ? "," : "", a->index, hex, rows);
		}
		off += (size_t)snprintf(out + off, cap - off, "]}");
	}
	snprintf(out + off, cap - off, "]}\n");
}

/* ---------- server ---------- */

static void serve_connection(struct status_hub *hub, int fd)
{
	char request[256];
	ssize_t n = read(fd, request, sizeof(request) - 1);
	char *out;
	size_t cap = 64 * 1024;

	if (n <= 0)
		return;
	request[n] = '\0';
	out = malloc(cap);
	if (!out)
		return;

	pthread_mutex_lock(&hub->lock);
	if (!strncmp(request, "STATUS", 6)) {
		status_json(out, cap, hub);
		write(fd, out, strlen(out));
	} else if (!strncmp(request, "DATAPATH", 8)) {
		data_path_json(out, cap, hub);
		write(fd, out, strlen(out));
	} else if (!strncmp(request, "EVENTS", 6)) {
		/* EVENTS 1 <after> */
		unsigned long long after = 0;
		const char *sp = strchr(request, ' ');

		if (sp) {
			const char *sp2 = strchr(sp + 1, ' ');

			if (sp2)
				after = strtoull(sp2 + 1, NULL, 10);
		}
		for (size_t i = 0; i < hub->n_events; i++) {
			/* oldest first: walk the ring from (ev_next - n_events) */
			size_t idx = (hub->ev_next + EVENT_CAPACITY - hub->n_events + i) % EVENT_CAPACITY;
			const struct status_event *e = &hub->events[idx];
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
	}
	pthread_mutex_unlock(&hub->lock);
	free(out);
}

static void *server_main(void *arg)
{
	struct status_hub *hub = arg;

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

int status_server_start(struct status_hub *hub, const char *socket_path)
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

void status_server_stop(void)
{
	server_stop = 1;
	if (server_fd >= 0) {
		shutdown(server_fd, SHUT_RDWR);
		close(server_fd);
		server_fd = -1;
	}
}
