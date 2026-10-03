/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ponctl - Airoha PON control and status utility.
 * C implementation of pbs05/openwrt-pon-userspace airoha-ponctl.
 *
 * Contract (must match the Rust original exactly):
 *   ponctl list
 *   ponctl [--device ponX] status [--json]
 *   ponctl [--device ponX] control
 *   ponctl [--device ponX] identity [show]
 *   ponctl [--device ponX] mode [show]
 *   ponctl [--device ponX] mode set MODE
 *   ponctl [--device ponX] identity set [--serial SN] [--registration-id ID]
 *   ponctl [--device ponX] data-path [show]
 *
 * Device discovery: a netdev is a PON device when /sys/class/net/<n>/xpon/serial_number
 * exists. Control netdevs (omci0/oam0) are ARPHRD_NONE interfaces whose iflink points
 * at the PON device. Line mode is set/queried through the "xpon" generic netlink family;
 * identity and data-path go through sysfs.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>

#define VERSION "0.2.0"
#define PROJECT_URL "https://github.com/pbs05/openwrt-pon-userspace/tree/main/airoha-ponctl"

#ifndef NET_CLASS
#define NET_CLASS "/sys/class/net"
#endif
#ifndef PONCTL_NO_MAIN
#define PONCTL_MAIN
#endif
#define ARPHRD_NONE_S "65534"

#define XPON_FAMILY_NAME "xpon"
#define XPON_VERSION 1
#define XPON_CMD_GET_LINE 1
#define XPON_CMD_SET_LINE 2
#define XPON_CMD_GET_STATUS 3
#define XPON_ATTR_IFINDEX 1
#define XPON_ATTR_MODE 2
#define XPON_ATTR_ACTIVE_MODE 3

#define MAX_DEVS 32
#define MAX_CONTROLS 16
#define MAX_GROUPS 8
#define MAX_FIELDS 32
#define MAX_GROUPS_JSON 8

/* ---------- errors ---------- */

static char err_buf[512];
static const char *fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(err_buf, sizeof(err_buf), fmt, ap);
	va_end(ap);
	return err_buf;
}

/* ---------- file helpers ---------- */

static int read_trimmed(const char *path, char *out, size_t out_len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -1;
	if (!fgets(out, (int)out_len, f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' ||
		     out[n - 1] == '\t'))
		out[--n] = '\0';
	return 0;
}

static int write_value(const char *path, const char *value)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	fprintf(f, "%s\n", value);
	return fclose(f) == 0 ? 0 : -1;
}

static bool is_regular_file(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ---------- PON device discovery ---------- */

struct pon_device {
	char name[64];
	char net_path[256];
	char xpon_path[256];
};

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int discover_pon_devices(struct pon_device *out, size_t cap)
{
	DIR *d = opendir(NET_CLASS);
	struct dirent *e;
	char serial[300];
	size_t n = 0;
	char names[MAX_DEVS][64];
	size_t n_names = 0;

	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		if (e->d_name[0] == '.')
			continue;
		if (snprintf(serial, sizeof(serial), NET_CLASS "/%s/xpon/serial_number",
			     e->d_name) >= (int)sizeof(serial))
			continue;
		if (!is_regular_file(serial))
			continue;
		if (strlen(e->d_name) >= 64)
			continue;
		if (n_names < MAX_DEVS)
			snprintf(names[n_names++], 64, "%s", e->d_name);
	}
	closedir(d);

	/* sorted by name like the Rust implementation */
	{
		char *ptrs[MAX_DEVS];
		size_t i;

		for (i = 0; i < n_names; i++)
			ptrs[i] = names[i];
		qsort(ptrs, n_names, sizeof(ptrs[0]), cmp_str);
		for (i = 0; i < n_names && n < cap; i++) {
			snprintf(out[n].name, sizeof(out[n].name), "%s", ptrs[i]);
			snprintf(out[n].net_path, sizeof(out[n].net_path), NET_CLASS "/%s",
				 ptrs[i]);
			snprintf(out[n].xpon_path, sizeof(out[n].xpon_path),
				 NET_CLASS "/%s/xpon", ptrs[i]);
			n++;
		}
	}
	return (int)n;
}

static int select_pon_device(const char *requested, struct pon_device *out)
{
	struct pon_device devs[MAX_DEVS];
	int n = discover_pon_devices(devs, MAX_DEVS);

	if (n < 0)
		return -1;
	if (requested) {
		for (int i = 0; i < n; i++)
			if (!strcmp(devs[i].name, requested)) {
				*out = devs[i];
				return 0;
			}
		fail("PON device '%s' was not found or has no xpon binding", requested);
		return -1;
	}
	if (n == 0) {
		fail("no PON device with an xpon binding was found");
		return -1;
	}
	if (n == 1) {
		*out = devs[0];
		return 0;
	}
	{
		char list[512] = "";
		size_t o = 0;

		for (int i = 0; i < n; i++)
			o += (size_t)snprintf(list + o, sizeof(list) - o, "%s%s",
					      i ? ", " : "", devs[i].name);
		fail("multiple PON devices are present (%s); select one with --device ponX", list);
	}
	return -1;
}

static int associated_controls(const struct pon_device *dev, char names[][64], size_t cap)
{
	char ifindex[32];
	DIR *d;
	struct dirent *e;
	size_t n = 0;
	char *ptrs[MAX_CONTROLS];

	{
		char p[400];

		snprintf(p, sizeof(p), "%s/ifindex", dev->net_path);
		if (read_trimmed(p, ifindex, sizeof(ifindex)) < 0)
			return -1;
	}
	d = opendir(NET_CLASS);
	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char path[300], type[32], iflink[32];

		if (e->d_name[0] == '.')
			continue;
		if (!strcmp(e->d_name, dev->name))
			continue;
		snprintf(path, sizeof(path), "%s/%s/type", NET_CLASS, e->d_name);
		if (read_trimmed(path, type, sizeof(type)) < 0)
			continue;
		if (strcmp(type, ARPHRD_NONE_S))
			continue;
		snprintf(path, sizeof(path), "%s/%s/iflink", NET_CLASS, e->d_name);
		if (read_trimmed(path, iflink, sizeof(iflink)) < 0)
			continue;
		if (strcmp(iflink, ifindex))
			continue;
		if (strlen(e->d_name) >= 64)
			continue;
		if (n < cap)
			snprintf(names[n], 64, "%s", e->d_name);
		n++;
	}
	closedir(d);
	if (n > cap)
		n = cap;
	for (size_t i = 0; i < n; i++)
		ptrs[i] = names[i];
	qsort(ptrs, n, sizeof(ptrs[0]), cmp_str);
	return (int)n;
}

/* ---------- identity value normalization ---------- */

static bool all_hex(const char *s)
{
	for (; *s; s++)
		if (!isxdigit((unsigned char)*s))
			return false;
	return true;
}

static void hex_encode_padded(const char *input, size_t len, size_t width, char *out)
{
	static const char digits[] = "0123456789abcdef";
	size_t o = 0;

	for (size_t i = 0; i < len; i++) {
		out[o++] = digits[((unsigned char)input[i] >> 4) & 0xf];
		out[o++] = digits[(unsigned char)input[i] & 0xf];
	}
	for (size_t i = len; i < width; i++) {
		out[o++] = '0';
		out[o++] = '0';
	}
	out[o] = '\0';
}

static const char *normalize_serial(const char *input, char *out, size_t out_len)
{
	size_t len = strlen(input);
	const char *s = input;

	while (*s == ' ' || *s == '\t')
		s++;
	len = strlen(s);
	while (len && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		len--;

	if (len == 0 || (len == 7 && !strncasecmp(s, "default", 7))) {
		snprintf(out, out_len, "default");
		return NULL;
	}
	if (len == 16 && all_hex(s)) {
		for (size_t i = 0; i < 16; i++)
			out[i] = (char)tolower((unsigned char)s[i]);
		out[16] = '\0';
		return NULL;
	}
	if (len == 12) {
		bool vend_ok = true, ser_ok = true;

		for (int i = 0; i < 4; i++)
			if (!isalnum((unsigned char)s[i]))
				vend_ok = false;
		for (int i = 4; i < 12; i++)
			if (!isxdigit((unsigned char)s[i]))
				ser_ok = false;
		if (vend_ok && ser_ok) {
			/* 16 hex digits total: 4 vendor bytes + the 8-digit serial. */
			char vend[17];

			for (int i = 0; i < 4; i++) {
				static const char digits[] = "0123456789abcdef";

				vend[i * 2] = digits[((unsigned char)s[i] >> 4) & 0xf];
				vend[i * 2 + 1] = digits[(unsigned char)s[i] & 0xf];
			}
			for (int i = 4; i < 12; i++)
				vend[i + 4] = (char)tolower((unsigned char)s[i]);
			vend[16] = '\0';
			snprintf(out, out_len, "%s", vend);
			return NULL;
		}
	}
	return "serial must be 'default', 16 raw hex digits, or VEND followed by 8 hex digits";
}

static const char *normalize_registration_id(const char *input, char *out, size_t out_len)
{
	char text[128];
	size_t len = strlen(input);

	while (len && (input[len - 1] == ' ' || input[len - 1] == '\t'))
		len--;
	if (len == 0 || (len == 7 && !strncasecmp(input, "default", 7))) {
		snprintf(out, out_len, "default");
		return NULL;
	}
	if (len > 4 && !strncmp(input, "hex:", 4)) {
		const char *raw = input + 4;

		if (strlen(raw) != 72 || !all_hex(raw))
			return "hex Registration-ID must contain exactly 72 hex digits";
		for (int i = 0; i < 72; i++)
			out[i] = (char)tolower((unsigned char)raw[i]);
		out[72] = '\0';
		return NULL;
	}
	if (len == 72 && all_hex(input)) {
		for (int i = 0; i < 72; i++)
			out[i] = (char)tolower((unsigned char)input[i]);
		out[72] = '\0';
		return NULL;
	}
	{
		const char *t = (len > 5 && !strncmp(input, "text:", 5)) ? input + 5 : input;
		size_t tlen = strlen(t);

		if (tlen > 36)
			return "text Registration-ID must not exceed 36 UTF-8 bytes";
		memcpy(text, t, tlen);
		text[tlen] = '\0';
		hex_encode_padded(text, tlen, 36, out);
	}
	(void)out_len;
	return NULL;
}

/* ---------- sysfs subcommands ---------- */

static int show_identity(const struct pon_device *dev)
{
	char path[400], value[256];

	snprintf(path, sizeof(path), "%s/serial_number", dev->xpon_path);
	if (read_trimmed(path, value, sizeof(value)) < 0)
		return -1;
	printf("serial_number: %s\n", value);
	snprintf(path, sizeof(path), "%s/active_serial_number", dev->xpon_path);
	if (read_trimmed(path, value, sizeof(value)) < 0)
		return -1;
	printf("active_serial_number: %s\n", value);
	snprintf(path, sizeof(path), "%s/registration_id", dev->xpon_path);
	if (read_trimmed(path, value, sizeof(value)) < 0)
		return -1;
	printf("registration_id: %s\n", value);
	return 0;
}

static int set_identity(const struct pon_device *dev, char **args, int n_args)
{
	char serial[80], registration[160], path[400];
	bool have_serial = false, have_registration = false;

	if (n_args % 2)
		return fail("%s requires a value", args[n_args - 1]) ? -1 : -1;
	for (int i = 0; i < n_args; i += 2) {
		if (!strcmp(args[i], "--serial")) {
			const char *e = normalize_serial(args[i + 1], serial, sizeof(serial));

			if (e)
				return fail("%s", e) ? -1 : -1;
			have_serial = true;
		} else if (!strcmp(args[i], "--registration-id")) {
			const char *e = normalize_registration_id(args[i + 1], registration,
								  sizeof(registration));

			if (e)
				return fail("%s", e) ? -1 : -1;
			have_registration = true;
		} else {
			return fail("unknown argument '%s'", args[i]) ? -1 : -1;
		}
	}
	if (!have_serial && !have_registration)
		return fail("identity set requires --serial or --registration-id") ? -1 : -1;

	if (have_serial) {
		snprintf(path, sizeof(path), "%s/serial_number", dev->xpon_path);
		if (write_value(path, serial) < 0)
			return -1;
	}
	if (have_registration) {
		snprintf(path, sizeof(path), "%s/registration_id", dev->xpon_path);
		if (write_value(path, registration) < 0)
			return -1;
	}
	return show_identity(dev);
}

/* ---------- netlink ---------- */

struct line_state {
	uint8_t configured;	/* 0 = unknown */
	uint8_t active;		/* 0 = none */
};

static const char *line_mode_name(uint8_t id)
{
	switch (id) {
	case 1: return "gpon";
	case 2: return "xgpon";
	case 3: return "xgspon";
	case 4: return "epon-1g";
	case 5: return "epon-10g-1g";
	case 6: return "epon-10g-10g";
	default: return NULL;
	}
}

static int line_mode_id(const char *name)
{
	if (!strcmp(name, "gpon")) return 1;
	if (!strcmp(name, "xgpon")) return 2;
	if (!strcmp(name, "xgspon")) return 3;
	if (!strcmp(name, "epon-1g")) return 4;
	if (!strcmp(name, "epon-10g-1g")) return 5;
	if (!strcmp(name, "epon-10g-10g")) return 6;
	return 0;
}

static uint32_t netdev_ifindex(const struct pon_device *dev)
{
	char path[400], value[32];

	snprintf(path, sizeof(path), "%s/ifindex", dev->net_path);
	if (read_trimmed(path, value, sizeof(value)) < 0)
		return 0;
	return (uint32_t)strtoul(value, NULL, 10);
}

static int nl_open(void)
{
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
	struct sockaddr_nl addr;

	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void append_attr(void *buf, size_t *len, uint16_t type, const void *data, size_t data_len)
{
	struct nlattr *a = (struct nlattr *)((char *)buf + *len);
	size_t total = NLA_HDRLEN + data_len;
	size_t aligned = (total + 3) & ~3u;

	a->nla_type = type;
	a->nla_len = (uint16_t)total;
	if (data_len)
		memcpy((char *)a + NLA_HDRLEN, data, data_len);
	if (aligned > total)
		memset((char *)a + total, 0, aligned - total);
	*len += aligned;
}

/* Send one request; on success returns the generic-netlink payload length and
 * points *payload at it (inside resp). */
static int nl_request(int fd, uint16_t family, uint8_t cmd, uint8_t version, bool ack,
		      const struct nlattr *attrs, size_t attrs_len,
		      char *resp, size_t resp_len, char **payload)
{
	char req[1024];
	struct nlmsghdr *nlh = (struct nlmsghdr *)req;
	struct genlmsghdr *gh;
	struct sockaddr_nl kernel;
	size_t req_len;
	ssize_t n;

	memset(req, 0, sizeof(req));
	nlh->nlmsg_type = family;
	nlh->nlmsg_flags = NLM_F_REQUEST | (ack ? NLM_F_ACK : 0);
	nlh->nlmsg_seq = 1;
	gh = (struct genlmsghdr *)(req + NLMSG_HDRLEN);
	gh->cmd = cmd;
	gh->version = version;
	req_len = NLMSG_HDRLEN + GENL_HDRLEN;
	if (attrs && attrs_len) {
		memcpy(req + req_len, attrs, attrs_len);
		req_len += attrs_len;
	}
	nlh->nlmsg_len = (uint32_t)req_len;

	memset(&kernel, 0, sizeof(kernel));
	kernel.nl_family = AF_NETLINK;
	if (sendto(fd, req, req_len, 0, (struct sockaddr *)&kernel, sizeof(kernel)) < 0)
		return -1;

	n = recv(fd, resp, resp_len, 0);
	if (n < 0)
		return -1;

	for (struct nlmsghdr *h = (struct nlmsghdr *)resp; NLMSG_OK(h, (unsigned)n);
	     h = NLMSG_NEXT(h, n)) {
		if (h->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(h);

			if (err->error) {
				errno = -err->error;
				return -1;
			}
			return 0;	/* ack-only reply */
		}
		if (h->nlmsg_type == family) {
			*payload = (char *)NLMSG_DATA(h) + GENL_HDRLEN;
			return (int)(h->nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN);
		}
	}
	return -1;
}

static int nl_family_id(int fd, const char *name, uint16_t *out)
{
	char attrs[256], resp[8192];
	size_t len = 0;
	char *payload;
	int n;

	append_attr(attrs, &len, CTRL_ATTR_FAMILY_NAME, name, strlen(name) + 1);
	n = nl_request(fd, GENL_ID_CTRL, CTRL_CMD_GETFAMILY, 1, false, (struct nlattr *)attrs,
		       len, resp, sizeof(resp), &payload);
	if (n < 0)
		return -1;
	for (struct nlattr *a = (struct nlattr *)payload; n >= (int)NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && (int)a->nla_len <= n;
	     n -= NLA_ALIGN(a->nla_len), a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len))) {
		if ((a->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID) {
			*out = *(uint16_t *)((char *)a + NLA_HDRLEN);
			return 0;
		}
	}
	fail("generic netlink family %s was not found", name);
	return -1;
}

static int xpon_nl_open(int *fd, uint16_t *family)
{
	*fd = nl_open();
	if (*fd < 0)
		return -1;
	if (nl_family_id(*fd, XPON_FAMILY_NAME, family) < 0) {
		close(*fd);
		*fd = -1;
		return -1;
	}
	return 0;
}

static int get_line(uint32_t ifindex, struct line_state *out)
{
	int fd;
	uint16_t family;
	char attrs[64], resp[8192];
	size_t len = 0;
	char *payload;
	int n;

	memset(out, 0, sizeof(*out));
	if (xpon_nl_open(&fd, &family) < 0)
		return -1;
	append_attr(attrs, &len, XPON_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
	n = nl_request(fd, family, XPON_CMD_GET_LINE, XPON_VERSION, false,
		       (struct nlattr *)attrs, len, resp, sizeof(resp), &payload);
	close(fd);
	if (n < 0)
		return -1;
	for (struct nlattr *a = (struct nlattr *)payload; n >= (int)NLA_HDRLEN && a->nla_len >= NLA_HDRLEN && (int)a->nla_len <= n;
	     n -= NLA_ALIGN(a->nla_len), a = (struct nlattr *)((char *)a + NLA_ALIGN(a->nla_len))) {
		const uint8_t *v = (const uint8_t *)a + NLA_HDRLEN;
		size_t vlen = a->nla_len - NLA_HDRLEN;
		uint16_t type = a->nla_type & NLA_TYPE_MASK;

		if (vlen != 1)
			continue;
		if (type == XPON_ATTR_MODE)
			out->configured = v[0];
		else if (type == XPON_ATTR_ACTIVE_MODE)
			out->active = v[0];
	}
	if (!out->configured) {
		fail("xpon reply has no valid configured mode");
		return -1;
	}
	return 0;
}

static int set_line(uint32_t ifindex, uint8_t mode)
{
	int fd;
	uint16_t family;
	char attrs[64], resp[8192];
	size_t len = 0;
	char *payload;
	int rc;

	if (xpon_nl_open(&fd, &family) < 0)
		return -1;
	append_attr(attrs, &len, XPON_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
	append_attr(attrs, &len, XPON_ATTR_MODE, &mode, 1);
	rc = nl_request(fd, family, XPON_CMD_SET_LINE, XPON_VERSION, true,
			(struct nlattr *)attrs, len, resp, sizeof(resp), &payload);
	close(fd);
	return rc < 0 ? -1 : 0;
}

/* ---------- status (typed snapshot) ---------- */

enum value_kind { V_TEXT, V_BOOL, V_INT, V_UNSIGNED, V_NUMBER };

struct value {
	char name[64];
	enum value_kind kind;
	char text[160];
	bool b;
	long long i;
	unsigned long long u;
	double d;
};

struct group {
	char name[32];
	struct value fields[MAX_FIELDS];
	size_t n;
};

enum status_kind { SK_STRING, SK_BOOL, SK_U32, SK_S32, SK_U64 };

struct field_def {
	const char *name;
	enum status_kind kind;
};

static const struct field_def line_fields[] = {
	{ "configured_mode", SK_STRING }, { "active_mode", SK_STRING },
	{ "mode_pending", SK_BOOL }, { "lifecycle", SK_STRING },
	{ "last_start_error", SK_S32 }, { "optical_signal", SK_BOOL },
	{ "rx_active", SK_BOOL }, { "phy_ready", SK_BOOL },
	{ "xgtc_sync", SK_STRING }, { "pcs_sync", SK_BOOL },
	{ "pcs_profile_valid", SK_BOOL }, { "pcs_profile", SK_STRING },
};
static const struct field_def frontend_fields[] = {
	{ "error", SK_S32 }, { "calibration", SK_STRING }, { "tx_gate_enabled", SK_BOOL },
	{ "temperature_8472", SK_S32 }, { "voltage_8472", SK_U32 },
	{ "tx_bias_8472", SK_U32 }, { "tx_power_8472", SK_U32 }, { "rx_power_8472", SK_U32 },
};
static const struct field_def registration_fields[] = {
	{ "onu_state", SK_STRING }, { "onu_id_valid", SK_BOOL }, { "onu_id", SK_U32 },
	{ "mpcp_state", SK_STRING }, { "llid_valid", SK_BOOL }, { "llid", SK_U32 },
	{ "upstream_tx_armed", SK_BOOL }, { "serial_configured", SK_BOOL },
	{ "registration_id_configured", SK_BOOL },
};
static const struct field_def datapath_fields[] = {
	{ "data_path_configured", SK_BOOL }, { "service_ready", SK_BOOL },
};
static const struct field_def counter_fields[] = {
	{ "rx_start_count", SK_U32 }, { "xgtc_rx", SK_U64 }, { "upstream_bursts_tx", SK_U64 },
	{ "ploamd_rx", SK_U64 }, { "ploamu_tx", SK_U64 }, { "xgem_rx", SK_U64 },
	{ "xgem_tx", SK_U64 }, { "discovery_gates", SK_U32 }, { "register_requests", SK_U32 },
	{ "register_messages", SK_U32 }, { "register_acks", SK_U32 },
	{ "register_nacks", SK_U32 }, { "mpcp_timeouts", SK_U32 }, { "mac_errors", SK_U32 },
	{ "sync_losses", SK_U32 }, { "recoveries", SK_U32 },
	{ "full_reinitializations", SK_U32 },
};

static const struct {
	uint16_t id;
	const char *name;
	const struct field_def *fields;
	size_t n_fields;
} status_groups[] = {
	{ 4, "line", line_fields, sizeof(line_fields) / sizeof(line_fields[0]) },
	{ 5, "frontend", frontend_fields, sizeof(frontend_fields) / sizeof(frontend_fields[0]) },
	{ 6, "registration", registration_fields, sizeof(registration_fields) / sizeof(registration_fields[0]) },
	{ 7, "datapath", datapath_fields, sizeof(datapath_fields) / sizeof(datapath_fields[0]) },
	{ 8, "counters", counter_fields, sizeof(counter_fields) / sizeof(counter_fields[0]) },
};

static int cmp_value(const void *a, const void *b)
{
	return strcmp(((const struct value *)a)->name, ((const struct value *)b)->name);
}

static int cmp_group(const void *a, const void *b)
{
	return strcmp(((const struct group *)a)->name, ((const struct group *)b)->name);
}

/* status_value(): native-endian typed decode, exactly like the Rust original. */
static int status_value(enum status_kind kind, const uint8_t *data, size_t len,
			struct value *out)
{
	switch (kind) {
	case SK_STRING:
		if (len < 1 || data[len - 1] != 0)
			return -1;
		{
			size_t n = len - 1;

			if (n >= sizeof(out->text))
				n = sizeof(out->text) - 1;
			memcpy(out->text, data, n);
			out->text[n] = '\0';
		}
		out->kind = V_TEXT;
		return 0;
	case SK_BOOL:
		if (len != 1 || data[0] > 1)
			return -1;
		out->kind = V_BOOL;
		out->b = data[0] != 0;
		return 0;
	case SK_U32:
		if (len != 4)
			return -1;
		{
			uint32_t v;

			memcpy(&v, data, 4);
			out->kind = V_INT;
			out->i = (long long)v;
		}
		return 0;
	case SK_S32:
		if (len != 4)
			return -1;
		{
			int32_t v;

			memcpy(&v, data, 4);
			out->kind = V_INT;
			out->i = v;
		}
		return 0;
	case SK_U64:
		if (len != 8)
			return -1;
		{
			uint64_t v;

			memcpy(&v, data, 8);
			out->kind = V_UNSIGNED;
			out->u = v;
		}
		return 0;
	}
	return -1;
}


/* convert_optics(): SFF-8472 raw integers become display units. Copied from
 * status.rs; only Integer-typed fields are converted (others stay raw). */
static void convert_optics(struct group *groups, size_t n_groups)
{
	for (size_t i = 0; i < n_groups; i++) {
		struct group *fr = &groups[i];

		if (strcmp(fr->name, "frontend"))
			continue;
		for (size_t j = 0; j < fr->n; j++) {
			struct value *v = &fr->fields[j];
			const char *raw = NULL, *name = NULL;
			double factor = 0.0;
			bool power = false;

			if (v->kind != V_INT)
				continue;
			if (!strcmp(v->name, "temperature_8472")) {
				raw = "temperature_8472"; name = "temperature_celsius"; factor = 1.0 / 256.0;
			} else if (!strcmp(v->name, "voltage_8472")) {
				raw = "voltage_8472"; name = "voltage_volts"; factor = 0.0001;
			} else if (!strcmp(v->name, "tx_bias_8472")) {
				raw = "tx_bias_8472"; name = "tx_bias_ma"; factor = 0.002;
			} else if (!strcmp(v->name, "tx_power_8472")) {
				raw = "tx_power_8472"; name = "tx_power_dbm"; power = true;
			} else if (!strcmp(v->name, "rx_power_8472")) {
				raw = "rx_power_8472"; name = "rx_power_dbm"; power = true;
			}
			if (!raw)
				continue;
			snprintf(v->name, sizeof(v->name), "%s", name);
			v->kind = V_NUMBER;
			if (power)
				v->d = v->i == 0 ? -40.0 : 10.0 * log10((double)v->i) - 40.0;
			else
				v->d = (double)v->i * factor;
		}
		qsort(fr->fields, fr->n, sizeof(fr->fields[0]), cmp_value);
	}
}

static int get_status(uint32_t ifindex, struct group *groups, size_t *n_groups)
{
	int fd;
	uint16_t family;
	char attrs[64], resp[65536];
	size_t len = 0;
	char *payload;
	int n;

	if (xpon_nl_open(&fd, &family) < 0)
		return -1;
	append_attr(attrs, &len, XPON_ATTR_IFINDEX, &ifindex, sizeof(ifindex));
	n = nl_request(fd, family, XPON_CMD_GET_STATUS, XPON_VERSION, false,
		       (struct nlattr *)attrs, len, resp, sizeof(resp), &payload);
	close(fd);
	if (n < 0)
		return -1;

	*n_groups = 0;
	for (struct nlattr *g = (struct nlattr *)payload;
	     n >= (int)NLA_HDRLEN && g->nla_len >= NLA_HDRLEN && (int)g->nla_len <= n;
	     n -= NLA_ALIGN(g->nla_len), g = (struct nlattr *)((char *)g + NLA_ALIGN(g->nla_len))) {
		uint16_t gid = g->nla_type & NLA_TYPE_MASK;
		const struct field_def *fields = NULL;
		const char *gname = NULL;
		size_t n_fields = 0;
		int gn = (int)g->nla_len - NLA_HDRLEN;
		char *gp = (char *)g + NLA_HDRLEN;
		struct group *out;

		for (size_t i = 0; i < sizeof(status_groups) / sizeof(status_groups[0]); i++)
			if (status_groups[i].id == gid) {
				gname = status_groups[i].name;
				fields = status_groups[i].fields;
				n_fields = status_groups[i].n_fields;
			}
		if (!gname)
			continue;	/* unknown group: skipped like the original */
		if (*n_groups >= MAX_GROUPS)
			break;
		out = &groups[*n_groups];
		memset(out, 0, sizeof(*out));
		snprintf(out->name, sizeof(out->name), "%s", gname);

		for (struct nlattr *f = (struct nlattr *)gp;
		     gn >= (int)NLA_HDRLEN && f->nla_len >= NLA_HDRLEN && (int)f->nla_len <= gn;
		     gn -= NLA_ALIGN(f->nla_len), f = (struct nlattr *)((char *)f + NLA_ALIGN(f->nla_len))) {
			uint16_t fid = f->nla_type & NLA_TYPE_MASK;
			const uint8_t *data = (const uint8_t *)f + NLA_HDRLEN;
			size_t dlen = f->nla_len - NLA_HDRLEN;

			if (!fid || (size_t)(fid - 1) >= n_fields)
				continue;
			if (out->n >= MAX_FIELDS)
				break;
			memset(&out->fields[out->n], 0, sizeof(out->fields[out->n]));
			snprintf(out->fields[out->n].name, sizeof(out->fields[out->n].name),
				 "%s", fields[fid - 1].name);
			if (status_value(fields[fid - 1].kind, data, dlen,
					 &out->fields[out->n]) == 0)
				out->n++;
		}
		qsort(out->fields, out->n, sizeof(out->fields[0]), cmp_value);
		(*n_groups)++;
	}
	qsort(groups, *n_groups, sizeof(groups[0]), cmp_group);
	convert_optics(groups, *n_groups);
	return 0;
}

/* ---------- status rendering ---------- */

static void json_quote(const char *text, char *out, size_t out_len)
{
	size_t o = 0;

	if (out_len < 3)
		return;
	out[o++] = '"';
	for (const char *p = text; *p && o + 8 < out_len; p++) {
		unsigned char c = (unsigned char)*p;

		switch (c) {
		case '"': out[o++] = '\\'; out[o++] = '"'; break;
		case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
		case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
		case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
		case '\t': out[o++] = '\\'; out[o++] = 't'; break;
		default:
			if (c < 0x20)
				o += (size_t)snprintf(out + o, out_len - o, "\\u%04x", c);
			else
				out[o++] = (char)c;
		}
	}
	out[o++] = '"';
	out[o] = '\0';
}

static void value_json(const struct value *v, char *out, size_t out_len)
{
	char q[400];

	switch (v->kind) {
	case V_TEXT:
		json_quote(v->text, out, out_len);
		break;
	case V_BOOL:
		snprintf(out, out_len, "%s", v->b ? "true" : "false");
		break;
	case V_INT:
		snprintf(out, out_len, "%lld", v->i);
		break;
	case V_UNSIGNED:
		/* Unsigned values are quoted in JSON (u64 precision). */
		snprintf(q, sizeof(q), "%llu", v->u);
		json_quote(q, out, out_len);
		break;
	case V_NUMBER:
		snprintf(out, out_len, "%.4f", v->d);
		break;
	}
}

static void value_text(const struct value *v, char *out, size_t out_len)
{
	switch (v->kind) {
	case V_TEXT: snprintf(out, out_len, "%s", v->text); break;
	case V_BOOL: snprintf(out, out_len, "%s", v->b ? "1" : "0"); break;
	case V_INT: snprintf(out, out_len, "%lld", v->i); break;
	case V_UNSIGNED: snprintf(out, out_len, "%llu", v->u); break;
	case V_NUMBER: snprintf(out, out_len, "%.2f", v->d); break;
	}
}

static void print_status_json(const struct group *groups, size_t n_groups)
{
	char name[128], val[512];
	size_t o = 0;

	printf("{\"schema_version\":1");
	for (size_t i = 0; i < n_groups; i++) {
		json_quote(groups[i].name, name, sizeof(name));
		printf(",%s:{", name);
		for (size_t j = 0; j < groups[i].n; j++) {
			json_quote(groups[i].fields[j].name, name, sizeof(name));
			value_json(&groups[i].fields[j], val, sizeof(val));
			printf("%s%s:%s", j ? "," : "", name, val);
		}
		printf("}");
		(void)o;
	}
	printf("}\n");
}

static void print_status_human(const struct group *groups, size_t n_groups)
{
	static const char *order[] = { "line", "frontend", "registration", "datapath",
				       "counters" };
	bool first = true;

	for (size_t k = 0; k < sizeof(order) / sizeof(order[0]); k++) {
		for (size_t i = 0; i < n_groups; i++) {
			char val[512];

			if (strcmp(groups[i].name, order[k]))
				continue;
			if (!first)
				putchar('\n');
			first = false;
			printf("[%s]\n", groups[i].name);
			for (size_t j = 0; j < groups[i].n; j++) {
				value_text(&groups[i].fields[j], val, sizeof(val));
				printf("%s: %s\n", groups[i].fields[j].name, val);
			}
		}
	}
}

/* ---------- mode ---------- */

static int show_mode(const struct pon_device *dev)
{
	struct line_state st;
	char controls[MAX_CONTROLS][64];
	int n;
	uint32_t idx = netdev_ifindex(dev);

	if (!idx)
		return -1;
	if (get_line(idx, &st) < 0)
		return -1;
	printf("configured=%s\n", line_mode_name(st.configured));
	printf("active=%s\n", st.active ? line_mode_name(st.active) : "none");
	n = associated_controls(dev, controls, MAX_CONTROLS);
	printf("controls=");
	if (n <= 0) {
		printf("none\n");
	} else {
		for (int i = 0; i < n; i++)
			printf("%s%s", i ? "," : "", controls[i]);
		printf("\n");
	}
	return 0;
}

/* ---------- CLI ---------- */

static void print_help(void)
{
	printf("ponctl %s\n%s\n\n"
	       "Usage:\n"
	       "  ponctl list\n"
	       "  ponctl [--device ponX] status [--json]\n"
	       "  ponctl [--device ponX] control|identity\n"
	       "  ponctl [--device ponX] mode [show]\n"
	       "  ponctl [--device ponX] mode set MODE\n"
	       "  ponctl [--device ponX] identity set [--serial SN] [--registration-id ID]\n"
	       "  ponctl [--device ponX] data-path [show]\n",
	       VERSION, PROJECT_URL);
}

#ifdef PONCTL_MAIN
int main(int argc, char **argv)
{
	char **args = argv + 1;
	int n_args = argc - 1;
	const char *requested = NULL;
	const char *command;
	struct pon_device dev;
	int rc = 0;

	if (n_args > 0 && (!strcmp(args[0], "-d") || !strcmp(args[0], "--device"))) {
		if (n_args < 2) {
			fprintf(stderr, "ponctl: --device requires a netdev name\n");
			return 1;
		}
		requested = args[1];
		args += 2;
		n_args -= 2;
	}
	command = n_args > 0 ? args[0] : "help";

	if (!strcmp(command, "help") || !strcmp(command, "--help") || !strcmp(command, "-h")) {
		print_help();
		return 0;
	}
	if (!strcmp(command, "--version") || !strcmp(command, "-V")) {
		printf("ponctl %s\n%s\n", VERSION, PROJECT_URL);
		return 0;
	}
	if (!strcmp(command, "list")) {
		struct pon_device devs[MAX_DEVS];
		int n;

		if (requested || n_args != 1) {
			fprintf(stderr, "ponctl: list does not accept --device or additional arguments\n");
			return 1;
		}
		n = discover_pon_devices(devs, MAX_DEVS);
		if (n <= 0) {
			fprintf(stderr, "ponctl: no PON device with an xpon binding was found\n");
			return 1;
		}
		for (int i = 0; i < n; i++) {
			char controls[MAX_CONTROLS][64];
			int nc = associated_controls(&devs[i], controls, MAX_CONTROLS);
			char list[512] = "";
			size_t o = 0;

			for (int j = 0; j < nc; j++)
				o += (size_t)snprintf(list + o, sizeof(list) - o, "%s%s",
						      j ? "," : "", controls[j]);
			printf("device=%s controls=%s xpon=%s\n", devs[i].name,
			       nc > 0 ? list : "none", devs[i].xpon_path);
		}
		return 0;
	}

	if (select_pon_device(requested, &dev) < 0) {
		fprintf(stderr, "ponctl: %s\n", err_buf);
		return 1;
	}

	if (!strcmp(command, "status") &&
	    (n_args == 1 || (n_args == 2 && !strcmp(args[1], "--json")))) {
		struct group groups[MAX_GROUPS];
		size_t n_groups = 0;
		uint32_t idx = netdev_ifindex(&dev);

		if (!idx) {
			fprintf(stderr, "ponctl: cannot read %s/ifindex\n", dev.net_path);
			return 1;
		}
		if (get_status(idx, groups, &n_groups) < 0) {
			fprintf(stderr, "ponctl: %s\n", err_buf[0] ? err_buf : strerror(errno));
			return 1;
		}
		if (n_args == 2)
			print_status_json(groups, n_groups);
		else
			print_status_human(groups, n_groups);
		return 0;
	}
	if (!strcmp(command, "control") && n_args == 1) {
		char controls[MAX_CONTROLS][64];
		int n = associated_controls(&dev, controls, MAX_CONTROLS);

		for (int i = 0; i < n; i++)
			printf("%s\n", controls[i]);
		return 0;
	}
	if (!strcmp(command, "mode") &&
	    (n_args == 1 || (n_args == 2 && !strcmp(args[1], "show")))) {
		if (n_args > 2) {
			fprintf(stderr, "ponctl: mode show accepts no additional arguments\n");
			return 1;
		}
		if (show_mode(&dev) < 0) {
			fprintf(stderr, "ponctl: %s\n", err_buf[0] ? err_buf : strerror(errno));
			return 1;
		}
		return 0;
	}
	if (!strcmp(command, "mode") && n_args >= 2 && !strcmp(args[1], "set")) {
		int mode;

		if (n_args != 3) {
			fprintf(stderr, "ponctl: mode set requires one mode name\n");
			return 1;
		}
		mode = line_mode_id(args[2]);
		if (!mode) {
			fprintf(stderr, "ponctl: mode must be gpon, xgpon, xgspon, epon-1g, "
					"epon-10g-1g, or epon-10g-10g\n");
			return 1;
		}
		if (set_line(netdev_ifindex(&dev), (uint8_t)mode) < 0) {
			fprintf(stderr, "ponctl: %s\n", err_buf[0] ? err_buf : strerror(errno));
			return 1;
		}
		if (show_mode(&dev) < 0)
			return 1;
		return 0;
	}
	if (!strcmp(command, "identity") &&
	    (n_args == 1 || (n_args == 2 && !strcmp(args[1], "show")))) {
		if (n_args > 2) {
			fprintf(stderr, "ponctl: identity show accepts no additional arguments\n");
			return 1;
		}
		rc = show_identity(&dev);
	} else if (!strcmp(command, "identity") && n_args >= 2 && !strcmp(args[1], "set")) {
		rc = set_identity(&dev, args + 2, n_args - 2);
	} else if (!strcmp(command, "data-path") &&
		   (n_args == 1 || (n_args == 2 && !strcmp(args[1], "show")))) {
		char path[400], buf[4096];
		FILE *f;

		if (n_args > 2) {
			fprintf(stderr, "ponctl: data-path show accepts no additional arguments\n");
			return 1;
		}
		snprintf(path, sizeof(path), "%s/data_path", dev.xpon_path);
		f = fopen(path, "r");
		if (!f) {
			fprintf(stderr, "ponctl: %s\n", strerror(errno));
			return 1;
		}
		while (fgets(buf, sizeof(buf), f))
			fputs(buf, stdout);
		fclose(f);
		return 0;
	} else {
		fprintf(stderr, "ponctl: unknown or malformed command '%s'; run 'ponctl --help'\n",
			command);
		return 1;
	}

	if (rc < 0) {
		fprintf(stderr, "ponctl: %s\n", err_buf[0] ? err_buf : strerror(errno));
		return 1;
	}
	return 0;
}
#endif /* PONCTL_MAIN */
