/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci_backend.h"
#include "runtime.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALLOC_ID_RECHECK_MS 1000

static int read_file(const char *path, char *out, size_t out_len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	size_t got = 0;

	if (fd < 0)
		return -errno;
	while (got + 1 < out_len) {
		ssize_t n = read(fd, out + got, out_len - 1 - got);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -errno;
		}
		if (!n)
			break;
		got += (size_t)n;
	}
	out[got] = '\0';
	close(fd);
	return 0;
}

/* Returns 0 on success, -EAGAIN when the kernel defers the request, <0 otherwise. */
static int write_file(const char *path, const char *value, size_t len)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	size_t done = 0;

	if (fd < 0)
		return -errno;
	while (done < len) {
		ssize_t n = write(fd, value + done, len - done);

		if (n < 0) {
			int e = -errno;

			close(fd);
			return e;
		}
		done += (size_t)n;
	}
	close(fd);
	return 0;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int decode_hex(const char *text, uint8_t *out, size_t out_cap, size_t *out_len)
{
	size_t n = 0, i = 0;

	while (text[i] && text[i + 1]) {
		int hi = hex_nibble(text[i]), lo = hex_nibble(text[i + 1]);

		if (hi < 0 || lo < 0 || n >= out_cap)
			return -EINVAL;
		out[n++] = (uint8_t)((hi << 4) | lo);
		i += 2;
	}
	if (text[i])
		return -EINVAL;
	*out_len = n;
	return 0;
}

/* ---------- data_path parsing ---------- */

static const char *field_value(const char *text, const char *key, size_t *value_len)
{
	size_t klen = strlen(key);
	const char *p = text;

	while (*p) {
		const char *start;

		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		start = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		if ((size_t)(p - start) > klen && !strncmp(start, key, klen) &&
		    start[klen] == '=') {
			*value_len = (size_t)(p - start) - klen - 1;
			return start + klen + 1;
		}
	}
	return NULL;
}

int backend_parse_mapping(const char *text, struct data_path_candidate *out,
			  size_t cap, size_t *n_out)
{
	const char *p = text;
	size_t configured_len = 0;
	const char *configured = field_value(text, "configured", &configured_len);

	*n_out = 0;
	if (configured && configured_len == 1 && configured[0] != '1')
		return 0;	/* not configured -> empty mapping */
	if (!configured)
		return 0;

	while (*p) {
		const char *start;
		size_t flen;
		char buf[96];
		struct data_path_candidate *c;

		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;
		start = p;
		while (*p && !isspace((unsigned char)*p))
			p++;
		flen = (size_t)(p - start);
		if (flen <= 5 || strncmp(start, "path=", 5))
			continue;
		if (flen - 5 >= sizeof(buf))
			return -EINVAL;
		memcpy(buf, start + 5, flen - 5);
		buf[flen - 5] = '\0';

		/* Split on ':' exactly like the Rust implementation. */
		{
			char *parts[8];
			int np = 0;
			char *q = buf;

			parts[np++] = q;
			while (*q && np < 8) {
				if (*q == ':') {
					*q = '\0';
					parts[np++] = q + 1;
				}
				q++;
			}
			if (np < 4 || np > 5)
				return -EINVAL;
			{
				char *endp;
				unsigned long a = strtoul(parts[0], &endp, 10);
				unsigned long g, v, pb;
				long mcv = 0;

				if (*endp) return -EINVAL;
				g = strtoul(parts[1], &endp, 10);
				if (*endp) return -EINVAL;
				v = strtoul(parts[2], &endp, 10);
				if (*endp) return -EINVAL;
				pb = strtoul(parts[3], &endp, 16);
				if (*endp) return -EINVAL;
				if (np == 5) {
					mcv = strtol(parts[4], &endp, 10);
					if (*endp || (mcv != 0 && mcv != 1)) return -EINVAL;
				}
				if (a > 0xffff || g > 0xffff || v > 0xffff || pb > 0xff)
					return -EINVAL;
				if (*n_out >= cap)
					return -E2BIG;
				c = &out[(*n_out)++];
				c->alloc_id = (uint16_t)a;
				c->gem_id = (uint16_t)g;
				c->vlan_id = (uint16_t)v;
				c->pbit_mask = (uint8_t)pb;
				c->multicast = mcv ? true : false;
			}
		}
	}

	if (!*n_out)
		return -EINVAL;	/* configured but contains no path entries */
	return 0;
}

static int read_current_mapping(struct data_path_backend *b)
{
	char text[8192];
	int rc = read_file(b->data_path, text, sizeof(text));

	if (rc < 0)
		return rc;
	rc = backend_parse_mapping(text, b->applied, BACKEND_MAX_APPLIED, &b->n_applied);
	if (rc < 0)
		b->n_applied = 0;
	return rc;
}

/* ---------- init ---------- */

int backend_init(struct data_path_backend *b, const char *interface,
		 unsigned alloc_id_timeout_s)
{
	memset(b, 0, sizeof(*b));
	b->alloc_id_timeout_s = alloc_id_timeout_s;

	if (pond_parent_xpon_attr(interface, "data_path", b->data_path,
				  sizeof(b->data_path)) < 0)
		return -ENOENT;
	if (pond_parent_xpon_attr(interface, "omci_msk", b->omci_msk,
				  sizeof(b->omci_msk)) < 0)
		return -ENOENT;
	if (pond_parent_xpon_attr(interface, "active_serial_number",
				  b->active_serial_number,
				  sizeof(b->active_serial_number)) < 0)
		return -ENOENT;
	if (pond_parent_xpon_attr(interface, "service_ready", b->service_ready,
				  sizeof(b->service_ready)) == 0)
		b->has_service_ready = true;

	snprintf(b->status.state, sizeof(b->status.state), "waiting-for-gem");
	if (read_current_mapping(b) == 0) {
		for (size_t i = 0; i < b->n_applied; i++) {
			if (b->applied[i].multicast)
				continue;
			snprintf(b->status.state, sizeof(b->status.state),
				 "kernel-mapping-present");
			b->status.has_alloc_id = true;
			b->status.active_alloc_id = b->applied[i].alloc_id;
			b->status.has_gem_id = true;
			b->status.active_gem_id = b->applied[i].gem_id;
			break;
		}
	}
	return 0;
}

unsigned backend_recheck_interval_ms(const struct data_path_backend *b)
{
	return b->waiting ? ALLOC_ID_RECHECK_MS : 0;
}

/* ---------- status / LED ---------- */

static void publish_service_ready(struct data_path_backend *b, bool ready)
{
	if (!b->has_service_ready)
		return;
	if (write_file(b->service_ready, ready ? "1\n" : "0\n", 2) < 0)
		fprintf(stderr, "PON service LED state update failed\n");
}

void backend_get_status(const struct data_path_backend *b, struct backend_status *out)
{
	bool any_unicast = false, all_any = true;

	*out = b->status;
	for (size_t i = 0; i < b->n_applied; i++) {
		if (b->applied[i].multicast)
			continue;
		any_unicast = true;
		if (b->applied[i].vlan_id != DATA_PATH_VLAN_ANY)
			all_any = false;
	}
	out->all_vlans = any_unicast && all_any;
}

static int clear_applied(struct data_path_backend *b)
{
	int rc;

	if (!b->n_applied)
		return 0;
	rc = write_file(b->data_path, "clear\n", 6);
	if (rc < 0)
		return rc;
	b->n_applied = 0;
	b->status.has_alloc_id = false;
	b->status.has_gem_id = false;
	return 0;
}

static void set_error(struct data_path_backend *b, const char *state, const char *msg)
{
	snprintf(b->status.state, sizeof(b->status.state), "%s", state);
	if (msg) {
		snprintf(b->status.error, sizeof(b->status.error), "%s", msg);
		b->status.has_error = true;
	}
}

/* ---------- reconcile ---------- */

static bool candidates_equal(const struct data_path_candidate *a, const struct data_path_candidate *b)
{
	return a->alloc_id == b->alloc_id && a->gem_id == b->gem_id &&
	       a->vlan_id == b->vlan_id && a->pbit_mask == b->pbit_mask &&
	       a->multicast == b->multicast;
}

void backend_reconcile(struct data_path_backend *b, const struct provisioning_snapshot *snap)
{
	struct data_path_candidate desired[BACKEND_MAX_APPLIED];
	size_t n_desired = 0;
	bool waiting_since_set = b->waiting;
	struct timespec waiting_since = b->waiting_since;
	bool service_ready;
	unsigned distinct_gems = 0;
	uint16_t gem_seen[BACKEND_MAX_APPLIED];
	size_t n_gem_seen = 0;

	b->status.has_error = false;
	b->status.error[0] = '\0';
	b->waiting = false;

	/* A new PON epoch clears hardware GEM mappings while the OMIB retains state. */
	if (read_current_mapping(b) < 0) {
		publish_service_ready(b, false);
		set_error(b, "apply-failed", "cannot read the kernel data path");
		return;
	}
	for (size_t i = 0; i < b->n_applied; i++) {
		if (b->applied[i].multicast)
			continue;
		b->status.has_alloc_id = true;
		b->status.active_alloc_id = b->applied[i].alloc_id;
		b->status.has_gem_id = true;
		b->status.active_gem_id = b->applied[i].gem_id;
		break;
	}

	for (size_t i = 0; i < snap->n_data_paths && n_desired < BACKEND_MAX_APPLIED; i++)
		desired[n_desired++] = snap->data_paths[i];

	/* distinct unicast GEM ids */
	for (size_t i = 0; i < n_desired; i++) {
		int seen = 0;

		if (desired[i].multicast)
			continue;
		for (size_t k = 0; k < n_gem_seen; k++)
			if (gem_seen[k] == desired[i].gem_id)
				seen = 1;
		if (!seen && n_gem_seen < BACKEND_MAX_APPLIED) {
			gem_seen[n_gem_seen++] = desired[i].gem_id;
			distinct_gems++;
		}
	}

	if (distinct_gems == 1) {
		/*
		 * A single upstream GEM accepts every VLAN and keeps multicast GEMs as
		 * separate hardware paths.
		 */
		struct data_path_candidate unicast;
		size_t w = 0;
		int found = 0;

		for (size_t i = 0; i < n_desired; i++) {
			if (!desired[i].multicast) {
				unicast = desired[i];
				found = 1;
				break;
			}
		}
		if (found) {
			unicast.vlan_id = DATA_PATH_VLAN_ANY;
			unicast.pbit_mask = 0xff;
			for (size_t i = 0; i < n_desired; i++) {
				if (!desired[i].multicast)
					continue;
				desired[w++] = desired[i];
			}
			memmove(&desired[1], &desired[0], w * sizeof(desired[0]));
			desired[0] = unicast;
			n_desired = w + 1;
		}
	} else if (distinct_gems > 1) {
		/*
		 * VID_ANY matches untagged frames and tagged frames after concrete VID
		 * lookup; equal VID and P-bit coverage creates ambiguity.
		 */
		for (size_t i = 0; i < n_desired; i++) {
			int conflict = 0;

			if (desired[i].multicast)
				continue;
			for (size_t k = 0; k < i; k++) {
				if (!desired[k].multicast &&
				    (desired[i].pbit_mask & desired[k].pbit_mask) &&
				    desired[i].vlan_id == desired[k].vlan_id)
					conflict = 1;
			}
			if (desired[i].pbit_mask == 0 || conflict) {
				if (clear_applied(b) < 0) {
					publish_service_ready(b, false);
					set_error(b, "clear-failed", "cannot clear the kernel data path");
					return;
				}
				set_error(b, "ambiguous-gem-mapping", NULL);
				snprintf(b->status.error, sizeof(b->status.error),
					 "%u bidirectional GEM paths contain overlapping VLAN/802.1p rules",
					 distinct_gems);
				b->status.has_error = true;
				publish_service_ready(b, false);
				return;
			}
		}
	}

	service_ready = false;
	for (size_t i = 0; i < n_desired; i++)
		if (!desired[i].multicast)
			service_ready = true;

	if (!n_desired) {
		if (clear_applied(b) < 0) {
			publish_service_ready(b, false);
			set_error(b, "clear-failed", "cannot clear the kernel data path");
			return;
		}
		set_error(b, "waiting-for-gem", NULL);
		publish_service_ready(b, false);
		return;
	}

	if (b->n_applied == n_desired) {
		int same = 1;

		for (size_t i = 0; i < n_desired; i++)
			if (!candidates_equal(&b->applied[i], &desired[i]))
				same = 0;
		if (same) {
			if (waiting_since_set) {
				struct timespec now;

				clock_gettime(CLOCK_MONOTONIC, &now);
				printf("OMCI data paths: kernel applied the deferred paths after %ld s\n",
				       (long)(now.tv_sec - waiting_since.tv_sec));
			}
			set_error(b, "applied", NULL);
			publish_service_ready(b, service_ready);
			return;
		}
	}

	{
		char value[BACKEND_MAX_APPLIED * 48 + 16];
		size_t off = 0;

		off += (size_t)snprintf(value + off, sizeof(value) - off, "replace");
		for (size_t i = 0; i < n_desired; i++) {
			off += (size_t)snprintf(value + off, sizeof(value) - off,
						" %u:%u:%u:%02x:%u",
						desired[i].alloc_id, desired[i].gem_id,
						desired[i].vlan_id, desired[i].pbit_mask,
						desired[i].multicast ? 1u : 0u);
			if (off >= sizeof(value) - 1)
				break;
		}
		value[off++] = '\n';

		{
			int rc = write_file(b->data_path, value, off);

			if (rc == 0) {
				memcpy(b->applied, desired, n_desired * sizeof(desired[0]));
				b->n_applied = n_desired;
				set_error(b, "applied", NULL);
				for (size_t i = 0; i < b->n_applied; i++) {
					if (b->applied[i].multicast)
						continue;
					b->status.has_alloc_id = true;
					b->status.active_alloc_id = b->applied[i].alloc_id;
					b->status.has_gem_id = true;
					b->status.active_gem_id = b->applied[i].gem_id;
					break;
				}
				publish_service_ready(b, service_ready);
			} else if (rc == -EAGAIN) {
				/*
				 * The kernel holds the request until PLOAM assigns every unicast
				 * Alloc-ID. A timeout only reports the failure; a late assignment
				 * still applies.
				 */
				publish_service_ready(b, false);
				clock_gettime(CLOCK_MONOTONIC, &b->waiting_since);
				if (waiting_since_set)
					b->waiting_since = waiting_since;
				b->waiting = true;
				if (b->has_timeout &&
				    (unsigned)(b->waiting_since.tv_sec - waiting_since.tv_sec) >=
					    b->alloc_id_timeout_s) {
					if (strcmp(b->status.state, "alloc-id-timeout")) {
						printf("OMCI data paths: PLOAM has not assigned every Alloc-ID after %u s\n",
						       b->alloc_id_timeout_s);
					}
					set_error(b, "alloc-id-timeout", NULL);
					snprintf(b->status.error, sizeof(b->status.error),
						 "PLOAM has not assigned every Alloc-ID after %u s",
						 b->alloc_id_timeout_s);
					b->status.has_error = true;
				} else {
					set_error(b, "waiting-for-alloc-id", NULL);
				}
			} else {
				publish_service_ready(b, false);
				set_error(b, "apply-failed", strerror(-rc));
			}
		}
	}
}

int backend_install_master_session_key(const struct data_path_backend *b,
				       const uint8_t key[16])
{
	/* The sysfs ABI accepts the session key as exactly 32 hexadecimal characters. */
	char encoded[40];
	size_t off = 0;

	for (int i = 0; i < 16; i++)
		off += (size_t)snprintf(encoded + off, sizeof(encoded) - off, "%02x", key[i]);
	encoded[off++] = '\n';
	return write_file(b->omci_msk, encoded, off);
}

int backend_active_serial_number(const struct data_path_backend *b,
				 uint8_t serial[8], bool *has_serial)
{
	char text[128];
	size_t len = 0;
	char *end;
	int rc = read_file(b->active_serial_number, text, sizeof(text));

	*has_serial = false;
	if (rc < 0)
		return rc;
	end = text + strlen(text);
	while (end > text && isspace((unsigned char)end[-1]))
		*--end = '\0';
	if (!strcmp(text, "none"))
		return 0;
	rc = decode_hex(text, serial, 8, &len);
	if (rc < 0)
		return rc;
	if (len != 8)
		return -EINVAL;
	*has_serial = true;
	return 0;
}
