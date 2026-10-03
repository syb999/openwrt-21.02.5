/* SPDX-License-Identifier: GPL-2.0-only */
/* Linux integration helpers (port of wire-side runtime.rs). */
#ifndef POND_RUNTIME_H
#define POND_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>

/* Resolve the parent netdev of a control interface via /sys/class/net/<if>/iflink. */
int pond_parent_interface(const char *interface, char *out, size_t out_len);

/* Read the MAC address of the parent interface. */
int pond_parent_mac(const char *interface, unsigned char mac[6]);

/* Path to a /sys/class/net/<parent>/xpon/<attribute> file, or -1 if absent. */
int pond_parent_xpon_attr(const char *interface, const char *attribute,
			  char *out, size_t out_len);

#endif
