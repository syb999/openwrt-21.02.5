/* sheet.c - sheet grid model */
#include "x2s.h"

void sheet_init(Sheet *s, const char *name)
{
	memset(s, 0, sizeof(*s));
	s->name = xstrdup(name ? name : "Sheet");
}

void sheet_free(Sheet *s)
{
	int i, j;

	for (i = 0; i < s->nrows; i++) {
		for (j = 0; j < s->rows[i].n; j++)
			free(s->rows[i].v[j]);
		free(s->rows[i].v);
	}
	free(s->rows);
	free(s->name);
	memset(s, 0, sizeof(*s));
}

static void sheet_grow_rows(Sheet *s, int row)
{
	int old = s->cap;

	if (row < s->cap)
		return;
	if (!s->cap)
		s->cap = 512;
	while (s->cap <= row)
		s->cap *= 2;
	s->rows = xrealloc(s->rows, sizeof(Row) * (size_t)s->cap);
	memset(s->rows + old, 0, sizeof(Row) * (size_t)(s->cap - old));
}

void sheet_set(Sheet *s, int row, int col, const char *text)
{
	Row *r;
	size_t len;

	if (row < 0 || col < 0 || row > X2S_MAX_ROWS || col > X2S_MAX_COLS)
		return;
	len = text ? strlen(text) : 0;
	if (len > X2S_MAX_CELL)
		len = X2S_MAX_CELL;
	if (row >= s->cap)
		sheet_grow_rows(s, row);
	if (row >= s->nrows)
		s->nrows = row + 1;
	r = &s->rows[row];
	if (col >= r->n) {
		int old = r->n;
		r->v = xrealloc(r->v, sizeof(char *) * (size_t)(col + 1));
		memset(r->v + old, 0, sizeof(char *) * (size_t)(col + 1 - old));
		r->n = col + 1;
	}
	if (col + 1 > s->maxcols)
		s->maxcols = col + 1;
	if (r->v[col])
		return;
	r->v[col] = xstrndup(text ? text : "", len);
}

const char *sheet_get(const Sheet *s, int row, int col)
{
	if (row < 0 || row >= s->nrows)
		return NULL;
	if (col < 0 || col >= s->rows[row].n)
		return NULL;
	return s->rows[row].v[col];
}
