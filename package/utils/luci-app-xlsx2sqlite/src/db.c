/* db.c - sqlite3 output and inspection */
#include "x2s.h"
#include <sqlite3.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

static void quote_id(Buf *b, const char *s)
{
	buf_putc(b, '"');
	for (; s && *s; s++) {
		if (*s == '"')
			buf_putc(b, '"');
		buf_putc(b, *s);
	}
	buf_putc(b, '"');
}

int mkpath(const char *path)
{
	char tmp[512];
	size_t n;
	char *p;

	if (!path || strlen(path) >= sizeof(tmp))
		return -1;
	strcpy(tmp, path);
	n = strlen(tmp);
	while (n > 1 && tmp[n - 1] == '/')
		tmp[--n] = 0;
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

int table_write_sqlite(Table *t, const char *dbpath, const char *table,
		char *err, size_t errlen)
{
	sqlite3 *db = NULL;
	Buf sql;
	int i, rc;
	char *emsg = NULL;

	if (t->schema.n == 0) {
		snprintf(err, errlen, "empty schema");
		return -1;
	}
	{
		char dir[512];
		char *slash;
		snprintf(dir, sizeof(dir), "%s", dbpath);
		slash = strrchr(dir, '/');
		if (slash && slash != dir) {
			*slash = 0;
			if (mkpath(dir) != 0) {
				snprintf(err, errlen, "cannot create directory %s", dir);
				return -1;
			}
		}
	}
	rc = sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
	if (rc != SQLITE_OK) {
		snprintf(err, errlen, "sqlite open %s: %s", dbpath,
			 db ? sqlite3_errmsg(db) : "?");
		if (db)
			sqlite3_close(db);
		return -1;
	}
	sqlite3_busy_timeout(db, 8000);

	buf_init(&sql);
	buf_puts(&sql, "BEGIN;");
	buf_puts(&sql, "DROP TABLE IF EXISTS ");
	quote_id(&sql, table);
	buf_puts(&sql, ";CREATE TABLE ");
	quote_id(&sql, table);
	buf_puts(&sql, " (");
	for (i = 0; i < t->schema.n; i++) {
		if (i)
			buf_putc(&sql, ',');
		quote_id(&sql, t->schema.name[i]);
		buf_puts(&sql, " TEXT");
	}
	buf_puts(&sql, ");");

	rc = sqlite3_exec(db, buf_str(&sql), NULL, NULL, &emsg);
	if (rc != SQLITE_OK) {
		snprintf(err, errlen, "sqlite create table: %s", emsg ? emsg : "?");
		sqlite3_free(emsg);
		buf_free(&sql);
		sqlite3_close(db);
		return -1;
	}
	buf_reset(&sql);
	buf_puts(&sql, "INSERT INTO ");
	quote_id(&sql, table);
	buf_puts(&sql, " VALUES (");
	for (i = 0; i < t->schema.n; i++) {
		if (i)
			buf_putc(&sql, ',');
		buf_putc(&sql, '?');
	}
	buf_puts(&sql, ");");
	{
		sqlite3_stmt *st = NULL;

		rc = sqlite3_prepare_v2(db, buf_str(&sql), -1, &st, NULL);
		if (rc != SQLITE_OK) {
			snprintf(err, errlen, "sqlite prepare: %s", sqlite3_errmsg(db));
			buf_free(&sql);
			sqlite3_close(db);
			return -1;
		}
		for (i = 0; i < t->nrows; i++) {
			int c;

			sqlite3_reset(st);
			for (c = 0; c < t->schema.n; c++) {
				const char *v = t->cell[i][c];
				if (v && v[0])
					sqlite3_bind_text(st, c + 1, v, -1, SQLITE_TRANSIENT);
				else
					sqlite3_bind_null(st, c + 1);
			}
			rc = sqlite3_step(st);
			if (rc != SQLITE_DONE) {
				snprintf(err, errlen, "sqlite insert row %d: %s", i + 1,
					 sqlite3_errmsg(db));
				sqlite3_finalize(st);
				sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
				buf_free(&sql);
				sqlite3_close(db);
				return -1;
			}
		}
		sqlite3_finalize(st);
	}
	buf_reset(&sql);
	if (t->schema.n > 0) {
		char idxname[200];
		snprintf(idxname, sizeof(idxname), "%s_idx", table);
		buf_puts(&sql, "CREATE INDEX IF NOT EXISTS ");
		quote_id(&sql, idxname);
		buf_puts(&sql, " ON ");
		quote_id(&sql, table);
		buf_puts(&sql, " (");
		quote_id(&sql, t->schema.name[0]);
		buf_puts(&sql, ");");
	}
	buf_puts(&sql, "COMMIT;");
	rc = sqlite3_exec(db, buf_str(&sql), NULL, NULL, &emsg);
	if (rc != SQLITE_OK) {
		snprintf(err, errlen, "sqlite commit: %s", emsg ? emsg : "?");
		sqlite3_free(emsg);
		buf_free(&sql);
		sqlite3_close(db);
		return -1;
	}
	buf_free(&sql);
	sqlite3_close(db);
	return 0;
}

int db_read_table(const char *dbpath, const char *table, Table *t,
		char *err, size_t errlen)
{
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	Buf q;
	int i, ncol;

	table_init(t);
	if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		snprintf(err, errlen, "cannot open %s", dbpath);
		if (db)
			sqlite3_close(db);
		return -1;
	}
	buf_init(&q);
	buf_puts(&q, "SELECT * FROM ");
	quote_id(&q, table);
	if (sqlite3_prepare_v2(db, buf_str(&q), -1, &st, NULL) != SQLITE_OK) {
		snprintf(err, errlen, "no such table: %s", table);
		buf_free(&q);
		sqlite3_close(db);
		return -1;
	}
	ncol = sqlite3_column_count(st);
	for (i = 0; i < ncol; i++) {
		t->schema.name = xrealloc(t->schema.name, sizeof(char *) * (size_t)(i + 1));
		t->schema.field = xrealloc(t->schema.field, sizeof(char *) * (size_t)(i + 1));
		t->schema.name[i] = xstrdup(sqlite3_column_name(st, i));
		t->schema.field[i] = NULL;
		t->schema.n = i + 1;
	}
	while (sqlite3_step(st) == SQLITE_ROW) {
		char **vals = xmalloc(sizeof(char *) * (size_t)ncol);

		for (i = 0; i < ncol; i++) {
			const unsigned char *v = sqlite3_column_text(st, i);

			vals[i] = (sqlite3_column_type(st, i) == SQLITE_NULL || !v) ? NULL :
				  xstrdup((const char *)v);
		}
		if (t->nrows == t->cap) {
			t->cap = t->cap ? t->cap * 2 : 512;
			t->cell = xrealloc(t->cell, sizeof(char **) * (size_t)t->cap);
		}
		t->cell[t->nrows++] = vals;
	}
	sqlite3_finalize(st);
	buf_free(&q);
	sqlite3_close(db);
	return 0;
}

int db_table_list(const char *dbpath, Buf *out, char *err, size_t errlen)
{
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	int first = 1;
	int rc;

	rc = sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL);
	if (rc != SQLITE_OK) {
		snprintf(err, errlen, "cannot open %s", dbpath);
		if (db)
			sqlite3_close(db);
		return -1;
	}
	buf_putc(out, '[');
	rc = sqlite3_prepare_v2(db,
		"SELECT name FROM sqlite_master WHERE type='table' "
		"AND name NOT LIKE 'sqlite_%' ORDER BY name", -1, &st, NULL);
	if (rc == SQLITE_OK) {
		while (sqlite3_step(st) == SQLITE_ROW) {
			const char *nm = (const char *)sqlite3_column_text(st, 0);
			Buf q;
			sqlite3_stmt *c2 = NULL;
			long long cnt = -1;

			buf_init(&q);
			buf_puts(&q, "SELECT COUNT(*) FROM ");
			quote_id(&q, nm ? nm : "");
			if (sqlite3_prepare_v2(db, buf_str(&q), -1, &c2, NULL) == SQLITE_OK &&
			    sqlite3_step(c2) == SQLITE_ROW)
				cnt = sqlite3_column_int64(c2, 0);
			buf_free(&q);
			if (c2)
				sqlite3_finalize(c2);
			if (!first)
				buf_putc(out, ',');
			first = 0;
			buf_puts(out, "{\"table\":\"");
			json_esc(out, nm);
			buf_printf(out, "\",\"rows\":%lld}", cnt);
		}
		sqlite3_finalize(st);
	}
	buf_putc(out, ']');
	sqlite3_close(db);
	return 0;
}

int db_preview(const char *dbpath, const char *table, int limit, int offset, Buf *out,
		char *err, size_t errlen)
{
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	Buf sql;
	int rc, ncol = 0, i;
	long long total = 0;

	if (sqlite3_open_v2(dbpath, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		snprintf(err, errlen, "cannot open %s", dbpath);
		if (db)
			sqlite3_close(db);
		return -1;
	}
	buf_init(&sql);
	buf_puts(&sql, "SELECT * FROM ");
	quote_id(&sql, table);
	if (limit > 0) {
		buf_printf(&sql, " LIMIT %d", limit);
		if (offset > 0)
			buf_printf(&sql, " OFFSET %d", offset);
	}
	rc = sqlite3_prepare_v2(db, buf_str(&sql), -1, &st, NULL);
	if (rc != SQLITE_OK) {
		snprintf(err, errlen, "no such table: %s", table);
		buf_free(&sql);
		sqlite3_close(db);
		return -1;
	}
	ncol = sqlite3_column_count(st);
	buf_puts(out, "\"columns\":[");
	for (i = 0; i < ncol; i++) {
		if (i)
			buf_putc(out, ',');
		buf_putc(out, '"');
		json_esc(out, sqlite3_column_name(st, i));
		buf_putc(out, '"');
	}
	buf_puts(out, "],\"rows\":[");
	i = 0;
	while (sqlite3_step(st) == SQLITE_ROW) {
		int c;

		if (i)
			buf_putc(out, ',');
		buf_putc(out, '[');
		for (c = 0; c < ncol; c++) {
			const unsigned char *v = sqlite3_column_text(st, c);
			if (c)
				buf_putc(out, ',');
			if (!v) {
				buf_puts(out, "null");
			} else {
				buf_putc(out, '"');
				json_esc(out, (const char *)v);
				buf_putc(out, '"');
			}
		}
		buf_putc(out, ']');
		i++;
	}
	buf_puts(out, "],\"shown\":");
	buf_printf(out, "%d", i);
	sqlite3_finalize(st);
	{
		Buf c2;
		buf_init(&c2);
		buf_puts(&c2, "SELECT COUNT(*) FROM ");
		quote_id(&c2, table);
		if (sqlite3_prepare_v2(db, buf_str(&c2), -1, &st, NULL) == SQLITE_OK &&
		    sqlite3_step(st) == SQLITE_ROW)
			total = sqlite3_column_int64(st, 0);
		if (st)
			sqlite3_finalize(st);
		buf_free(&c2);
	}
	buf_printf(out, ",\"total\":%lld,\"offset\":%d,\"limit\":%d", total,
		   offset > 0 ? offset : 0, limit > 0 ? limit : 0);
	buf_free(&sql);
	sqlite3_close(db);
	return 0;
}
