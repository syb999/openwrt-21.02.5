/* sqldump.c - .sql text dump export and import */
#include "x2s.h"
#include <ctype.h>
#include <sqlite3.h>

/* ---------------- export ---------------- */
static void sql_quote(Buf *b, const unsigned char *s)
{
	buf_putc(b, '\'');
	for (; s && *s; s++) {
		if (*s == '\'')
			buf_putc(b, '\'');
		buf_putc(b, *s);
	}
	buf_putc(b, '\'');
}

static void sql_ident(Buf *b, const char *s)
{
	buf_putc(b, '"');
	for (; s && *s; s++) {
		if (*s == '"')
			buf_putc(b, '"');
		buf_putc(b, *s);
	}
	buf_putc(b, '"');
}

int db_export_sql(const char *dbpath, const char *table, FILE *out, char *err, size_t errlen)
{
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	Buf q;
	int i, ncol, rc;

	if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		snprintf(err, errlen, "cannot open %s", dbpath);
		if (db)
			sqlite3_close(db);
		return -1;
	}
	buf_init(&q);
	buf_puts(&q, "SELECT * FROM ");
	sql_ident(&q, table);
	if (sqlite3_prepare_v2(db, buf_str(&q), -1, &st, NULL) != SQLITE_OK) {
		snprintf(err, errlen, "no such table: %s", table);
		buf_free(&q);
		sqlite3_close(db);
		return -1;
	}
	ncol = sqlite3_column_count(st);
	fputs("PRAGMA foreign_keys=OFF;\nBEGIN TRANSACTION;\n", out);
	{
		Buf ct;

		buf_init(&ct);
		buf_puts(&ct, "CREATE TABLE IF NOT EXISTS ");
		sql_ident(&ct, table);
		buf_puts(&ct, " (");
		for (i = 0; i < ncol; i++) {
			if (i)
				buf_putc(&ct, ',');
			sql_ident(&ct, sqlite3_column_name(st, i));
			buf_puts(&ct, " TEXT");
		}
		buf_puts(&ct, ");\n");
		fwrite(ct.p, 1, ct.n, out);
		buf_free(&ct);
	}
	{
		long long nrow = 0;
		Buf row;

		buf_init(&row);
		while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
			buf_reset(&row);
			buf_puts(&row, "INSERT INTO ");
			sql_ident(&row, table);
			buf_puts(&row, " VALUES(");
			for (i = 0; i < ncol; i++) {
				const unsigned char *v;

				if (i)
					buf_putc(&row, ',');
				if (sqlite3_column_type(st, i) == SQLITE_NULL) {
					buf_puts(&row, "NULL");
					continue;
				}
				v = sqlite3_column_text(st, i);
				sql_quote(&row, v);
			}
			buf_puts(&row, ");\n");
			fwrite(row.p, 1, row.n, out);
			nrow++;
		}
		buf_free(&row);
		if (rc != SQLITE_DONE) {
			snprintf(err, errlen, "sqlite read: %s", sqlite3_errmsg(db));
			sqlite3_finalize(st);
			buf_free(&q);
			sqlite3_close(db);
			return -1;
		}
		fputs("COMMIT;\n", out);
		if (nrow == 0)
			fputs("-- table is empty\n", out);
	}
	if (ferror(out)) {
		snprintf(err, errlen, "write failed");
		sqlite3_finalize(st);
		buf_free(&q);
		sqlite3_close(db);
		return -1;
	}
	sqlite3_finalize(st);
	buf_free(&q);
	sqlite3_close(db);
	return 0;
}

/* ---------------- import (tolerant dump parser) ---------------- */
typedef struct {
	const char *p;
	const char *end;
} Cur;

/* mysqldump emits backslash escapes, sqlite3 .dump emits '' doubling only */
static int g_mysql_esc;

static int has_bytes(const unsigned char *h, size_t n, const char *needle)
{
	size_t k = strlen(needle), i;

	if (k == 0 || n < k)
		return 0;
	for (i = 0; i + k <= n; i++)
		if (!memcmp(h + i, needle, k))
			return 1;
	return 0;
}

static void skip_ws(Cur *c)
{
	for (;;) {
		while (c->p < c->end && isspace((unsigned char)*c->p))
			c->p++;
		if (c->p + 1 < c->end && c->p[0] == '-' && c->p[1] == '-') {
			while (c->p < c->end && *c->p != '\n')
				c->p++;
			continue;
		}
		if (c->p + 1 < c->end && c->p[0] == '/' && c->p[1] == '*') {
			c->p += 2;
			while (c->p + 1 < c->end && !(c->p[0] == '*' && c->p[1] == '/'))
				c->p++;
			if (c->p + 1 < c->end)
				c->p += 2;
			continue;
		}
		return;
	}
}

static int kw(Cur *c, const char *word)
{
	size_t n = strlen(word);
	const char *p = c->p;
	size_t i;

	for (i = 0; i < n; i++) {
		if (p + i >= c->end || tolower((unsigned char)p[i]) != word[i])
			return 0;
	}
	if (p + n < c->end && (isalnum((unsigned char)p[n]) || p[n] == '_'))
		return 0;
	return 1;
}

static void take_kw(Cur *c, size_t n)
{
	c->p += n;
}

/* read an identifier: bare, "quoted", `quoted` or [bracketed]; keeps only the
 * last dotted component (schema.table -> table) */
static int read_ident(Cur *c, Buf *out)
{
	Buf tmp;
	int any = 0;

	buf_init(&tmp);
	skip_ws(c);
	if (c->p >= c->end)
		return 0;
	if (*c->p == '"' || *c->p == '`' || *c->p == '[') {
		char q = *c->p++;
		char close = (q == '[') ? ']' : q;

		while (c->p < c->end && *c->p != close) {
			if (*c->p == q && q != '[' && c->p + 1 < c->end && c->p[1] == q) {
				buf_putc(&tmp, q);
				c->p += 2;
				continue;
			}
			buf_putc(&tmp, *c->p++);
		}
		if (c->p < c->end)
			c->p++;
		any = 1;
	} else {
		while (c->p < c->end &&
		       (isalnum((unsigned char)*c->p) || *c->p == '_' || *c->p == '$' ||
			(unsigned char)*c->p >= 0x80))
			buf_putc(&tmp, *c->p++), any = 1;
	}
	if (!any) {
		buf_free(&tmp);
		return 0;
	}
	if (out) {
		buf_reset(out);
		buf_add(out, tmp.p, tmp.n);
	}
	buf_free(&tmp);
	/* skip ".something" */
	{
		const char *save = c->p;

		skip_ws(c);
		if (c->p < c->end && *c->p == '.') {
			c->p++;
			return read_ident(c, out);
		}
		c->p = save;
	}
	return 1;
}

/* one value: 'str' | "str" | NULL | number | x'..' */
static int read_value(Cur *c, Buf *out)
{
	skip_ws(c);
	buf_reset(out);
	if (c->p >= c->end)
		return 0;
	if (*c->p == '\'') {
		c->p++;
		while (c->p < c->end) {
			if (*c->p == '\\' && g_mysql_esc && c->p + 1 < c->end) {
				char e = c->p[1];

				switch (e) {
				case 'n': buf_putc(out, '\n'); break;
				case 'r': buf_putc(out, '\r'); break;
				case 't': buf_putc(out, '	'); break;
				case '0': buf_putc(out, '\0'); break;
				case 'b': buf_putc(out, '\b'); break;
				case 'Z': buf_putc(out, 0x1A); break;
				default: buf_putc(out, e); break;
				}
				c->p += 2;
				continue;
			}
			if (*c->p == '\'') {
				if (c->p + 1 < c->end && c->p[1] == '\'') {
					buf_putc(out, '\'');
					c->p += 2;
					continue;
				}
				c->p++;
				break;
			}
			buf_putc(out, *c->p++);
		}
		return 1;
	}
	if (*c->p == '"') {
		c->p++;
		while (c->p < c->end) {
			if (*c->p == '"') {
				if (c->p + 1 < c->end && c->p[1] == '"') {
					buf_putc(out, '"');
					c->p += 2;
					continue;
				}
				c->p++;
				break;
			}
			buf_putc(out, *c->p++);
		}
		return 1;
	}
	if (kw(c, "null")) {
		take_kw(c, 4);
		return 1;
	}
	{
		/* number or 0x.. or bare word */
		const char *s = c->p;
		while (c->p < c->end && *c->p != ',' && *c->p != ')' &&
		       !isspace((unsigned char)*c->p))
			c->p++;
		if (c->p > s) {
			buf_add(out, s, (size_t)(c->p - s));
			return 1;
		}
	}
	return 0;
}

typedef struct {
	char **name;
	int n;
} Cols;

static void cols_free(Cols *c)
{
	int i;

	for (i = 0; i < c->n; i++)
		free(c->name[i]);
	free(c->name);
	memset(c, 0, sizeof(*c));
}

static void cols_add(Cols *c, const char *s)
{
	c->name = xrealloc(c->name, sizeof(char *) * (size_t)(c->n + 1));
	c->name[c->n++] = xstrdup(s);
}

/* CREATE TABLE t (name type, name type, PRIMARY KEY ...) */
static void parse_create(Cur *c, Cols *cols)
{
	Buf id, tmp;
	int depth = 0;

	buf_init(&id);
	buf_init(&tmp);
	if (!read_ident(c, &id)) {
		buf_free(&id);
		buf_free(&tmp);
		return;
	}
	skip_ws(c);
	if (c->p >= c->end || *c->p != '(') {
		buf_free(&id);
		buf_free(&tmp);
		return;
	}
	c->p++;
	depth = 1;
	while (c->p < c->end && depth > 0) {
		/* start of a column definition */
		skip_ws(c);
		if (c->p < c->end && *c->p == ')') {
			depth--;
			c->p++;
			break;
		}
		buf_reset(&tmp);
		if (!read_ident(c, &tmp))
			break;
		if (kw(c, "primary") || kw(c, "unique") || kw(c, "key") ||
		    kw(c, "constraint") || kw(c, "foreign") || kw(c, "check") ||
		    kw(c, "index") || kw(c, "fulltext"))
			buf_reset(&tmp);
		if (tmp.n)
			cols_add(cols, buf_str(&tmp));
		/* skip to the end of this definition (top level comma) */
		while (c->p < c->end) {
			if (*c->p == '\'' || *c->p == '"' || *c->p == '`') {
				char q = *c->p++;

				while (c->p < c->end && *c->p != q)
					c->p++;
				if (c->p < c->end)
					c->p++;
				continue;
			}
			if (*c->p == '(') {
				depth++;
				c->p++;
				continue;
			}
			if (*c->p == ')') {
				depth--;
				c->p++;
				if (depth == 0)
					break;
				continue;
			}
			if (*c->p == ',' && depth == 1) {
				c->p++;
				break;
			}
			c->p++;
		}
	}
	buf_free(&id);
	buf_free(&tmp);
}

typedef struct {
	Sheet *sh;
	int nrows;
	Buf tname;
	int have_tname;
	Cols table_cols;
} SqlCtx;

static void add_row(SqlCtx *s, char **vals, int nvals)
{
	int r = s->nrows + 1;
	int i;

	if (s->nrows == 0) {
		for (i = 0; i < nvals; i++) {
			const char *h = sheet_get(s->sh, 0, i);

			if (!h || !*h) {
				char b[16];
				snprintf(b, sizeof(b), "c%d", i + 1);
				sheet_set(s->sh, 0, i, b);
			}
		}
	}
	for (i = 0; i < nvals; i++) {
		if (vals[i] && vals[i][0])
			sheet_set(s->sh, r, i, vals[i]);
		else
			sheet_set(s->sh, r, i, "");
	}
	s->nrows++;
}

/* INSERT INTO t [(a,b)] VALUES (...),(...) */
static void parse_insert(Cur *c, SqlCtx *s)
{
	Buf name, type;
	Cols cols;
	int have_cols = 0;

	buf_init(&name);
	buf_init(&type);
	memset(&cols, 0, sizeof(cols));
	if (kw(c, "or")) {		/* INSERT OR REPLACE/IGNORE */
		take_kw(c, 2);
		skip_ws(c);
		read_ident(c, &type);
	}
	if (kw(c, "ignore"))
		take_kw(c, 6);
	skip_ws(c);
	if (!kw(c, "into")) {
		buf_free(&name);
		buf_free(&type);
		return;
	}
	take_kw(c, 4);
	if (!read_ident(c, &name)) {
		buf_free(&name);
		buf_free(&type);
		return;
	}
	skip_ws(c);
	if (c->p < c->end && *c->p == '(') {
		c->p++;
		have_cols = 1;
		for (;;) {
			Buf cn;

			buf_init(&cn);
			if (!read_ident(c, &cn)) {
				buf_free(&cn);
				break;
			}
			cols_add(&cols, buf_str(&cn));
			buf_free(&cn);
			skip_ws(c);
			if (c->p < c->end && *c->p == ',') {
				c->p++;
				continue;
			}
			if (c->p < c->end && *c->p == ')') {
				c->p++;
				break;
			}
			break;
		}
	}
	skip_ws(c);
	if (!kw(c, "values")) {
		cols_free(&cols);
		buf_free(&name);
		buf_free(&type);
		return;
	}
	take_kw(c, 6);

	if (!s->have_tname) {
		int i;

		buf_reset(&s->tname);
		buf_add(&s->tname, name.p, name.n);
		s->have_tname = 1;
		if (!have_cols)
			for (i = 0; i < s->table_cols.n; i++)
				cols_add(&cols, s->table_cols.name[i]);
		for (i = 0; i < cols.n; i++)
			sheet_set(s->sh, 0, i, cols.name[i]);
	} else if (str_ieq(buf_str(&s->tname), buf_str(&name))) {
		/* same table, keep appending */
	} else {
		cols_free(&cols);
		buf_free(&name);
		buf_free(&type);
		return;		/* another table: ignore */
	}

	for (;;) {
		char **vals = NULL;
		int nvals = 0;
		Buf v;

		buf_init(&v);
		skip_ws(c);
		if (c->p >= c->end || *c->p != '(')
			break;
		c->p++;
		for (;;) {
			skip_ws(c);
			if (c->p >= c->end)
				break;
			if (*c->p == ')') {
				c->p++;
				break;
			}
			if (!read_value(c, &v))
				break;
			vals = xrealloc(vals, sizeof(char *) * (size_t)(nvals + 1));
			vals[nvals++] = xstrdup(buf_str(&v));
			skip_ws(c);
			if (c->p < c->end && *c->p == ',') {
				c->p++;
				continue;
			}
			if (c->p < c->end && *c->p == ')') {
				c->p++;
				break;
			}
			break;
		}
		buf_free(&v);
		if (nvals > 0) {
			if (!s->have_tname) {
				int i;
				for (i = 0; i < nvals; i++) {
					char b[16];
					snprintf(b, sizeof(b), "c%d", i + 1);
					sheet_set(s->sh, 0, i, b);
				}
			}
			add_row(s, vals, nvals);
		}
		{
			int i;
			for (i = 0; i < nvals; i++)
				free(vals[i]);
			free(vals);
		}
		skip_ws(c);
		if (c->p < c->end && *c->p == ',') {
			c->p++;
			continue;
		}
		break;
	}
	cols_free(&cols);
	buf_free(&name);
	buf_free(&type);
}

static void stmt_dispatch(const char *s, size_t n, SqlCtx *ctx, Cols *last_cols)
{
	Cur c;

	c.p = s;
	c.end = s + n;
	skip_ws(&c);
	if (kw(&c, "create")) {
		take_kw(&c, 6);
		skip_ws(&c);
		if (kw(&c, "temporary")) {
			take_kw(&c, 9);
			skip_ws(&c);
		}
		if (kw(&c, "table")) {
			take_kw(&c, 5);
			skip_ws(&c);
			if (kw(&c, "if")) {
				take_kw(&c, 2);
				skip_ws(&c);
				if (kw(&c, "not")) {
					take_kw(&c, 3);
					skip_ws(&c);
				}
				if (kw(&c, "exists")) {
					take_kw(&c, 6);
					skip_ws(&c);
				}
			}
			cols_free(last_cols);
			parse_create(&c, last_cols);
		}
		return;
	}
	if (kw(&c, "insert")) {
		take_kw(&c, 6);
		if (!ctx->have_tname) {
			int i;
			for (i = 0; i < last_cols->n; i++)
				cols_add(&ctx->table_cols, last_cols->name[i]);
		}
		parse_insert(&c, ctx);
	}
}

int sql_read(const char *path, Sheet **out, int *nout, char *err, size_t errlen)
{
	unsigned char *data = NULL;
	size_t len = 0, i;
	SqlCtx ctx;
	Cols last_cols;
	size_t start = 0;
	int in_q = 0;
	char q = 0;

	*out = NULL;
	*nout = 0;
	if (slurp(path, &data, &len, err, errlen) != 0)
		return -1;
	if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
		memmove(data, data + 3, len - 3);
		len -= 3;
	}
	g_mysql_esc = 0;
	{
		size_t probe = len < 4096 ? len : 4096;

		if (has_bytes(data, probe, "SET NAMES") || has_bytes(data, probe, "ENGINE=") ||
		    has_bytes(data, probe, "/*!") || has_bytes(data, probe, "LOCK TABLES"))
			g_mysql_esc = 1;
	}
	memset(&ctx, 0, sizeof(ctx));
	memset(&last_cols, 0, sizeof(last_cols));
	buf_init(&ctx.tname);
	ctx.sh = xmalloc(sizeof(Sheet));
	sheet_init(ctx.sh, path);

	for (i = 0; i <= len; i++) {
		unsigned char ch = (i < len) ? data[i] : ';';

		if (in_q) {
			if (ch == q) {
				if (i + 1 < len && data[i + 1] == q) {
					i++;
					continue;
				}
				in_q = 0;
			}
			continue;
		}
		if (ch == '\'' || ch == '"' || ch == '`') {
			in_q = 1;
			q = (char)ch;
			continue;
		}
		if (ch == '-' && i + 1 < len && data[i + 1] == '-') {
			while (i < len && data[i] != '\n')
				i++;
			continue;
		}
		if (ch == '/' && i + 1 < len && data[i + 1] == '*') {
			i += 2;
			while (i + 1 < len && !(data[i] == '*' && data[i + 1] == '/'))
				i++;
			i++;
			continue;
		}
		if (ch == ';') {
			stmt_dispatch((const char *)data + start, i - start, &ctx, &last_cols);
			start = i + 1;
		}
	}
	cols_free(&last_cols);
	cols_free(&ctx.table_cols);
	buf_free(&ctx.tname);
	free(data);

	if (ctx.nrows == 0) {
		snprintf(err, errlen, "no INSERT statements found in the .sql file");
		sheet_free(ctx.sh);
		free(ctx.sh);
		return -1;
	}
	*out = ctx.sh;
	*nout = 1;
	return 0;
}
