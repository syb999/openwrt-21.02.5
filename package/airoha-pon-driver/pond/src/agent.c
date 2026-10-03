/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * airoha-pond: userspace OMCI/OAM agent for Airoha PON devices.
 * Port of airoha-pon-daemons agent.rs + omci/mod.rs run_agent.
 *
 * Usage:  airoha-pond --line NAME
 *         pondctl status|datapath|events --line NAME   (same binary, symlink)
 */
#include "omci_backend.h"
#include "omci_config.h"
#include "omci_control.h"
#include "omci_mib.h"
#include "omci_protocol.h"
#include "oam_agent.h"
#include "oam_config.h"
#include "omci_provisioning.h"
#include "omci_security.h"
#include "transport.h"

#include <errno.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define VERSION "0.3.0"
#define PROJECT_URL "https://github.com/pbs05/openwrt-pon-userspace"
#define RECEIVE_BUFFER_LEN 2048

/* ---------- helpers ---------- */

static const char *protocol_for_mode(const char *mode)
{
	return !strncmp(mode, "epon-", 5) ? "oam" : "omci";
}

static void control_socket_path(char *out, size_t cap, const char *line)
{
	snprintf(out, cap, "/var/run/airoha-pond.%s.sock", line);
}

static const char *ctc_authentication_status_name(uint8_t status)
{
	/* Q/CT 2360-2011 result codes for the CTC LOID Authentication ME. */
	switch (status) {
	case 0: return "not-authenticated";
	case 1: return "accepted";
	case 2: return "loid-not-found";
	case 3: return "password-mismatch";
	case 4: return "loid-conflict";
	default: return "reserved-status";
	}
}

/* ---------- response cache (retransmission) ---------- */

#define RESPONSE_CACHE_LIMIT 32
#define RESPONSE_CACHE_LEN 256

struct cached_response {
	uint8_t request[RESPONSE_CACHE_LEN];
	size_t request_len;
	struct omci_response response;
};

struct response_cache {
	struct cached_response entries[RESPONSE_CACHE_LIMIT];
	size_t n;		/* count, oldest first */
	size_t next;
};

static void cache_clear(struct response_cache *c)
{
	c->n = 0;
	c->next = 0;
}

static struct omci_response *cache_lookup(struct response_cache *c, const uint8_t *frame,
					  size_t len)
{
	for (size_t i = 0; i < c->n; i++) {
		size_t idx = (c->next + RESPONSE_CACHE_LIMIT - c->n + i) % RESPONSE_CACHE_LIMIT;

		if (c->entries[idx].request_len == len &&
		    !memcmp(c->entries[idx].request, frame, len))
			return &c->entries[idx].response;
	}
	return NULL;
}

static void cache_insert(struct response_cache *c, const uint8_t *frame, size_t len,
			 const struct omci_response *resp)
{
	struct cached_response *e = &c->entries[c->next];

	if (len > sizeof(e->request))
		return;
	memcpy(e->request, frame, len);
	e->request_len = len;
	e->response = *resp;
	c->next = (c->next + 1) % RESPONSE_CACHE_LIMIT;
	if (c->n < RESPONSE_CACHE_LIMIT)
		c->n++;
}

/* ---------- OMCI agent ---------- */

static int omci_run_agent(const char *interface, struct identity_config *identity,
			  const char *socket_path)
{
	struct status_hub hub;
	struct data_path_backend backend;
	struct mib mib;
	struct pond_sock sock;
	uint8_t serial[8];
	bool have_serial = false;
	struct response_cache cache;
	int rc;

	memset(&cache, 0, sizeof(cache));

	rc = backend_init(&backend, interface, identity->alloc_id_timeout);
	if (rc < 0) {
		fprintf(stderr, "OMCI interface %s has no parent xpon data path attributes\n",
			interface);
		return 1;
	}
	if (backend_active_serial_number(&backend, serial, &have_serial) == 0 && have_serial) {
		memcpy(identity->serial_number, serial, 8);
		identity->serial_number_len = 8;
	}

	mib_init(&mib);
	{
		/* Build the initial MIB and the autonomous defaults. */
		mib_populate_from_identity(&mib, identity);
	}

	if (pond_sock_open(&sock, interface) < 0) {
		fprintf(stderr, "cannot open AF_PACKET socket on %s\n", interface);
		mib_free(&mib);
		return 1;
	}

	status_hub_init(&hub, interface, identity->loid_len > 0);

	{
		struct provisioning_snapshot snap;
		struct data_path_graph graph;
		struct backend_status bst;

		omci_provisioning_snapshot(&mib, &snap);
		backend_reconcile(&backend, &snap);
		omci_data_path_graph(&mib, &graph);
		backend_get_status(&backend, &bst);
		status_record_provisioning(&hub, &snap, &graph, &bst);
		omci_provisioning_snapshot_free(&snap);
		omci_data_path_graph_free(&graph);
	}

	if (status_server_start(&hub, socket_path) < 0) {
		fprintf(stderr, "cannot start control socket %s\n", socket_path);
		pond_sock_close(&sock);
		mib_free(&mib);
		return 1;
	}

	printf("OMCI agent started: interface=%s control_socket=%s loid_configured=%s "
	       "omcc_version=0x%02x disable_enhanced_security=%s\n",
	       interface, socket_path, identity->loid_len ? "true" : "false",
	       identity->omcc_version,
	       identity->disable_enhanced_security ? "true" : "false");
	fflush(stdout);

	for (;;) {
		unsigned recheck = backend_recheck_interval_ms(&backend);
		uint8_t frame[RECEIVE_BUFFER_LEN];
		uint8_t pkt_type = 0;
		int n;

		if (recheck) {
			int ready = pond_sock_wait(&sock, (int)recheck);

			if (ready == 0) {
				/* The kernel may have applied deferred paths since the last request. */
				struct provisioning_snapshot snap;
				struct data_path_graph graph;
				struct backend_status bst;

				omci_provisioning_snapshot(&mib, &snap);
				backend_reconcile(&backend, &snap);
				omci_data_path_graph(&mib, &graph);
				backend_get_status(&backend, &bst);
				status_record_provisioning(&hub, &snap, &graph, &bst);
				omci_provisioning_snapshot_free(&snap);
				omci_data_path_graph_free(&graph);
				continue;
			}
			if (ready < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
		}
		n = pond_sock_recv(&sock, frame, sizeof(frame), &pkt_type);
		if (n < 0) {
			char msg[96];

			snprintf(msg, sizeof(msg), "receive failed: %s", strerror(-n));
			status_record_transport_error(&hub, msg);
			if (-n == ENETDOWN && pond_sock_present(&sock)) {
				/* Retransmissions never span a line down; the MIB itself is retained. */
				cache_clear(&cache);
				continue;
			}
			break;
		}
		if (pkt_type == POND_PACKET_OUTGOING)
			continue;

		{
			struct omci_request req;
			struct omci_response resp, *cached;
			int parse = omci_request_parse(&req, frame, (size_t)n);

			if (parse < 0) {
				status_record_parse_error(&hub, "invalid OMCI PDU");
				continue;
			}
			status_record_rx(&hub, &req);

			/* CTC LOID Authentication attribute 4 carries the OLT result. */
			if (req.action == OMCI_ACTION_SET && req.class_id == 0xfffa &&
			    req.entity_id == 0 && req.attribute_mask == 0x1000) {
				uint8_t st = req.content_len ? req.content[0] : 0;

				status_record_authentication_status(
					&hub, st, ctc_authentication_status_name(st));
				printf("OMCI CTC LOID authentication result: status=%u meaning=%s "
				       "loid_configured=%s\n",
				       st, ctc_authentication_status_name(st),
				       identity->loid_len ? "true" : "false");
			}

			cached = cache_lookup(&cache, frame, (size_t)n);
			if (cached) {
				resp = *cached;
			} else {
				struct avc *avcs = NULL;
				size_t n_avcs;

				if (backend_active_serial_number(&backend, serial, &have_serial) == 0 &&
				    have_serial)
					mib_set_onu_serial(&mib, serial);
				if (mib_dispatch(&mib, &req, &resp) < 0)
					continue;

				if (mib.has_pending_msk) {
					int installed = backend_install_master_session_key(
						&backend, mib.pending_msk) == 0;

					mib.has_pending_msk = false;
					mib_complete_msk_install(&mib, installed);
					if (installed)
						printf("OMCI enhanced security authentication: accepted\n");
					else
						printf("OMCI enhanced security authentication: "
						       "kernel key install failed\n");
				}

				{
					struct provisioning_snapshot snap;
					struct data_path_graph graph;
					struct backend_status bst;

					omci_provisioning_snapshot(&mib, &snap);
					backend_reconcile(&backend, &snap);
					omci_data_path_graph(&mib, &graph);
					backend_get_status(&backend, &bst);
					status_record_provisioning(&hub, &snap, &graph, &bst);
					omci_provisioning_snapshot_free(&snap);
					omci_data_path_graph_free(&graph);
				}
				cache_insert(&cache, frame, (size_t)n, &resp);

				n_avcs = mib_take_avcs(&mib, &avcs);
				for (size_t i = 0; i < n_avcs; i++) {
					struct omci_response note;

					if (!omci_response_avc(0, avcs[i].class_id,
							       avcs[i].entity_id,
							       avcs[i].attribute_mask,
							       avcs[i].value, avcs[i].value_len,
							       &note))
						pond_sock_send(&sock, note.bytes, note.len);
				}
				free(avcs);
			}

			if (pond_sock_send(&sock, resp.bytes, resp.len) < 0) {
				status_record_transport_error(&hub, "send failed");
				continue;
			}
			status_record_tx(&hub, &req, resp.has_result, resp.result, cached != NULL);
		}
	}

	status_server_stop();
	pond_sock_close(&sock);
	status_hub_free(&hub);
	mib_free(&mib);
	return 0;
}

/* ---------- control client (pondctl) ---------- */

static int run_control(const char *line, const char *request)
{
	char path[128];
	struct sockaddr_un addr;
	int fd, rc = 0;

	control_socket_path(path, sizeof(path), line);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "cannot connect to %s: %s\n", path, strerror(errno));
		close(fd);
		return 1;
	}
	if (write(fd, request, strlen(request)) < 0) {
		perror("write");
		rc = 1;
	}
	shutdown(fd, SHUT_WR);
	for (;;) {
		char buf[4096];
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			rc = 1;
			break;
		}
		if (!n)
			break;
		fwrite(buf, 1, (size_t)n, stdout);
	}
	close(fd);
	return rc;
}

/* ---------- main ---------- */

static void usage(const char *prog)
{
	printf("%s %s\n%s\n\n", prog, VERSION, PROJECT_URL);
	if (!strcmp(prog, "pondctl"))
		printf("Usage:\n  pondctl status --line NAME\n"
		       "  pondctl datapath --line NAME\n"
		       "  pondctl events --line NAME [--after SEQUENCE]\n");
	else
		printf("Usage:\n  airoha-pond --line NAME\n");
}

int main(int argc, char **argv)
{
	char prog_buf[128];
	const char *prog = "airoha-pond";
	const char *line = NULL;
	int i;

	snprintf(prog_buf, sizeof(prog_buf), "%s", argv[0]);
	{
		const char *base = strrchr(prog_buf, '/');

		prog = !strcmp(base ? base + 1 : prog_buf, "pondctl") ? "pondctl"
								      : "airoha-pond";
	}

	if (argc < 2) {
		usage(prog);
		return 0;
	}
	if (!strcmp(argv[1], "help") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) {
		usage(prog);
		return 0;
	}
	if (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-V")) {
		printf("%s %s\n%s\n", prog, VERSION, PROJECT_URL);
		return 0;
	}

	if (!strcmp(prog, "pondctl")) {
		char request[64];
		unsigned long long after = 0;

		if (argc < 4) {
			fprintf(stderr, "run 'pondctl --help' for usage\n");
			return 2;
		}
		if (strcmp(argv[2], "--line")) {
			fprintf(stderr, "run 'pondctl --help' for usage\n");
			return 2;
		}
		line = argv[3];
		if (!strcmp(argv[1], "status")) {
			snprintf(request, sizeof(request), "STATUS 1");
		} else if (!strcmp(argv[1], "datapath")) {
			snprintf(request, sizeof(request), "DATAPATH 1");
		} else if (!strcmp(argv[1], "events")) {
			for (i = 4; i + 1 < argc; i++)
				if (!strcmp(argv[i], "--after"))
					after = strtoull(argv[i + 1], NULL, 10);
			snprintf(request, sizeof(request), "EVENTS 1 %llu", after);
		} else {
			fprintf(stderr, "run 'pondctl --help' for usage\n");
			return 2;
		}
		return run_control(line, request);
	}

	/* daemon */
	for (i = 1; i + 1 < argc; i++)
		if (!strcmp(argv[i], "--line"))
			line = argv[i + 1];
	if (!line) {
		fprintf(stderr, "usage: airoha-pond --line NAME\n");
		return 2;
	}

	{
		struct pond_config cfg;
		char errbuf[160];
		const struct pond_section *xpon, *linked;
		struct identity_config identity;

		if (pond_config_load(&cfg, "/etc/config/pon", errbuf, sizeof(errbuf)) < 0) {
			fprintf(stderr, "%s\n", errbuf);
			return 1;
		}
		xpon = pond_config_section(&cfg, line, "xpon");
		if (!xpon) {
			fprintf(stderr, "UCI xpon section '%s' was not found\n", line);
			return 1;
		}
		{
			const char *mode = pond_section_option(xpon, "mode");
			const char *proto = protocol_for_mode(mode ? mode : "");

			if (!strcmp(proto, "oam")) {
				/* EPON: IEEE 802.3ah discovery + CTC organization-specific OAM. */
				const struct pond_section *osec =
					pond_config_linked(&cfg, "oam", line);
				struct oam_config ocfg;
				char sock_path[128], errbuf[160];
				const char *device;

				if (!osec) {
					fprintf(stderr,
						"UCI oam section for line '%s' was not found\n", line);
					return 1;
				}
				if (oam_config_from_section(&ocfg, osec, errbuf,
							    sizeof(errbuf)) < 0) {
					fprintf(stderr, "%s\n", errbuf);
					return 1;
				}
				device = pond_section_option(osec, "device");
				if (!device) {
					fprintf(stderr, "UCI oam section '%s' has no device option\n",
						line);
					return 1;
				}
				control_socket_path(sock_path, sizeof(sock_path), line);
				return oam_run_agent(device, &ocfg, sock_path);
			}
		}
		linked = pond_config_linked(&cfg, "omci", line);
		if (!linked) {
			fprintf(stderr, "UCI omci section for line '%s' was not found\n", line);
			return 1;
		}
		{
			const char *device = pond_section_option(linked, "device");
			char sock_path[128];

			if (!device) {
				fprintf(stderr, "UCI omci section '%s' has no device option\n", line);
				return 1;
			}
			identity_from_section(&identity, linked);
			control_socket_path(sock_path, sizeof(sock_path), line);
			return omci_run_agent(device, &identity, sock_path);
		}
	}
}
