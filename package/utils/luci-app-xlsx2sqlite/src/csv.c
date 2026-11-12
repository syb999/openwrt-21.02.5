/* csv.c - delimited text reader (utf-8 / gbk, , ; tab |) */
#include "x2s.h"

static int sniff_delim(const unsigned char *s, size_t n)
{
	int cnt[256];
	int best = ',';
	int i, bestn = 0;
	int in_q = 0;
	size_t k = 0;

	memset(cnt, 0, sizeof(cnt));
	for (k = 0; k < n; k++) {
		unsigned char c = s[k];
		if (c == '"') {
			in_q = !in_q;
			continue;
		}
		if (in_q)
			continue;
		if (c == '\n' || c == '\r')
			break;
		cnt[c]++;
	}
	{
		const char *cand = ",;\t|";
		for (i = 0; cand[i]; i++) {
			if (cnt[(unsigned char)cand[i]] > bestn) {
				bestn = cnt[(unsigned char)cand[i]];
				best = cand[i];
			}
		}
	}
	if (bestn == 0)
		best = ',';
	return best;
}

int csv_read(const char *path, Sheet **out, int *nout, char *err, size_t errlen)
{
	unsigned char *data = NULL;
	size_t len = 0, i;
	int utf8;
	int delim;
	Sheet *sh;
	Buf field, cell;
	int row = 0, col = 0;
	int in_q = 0;
	int ws_only = 1;
	int last_was_delim = 0;

	*out = NULL;
	*nout = 0;
	if (slurp(path, &data, &len, err, errlen) != 0)
		return -1;
	if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
		memmove(data, data + 3, len - 3);
		len -= 3;
	}
	if (len == 0) {
		snprintf(err, errlen, "empty file");
		free(data);
		return -1;
	}
	utf8 = utf8_valid(data, len);
	delim = sniff_delim(data, len);

	sh = xmalloc(sizeof(Sheet));
	sheet_init(sh, path);
	buf_init(&field);
	buf_init(&cell);

	for (i = 0; i <= len; i++) {
		unsigned char c = (i < len) ? data[i] : '\n';
		int endfield = 0, endrow = 0;

		if (in_q) {
			if (c == '"' && i + 1 < len && data[i + 1] == '"') {
				buf_putc(&field, '"');
				i++;
				continue;
			}
			if (c == '"') {
				in_q = 0;
				ws_only = 0;
				continue;
			}
			buf_putc(&field, c);
			continue;
		}
		if (c == '"') {
			/* a quote only opens a quoted field at the start of the field;
			 * a stray quote in the middle of unquoted text is literal data
			 * (otherwise one bad quote swallows every following line) */
			if (ws_only) {
				in_q = 1;
				buf_reset(&field);
				ws_only = 1;
				continue;
			}
			buf_putc(&field, '"');
			continue;
		}
		if (c == delim) {
			endfield = 1;
			last_was_delim = 1;
		} else if (c == '\n' || c == '\r') {
			if (c == '\r' && i + 1 < len && data[i + 1] == '\n')
				i++;
			endfield = 1;
			endrow = 1;
			last_was_delim = 0;
		} else {
			buf_putc(&field, c);
			if (c != ' ' && c != '	')
				ws_only = 0;
			last_was_delim = 0;
		}
		if (endfield) {
			buf_reset(&cell);
			if (utf8) {
				buf_add(&cell, buf_str(&field), field.n);
			} else {
				size_t k = 0;
				const unsigned char *f = (const unsigned char *)buf_str(&field);
				while (k < field.n)
					k += gbk_to_utf8(&cell, f + k, field.n - k);
			}
			{
				char *t = str_trim((char *)buf_str(&cell));
				if (*t)
					sheet_set(sh, row, col, t);
			}
			buf_reset(&field);
			ws_only = 1;
			col++;
			if (endrow) {
				row++;
				col = 0;
			}
			(void)last_was_delim;
		}
	}
	buf_free(&field);
	buf_free(&cell);
	free(data);

	while (sh->nrows > 0) {
		int k, empty = 1;
		for (k = 0; k < sh->rows[sh->nrows - 1].n; k++)
			if (!str_is_empty(sh->rows[sh->nrows - 1].v[k]))
				empty = 0;
		if (!empty)
			break;
		sh->nrows--;
	}
	if (sh->nrows == 0) {
		snprintf(err, errlen, "no data rows in %s", path);
		sheet_free(sh);
		free(sh);
		return -1;
	}
	*out = sh;
	*nout = 1;
	return 0;
}
