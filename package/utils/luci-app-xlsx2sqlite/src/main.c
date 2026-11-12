/* main.c - CLI frontend */
#include "x2s.h"
#include <strings.h>

int cgi_main(void);
int mkpath(const char *path);

static const char *str_istr(const char *h, const char *n)
{
	size_t nl = strlen(n);
	const char *p;

	for (p = h; *p; p++)
		if (!strncasecmp(p, n, nl))
			return p;
	return NULL;
}

/* sql text dumps start with comments or DDL; a plain csv must not match */
static int sniff_sql(const char *path)
{
	FILE *f = fopen(path, "rb");
	char buf[1024];
	size_t n;

	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (str_istr(buf, "insert into") || str_istr(buf, "create table") ||
	    str_istr(buf, "pragma ") || str_istr(buf, "begin transaction") ||
	    str_istr(buf, "/*!40"))
		return 1;
	return 0;
}

int file_format(const char *path)
{
	FILE *f = fopen(path, "rb");
	unsigned char h[8];
	size_t n;
	size_t l = strlen(path);

	if (!f)
		return FMT_UNKNOWN;
	n = fread(h, 1, sizeof(h), f);
	fclose(f);
	if (n >= 4 && h[0] == 'P' && h[1] == 'K' && (h[2] == 3 || h[2] == 5 || h[2] == 7))
		return FMT_XLSX;
	if (n >= 8 && h[0] == 0xD0 && h[1] == 0xCF && h[2] == 0x11 && h[3] == 0xE0)
		return FMT_XLS;
	if (l > 4 && !strcasecmp(path + l - 4, ".sql"))
		return FMT_SQL;
	if (sniff_sql(path))
		return FMT_SQL;
	return FMT_CSV;
}

int read_any(const char *path, Sheet **out, int *nout, char *err, size_t errlen)
{
	int fmt = file_format(path);

	switch (fmt) {
	case FMT_SQL:
		return sql_read(path, out, nout, err, errlen);
	case FMT_XLSX:
		return xlsx_read(path, out, nout, err, errlen);
	case FMT_XLS:
		return xls_read(path, out, nout, err, errlen);
	default:
		return csv_read(path, out, nout, err, errlen);
	}
}

static void usage(void)
{
	fprintf(stderr,
		"xlsx2sqlite " X2S_VERSION "\n"
		"usage:\n"
		"  xlsx2sqlite info <file...>\n"
		"  xlsx2sqlite merge [-o db] [-t table] [--dedupe exact|off]\n"
		"                    [--header-row N] <file...>\n"
		"  xlsx2sqlite export -d <db> -t <table> [-o out.xlsx]\n"
		"  xlsx2sqlite export-sql -d <db> -t <table> [-o out.sql]\n"
		"  xlsx2sqlite export-csv -d <db> -t <table> [-o out.csv]\n"
		"  xlsx2sqlite tables -d <db>\n"
		"  xlsx2sqlite dump -d <db> -t <table> [--limit N]\n"
		"  xlsx2sqlite version\n");
}

/* fmt: 0 = xlsx, 1 = .sql, 2 = .csv */
static int cmd_export(char **argv, int argc, int fmt)
{
	int as_sql = (fmt == 1);
	const char *db = NULL, *tb = "questions", *outp = NULL;
	char defpath[600];
	int i;
	char err[512] = "";
	Table t;
	FILE *f;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			db = argv[++i];
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			tb = argv[++i];
		else if (!strcmp(argv[i], "-o") && i + 1 < argc)
			outp = argv[++i];
	}
	if (!db) {
		fprintf(stderr, "{\"ok\":false,\"error\":\"missing -d <db>\"}\n");
		return 1;
	}
	if (!outp) {
		const char *slash = strrchr(db, '/');
		size_t dl = slash ? (size_t)(slash - db) : 0;
		const char *ext = (fmt == 1) ? "sql" : (fmt == 2) ? "csv" : "xlsx";

		snprintf(defpath, sizeof(defpath), "%.*s/%s.%s", (int)dl, db, tb, ext);
		outp = defpath;
	}
	if (as_sql) {
		long bytes;

		f = fopen(outp, "wb");
		if (!f) {
			fprintf(stderr, "{\"ok\":false,\"error\":\"cannot write %s\"}\n", outp);
			return 1;
		}
		if (db_export_sql(db, tb, f, err, sizeof(err)) != 0) {
			fclose(f);
			fprintf(stderr, "{\"ok\":false,\"error\":\"%s\"}\n", err);
			return 1;
		}
		bytes = ftell(f);
		fclose(f);
		if (bytes < 0)
			bytes = 0;
		printf("{\"ok\":true,\"file\":\"%s\",\"bytes\":%ld}\n", outp, bytes);
		return 0;
	}
	if (db_read_table(db, tb, &t, err, sizeof(err)) != 0) {
		fprintf(stderr, "{\"ok\":false,\"error\":\"%s\"}\n", err);
		return 1;
	}
	if (fmt == 2) {
		if (csv_write(&t, outp, 1, err, sizeof(err)) != 0) {
			fprintf(stderr, "{\"ok\":false,\"error\":\"%s\"}\n", err);
			table_free(&t);
			return 1;
		}
		printf("{\"ok\":true,\"file\":\"%s\",\"rows\":%d,\"cols\":%d}\n", outp,
		       t.nrows, t.schema.n);
		table_free(&t);
		return 0;
	}
	if (xlsx_write(&t, outp, tb, err, sizeof(err)) != 0) {
		fprintf(stderr, "{\"ok\":false,\"error\":\"%s\"}\n", err);
		table_free(&t);
		return 1;
	}
	printf("{\"ok\":true,\"file\":\"%s\",\"rows\":%d,\"cols\":%d}\n", outp, t.nrows,
	       t.schema.n);
	table_free(&t);
	return 0;
}

static const char *fmt_name(int f)
{
	return f == FMT_XLSX ? "xlsx" : f == FMT_XLS ? "xls" :
	       f == FMT_SQL ? "sql" : "csv";
}

static int cmd_info(char **argv, int argc)
{
	Buf out;
	int a;
	int ok = 0;

	buf_init(&out);
	buf_puts(&out, "[");
	for (a = 0; a < argc; a++) {
		Sheet *sh = NULL;
		int n = 0;
		char err[256] = "";
		int i;

		if (a)
			buf_putc(&out, ',');
		if (read_any(argv[a], &sh, &n, err, sizeof(err)) != 0) {
			buf_puts(&out, "{\"file\":\"");
			json_esc(&out, argv[a]);
			buf_puts(&out, "\",\"ok\":false,\"error\":\"");
			json_esc(&out, err);
			buf_puts(&out, "\"}");
			continue;
		}
		ok++;
		buf_puts(&out, "{\"file\":\"");
		json_esc(&out, argv[a]);
		buf_printf(&out, "\",\"ok\":true,\"format\":\"%s\",\"sheets\":[",
			   fmt_name(file_format(argv[a])));
		for (i = 0; i < n; i++) {
			int c;

			if (i)
				buf_putc(&out, ',');
			buf_puts(&out, "{\"name\":\"");
			json_esc(&out, sh[i].name);
			buf_printf(&out, "\",\"rows\":%d,\"cols\":%d,\"head\":[",
				   sh[i].nrows, sh[i].maxcols);
			if (sh[i].nrows > 0) {
				const Row *r = &sh[i].rows[0];
				for (c = 0; c < r->n; c++) {
					if (c)
						buf_putc(&out, ',');
					buf_putc(&out, '"');
					json_esc(&out, r->v[c]);
					buf_putc(&out, '"');
				}
			}
			buf_puts(&out, "]}");
		}
		buf_puts(&out, "]}");
		for (i = 0; i < n; i++)
			sheet_free(&sh[i]);
		free(sh);
	}
	buf_puts(&out, "]");
	printf("%s\n", buf_str(&out));
	buf_free(&out);
	return ok ? 0 : 1;
}

static int cmd_merge(char **argv, int argc)
{
	const char *dbpath = "/root/xlsx2sqlite/merge.db";
	const char *table = "questions";
	int dedupe = 0;
	int header_row = 0;
	int i;
	char **files = xmalloc(sizeof(char *) * (size_t)(argc ? argc : 1));
	int nfiles = 0;
	Sheet *sheets = NULL;
	int nsheets = 0;
	char err[512] = "";
	Table t;

	for (i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc)
			dbpath = argv[++i];
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			table = argv[++i];
		else if (!strcmp(argv[i], "--dedupe") && i + 1 < argc) {
			const char *v = argv[++i];
			dedupe = !strcmp(v, "off") ? 2 : 0;
		} else if (!strcmp(argv[i], "--header-row") && i + 1 < argc)
			header_row = atoi(argv[++i]);
		else
			files[nfiles++] = argv[i];
	}
	if (nfiles == 0) {
		fprintf(stderr, "{\"ok\":false,\"error\":\"no input files\"}\n");
		free(files);
		return 1;
	}
	for (i = 0; i < nfiles; i++) {
		Sheet *sh = NULL;
		int n = 0;
		int k;

		if (read_any(files[i], &sh, &n, err, sizeof(err)) != 0) {
			Buf b;

			buf_init(&b);
			buf_printf(&b, "{\"ok\":false,\"file\":\"%s\",\"error\":\"", files[i]);
			json_esc(&b, err);
			buf_puts(&b, "\"}\n");
			fputs(buf_str(&b), stderr);
			buf_free(&b);
			for (k = 0; k < nsheets; k++)
				sheet_free(&sheets[k]);
			free(sheets);
			free(files);
			return 1;
		}
		sheets = xrealloc(sheets, sizeof(Sheet) * (size_t)(nsheets + n));
		for (k = 0; k < n; k++)
			sheets[nsheets + k] = sh[k];
		nsheets += n;
		free(sh);
	}
	if (table_build(&t, sheets, nsheets, NULL, header_row, dedupe, err, sizeof(err)) != 0) {
		Buf b;
		buf_init(&b);
		buf_printf(&b, "{\"ok\":false,\"error\":\"%s\"}\n", err);
		fprintf(stderr, "%s", buf_str(&b));
		buf_free(&b);
		{
			int k;
			for (k = 0; k < nsheets; k++)
				sheet_free(&sheets[k]);
			free(sheets);
			free(files);
		}
		return 1;
	}
	if (table_write_sqlite(&t, dbpath, table, err, sizeof(err)) != 0) {
		fprintf(stderr, "{\"ok\":false,\"error\":\"");
		{
			Buf b;
			buf_init(&b);
			json_esc(&b, err);
			fprintf(stderr, "%s", buf_str(&b));
			buf_free(&b);
		}
		fprintf(stderr, "\"}\n");
		table_free(&t);
		{
			int k;
			for (k = 0; k < nsheets; k++)
				sheet_free(&sheets[k]);
			free(sheets);
			free(files);
		}
		return 1;
	}
	{
		Buf b;
		int c;

		buf_init(&b);
		buf_puts(&b, "{\"ok\":true,\"db\":\"");
		json_esc(&b, dbpath);
		buf_puts(&b, "\",\"table\":\"");
		json_esc(&b, table);
		buf_printf(&b, "\",\"rows\":%d,\"in_rows\":%d,\"dropped\":%d,\"columns\":[",
			   t.nrows, t.in_rows, t.dup_dropped);
		for (c = 0; c < t.schema.n; c++) {
			if (c)
				buf_putc(&b, ',');
			buf_putc(&b, '"');
			json_esc(&b, t.schema.name[c]);
			buf_putc(&b, '"');
		}
		buf_printf(&b, "],\"stat\":\"%s\"}\n", t.msg);
		printf("%s", buf_str(&b));
		buf_free(&b);
	}
	table_free(&t);
	{
		int k;
		for (k = 0; k < nsheets; k++)
			sheet_free(&sheets[k]);
		free(sheets);
		free(files);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (getenv("REQUEST_METHOD"))
		return cgi_main();
	if (argc < 2) {
		usage();
		return 1;
	}
	if (!strcmp(argv[1], "version") || !strcmp(argv[1], "-V")) {
		printf("xlsx2sqlite " X2S_VERSION "\n");
		return 0;
	}
	if (!strcmp(argv[1], "info"))
		return cmd_info(argv + 2, argc - 2);
	if (!strcmp(argv[1], "merge"))
		return cmd_merge(argv + 2, argc - 2);
	if (!strcmp(argv[1], "export"))
		return cmd_export(argv + 2, argc - 2, 0);
	if (!strcmp(argv[1], "export-sql"))
		return cmd_export(argv + 2, argc - 2, 1);
	if (!strcmp(argv[1], "export-csv"))
		return cmd_export(argv + 2, argc - 2, 2);
	if (!strcmp(argv[1], "tables") || !strcmp(argv[1], "dump")) {
		const char *db = NULL, *tb = "questions";
		int limit = 50, offset = 0;
		int i;
		Buf out;
		char err[256] = "";

		for (i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "-d") && i + 1 < argc)
				db = argv[++i];
			else if (!strcmp(argv[i], "-t") && i + 1 < argc)
				tb = argv[++i];
			else if (!strcmp(argv[i], "--limit") && i + 1 < argc)
				limit = atoi(argv[++i]);
			else if (!strcmp(argv[i], "--offset") && i + 1 < argc)
				offset = atoi(argv[++i]);
		}
		if (!db) {
			fprintf(stderr, "{\"ok\":false,\"error\":\"missing -d <db>\"}\n");
			return 1;
		}
		buf_init(&out);
		if (!strcmp(argv[1], "tables")) {
			if (db_table_list(db, &out, err, sizeof(err)) != 0) {
				printf("{\"ok\":false,\"error\":\"%s\"}\n", err);
				buf_free(&out);
				return 1;
			}
			printf("{\"ok\":true,\"tables\":%s}\n", buf_str(&out));
		} else {
			if (db_preview(db, tb, limit, offset, &out, err, sizeof(err)) != 0) {
				printf("{\"ok\":false,\"error\":\"%s\"}\n", err);
				buf_free(&out);
				return 1;
			}
			printf("{\"ok\":true,");
			printf("%s", buf_str(&out));
			printf("}\n");
		}
		buf_free(&out);
		return 0;
	}
	usage();
	return 1;
}
