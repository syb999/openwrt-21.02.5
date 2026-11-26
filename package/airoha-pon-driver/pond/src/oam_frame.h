/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IEEE 802.3ah OAM Ethernet frame codec.
 * Port of airoha-pon-daemons oam/frame.rs.
 */
#ifndef POND_OAM_FRAME_H
#define POND_OAM_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OAM_CODE_INFORMATION			0x00
#define OAM_CODE_EVENT_NOTIFICATION		0x01
#define OAM_CODE_VARIABLE_REQUEST		0x02
#define OAM_CODE_LOOPBACK_CONTROL		0x04
#define OAM_CODE_ORGANIZATION_SPECIFIC		0xfe

#define OAM_FLAGS_STABLE			0x0050
/* OAMPDU 头 = 以太网头(DA6+SA6+0x8809) + subtype(0x03) + flags(2) + code(1)。
 * 🔴 驱动 oam0 虽然 hard_header_len=0，但 RX 侧是 napi_build_skb + protocol=0，
 * 没有做 eth_type_trans/skb_pull，交付的 frame 字节里【含完整以太网头】。
 * 实测 25 帧 code 恒为 0xc2 = 组播 DA 01:80:c2:00:00:02 的第 3 字节。
 * 与上游 oam/frame.rs 的 OAM_HEADER_LEN=18 一致。 */
#define OAM_HEADER_LEN				18
#define OAM_MIN_ETHERNET_FRAME_LEN		60

extern const uint8_t oam_slow_protocols_multicast[6];

struct oam_pdu {
	uint8_t code;
	const uint8_t *payload;
	size_t payload_len;
};

/* Parse an OAMPDU; 0 on success, <0 on a malformed frame. */
int oam_pdu_parse(const uint8_t *frame, size_t len, struct oam_pdu *out);

/*
 * Build an OAMPDU. `out` must hold at least oam_frame_len(payload_len) bytes.
 * Returns the frame length (>= OAM_MIN_ETHERNET_FRAME_LEN), or <0 on error.
 */
int oam_pdu_build(const uint8_t source[6], uint16_t flags, uint8_t code,
		  const uint8_t *payload, size_t payload_len,
		  uint8_t *out, size_t out_cap);

size_t oam_frame_len(size_t payload_len);

#endif
