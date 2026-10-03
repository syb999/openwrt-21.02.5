/* SPDX-License-Identifier: GPL-2.0-only */
#include "omci_security.h"
#include "omci_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/if_alg.h>

/* Fixed AES-CMAC-128 key used by Class 332 authentication. */
const uint8_t pond_enhanced_psk[16] = {
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
	0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x00,
};

/* The MSK name derivation appends this fixed peer identity. */
static const uint8_t msk_name_identity[16] = {
	0x31, 0x41, 0x59, 0x26, 0x53, 0x58, 0x97, 0x93,
	0x31, 0x41, 0x59, 0x26, 0x53, 0x58, 0x97, 0x93,
};

/* ---------- AES-CMAC-128 via AF_ALG ---------- */

int omci_security_aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
			   uint8_t out[16])
{
	struct sockaddr_alg sa;
	int fd, op = -1, rc = -EIO;
	struct iovec iov;
	struct msghdr mh;
	ssize_t n;

	memset(&sa, 0, sizeof(sa));
	sa.salg_family = AF_ALG;
	strcpy((char *)sa.salg_type, "hash");
	strcpy((char *)sa.salg_name, "cmac(aes)");

	fd = socket(AF_ALG, SOCK_SEQPACKET, 0);
	if (fd < 0)
		return -errno;
	if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		rc = -errno;
		goto out;
	}
	if (setsockopt(fd, SOL_ALG, ALG_SET_KEY, key, 16) < 0) {
		rc = -errno;
		goto out;
	}
	op = accept(fd, NULL, NULL);
	if (op < 0) {
		rc = -errno;
		goto out;
	}

	iov.iov_base = (void *)msg;
	iov.iov_len = len;
	memset(&mh, 0, sizeof(mh));
	mh.msg_iov = &iov;
	mh.msg_iovlen = 1;
	/* A zero-length message must still be hashed, so send an empty record. */
	if (sendmsg(op, &mh, 0) < 0) {
		rc = -errno;
		goto out;
	}
	n = read(op, out, 16);
	rc = (n == 16) ? 0 : -EIO;
out:
	if (op >= 0)
		close(op);
	close(fd);
	return rc;
}

int omci_security_fill_random(uint8_t *bytes, size_t len)
{
	int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
	size_t got = 0;

	if (fd < 0)
		return -errno;
	while (got < len) {
		ssize_t n = read(fd, bytes + got, len - got);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -errno;
		}
		got += (size_t)n;
	}
	close(fd);
	return 0;
}

void omci_security_onu_random_challenge(const uint8_t olt[16], const uint8_t random[16],
					uint8_t out[16])
{
	for (int i = 0; i < 16; i++)
		out[i] = (uint8_t)(olt[i] + random[i]);	/* modulo-256 addition */
}

/* ---------- challenge composition ---------- */

struct cmac_input {
	uint8_t buf[1 + 32 * 16 + 16];
	size_t len;
};

static void append_challenges(struct cmac_input *in, const uint8_t (*ch)[16], size_t n)
{
	for (size_t i = 0; i < n; i++) {
		memcpy(in->buf + in->len, ch[i], 16);
		in->len += 16;
	}
}

int omci_security_authentication_result(const uint8_t (*olt)[16], size_t n_olt,
					const uint8_t (*onu)[16], size_t n_onu,
					const uint8_t identity[8], bool olt_first,
					uint8_t out[16])
{
	struct cmac_input in = { .len = 0 };

	if (!n_olt || n_olt != n_onu)
		return -EINVAL;
	if (n_olt * 32 + 9 > sizeof(in.buf))
		return -E2BIG;

	in.buf[in.len++] = 1;			/* selected crypto 1 = AES-CMAC-128 */
	if (olt_first) {
		append_challenges(&in, olt, n_olt);
		append_challenges(&in, onu, n_onu);
	} else {
		append_challenges(&in, onu, n_onu);
		append_challenges(&in, olt, n_olt);
	}
	memcpy(in.buf + in.len, identity, 8);
	in.len += 8;
	return omci_security_aes_cmac(pond_enhanced_psk, in.buf, in.len, out);
}

static int session_value(const uint8_t (*olt)[16], size_t n_olt,
			 const uint8_t (*onu)[16], size_t n_onu,
			 const uint8_t *identity, size_t identity_len, uint8_t out[16])
{
	struct cmac_input in = { .len = 0 };

	if (!n_olt || n_olt != n_onu)
		return -EINVAL;
	if (n_olt * 32 + identity_len > sizeof(in.buf))
		return -E2BIG;
	append_challenges(&in, olt, n_olt);
	append_challenges(&in, onu, n_onu);
	if (identity_len) {
		memcpy(in.buf + in.len, identity, identity_len);
		in.len += identity_len;
	}
	return omci_security_aes_cmac(pond_enhanced_psk, in.buf, in.len, out);
}

int omci_security_master_session_key(const uint8_t (*olt)[16], size_t n_olt,
				     const uint8_t (*onu)[16], size_t n_onu, uint8_t out[16])
{
	return session_value(olt, n_olt, onu, n_onu, NULL, 0, out);
}

int omci_security_master_session_key_name(const uint8_t (*olt)[16], size_t n_olt,
					  const uint8_t (*onu)[16], size_t n_onu, uint8_t out[16])
{
	return session_value(olt, n_olt, onu, n_onu, msk_name_identity,
			     sizeof(msk_name_identity), out);
}
