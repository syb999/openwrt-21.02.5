/* SPDX-License-Identifier: GPL-2.0-only */
#include "runtime.h"
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_trimmed(const char *path, char *out, size_t out_len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	if (!f)
		return -errno;
	n = fread(out, 1, out_len - 1, f);
	fclose(f);
	out[n] = '\0';
	while (n && (out[n - 1] == '\n' || out[n - 1] == ' ' || out[n - 1] == '\t'))
		out[--n] = '\0';
	return n ? 0 : -EIO;
}

int pond_parent_interface(const char *interface, char *out, size_t out_len)
{
	char path[256], iflink[64];
	DIR *d;
	struct dirent *e;
	int rc;

	snprintf(path, sizeof(path), "/sys/class/net/%s/iflink", interface);
	rc = read_trimmed(path, iflink, sizeof(iflink));
	if (rc)
		return rc;

	d = opendir("/sys/class/net");
	if (!d)
		return -errno;
	while ((e = readdir(d))) {
		char cand[256], idx[64];

		if (e->d_name[0] == '.')
			continue;
		snprintf(cand, sizeof(cand), "/sys/class/net/%s/ifindex", e->d_name);
		if (read_trimmed(cand, idx, sizeof(idx)))
			continue;
		if (!strcmp(idx, iflink)) {
			snprintf(out, out_len, "%s", e->d_name);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -ENOENT;
}

int pond_parent_mac(const char *interface, unsigned char mac[6])
{
	char parent[64], path[256], addr[64];
	unsigned int m[6];
	int rc;

	rc = pond_parent_interface(interface, parent, sizeof(parent));
	if (rc)
		return rc;
	snprintf(path, sizeof(path), "/sys/class/net/%s/address", parent);
	rc = read_trimmed(path, addr, sizeof(addr));
	if (rc)
		return rc;
	if (sscanf(addr, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6)
		return -EINVAL;
	for (int i = 0; i < 6; i++)
		mac[i] = (unsigned char)m[i];
	return 0;
}

int pond_parent_xpon_attr(const char *interface, const char *attribute,
			  char *out, size_t out_len)
{
	char parent[64], path[256];
	int rc;

	rc = pond_parent_interface(interface, parent, sizeof(parent));
	if (rc)
		return rc;
	if (snprintf(path, sizeof(path), "/sys/class/net/%s/xpon/%s", parent,
		     attribute) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	if (access(path, F_OK) != 0)
		return -ENOENT;
	snprintf(out, out_len, "%s", path);
	return 0;
}
