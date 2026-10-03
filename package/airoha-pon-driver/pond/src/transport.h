/* SPDX-License-Identifier: GPL-2.0-only */
/* AF_PACKET raw socket for PON OMCI/OAM frames (port of transport.rs). */
#ifndef POND_TRANSPORT_H
#define POND_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POND_PACKET_OUTGOING 4

struct pond_sock {
	int fd;
	int ifindex;
	char ifname[64];
};

int pond_sock_open(struct pond_sock *s, const char *interface);
void pond_sock_close(struct pond_sock *s);

/* true when the interface still exists with the same ifindex */
bool pond_sock_present(const struct pond_sock *s);

/* 0 = timeout, 1 = readable, <0 = error */
int pond_sock_wait(const struct pond_sock *s, int timeout_ms);

/* >=0 length, <0 error. *pkt_type receives sll_pkttype. */
int pond_sock_recv(struct pond_sock *s, uint8_t *buf, size_t cap, uint8_t *pkt_type);

int pond_sock_send(struct pond_sock *s, const uint8_t *frame, size_t len);

#endif
