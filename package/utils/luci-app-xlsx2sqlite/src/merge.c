/* merge.c - header detection, column mapping, merge, dedupe */
#include "x2s.h"

typedef struct {
	const char *id;
	const char *label;
	const char *syn[9];
} Field;

static const Field FIELDS[] = {
	{ "seq", "序号",
	  { "序号", "题号", "编号", "题目编号", "试题编号", "id", "no", "no.", NULL } },
	{ "stem", "题干",
	  { "题干", "题目", "试题", "题目内容", "问题", "question", "内容", "title", NULL } },
	{ "opt", "选项",
	  { "选项", "选择项", "备选项", "option", "options", "choices", "abcd", NULL } },
	{ "ans", "答案",
	  { "答案", "正确答案", "参考答案", "answer", "key", NULL } },
	{ "typ", "题型",
	  { "题型", "题目类型", "试题类型", "类型", "type", NULL } },
	{ "basis", "法律依据",
	  { "法律依据", "法律名称和法条", "依据", "法律", "法条", "法规依据", "出处", NULL } },
};
#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

static int match_field(const char *hdr)
{
	int i, j;

	if (!hdr || !hdr[0])
		return -1;
	for (i = 0; i < NFIELDS; i++)
		for (j = 0; FIELDS[i].syn[j]; j++)
			if (!strcmp(hdr, FIELDS[i].syn[j]))
				return i;
	for (i = 0; i < NFIELDS; i++)
		for (j = 0; FIELDS[i].syn[j]; j++)
			if (strstr(hdr, FIELDS[i].syn[j]))
				return i;
	return -1;
}

void table_init(Table *t)
{
	memset(t, 0, sizeof(*t));
}

void table_free(Table *t)
{
	int i, j;

	for (i = 0; i < t->schema.n; i++) {
		free(t->schema.name[i]);
		free(t->schema.field[i]);
	}
	free(t->schema.name);
	free(t->schema.field);
	for (i = 0; i < t->nrows; i++) {
		for (j = 0; j < t->schema.n; j++)
			free(t->cell[i][j]);
		free(t->cell[i]);
	}
	free(t->cell);
	memset(t, 0, sizeof(*t));
}

static void schema_add(Table *t, const char *name, const char *field)
{
	int n = t->schema.n;

	t->schema.name = xrealloc(t->schema.name, sizeof(char *) * (size_t)(n + 1));
	t->schema.field = xrealloc(t->schema.field, sizeof(char *) * (size_t)(n + 1));
	t->schema.name[n] = xstrdup(name);
	t->schema.field[n] = field ? xstrdup(field) : NULL;
	t->schema.n = n + 1;
}

static int schema_find_field(const Table *t, const char *field)
{
	int i;

	for (i = 0; i < t->schema.n; i++)
		if (t->schema.field[i] && !strcmp(t->schema.field[i], field))
			return i;
	return -1;
}

/* detect the header row of a sheet; returns row index or -1 */
static int detect_header(const Sheet *sh, int *scores_out)
{
	int best = -1, bestn = 0;
	int limit = sh->nrows < 15 ? sh->nrows : 15;
	int r;

	for (r = 0; r < limit; r++) {
		const Row *row = &sh->rows[r];
		int c, n = 0;

		for (c = 0; c < row->n; c++) {
			char *h;
			int f;

			if (str_is_empty(row->v[c]))
				continue;
			h = xstrdup(row->v[c]);
			hdr_normalize(str_trim(h));
			f = match_field(h);
			if (f >= 0)
				n++;
			free(h);
		}
		if (scores_out)
			scores_out[r] = n;
		if (n > bestn) {
			bestn = n;
			best = r;
		}
	}
	if (bestn < 2)
		return -1;
	return best;
}

/* per-sheet column map: cmap[c] = field idx or extra index (>= NFIELDS) or -1
 * letter splits ("选项 A" .. "选项 F") are collected in opt_col[] and joined
 * into the single 选项 cell when rows are copied. */
typedef struct {
	int hdr_row;
	int *cmap;
	int ncols;
	char **extra_name;
	int nextra;
	int opt_col[16];
	int opt_letter[16];
	int nopt;
	int plain_opt;		/* sheet column already holding "A、x\nB、y" */
	unsigned char *hdr_empty;
} SheetMap;

static int field_by_id(const char *id)
{
	int i;

	for (i = 0; i < NFIELDS; i++)
		if (!strcmp(FIELDS[i].id, id))
			return i;
	return -1;
}

/* "选项a" / "选择项b" / "option c" / "选项  d" / single "a".."h" -> 0..7 */
static int opt_letter_of(const char *h)
{
	static const char *pre[] = {
		"\xE9\x80\x89\xE9\xA1\xB9",			/* 选项 */
		"\xE9\x80\x89\xE6\x8B\xA9\xE9\xA1\xB9",		/* 选择项 */
		"\xE5\xA4\x87\xE9\x80\x89\xE9\xA1\xB9",		/* 备选项 */
		"option", "opt", "choice", "choices", NULL
	};
	size_t n = strlen(h);
	char last;
	int i;

	if (n == 1 && h[0] >= 'a' && h[0] <= 'h')
		return h[0] - 'a';
	if (n < 2)
		return -1;
	last = h[n - 1];
	if (last < 'a' || last > 'h')
		return -1;
	for (i = 0; pre[i]; i++)
		if (!strncmp(h, pre[i], strlen(pre[i])))
			return last - 'a';
	return -1;
}

static void map_free(SheetMap *m)
{
	int i;

	free(m->cmap);
	free(m->hdr_empty);
	for (i = 0; i < m->nextra; i++)
		free(m->extra_name[i]);
	free(m->extra_name);
	memset(m, 0, sizeof(*m));
}

static void map_sheet(const Sheet *sh, int hdr_row, SheetMap *m, Table *t)
{
	const Row *hr = &sh->rows[hdr_row];
	int c;
	int optf = field_by_id("opt");

	memset(m, 0, sizeof(*m));
	m->hdr_row = hdr_row;
	m->ncols = hr->n;
	m->plain_opt = -1;
	m->cmap = xmalloc(sizeof(int) * (size_t)(hr->n ? hr->n : 1));
	m->hdr_empty = xmalloc((size_t)(hr->n ? hr->n : 1));
	memset(m->hdr_empty, 1, (size_t)(hr->n ? hr->n : 1));
	for (c = 0; c < hr->n; c++) {
		char *h;
		int f, letter;

		m->cmap[c] = -1;
		if (str_is_empty(hr->v[c]))
			continue;
		m->hdr_empty[c] = 0;
		h = xstrdup(hr->v[c]);
		hdr_normalize(str_trim(h));
		letter = opt_letter_of(h);
		if (letter >= 0 && m->nopt < 16) {
			m->opt_col[m->nopt] = c;
			m->opt_letter[m->nopt] = letter;
			m->nopt++;
			if (optf >= 0 && schema_find_field(t, FIELDS[optf].id) < 0)
				schema_add(t, FIELDS[optf].label, FIELDS[optf].id);
			free(h);
			continue;
		}
		f = match_field(h);
		if (f >= 0) {
			m->cmap[c] = f;
			if (f == optf)
				m->plain_opt = c;
			if (schema_find_field(t, FIELDS[f].id) < 0)
				schema_add(t, FIELDS[f].label, FIELDS[f].id);
		} else {
			int i, dup = 0;

			for (i = 0; i < t->schema.n; i++)
				if (!t->schema.field[i] && !strcmp(t->schema.name[i], h))
					dup = 1;
			if (!dup)
				schema_add(t, h, NULL);
			for (i = 0; i < m->nextra; i++)
				if (!strcmp(m->extra_name[i], h))
					dup = 2;
			if (dup != 2) {
				m->extra_name = xrealloc(m->extra_name,
							 sizeof(char *) * (size_t)(m->nextra + 1));
				m->extra_name[m->nextra] = xstrdup(h);
				m->nextra++;
			}
			for (i = 0; i < m->nextra; i++)
				if (!strcmp(m->extra_name[i], h)) {
					m->cmap[c] = NFIELDS + i;
					break;
				}
		}
		free(h);
	}
	/* no named option columns: take the unnamed columns between 题干 and 答案 */
	if (m->nopt == 0 && m->plain_opt < 0) {
		int stemc = -1, ansc = -1;
		int stemf = field_by_id("stem"), ansf = field_by_id("ans");

		for (c = 0; c < m->ncols; c++) {
			if (stemf >= 0 && m->cmap[c] == stemf)
				stemc = c;
			if (ansf >= 0 && m->cmap[c] == ansf)
				ansc = c;
		}
		if (stemc >= 0 && ansc > stemc + 1) {
			for (c = stemc + 1; c < ansc && m->nopt < 16; c++) {
				if (m->cmap[c] >= 0 || !m->hdr_empty[c])
					continue;
				m->opt_col[m->nopt] = c;
				m->opt_letter[m->nopt] = m->nopt;
				m->nopt++;
			}
			if (m->nopt >= 2 && optf >= 0 && schema_find_field(t, FIELDS[optf].id) < 0)
				schema_add(t, FIELDS[optf].label, FIELDS[optf].id);
			else if (m->nopt < 2)
				m->nopt = 0;
		}
	}
}

/* order the schema: canonical fields first (fixed order), extras after */
static void schema_reorder(Table *t)
{
	Table tmp;
	int i;

	table_init(&tmp);
	memset(&tmp.schema, 0, sizeof(tmp.schema));
	for (i = 0; i < NFIELDS; i++) {
		int k = schema_find_field(t, FIELDS[i].id);
		if (k >= 0) {
			tmp.schema.name = xrealloc(tmp.schema.name, sizeof(char *) * (size_t)(tmp.schema.n + 1));
			tmp.schema.field = xrealloc(tmp.schema.field, sizeof(char *) * (size_t)(tmp.schema.n + 1));
			tmp.schema.name[tmp.schema.n] = t->schema.name[k];
			tmp.schema.field[tmp.schema.n] = t->schema.field[k];
			tmp.schema.n++;
		}
	}
	for (i = 0; i < t->schema.n; i++) {
		if (t->schema.field[i])
			continue;
		tmp.schema.name = xrealloc(tmp.schema.name, sizeof(char *) * (size_t)(tmp.schema.n + 1));
		tmp.schema.field = xrealloc(tmp.schema.field, sizeof(char *) * (size_t)(tmp.schema.n + 1));
		tmp.schema.name[tmp.schema.n] = t->schema.name[i];
		tmp.schema.field[tmp.schema.n] = NULL;
		tmp.schema.n++;
	}
	free(t->schema.name);
	free(t->schema.field);
	t->schema = tmp.schema;
}

static void table_add_row(Table *t, char **vals)
{
	if (t->nrows == t->cap) {
		t->cap = t->cap ? t->cap * 2 : 512;
		t->cell = xrealloc(t->cell, sizeof(char **) * (size_t)t->cap);
	}
	t->cell[t->nrows++] = vals;
}

/* find the column in schema that a sheet column maps to */
static int schema_col_for(const Table *t, const SheetMap *m, int sheetcol)
{
	int tag;

	if (sheetcol < 0 || sheetcol >= m->ncols)
		return -1;
	tag = m->cmap[sheetcol];
	if (tag < 0)
		return -1;
	if (tag < NFIELDS)
		return schema_find_field(t, FIELDS[tag].id);
	{
		const char *nm = m->extra_name[tag - NFIELDS];
		int i;
		for (i = 0; i < t->schema.n; i++)
			if (!t->schema.field[i] && !strcmp(t->schema.name[i], nm))
				return i;
	}
	return -1;
}


static unsigned int hash_str(const char *s)
{
	unsigned int h = 2166136261u;

	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 16777619u;
	}
	return h;
}

/* dedupe: mode 0 exact (byte compare after trim), 2 none.
 * question numbers are compared verbatim on purpose: 04697 and 4697 are
 * different questions and must never be merged. */
static void table_dedupe(Table *t, int mode)
{
	int n = t->nrows;
	int *group = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
	int *keep = xmalloc(sizeof(int) * (size_t)(n ? n : 1));
	int *gcount = NULL;
	int nga = 0, capga = 0;
	char **gkey = NULL;
	int keycol = schema_find_field(t, "seq");
	int anscol = schema_find_field(t, "ans");
	int *slot = NULL;
	int nslot = 1, i, out = 0;

	if (mode == 2 || n == 0) {
		free(group);
		free(keep);
		return;
	}
	if (keycol < 0) {
		for (i = 0; i < t->schema.n; i++)
			if (t->schema.field[i]) {
				keycol = i;
				break;
			}
	}
	if (keycol < 0)
		keycol = 0;

	while (nslot < n * 2 + 8)
		nslot *= 2;
	slot = xmalloc(sizeof(int) * (size_t)nslot);
	for (i = 0; i < nslot; i++)
		slot[i] = -1;

	for (i = 0; i < n; i++) {
		const char *raw = t->cell[i][keycol];
		char *key = xstrdup(raw ? raw : "");
		unsigned int h;
		int g = -1;

		if (!key[0] && str_is_empty(raw)) {
			/* empty key rows never group together */
			free(key);
			group[i] = -1;
			keep[i] = 1;
			continue;
		}
		h = hash_str(key);
		for (;;) {
			int s = (int)(h & (unsigned int)(nslot - 1));
			if (slot[s] < 0) {
				slot[s] = nga;
				if (nga == capga) {
					capga = capga ? capga * 2 : 256;
					gcount = xrealloc(gcount, sizeof(int) * (size_t)capga);
					gkey = xrealloc(gkey, sizeof(char *) * (size_t)capga);
				}
				gcount[nga] = 0;
				gkey[nga] = key;
				g = nga++;
				break;
			}
			if (!strcmp(gkey[slot[s]], key)) {
				g = slot[s];
				free(key);
				break;
			}
			h++;
		}
		group[i] = g;
		gcount[g]++;
		keep[i] = 1;
	}

	/* Duplicate means the SAME question record: 序号 equal AND 题干 equal AND
	 * 答案 equal. 序号 alone is not enough - question banks restart the
	 * numbering per section, so 序号 1 appears in every section with a
	 * different 题干. Dropping those would silently lose whole sections. */
	{
		int stemcol = schema_find_field(t, "stem");
		int cmpcols[3];
		int ncmp = 0;

		if (stemcol >= 0)
			cmpcols[ncmp++] = stemcol;
		if (anscol >= 0)
			cmpcols[ncmp++] = anscol;

		for (i = 0; i < n; i++) {
			int g = group[i], j;

			if (g < 0 || gcount[g] < 2)
				continue;
			for (j = 0; j < i; j++) {
				int k;
				int same = 1;

				if (!keep[j] || group[j] != g)
					continue;
				if (ncmp == 0) {
					/* no 题干/答案 column: compare the whole row */
					for (k = 0; k < t->schema.n && same; k++) {
						const char *a = t->cell[i][k];
						const char *b = t->cell[j][k];

						if (strcmp(a ? a : "", b ? b : ""))
							same = 0;
					}
				} else {
					for (k = 0; k < ncmp && same; k++) {
						const char *a = t->cell[i][cmpcols[k]];
						const char *b = t->cell[j][cmpcols[k]];

						if (strcmp(a ? a : "", b ? b : ""))
							same = 0;
					}
				}
				if (same) {
					keep[i] = 0;
					break;
				}
			}
		}
	}
	{
		int kept = 0;
		for (i = 0; i < n; i++) {
			if (keep[i]) {
				if (out != i)
					t->cell[out] = t->cell[i];
				out++;
				kept++;
			} else {
				int j;
				for (j = 0; j < t->schema.n; j++)
					free(t->cell[i][j]);
				free(t->cell[i]);
			}
		}
		t->dup_dropped = n - kept;
		t->nrows = out;
	}
	free(group);
	free(keep);
	free(slot);
	for (i = 0; i < nga; i++)
		free(gkey[i]);
	free(gkey);
	free(gcount);
}

int table_build(Table *t, Sheet *sheets, int nsheets, const char *refhdr,
		int header_row, int dedupe, char *err, size_t errlen)
{
	SheetMap *maps = NULL;
	int *hdrrows = NULL;
	int s;
	int nskip_hdr = 0;

	(void)refhdr;
	table_init(t);
	if (nsheets <= 0) {
		snprintf(err, errlen, "no sheets");
		return -1;
	}
	maps = xmalloc(sizeof(SheetMap) * (size_t)nsheets);
	hdrrows = xmalloc(sizeof(int) * (size_t)nsheets);
	memset(maps, 0, sizeof(SheetMap) * (size_t)nsheets);

	/* pass 1: locate header rows and collect the union schema */
	for (s = 0; s < nsheets; s++) {
		int hr;

		if (header_row > 0) {
			hr = header_row - 1;
			if (hr >= sheets[s].nrows)
				hr = sheets[s].nrows ? sheets[s].nrows - 1 : 0;
		} else {
			hr = detect_header(&sheets[s], NULL);
		}
		hdrrows[s] = hr;
		if (hr < 0)
			continue;
		map_sheet(&sheets[s], hr, &maps[s], t);
	}
	if (t->schema.n == 0) {
		snprintf(err, errlen, "no header row detected (need >=2 known column names)");
		for (s = 0; s < nsheets; s++)
			map_free(&maps[s]);
		free(maps);
		free(hdrrows);
		return -1;
	}
	schema_reorder(t);

	/* pass 2: copy rows */
	for (s = 0; s < nsheets; s++) {
		int r, c;

		if (hdrrows[s] < 0)
			continue;
		for (r = hdrrows[s] + 1; r < sheets[s].nrows; r++) {
			const Row *row = &sheets[s].rows[r];
			char **vals;
			int nonempty = 0;
			int is_hdr = 1;

			for (c = 0; c < row->n; c++)
				if (!str_is_empty(row->v[c]))
					nonempty++;
			if (!nonempty)
				continue;
			/* skip a repeated header line inside the data */
			{
				int cmp = 0, cells = 0;
				for (c = 0; c < row->n; c++) {
					int sc = schema_col_for(t, &maps[s], c);
					if (sc < 0 || str_is_empty(row->v[c]))
						continue;
					cells++;
					if (!strcmp(row->v[c], t->schema.name[sc]))
						cmp++;
				}
				if (cells > 0 && cmp == cells)
					is_hdr = 1;
				else
					is_hdr = 0;
			}
			if (is_hdr) {
				nskip_hdr++;
				continue;
			}
			vals = xmalloc(sizeof(char *) * (size_t)t->schema.n);
			for (c = 0; c < t->schema.n; c++)
				vals[c] = NULL;
			for (c = 0; c < row->n; c++) {
				int sc = schema_col_for(t, &maps[s], c);
				char *v;
				if (sc < 0 || str_is_empty(row->v[c]))
					continue;
				v = str_trim(row->v[c]);
				if (!*v)
					continue;
				free(vals[sc]);
				vals[sc] = xstrdup(v);
			}
			/* split option columns -> one "A、..\nB、.." cell (1.xlsx layout) */
			if (maps[s].plain_opt < 0 && maps[s].nopt > 0) {
				int oi = schema_find_field(t, "opt");
				Buf b;
				int k;

				if (oi >= 0) {
					buf_init(&b);
					for (k = 0; k < maps[s].nopt; k++) {
						int col = maps[s].opt_col[k];
						const char *v;
						char *tr;

						if (col < 0 || col >= row->n)
							continue;
						v = row->v[col];
						if (str_is_empty(v))
							continue;
						tr = str_trim((char *)v);
						if (!*tr)
							continue;
						if (b.n)
							buf_putc(&b, '\n');
						buf_putc(&b, (char)('A' + maps[s].opt_letter[k]));
						buf_puts(&b, "\xE3\x80\x81");
						buf_puts(&b, tr);
					}
					if (b.n) {
						free(vals[oi]);
						vals[oi] = buf_detach(&b);
					} else {
						buf_free(&b);
					}
				}
			}
			table_add_row(t, vals);
			t->in_rows++;
		}
	}
	table_dedupe(t, dedupe);

	snprintf(t->msg, sizeof(t->msg), "sheets=%d in_rows=%d out_rows=%d dropped=%d hdr_skipped=%d",
		 nsheets, t->in_rows, t->nrows, t->dup_dropped, nskip_hdr);
	for (s = 0; s < nsheets; s++)
		map_free(&maps[s]);
	free(maps);
	free(hdrrows);
	return 0;
}
