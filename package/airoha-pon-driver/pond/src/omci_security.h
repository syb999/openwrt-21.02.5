/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OMCI enhanced-security authentication (Class 332) and message integrity.
 * Port of airoha-pon-daemons omci/security.rs.
 *
 * AES-CMAC-128 is obtained through the kernel's AF_ALG interface
 * ("cmac(aes)"), which every kernel with CONFIG_CRYPTO_CMAC provides --
 * this avoids vendoring an AES implementation into the userspace agent.
 */
#ifndef POND_OMCI_SECURITY_H
#define POND_OMCI_SECURITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "omci_mib.h"

/* One validated Set update handed to the Class 332 state machine. */
struct sec_update {
	uint8_t index;
	const uint8_t *value;
	size_t len;
	int is_table;
};

/* Fixed AES-CMAC-128 key used by Class 332 authentication. */
extern const uint8_t pond_enhanced_psk[16];

/* AES-CMAC over an arbitrary message; 0 on success. */
int omci_security_aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
			   uint8_t out[16]);

/* /dev/urandom */
int omci_security_fill_random(uint8_t *bytes, size_t len);

/* Modulo-256 addition of the OLT challenge and the ONU random value. */
void omci_security_onu_random_challenge(const uint8_t olt[16], const uint8_t random[16],
					uint8_t out[16]);

/*
 * Combined authentication result. `identity` is 8 bytes (the ONU serial) for
 * step 2 and ignored (pass zeros) for step 1. Returns 0 on success.
 */
int omci_security_authentication_result(const uint8_t (*olt)[16], size_t n_olt,
					const uint8_t (*onu)[16], size_t n_onu,
					const uint8_t identity[8], bool olt_first,
					uint8_t out[16]);

int omci_security_master_session_key(const uint8_t (*olt)[16], size_t n_olt,
				     const uint8_t (*onu)[16], size_t n_onu,
				     uint8_t out[16]);

int omci_security_master_session_key_name(const uint8_t (*olt)[16], size_t n_olt,
					  const uint8_t (*onu)[16], size_t n_onu,
					  uint8_t out[16]);

/* Apply validated Class 332 Set updates (mib.rs apply_enhanced_security_updates). */
int omci_enhanced_security_apply(struct mib *m, const struct sec_update *updates,
				 size_t n_updates);

#endif
