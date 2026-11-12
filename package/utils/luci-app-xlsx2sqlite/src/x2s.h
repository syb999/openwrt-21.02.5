/* x2s.h - shared declarations */
#ifndef X2S_H
#define X2S_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#define X2S_VERSION "1.2.9"

/* hard limits so a crafted sheet/xml cannot make us allocate absurd grids */
#define X2S_MAX_ROWS 500000
#define X2S_MAX_COLS 1024
#define X2S_MAX_CELL (4u * 1024u * 1024u)
#define X2S_MAX_INFLATE (64u * 1024u * 1024u)

/* ---------- growable buffer ---------- */
typedef struct {
	char *p;
	size_t n;
	size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reset(Buf *b);
void buf_add(Buf *b, const void *data, size_t len);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, int c);
void buf_printf(Buf *b, const char *fmt, ...);
char *buf_detach(Buf *b);
const char *buf_str(Buf *b);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---------- text helpers ---------- */
char *str_trim(char *s);
int str_ieq(const char *a, const char *b);
int str_is_empty(const char *s);

/* append s (UTF-8) to out, escaping for JSON, no surrounding quotes */
void json_esc(Buf *out, const char *s);
void json_esc_n(Buf *out, const char *s, size_t n);

/* utf16le -> utf8 ; nchars = number of UTF-16 code units */
void utf16le_to_utf8(Buf *out, const uint8_t *s, size_t nchars);
/* single code point -> utf8 */
void utf8_put(Buf *out, uint32_t cp);
/* latin1 -> utf8 */
void latin1_to_utf8(Buf *out, const uint8_t *s, size_t n);
/* gbk/gb18030 byte pair expansion to utf8. returns bytes consumed */
size_t gbk_to_utf8(Buf *out, const uint8_t *s, size_t n);
/* 1 if the buffer is valid utf-8 */
int utf8_valid(const uint8_t *s, size_t n);
/* normalize an utf-8 string for header comparison (in place), returns s */
char *hdr_normalize(char *s);

/* ---------- sheet model ---------- */
typedef struct {
	char **v;	/* cells, utf-8, may be NULL for empty */
	int n;
} Row;

typedef struct {
	char *name;
	Row *rows;
	int nrows;
	int cap;
	int maxcols;
} Sheet;

void sheet_init(Sheet *s, const char *name);
void sheet_free(Sheet *s);
void sheet_set(Sheet *s, int row, int col, const char *text);
const char *sheet_get(const Sheet *s, int row, int col);

/* ---------- readers ---------- */
enum { FMT_UNKNOWN = 0, FMT_XLSX, FMT_XLS, FMT_CSV, FMT_SQL };
int file_format(const char *path);

/* returns 0 on success; on error returns -1 and fills err */
int xlsx_read(const char *path, Sheet **out, int *nout, char *err, size_t errlen);
int xls_read(const char *path, Sheet **out, int *nout, char *err, size_t errlen);
int csv_read(const char *path, Sheet **out, int *nout, char *err, size_t errlen);
int read_any(const char *path, Sheet **out, int *nout, char *err, size_t errlen);
int sql_read(const char *path, Sheet **out, int *nout, char *err, size_t errlen);

/* read whole file into memory */
int slurp(const char *path, unsigned char **data, size_t *len, char *err, size_t errlen);

/* ---------- zip ---------- */
typedef struct {
	char *name;
	uint32_t csize;
	uint32_t usize;
	uint32_t off;
	uint16_t method;
} ZEntry;

typedef struct {
	unsigned char *data;
	size_t len;
	ZEntry *e;
	int n;
} Zip;

int zip_open(Zip *z, unsigned char *data, size_t len, char *err, size_t errlen);
void zip_close(Zip *z);
int zip_extract(Zip *z, const char *name, unsigned char **out, size_t *outlen);
int zip_extract_soft(Zip *z, const char *name, unsigned char **out, size_t *outlen);

/* ---------- merge ---------- */
typedef struct {
	char **name;		/* output column names */
	char **field;		/* canonical field id or NULL */
	int n;
} Schema;

typedef struct {
	Schema schema;
	char ***cell;		/* rows x n cols, utf-8 or NULL */
	int nrows;
	int cap;
	int in_rows;
	int dup_kept;
	int dup_dropped;
	char msg[512];
} Table;

void table_init(Table *t);
void table_free(Table *t);
/* dedupe: 0 exact, 1 normalized, 2 off */
int table_build(Table *t, Sheet *sheets, int nsheets, const char *refhdr,
		int header_row, int dedupe, char *err, size_t errlen);
int table_write_sqlite(Table *t, const char *dbpath, const char *table,
		char *err, size_t errlen);
int mkpath(const char *path);

/* write / export */
int xlsx_write(const Table *t, const char *path, const char *sheetname,
		char *err, size_t errlen);
int db_read_table(const char *dbpath, const char *table, Table *t,
		char *err, size_t errlen);
int db_export_sql(const char *dbpath, const char *table, FILE *out,
		char *err, size_t errlen);
int csv_write(const Table *t, const char *path, int bom, char *err, size_t errlen);

/* sqlite read helpers */
int db_table_list(const char *dbpath, Buf *out, char *err, size_t errlen);
int db_preview(const char *dbpath, const char *table, int limit, int offset, Buf *out,
		char *err, size_t errlen);

#endif
