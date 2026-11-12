/* util.c - buffers, allocation, text helpers */
#include "x2s.h"
#include <ctype.h>
#include <errno.h>

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q) {
		fprintf(stderr, "out of memory\n");
		exit(1);
	}
	return q;
}

char *xstrdup(const char *s)
{
	size_t n;
	char *p;

	if (!s)
		return NULL;
	n = strlen(s);
	p = xmalloc(n + 1);
	memcpy(p, s, n + 1);
	return p;
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

void buf_init(Buf *b)
{
	b->p = NULL;
	b->n = 0;
	b->cap = 0;
}

void buf_free(Buf *b)
{
	free(b->p);
	buf_init(b);
}

void buf_reset(Buf *b)
{
	b->n = 0;
	if (b->p)
		b->p[0] = 0;
}

static void buf_grow(Buf *b, size_t need)
{
	if (b->n + need + 1 <= b->cap)
		return;
	if (!b->cap)
		b->cap = 256;
	while (b->cap < b->n + need + 1)
		b->cap *= 2;
	b->p = xrealloc(b->p, b->cap);
}

void buf_add(Buf *b, const void *data, size_t len)
{
	buf_grow(b, len);
	if (len)
		memcpy(b->p + b->n, data, len);
	b->n += len;
	b->p[b->n] = 0;
}

void buf_puts(Buf *b, const char *s)
{
	if (s)
		buf_add(b, s, strlen(s));
}

void buf_putc(Buf *b, int c)
{
	char ch = (char)c;
	buf_add(b, &ch, 1);
}

void buf_printf(Buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;
	char tmp[512];

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n < sizeof(tmp)) {
		buf_add(b, tmp, (size_t)n);
		return;
	}
	{
		char *big = xmalloc((size_t)n + 1);
		va_start(ap, fmt);
		vsnprintf(big, (size_t)n + 1, fmt, ap);
		va_end(ap);
		buf_add(b, big, (size_t)n);
		free(big);
	}
}

char *buf_detach(Buf *b)
{
	char *p = b->p;
	if (!p)
		p = xstrdup("");
	b->p = NULL;
	b->n = b->cap = 0;
	return p;
}

const char *buf_str(Buf *b)
{
	if (!b->p)
		return "";
	return b->p;
}

char *str_trim(char *s)
{
	char *e;

	if (!s)
		return s;
	while (*s && (unsigned char)*s <= ' ')
		s++;
	e = s + strlen(s);
	while (e > s && (unsigned char)e[-1] <= ' ')
		e--;
	*e = 0;
	return s;
}

int str_ieq(const char *a, const char *b)
{
	if (!a || !b)
		return 0;
	while (*a && *b) {
		int ca = tolower((unsigned char)*a);
		int cb = tolower((unsigned char)*b);
		if (ca != cb)
			return 0;
		a++;
		b++;
	}
	return *a == 0 && *b == 0;
}

int str_is_empty(const char *s)
{
	if (!s)
		return 1;
	while (*s) {
		if ((unsigned char)*s > ' ')
			return 0;
		s++;
	}
	return 1;
}

void json_esc_n(Buf *out, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"': buf_puts(out, "\\\""); break;
		case '\\': buf_puts(out, "\\\\"); break;
		case '\b': buf_puts(out, "\\b"); break;
		case '\f': buf_puts(out, "\\f"); break;
		case '\n': buf_puts(out, "\\n"); break;
		case '\r': buf_puts(out, "\\r"); break;
		case '\t': buf_puts(out, "\\t"); break;
		default:
			if (c < 0x20)
				buf_printf(out, "\\u%04x", c);
			else
				buf_putc(out, c);
		}
	}
}

void json_esc(Buf *out, const char *s)
{
	if (!s)
		return;
	json_esc_n(out, s, strlen(s));
}

void utf8_put(Buf *out, uint32_t cp)
{
	char t[4];

	if (cp < 0x80) {
		t[0] = (char)cp;
		buf_add(out, t, 1);
	} else if (cp < 0x800) {
		t[0] = (char)(0xC0 | (cp >> 6));
		t[1] = (char)(0x80 | (cp & 0x3F));
		buf_add(out, t, 2);
	} else if (cp < 0x10000) {
		t[0] = (char)(0xE0 | (cp >> 12));
		t[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		t[2] = (char)(0x80 | (cp & 0x3F));
		buf_add(out, t, 3);
	} else {
		t[0] = (char)(0xF0 | (cp >> 18));
		t[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		t[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		t[3] = (char)(0x80 | (cp & 0x3F));
		buf_add(out, t, 4);
	}
}

void utf16le_to_utf8(Buf *out, const uint8_t *s, size_t nchars)
{
	size_t i = 0;

	while (i < nchars) {
		uint32_t c = s[2 * i] | ((uint32_t)s[2 * i + 1] << 8);
		i++;
		if (c >= 0xD800 && c <= 0xDBFF && i < nchars) {
			uint32_t c2 = s[2 * i] | ((uint32_t)s[2 * i + 1] << 8);
			if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
				c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
				i++;
			}
		}
		if (c == 0)
			continue;
		utf8_put(out, c);
	}
}

void latin1_to_utf8(Buf *out, const uint8_t *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		utf8_put(out, s[i]);
}

int utf8_valid(const uint8_t *s, size_t n)
{
	size_t i = 0;
	int need = 0;
	uint32_t cp = 0;

	while (i < n) {
		uint8_t c = s[i];
		if (!need) {
			if (c < 0x80) {
				i++;
				continue;
			} else if ((c & 0xE0) == 0xC0) {
				need = 1;
				cp = c & 0x1F;
			} else if ((c & 0xF0) == 0xE0) {
				need = 2;
				cp = c & 0x0F;
			} else if ((c & 0xF8) == 0xF0) {
				need = 3;
				cp = c & 0x07;
			} else {
				return 0;
			}
			i++;
		} else {
			if ((c & 0xC0) != 0x80)
				return 0;
			cp = (cp << 6) | (c & 0x3F);
			i++;
			need--;
			if (!need) {
				if (cp > 0x10FFFF)
					return 0;
			}
		}
	}
	return need == 0;
}

/* fold full-width punctuation/space to ascii, lowercase, drop spaces */
char *hdr_normalize(char *s)
{
	char *r = s, *w = s;

	if (!s)
		return s;
	while (*r) {
		unsigned char c = (unsigned char)*r;
		if (c == 0xEF && r[1] && (unsigned char)r[1] == 0xBC && r[2]) {
			/* full width forms in U+FF00 block: EF BC xx / EF BD xx */
			unsigned char d = (unsigned char)r[2];
			if (d == 0x88) { *w++ = '('; r += 3; continue; }
			if (d == 0x89) { *w++ = ')'; r += 3; continue; }
			if (d == 0x9A) { *w++ = ':'; r += 3; continue; }
			if (d == 0x8C) { *w++ = ','; r += 3; continue; }
			if (d == 0x8E) { *w++ = ';'; r += 3; continue; }
			if (d == 0x8F) { *w++ = '?'; r += 3; continue; }
			if (d == 0x81) { *w++ = '!'; r += 3; continue; }
			if (d == 0x8B) { *w++ = '/'; r += 3; continue; }
			if (d == 0x8D) { *w++ = '\\'; r += 3; continue; }
		}
		if (c >= 0x80) {
			/* keep multibyte sequence as-is */
			int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
			int i;
			for (i = 0; i < len && r[i]; i++)
				*w++ = r[i];
			r += i;
			continue;
		}
		if (c == ' ' || c == '	' || c == '\r' || c == '\n' || c == 0xA1) {
			r++;
			continue;
		}
		if (c == '*' || c == '/' || c == '\\' || c == '-' || c == '_' ||
		    c == '.' || c == ':') {
			r++;
			continue;
		}
		*w++ = (char)tolower(c);
		r++;
	}
	*w = 0;
	return s;
}

int slurp(const char *path, unsigned char **data, size_t *len, char *err, size_t errlen)
{
	FILE *f = fopen(path, "rb");
	long sz;
	unsigned char *p;

	*data = NULL;
	*len = 0;
	if (!f) {
		snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		snprintf(err, errlen, "seek failed on %s", path);
		fclose(f);
		return -1;
	}
	sz = ftell(f);
	if (sz < 0) {
		snprintf(err, errlen, "tell failed on %s", path);
		fclose(f);
		return -1;
	}
	rewind(f);
	p = xmalloc((size_t)sz + 1);
	if (sz > 0 && fread(p, 1, (size_t)sz, f) != (size_t)sz) {
		snprintf(err, errlen, "short read on %s", path);
		free(p);
		fclose(f);
		return -1;
	}
	p[sz] = 0;
	fclose(f);
	*data = p;
	*len = (size_t)sz;
	return 0;
}
