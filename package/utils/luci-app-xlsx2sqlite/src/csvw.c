/* csvw.c - table -> RFC4180 csv (utf-8, optional BOM) */
#include "x2s.h"

static void csv_field(Buf *b, const char *v)
{
	const char *p;
	int need = 0;

	if (!v)
		return;
	for (p = v; *p; p++)
		if (*p == ',' || *p == '"' || *p == '\r' || *p == '\n') {
			need = 1;
			break;
		}
	if (!need && (*v == ' ' || (v[0] && v[strlen(v) - 1] == ' ')))
		need = 1;
	if (!need) {
		buf_puts(b, v);
		return;
	}
	buf_putc(b, '"');
	for (p = v; *p; p++) {
		if (*p == '"')
			buf_putc(b, '"');
		buf_putc(b, *p);
	}
	buf_putc(b, '"');
}

int csv_write(const Table *t, const char *path, int bom, char *err, size_t errlen)
{
	FILE *f;
	Buf line;
	int r, c;
	size_t w = 0;

	if (t->schema.n == 0) {
		snprintf(err, errlen, "nothing to export (empty table)");
		return -1;
	}
	f = fopen(path, "wb");
	if (!f) {
		snprintf(err, errlen, "cannot write %s", path);
		return -1;
	}
	if (bom)
		fwrite("\xEF\xBB\xBF", 1, 3, f);
	buf_init(&line);
	for (r = -1; r < t->nrows; r++) {
		buf_reset(&line);
		for (c = 0; c < t->schema.n; c++) {
			if (c)
				buf_putc(&line, ',');
			csv_field(&line, (r < 0) ? t->schema.name[c] : t->cell[r][c]);
		}
		buf_puts(&line, "\r\n");
		w += fwrite(line.p, 1, line.n, f);
	}
	buf_free(&line);
	fclose(f);
	if (w == 0) {
		snprintf(err, errlen, "short write on %s", path);
		return -1;
	}
	return 0;
}
