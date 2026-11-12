/* cgi.c - uhttpd CGI frontend (upload / merge / preview / download) */
#include "x2s.h"
#include <unistd.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>

#define X2S_BASE "/root/xlsx2sqlite"
#define X2S_MAX_UPLOAD (48u * 1024u * 1024u)
#define MAX_FILES 24

static const char *base_dir(void)
{
	const char *e = getenv("X2S_BASE");

	return (e && e[0] == '/') ? e : X2S_BASE;
}

static void hdr(const char *ctype, int code)
{
	printf("Status: %d\r\n", code);
	printf("Content-Type: %s\r\n", ctype);
	printf("Cache-Control: no-store\r\n");
	printf("X-Content-Type-Options: nosniff\r\n");
}

static void hdr_done(void)
{
	printf("\r\n");
}

static void json_end(void)
{
	printf("\r\n");
	fflush(stdout);
}

static void jerr(const char *msg)
{
	Buf b;

	hdr("application/json", 200);
	hdr_done();
	buf_init(&b);
	buf_puts(&b, "{\"ok\":false,\"error\":\"");
	json_esc(&b, msg);
	buf_puts(&b, "\"}");
	printf("%s", buf_str(&b));
	buf_free(&b);
	json_end();
}

static char *qget(const char *qs, const char *key)
{
	size_t kl = strlen(key);
	const char *p = qs;

	if (!qs)
		return NULL;
	while (*p) {
		const char *e = strchr(p, '&');
		size_t len = e ? (size_t)(e - p) : strlen(p);

		if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			const char *v = p + kl + 1;
			size_t vl = len - kl - 1;
			char *out = xmalloc(vl + 1);
			size_t i, o = 0;

			for (i = 0; i < vl; i++) {
				if (v[i] == '+') {
					out[o++] = ' ';
				} else if (v[i] == '%' && i + 2 < vl &&
					   isxdigit((unsigned char)v[i + 1]) &&
					   isxdigit((unsigned char)v[i + 2])) {
					char h[3];
					h[0] = v[i + 1];
					h[1] = v[i + 2];
					h[2] = 0;
					out[o++] = (char)strtol(h, NULL, 16);
					i += 2;
				} else {
					out[o++] = v[i];
				}
			}
			out[o] = 0;
			return out;
		}
		if (!e)
			break;
		p = e + 1;
	}
	return NULL;
}

static int path_ok(const char *p)
{
	const char *base = base_dir();
	size_t bl = strlen(base);

	if (!p || strncmp(p, base, bl) != 0)
		return 0;
	if (p[bl] != '/' && p[bl] != 0)
		return 0;
	if (strstr(p, ".."))
		return 0;
	if (strlen(p) >= 200)
		return 0;
	return 1;
}

static int ubus_session_ok(const char *sid)
{
	char cmd[256];
	FILE *fp;
	char buf[2048];
	size_t got;

	snprintf(cmd, sizeof(cmd),
		 "ubus call session get '{\"ubus_rpc_session\":\"%s\"}' 2>/dev/null", sid);
	fp = popen(cmd, "r");
	if (!fp)
		return 0;
	got = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[got] = 0;
	pclose(fp);
	return strstr(buf, "\"acls\"") != NULL || strstr(buf, "\"username\"") != NULL;
}

/* accept a hex session id, return 1 when it is well formed */
static int take_sid(const char *s, char *out, size_t outlen)
{
	size_t n = 0;

	if (!s)
		return 0;
	while (s[n] && n < outlen - 1) {
		if (!isxdigit((unsigned char)s[n]))
			break;
		out[n] = s[n];
		n++;
	}
	out[n] = 0;
	return n >= 16 && n <= 40;
}

/* uhttpd only forwards a fixed set of headers to CGI (no X-*), so the session
 * id travels in a cookie set by the page; the sysauth cookie is scoped to
 * /cgi-bin/luci and never reaches us. */
static int session_ok(void)
{
	const char *ck = getenv("HTTP_COOKIE");
	const char *p;
	char sid[65];

	if (getenv("X2S_NOAUTH") && !strcmp(getenv("X2S_NOAUTH"), "1"))
		return 1;
	if (take_sid(getenv("HTTP_X_X2S_SID"), sid, sizeof(sid)))
		return ubus_session_ok(sid);
	if (!ck)
		return 0;
	p = strstr(ck, "x2s_sid=");
	if (p && take_sid(p + 8, sid, sizeof(sid)))
		return ubus_session_ok(sid);
	p = strstr(ck, "sysauth=");
	if (p && take_sid(p + 8, sid, sizeof(sid)))
		return ubus_session_ok(sid);
	return 0;
}

/* read and drop the request body so uhttpd does not get EPIPE when we answer
 * before consuming it (otherwise the browser reports "Failed to fetch") */
static void drain_stdin(void)
{
	char b[8192];

	while (fread(b, 1, sizeof(b), stdin) > 0)
		;
}

/* ---------------- multipart ---------------- */
typedef struct {
	char *name;
	char *filename;
	char *value;
	size_t valuelen;
} Part;

static void parts_free(Part *p, int n)
{
	int i;

	if (!p)
		return;
	for (i = 0; i < n; i++) {
		free(p[i].name);
		free(p[i].filename);
		free(p[i].value);
	}
	free(p);
}

static int parse_multipart(const unsigned char *body, size_t len, const char *boundary,
		Part **parts_out, int *nparts_out, char *err, size_t errlen)
{
	char delim[160];
	size_t dl;
	Part *parts = NULL;
	int n = 0;
	size_t i;

	snprintf(delim, sizeof(delim), "--%s", boundary);
	dl = strlen(delim);

	for (i = 0; i + dl <= len; i++) {
		size_t hs, he, cs, ce, q;
		Part pt;

		if (memcmp(body + i, delim, dl))
			continue;
		memset(&pt, 0, sizeof(pt));
		q = i + dl;
		if (q + 2 <= len && body[q] == '-' && body[q + 1] == '-')
			break;
		while (q < len && (body[q] == '\r' || body[q] == '\n'))
			q++;
		hs = q;
		he = hs;
		while (he + 4 <= len) {
			if (body[he] == '\r' && body[he + 1] == '\n' &&
			    body[he + 2] == '\r' && body[he + 3] == '\n') {
				he += 4;
				break;
			}
			he++;
		}
		if (he + 4 > len)
			break;
		cs = he;
		ce = len;
		{
			size_t k = cs;
			while (k + dl + 2 <= len) {
				if (body[k] == '\r' && body[k + 1] == '\n' &&
				    !memcmp(body + k + 2, delim, dl)) {
					ce = k;
					break;
				}
				k++;
			}
		}
		{
			size_t k = hs;
			while (k + 10 < he) {
				if (!strncasecmp((const char *)body + k, "filename=\"", 10)) {
					size_t s = k + 10, e = s;
					while (e < he && body[e] != '"')
						e++;
					free(pt.filename);
					pt.filename = xstrndup((const char *)body + s, e - s);
					k += 10;
					continue;
				}
				if (!strncasecmp((const char *)body + k, "name=\"", 6)) {
					size_t s = k + 6, e = s;
					while (e < he && body[e] != '"')
						e++;
					free(pt.name);
					pt.name = xstrndup((const char *)body + s, e - s);
					k += 6;
					continue;
				}
				k++;
			}
		}
		pt.value = xstrndup((const char *)(body + cs), ce - cs);
		pt.valuelen = ce - cs;
		if (pt.name) {
			parts = xrealloc(parts, sizeof(Part) * (size_t)(n + 1));
			parts[n++] = pt;
		} else {
			free(pt.filename);
			free(pt.value);
		}
		if (ce < len) {
			i = ce + 1;
		} else {
			break;
		}
	}
	if (n == 0) {
		snprintf(err, errlen, "no multipart fields");
		return -1;
	}
	*parts_out = parts;
	*nparts_out = n;
	return 0;
}

static const char *part_get(Part *parts, int n, const char *name)
{
	int i;

	for (i = 0; i < n; i++)
		if (parts[i].name && !strcmp(parts[i].name, name)) {
			char *t = str_trim(parts[i].value);
			if (t != parts[i].value)
				memmove(parts[i].value, t, strlen(t) + 1);
			parts[i].valuelen = strlen(parts[i].value);
			return parts[i].value;
		}
	return NULL;
}

static char *safe_name(const char *fn)
{
	const char *b = strrchr(fn, '/');
	const char *c = strrchr(fn, '\\');
	char *out;
	size_t i, o = 0;

	if (c && (!b || c > b))
		b = c;
	b = b ? b + 1 : fn;
	out = xmalloc(strlen(b) + 8);
	for (i = 0; b[i]; i++) {
		unsigned char ch = (unsigned char)b[i];
		if (isalnum(ch) || ch == '.' || ch == '-' || ch == '_' || ch >= 0x80)
			out[o++] = (char)ch;
		else
			out[o++] = '_';
	}
	out[o] = 0;
	if (!out[0])
		strcpy(out, "upload");
	return out;
}

/* ---------------- actions ---------------- */
static void do_download(const char *qs)
{
	char *db = qget(qs, "db");
	FILE *f;
	char buf[8192];
	size_t n;
	const char *base;

	if (!db || !path_ok(db)) {
		jerr("bad db path");
		free(db);
		return;
	}
	f = fopen(db, "rb");
	if (!f) {
		jerr("cannot open database");
		free(db);
		return;
	}
	base = strrchr(db, '/');
	base = base ? base + 1 : db;
	hdr("application/octet-stream", 200);
	printf("Content-Disposition: attachment; filename=\"%s\"\r\n", base);
	printf("\r\n");
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
		fwrite(buf, 1, n, stdout);
	fclose(f);
	fflush(stdout);
	free(db);
}

static void json_owned(Buf *out)
{
	printf("%s", buf_str(out));
	buf_free(out);
}

static void do_list(const char *qs)
{
	char *db = qget(qs, "db");
	Buf out;
	char err[256] = "";

	if (!db || !path_ok(db)) {
		jerr("bad db path");
		free(db);
		return;
	}
	buf_init(&out);
	if (db_table_list(db, &out, err, sizeof(err)) != 0) {
		jerr(err);
		buf_free(&out);
		free(db);
		return;
	}
	hdr("application/json", 200);
	hdr_done();
	printf("{\"ok\":true,\"db\":\"");
	{
		Buf b;
		buf_init(&b);
		json_esc(&b, db);
		printf("%s", buf_str(&b));
		buf_free(&b);
	}
	printf("\",\"tables\":%s}", buf_str(&out));
	buf_free(&out);
	free(db);
	json_end();
}

static void do_preview(const char *qs)
{
	char *db = qget(qs, "db");
	char *tb = qget(qs, "table");
	char *lim = qget(qs, "limit");
	char *off = qget(qs, "offset");
	Buf out;
	char err[256] = "";
	int limit = lim ? atoi(lim) : 20;
	int offset = off ? atoi(off) : 0;

	if (!db || !path_ok(db) || !tb) {
		jerr("bad request");
		free(db);
		free(tb);
		free(lim);
		free(off);
		return;
	}
	if (limit <= 0 || limit > 500)
		limit = 20;
	if (offset < 0)
		offset = 0;
	buf_init(&out);
	if (db_preview(db, tb, limit, offset, &out, err, sizeof(err)) != 0) {
		jerr(err);
		buf_free(&out);
		free(db);
		free(tb);
		free(lim);
		free(off);
		return;
	}
	hdr("application/json", 200);
	hdr_done();
	printf("{\"ok\":true,");
	json_owned(&out);
	printf("}");
	free(db);
	free(tb);
	free(lim);
	free(off);
	json_end();
}

/* action=del&db=..  -> remove one database file (plus its -wal/-shm/-journal) */
static void do_delete(const char *qs)
{
	char *db = qget(qs, "db");
	Buf out, aux;
	struct stat st;

	if (!db || !path_ok(db)) {
		jerr("bad request");
		free(db);
		return;
	}
	if (stat(db, &st) != 0) {
		jerr("no such database");
		free(db);
		return;
	}
	if (!S_ISREG(st.st_mode)) {
		jerr("not a regular file");
		free(db);
		return;
	}
	if (unlink(db) != 0) {
		char msg[600];

		snprintf(msg, sizeof(msg), "delete failed: %s", strerror(errno));
		jerr(msg);
		free(db);
		return;
	}
	buf_init(&aux);
	buf_printf(&aux, "%s-wal", db);
	unlink(buf_str(&aux));
	buf_reset(&aux);
	buf_printf(&aux, "%s-shm", db);
	unlink(buf_str(&aux));
	buf_reset(&aux);
	buf_printf(&aux, "%s-journal", db);
	unlink(buf_str(&aux));
	buf_free(&aux);

	buf_init(&out);
	buf_puts(&out, "{\"ok\":true,\"deleted\":\"");
	json_esc(&out, db);
	buf_puts(&out, "\"}");
	hdr("application/json", 200);
	hdr_done();
	printf("%s", buf_str(&out));
	buf_free(&out);
	free(db);
	json_end();
}

/* action=export&db=..&table=..&fmt=xlsx|sql  -> file download */
static void do_export(const char *qs)
{
	char *db = qget(qs, "db");
	char *tb = qget(qs, "table");
	char *fmt = qget(qs, "fmt");
	int as_sql = fmt && !strcmp(fmt, "sql");
	int as_csv = fmt && !strcmp(fmt, "csv");
	char tmp[256];
	char fname[300];
	char err[512] = "";
	int i, o = 0;
	FILE *f;

	if (!db || !path_ok(db) || !tb || !tb[0]) {
		jerr("bad request");
		free(db);
		free(tb);
		free(fmt);
		return;
	}
	snprintf(tmp, sizeof(tmp), "/tmp/x2s-exp-%d.%s", (int)getpid(),
		 as_sql ? "sql" : as_csv ? "csv" : "xlsx");
	if (as_csv) {
		Table t;

		if (db_read_table(db, tb, &t, err, sizeof(err)) != 0) {
			jerr(err);
			free(db);
			free(tb);
			free(fmt);
			return;
		}
		if (csv_write(&t, tmp, 1, err, sizeof(err)) != 0) {
			jerr(err);
			table_free(&t);
			free(db);
			free(tb);
			free(fmt);
			return;
		}
		table_free(&t);
	} else if (as_sql) {
		f = fopen(tmp, "wb");
		if (!f) {
			jerr("cannot write temporary file");
			free(db);
			free(tb);
			free(fmt);
			return;
		}
		if (db_export_sql(db, tb, f, err, sizeof(err)) != 0) {
			fclose(f);
			jerr(err);
			free(db);
			free(tb);
			free(fmt);
			return;
		}
		fclose(f);
	} else {
		Table t;

		if (db_read_table(db, tb, &t, err, sizeof(err)) != 0) {
			jerr(err);
			free(db);
			free(tb);
			free(fmt);
			return;
		}
		if (xlsx_write(&t, tmp, tb, err, sizeof(err)) != 0) {
			jerr(err);
			table_free(&t);
			free(db);
			free(tb);
			free(fmt);
			return;
		}
		table_free(&t);
	}
	for (i = 0; tb[i] && o < 200; i++) {
		unsigned char ch = (unsigned char)tb[i];

		fname[o++] = (isalnum(ch) || ch == '-' || ch == '_' || ch >= 0x80) ? (char)ch : '_';
	}
	snprintf(fname + o, sizeof(fname) - (size_t)o, ".%s",
		 as_sql ? "sql" : as_csv ? "csv" : "xlsx");
	f = fopen(tmp, "rb");
	if (!f) {
		jerr("cannot read temporary file");
		free(db);
		free(tb);
		free(fmt);
		return;
	}
	hdr(as_sql ? "text/plain; charset=utf-8" : as_csv ? "text/csv; charset=utf-8" :
	    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", 200);
	printf("Content-Disposition: attachment; filename=\"%s\"\r\n", fname);
	printf("\r\n");
	{
		char buf[16384];
		size_t n;

		while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
			fwrite(buf, 1, n, stdout);
	}
	fclose(f);
	fflush(stdout);
	unlink(tmp);
	free(db);
	free(tb);
	free(fmt);
}

static void do_dbs(void)
{
	DIR *d = opendir(base_dir());
	Buf out;
	int first = 1;

	buf_init(&out);
	buf_putc(&out, '[');
	if (d) {
		struct dirent *e;

		while ((e = readdir(d))) {
			char path[512];
			struct stat st;
			size_t l = strlen(e->d_name);

			if (l < 4 || strcmp(e->d_name + l - 3, ".db"))
				continue;
			snprintf(path, sizeof(path), "%s/%s", base_dir(), e->d_name);
			if (stat(path, &st) != 0)
				continue;
			if (!first)
				buf_putc(&out, ',');
			first = 0;
			buf_puts(&out, "{\"name\":\"");
			json_esc(&out, e->d_name);
			buf_printf(&out, "\",\"size\":%ld,\"mtime\":%ld,\"tables\":",
				   (long)st.st_size, (long)st.st_mtime);
			{
				Buf tb;
				char terr[128] = "";

				buf_init(&tb);
				if (db_table_list(path, &tb, terr, sizeof(terr)) == 0)
					buf_add(&out, tb.p, tb.n);
				else
					buf_puts(&out, "[]");
				buf_free(&tb);
			}
			buf_putc(&out, '}');
		}
		closedir(d);
	}
	buf_putc(&out, ']');
	hdr("application/json", 200);
	hdr_done();
	printf("{\"ok\":true,\"dir\":\"%s\",\"dbs\":%s}", base_dir(), buf_str(&out));
	buf_free(&out);
	json_end();
}

static void tmp_cleanup(char **files, int nfiles, const char *tmpdir)
{
	int i;

	for (i = 0; i < nfiles; i++) {
		unlink(files[i]);
		free(files[i]);
	}
	if (tmpdir)
		rmdir(tmpdir);
}

static void do_merge(const char *ctype)
{
	const char *bl;
	unsigned char *body = NULL;
	size_t len = 0, cap = 0, got;
	long clen = 0;
	const char *cls = getenv("CONTENT_LENGTH");
	char tmpdir[128];
	Part *parts = NULL;
	int nparts = 0;
	char err[512] = "";
	char *dbv, *tbv, *ddv, *hrv;
	const char *dbpath;
	const char *table;
	int dedupe = 0, header_row = 0;
	char *files[MAX_FILES];
	int nfiles = 0;
	Sheet *sheets = NULL;
	int nsheets = 0;
	Table t;
	int i, k;

	memset(files, 0, sizeof(files));
	bl = strstr(ctype ? ctype : "", "boundary=");
	if (!bl) {
		drain_stdin();
		jerr("expect multipart/form-data");
		return;
	}
	bl += 9;
	if (cls)
		clen = atol(cls);
	if (clen > (long)X2S_MAX_UPLOAD) {
		drain_stdin();
		hdr("application/json", 413);
		hdr_done();
		printf("{\"ok\":false,\"error\":\"upload too large (limit 48MB)\"}");
		json_end();
		return;
	}
	cap = 65536;
	body = xmalloc(cap);
	while ((got = fread(body + len, 1, cap - len, stdin)) > 0) {
		len += got;
		if (len + 1 >= cap) {
			if (cap >= (size_t)X2S_MAX_UPLOAD) {
				free(body);
				jerr("upload too large");
				return;
			}
			cap *= 2;
			body = xrealloc(body, cap);
		}
	}
	if (len == 0) {
		free(body);
		jerr("empty upload");
		return;
	}
	if (parse_multipart(body, len, bl, &parts, &nparts, err, sizeof(err)) != 0) {
		free(body);
		jerr(err);
		return;
	}
	dbv = (char *)part_get(parts, nparts, "db");
	tbv = (char *)part_get(parts, nparts, "table");
	ddv = (char *)part_get(parts, nparts, "dedupe");
	hrv = (char *)part_get(parts, nparts, "header_row");
	{
		static char defdb[512];
		snprintf(defdb, sizeof(defdb), "%s/merge.db", base_dir());
		dbpath = (dbv && dbv[0]) ? dbv : defdb;
	}
	table = (tbv && tbv[0]) ? tbv : "questions";
	if (!path_ok(dbpath)) {
		parts_free(parts, nparts);
		free(body);
		jerr("bad db path (must be under the output directory)");
		return;
	}
	if (ddv && !strcmp(ddv, "off"))
		dedupe = 2;
	if (hrv && atoi(hrv) > 0)
		header_row = atoi(hrv);
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/x2s-%d", (int)getpid());
	mkpath(tmpdir);
	for (i = 0; i < nparts && nfiles < MAX_FILES; i++) {
		char *nm;
		FILE *f;
		char path[512];

		if (!parts[i].filename || !parts[i].filename[0])
			continue;
		nm = safe_name(parts[i].filename);
		snprintf(path, sizeof(path), "%s/%d-%s", tmpdir, nfiles, nm);
		f = fopen(path, "wb");
		if (!f) {
			free(nm);
			continue;
		}
		if (parts[i].valuelen)
			fwrite(parts[i].value, 1, parts[i].valuelen, f);
		fclose(f);
		files[nfiles++] = xstrdup(path);
		free(nm);
	}
	if (nfiles == 0) {
		parts_free(parts, nparts);
		free(body);
		jerr("no excel files uploaded");
		return;
	}
	for (i = 0; i < nfiles; i++) {
		Sheet *sh = NULL;
		int n = 0;

		if (read_any(files[i], &sh, &n, err, sizeof(err)) != 0) {
			char msg[600];

			snprintf(msg, sizeof(msg), "%s: %s", files[i] + strlen(tmpdir) + 1, err);
			for (k = 0; k < nsheets; k++)
				sheet_free(&sheets[k]);
			free(sheets);
			tmp_cleanup(files, nfiles, tmpdir);
			parts_free(parts, nparts);
			free(body);
			jerr(msg);
			return;
		}
		sheets = xrealloc(sheets, sizeof(Sheet) * (size_t)(nsheets + n));
		for (k = 0; k < n; k++)
			sheets[nsheets + k] = sh[k];
		nsheets += n;
		free(sh);
	}
	if (table_build(&t, sheets, nsheets, NULL, header_row, dedupe, err, sizeof(err)) != 0) {
		tmp_cleanup(files, nfiles, tmpdir);
		free(sheets);
		parts_free(parts, nparts);
		free(body);
		jerr(err);
		return;
	}
	if (table_write_sqlite(&t, dbpath, table, err, sizeof(err)) != 0) {
		tmp_cleanup(files, nfiles, tmpdir);
		table_free(&t);
		for (k = 0; k < nsheets; k++)
			sheet_free(&sheets[k]);
		free(sheets);
		parts_free(parts, nparts);
		free(body);
		jerr(err);
		return;
	}
	{
		Buf b;
		struct stat st;
		long dbsize = 0;
		int c;

		if (stat(dbpath, &st) == 0)
			dbsize = (long)st.st_size;
		hdr("application/json", 200);
		hdr_done();
		buf_init(&b);
		buf_puts(&b, "{\"ok\":true,\"db\":\"");
		json_esc(&b, dbpath);
		buf_puts(&b, "\",\"table\":\"");
		json_esc(&b, table);
		buf_printf(&b, "\",\"rows\":%d,\"in_rows\":%d,\"dropped\":%d,\"files\":%d"
			   ",\"size\":%ld,\"columns\":[", t.nrows, t.in_rows, t.dup_dropped,
			   nfiles, dbsize);
		for (c = 0; c < t.schema.n; c++) {
			if (c)
				buf_putc(&b, ',');
			buf_putc(&b, '"');
			json_esc(&b, t.schema.name[c]);
			buf_putc(&b, '"');
		}
		buf_printf(&b, "],\"stat\":\"%s\"}", t.msg);
		json_owned(&b);
		json_end();
	}
	tmp_cleanup(files, nfiles, tmpdir);
	table_free(&t);
	for (k = 0; k < nsheets; k++)
		sheet_free(&sheets[k]);
	free(sheets);
	parts_free(parts, nparts);
	free(body);
}

int cgi_main(void)
{
	const char *method = getenv("REQUEST_METHOD");
	const char *qs = getenv("QUERY_STRING");
	char *action;

	if (!method)
		return 1;
	if (!session_ok()) {
		if (!strcmp(method, "POST"))
			drain_stdin();
		hdr("application/json", 403);
		hdr_done();
		printf("{\"ok\":false,\"error\":\"not logged in\"}");
		json_end();
		return 0;
	}
	action = qget(qs, "action");
	if (!action)
		action = xstrdup(!strcmp(method, "POST") ? "merge" : "ping");
	if (!strcmp(action, "ping")) {
		hdr("application/json", 200);
		hdr_done();
		printf("{\"ok\":true,\"version\":\"" X2S_VERSION "\",\"base\":\"%s\"}", base_dir());
		json_end();
	} else if (!strcmp(action, "dbs")) {
		do_dbs();
	} else if (!strcmp(action, "tables")) {
		do_list(qs);
	} else if (!strcmp(action, "preview")) {
		do_preview(qs);
	} else if (!strcmp(action, "download")) {
		do_download(qs);
	} else if (!strcmp(action, "export")) {
		do_export(qs);
	} else if (!strcmp(action, "del")) {
		do_delete(qs);
	} else if (!strcmp(action, "merge")) {
		do_merge(getenv("CONTENT_TYPE"));
	} else {
		jerr("unknown action");
	}
	free(action);
	return 0;
}
