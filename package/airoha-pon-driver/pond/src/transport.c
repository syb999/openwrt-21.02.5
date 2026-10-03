/* SPDX-License-Identifier: GPL-2.0-only */
#include "transport.h"
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <unistd.h>

#ifndef PACKET_IGNORE_OUTGOING
#define PACKET_IGNORE_OUTGOING 23
#endif

int pond_sock_open(struct pond_sock *s, const char *interface)
{
	struct sockaddr_ll addr;
	int one = 1;

	memset(s, 0, sizeof(*s));
	s->fd = -1;
	snprintf(s->ifname, sizeof(s->ifname), "%s", interface);
	s->ifindex = (int)if_nametoindex(interface);
	if (!s->ifindex)
		return -errno ? -errno : -ENODEV;

	s->fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ALL));
	if (s->fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sll_family = AF_PACKET;
	addr.sll_protocol = htons(ETH_P_ALL);
	addr.sll_ifindex = s->ifindex;
	if (bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int e = -errno;
		close(s->fd);
		s->fd = -1;
		return e;
	}

	/* PACKET_IGNORE_OUTGOING filters copies of frames emitted by this socket. */
	setsockopt(s->fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &one, sizeof(one));
	return 0;
}

void pond_sock_close(struct pond_sock *s)
{
	if (s && s->fd >= 0) {
		close(s->fd);
		s->fd = -1;
	}
}

bool pond_sock_present(const struct pond_sock *s)
{
	int idx = (int)if_nametoindex(s->ifname);

	return idx != 0 && idx == s->ifindex;
}

int pond_sock_wait(const struct pond_sock *s, int timeout_ms)
{
	struct pollfd p = { .fd = s->fd, .events = POLLIN };
	int rc = poll(&p, 1, timeout_ms);

	if (rc < 0)
		return -errno;
	return rc > 0 ? 1 : 0;
}

int pond_sock_recv(struct pond_sock *s, uint8_t *buf, size_t cap, uint8_t *pkt_type)
{
	struct sockaddr_ll peer;
	socklen_t plen = sizeof(peer);
	ssize_t n;

	memset(&peer, 0, sizeof(peer));
	n = recvfrom(s->fd, buf, cap, 0, (struct sockaddr *)&peer, &plen);
	if (n < 0)
		return -errno;
	if (pkt_type)
		*pkt_type = peer.sll_pkttype;
	return (int)n;
}

int pond_sock_send(struct pond_sock *s, const uint8_t *frame, size_t len)
{
	struct sockaddr_ll addr;
	ssize_t n;

	memset(&addr, 0, sizeof(addr));
	addr.sll_family = AF_PACKET;
	addr.sll_protocol = htons(ETH_P_ALL);
	addr.sll_ifindex = s->ifindex;
	n = sendto(s->fd, frame, len, 0, (struct sockaddr *)&addr, sizeof(addr));
	if (n < 0)
		return -errno;
	if ((size_t)n != len)
		return -EIO;
	return 0;
}
