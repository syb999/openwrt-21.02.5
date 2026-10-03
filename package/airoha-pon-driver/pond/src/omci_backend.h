/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PON data-path backend: applies the provisioning snapshot to the kernel.
 * Port of airoha-pon-daemons omci/backend.rs.
 *
 * The airoha-xpon driver exposes its data-path ABI through sysfs
 * (/sys/class/net/<parent>/xpon/<attr>), not netlink:
 *   data_path            read the current mapping, write "replace ..." / "clear"
 *   service_ready        write 0/1
 *   omci_msk             write the 16-byte master session key as 32 hex chars
 *   active_serial_number read the PLOAM serial as hex, or "none"
 */
#ifndef POND_OMCI_BACKEND_H
#define POND_OMCI_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "omci_provisioning.h"

#define BACKEND_MAX_APPLIED 256
#define BACKEND_PATH_LEN    256
#define BACKEND_STATE_LEN   32

struct backend_status {
	char state[BACKEND_STATE_LEN];
	bool has_alloc_id;
	uint16_t active_alloc_id;
	bool has_gem_id;
	uint16_t active_gem_id;
	bool all_vlans;
	bool has_error;
	char error[160];
};

struct data_path_backend {
	char data_path[BACKEND_PATH_LEN];
	char service_ready[BACKEND_PATH_LEN];
	bool has_service_ready;
	char omci_msk[BACKEND_PATH_LEN];
	char active_serial_number[BACKEND_PATH_LEN];
	struct data_path_candidate applied[BACKEND_MAX_APPLIED];
	size_t n_applied;
	struct backend_status status;
	bool has_timeout;
	unsigned alloc_id_timeout_s;
	bool waiting;
	struct timespec waiting_since;
};

/* Resolve the parent xpon attributes for an OMCI interface. 0 on success. */
int backend_init(struct data_path_backend *b, const char *interface,
		 unsigned alloc_id_timeout_s);

/* Interval (ms) at which the caller should reconcile again, or 0 for "only on demand". */
unsigned backend_recheck_interval_ms(const struct data_path_backend *b);

/* Apply the snapshot to the kernel. */
void backend_reconcile(struct data_path_backend *b, const struct provisioning_snapshot *snap);

/* Status with the derived all_vlans flag. */
void backend_get_status(const struct data_path_backend *b, struct backend_status *out);

int backend_install_master_session_key(const struct data_path_backend *b,
				       const uint8_t key[16]);

/* 0 = read ok (has_serial tells whether "none"), <0 on error. */
int backend_active_serial_number(const struct data_path_backend *b,
				 uint8_t serial[8], bool *has_serial);

/* Parse a data_path sysfs dump. Exposed for testing. */
int backend_parse_mapping(const char *text, struct data_path_candidate *out,
			  size_t cap, size_t *n_out);

#endif
