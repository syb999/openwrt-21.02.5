/* zip.c - minimal ZIP reader (store + deflate) */
#include "x2s.h"
#include <zlib.h>

static uint16_t rd16(const unsigned char *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

int zip_open(Zip *z, unsigned char *data, size_t len, char *err, size_t errlen)
{
	size_t i;
	long eocd = -1;

	memset(z, 0, sizeof(*z));
	z->data = data;
	z->len = len;

	if (len < 22) {
		snprintf(err, errlen, "not a zip file (too short)");
		return -1;
	}
	for (i = len - 22; i + 4 <= len; i--) {
		if (rd32(data + i) == 0x06054b50u) {
			eocd = (long)i;
			break;
		}
		if (i == 0)
			break;
	}
	if (eocd < 0) {
		snprintf(err, errlen, "zip: end of central directory not found");
		return -1;
	}
	{
		uint16_t count = rd16(data + eocd + 10);
		uint32_t cdoff = rd32(data + eocd + 16);
		size_t p = cdoff;
		int k;

		z->e = xmalloc(sizeof(ZEntry) * (count ? count : 1));
		z->n = 0;
		for (k = 0; k < count; k++) {
			uint16_t namelen, extralen, commentlen;
			ZEntry *e;

			if (p + 46 > len || rd32(data + p) != 0x02014b50u)
				break;
			namelen = rd16(data + p + 28);
			extralen = rd16(data + p + 30);
			commentlen = rd16(data + p + 32);
			if (p + 46 + namelen > len)
				break;
			e = &z->e[z->n];
			e->method = rd16(data + p + 10);
			e->csize = rd32(data + p + 20);
			e->usize = rd32(data + p + 24);
			e->off = rd32(data + p + 42);
			e->name = xstrndup((const char *)data + p + 46, namelen);
			z->n++;
			p += 46 + namelen + extralen + commentlen;
		}
	}
	if (z->n <= 0) {
		snprintf(err, errlen, "zip: no entries");
		return -1;
	}
	return 0;
}

void zip_close(Zip *z)
{
	int i;

	for (i = 0; i < z->n; i++)
		free(z->e[i].name);
	free(z->e);
	free(z->data);
	memset(z, 0, sizeof(*z));
}

static ZEntry *zip_find(Zip *z, const char *name)
{
	int i;

	for (i = 0; i < z->n; i++)
		if (strcmp(z->e[i].name, name) == 0)
			return &z->e[i];
	return NULL;
}

int zip_extract_soft(Zip *z, const char *name, unsigned char **out, size_t *outlen)
{
	ZEntry *e = zip_find(z, name);
	uint16_t namelen, extralen;
	size_t off;
	unsigned char *dst;

	*out = NULL;
	*outlen = 0;
	if (!e)
		return -1;
	if (e->usize > X2S_MAX_INFLATE || e->csize > X2S_MAX_INFLATE)
		return -1;
	off = e->off;
	if (off + 30 > z->len || rd32(z->data + off) != 0x04034b50u)
		return -1;
	namelen = rd16(z->data + off + 26);
	extralen = rd16(z->data + off + 28);
	off += 30 + namelen + extralen;
	if (off + e->csize > z->len)
		return -1;

	if (e->method == 0) {
		dst = xmalloc(e->usize + 1);
		memcpy(dst, z->data + off, e->csize < e->usize ? e->csize : e->usize);
		dst[e->usize] = 0;
		*out = dst;
		*outlen = e->usize;
		return 0;
	}
	if (e->method != 8)
		return -1;
	{
		z_stream s;
		int rc;
		size_t cap = e->usize ? e->usize : (e->csize * 4 + 1024);
		size_t total = 0;

		memset(&s, 0, sizeof(s));
		if (inflateInit2(&s, -15) != Z_OK)
			return -1;
		dst = xmalloc(cap + 1);
		s.next_in = z->data + off;
		s.avail_in = e->csize;
		for (;;) {
			s.next_out = dst + total;
			s.avail_out = (uInt)(cap - total);
			rc = inflate(&s, Z_NO_FLUSH);
			total = (size_t)(s.next_out - dst);
			if (rc == Z_STREAM_END)
				break;
			if (rc != Z_OK && rc != Z_BUF_ERROR) {
				inflateEnd(&s);
				free(dst);
				return -1;
			}
			if (s.avail_out == 0) {
				if (total > X2S_MAX_INFLATE) {
					inflateEnd(&s);
					free(dst);
					return -1;
				}
				cap = cap * 2 + 1024;
				dst = xrealloc(dst, cap + 1);
				continue;
			}
			if (s.avail_in == 0) {
				inflateEnd(&s);
				free(dst);
				return -1;
			}
		}
		inflateEnd(&s);
		dst[total] = 0;
		*out = dst;
		*outlen = total;
	}
	return 0;
}

int zip_extract(Zip *z, const char *name, unsigned char **out, size_t *outlen)
{
	if (zip_extract_soft(z, name, out, outlen) != 0) {
		*out = NULL;
		*outlen = 0;
		return -1;
	}
	return 0;
}
