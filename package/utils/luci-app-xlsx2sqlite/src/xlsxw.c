/* xlsxw.c - minimal xlsx writer (zip + inline strings) */
#include "x2s.h"
#include <zlib.h>
#include <sys/stat.h>
#include <errno.h>

typedef struct {
	char *name;
	unsigned char *cdata;
	size_t csize;
	unsigned char *udata;
	size_t usize;
	uint32_t crc;
	uint16_t method;
	uint32_t off;
} WEntry;

typedef struct {
	WEntry *e;
	int n;
	int cap;
	Buf out;
} ZipW;

static void put16(Buf *b, uint32_t v)
{
	unsigned char t[2];

	t[0] = (unsigned char)(v & 0xFF);
	t[1] = (unsigned char)((v >> 8) & 0xFF);
	buf_add(b, t, 2);
}

static void put32(Buf *b, uint32_t v)
{
	unsigned char t[4];

	t[0] = (unsigned char)(v & 0xFF);
	t[1] = (unsigned char)((v >> 8) & 0xFF);
	t[2] = (unsigned char)((v >> 16) & 0xFF);
	t[3] = (unsigned char)((v >> 24) & 0xFF);
	buf_add(b, t, 4);
}

static int deflate_raw(const unsigned char *in, size_t n, unsigned char **out, size_t *outn)
{
	z_stream s;
	int rc;
	size_t cap = n + n / 100 + 128;
	unsigned char *buf = xmalloc(cap);

	memset(&s, 0, sizeof(s));
	if (deflateInit2(&s, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
			 Z_DEFAULT_STRATEGY) != Z_OK) {
		free(buf);
		return -1;
	}
	s.next_in = (unsigned char *)in;
	s.avail_in = (uInt)n;
	s.next_out = buf;
	s.avail_out = (uInt)cap;
	rc = deflate(&s, Z_FINISH);
	*outn = (size_t)s.total_out;
	deflateEnd(&s);
	if (rc != Z_STREAM_END) {
		free(buf);
		return -1;
	}
	*out = buf;
	return 0;
}

static void zipw_add(ZipW *z, const char *name, const unsigned char *data, size_t len)
{
	WEntry e;
	unsigned char *cd = NULL;
	size_t cdl = 0;

	memset(&e, 0, sizeof(e));
	e.name = xstrdup(name);
	e.usize = len;
	e.crc = (uint32_t)crc32(0, data, (uInt)len);
	e.udata = xmalloc(len ? len : 1);
	if (len)
		memcpy(e.udata, data, len);
	if (len > 64 && deflate_raw(data, len, &cd, &cdl) == 0 && cdl < len) {
		e.method = 8;
		e.cdata = cd;
		e.csize = cdl;
	} else {
		free(cd);
		e.method = 0;
		e.cdata = e.udata;
		e.csize = len;
	}
	e.off = (uint32_t)z->out.n;
	put32(&z->out, 0x04034b50u);
	put16(&z->out, 20);
	put16(&z->out, 0);
	put16(&z->out, e.method);
	put16(&z->out, 0);
	put16(&z->out, 0x5C21);
	put32(&z->out, e.crc);
	put32(&z->out, (uint32_t)e.csize);
	put32(&z->out, (uint32_t)e.usize);
	put16(&z->out, (uint32_t)strlen(e.name));
	put16(&z->out, 0);
	buf_add(&z->out, e.name, strlen(e.name));
	buf_add(&z->out, e.cdata, e.csize);

	if (z->n == z->cap) {
		z->cap = z->cap ? z->cap * 2 : 16;
		z->e = xrealloc(z->e, sizeof(WEntry) * (size_t)z->cap);
	}
	z->e[z->n++] = e;
}

static int zipw_save(ZipW *z, const char *path)
{
	size_t cdstart = z->out.n;
	int i;

	for (i = 0; i < z->n; i++) {
		WEntry *e = &z->e[i];

		put32(&z->out, 0x02014b50u);
		put16(&z->out, 20);
		put16(&z->out, 20);
		put16(&z->out, 0);
		put16(&z->out, e->method);
		put16(&z->out, 0);
		put16(&z->out, 0x5C21);
		put32(&z->out, e->crc);
		put32(&z->out, (uint32_t)e->csize);
		put32(&z->out, (uint32_t)e->usize);
		put16(&z->out, (uint32_t)strlen(e->name));
		put16(&z->out, 0);
		put16(&z->out, 0);
		put16(&z->out, 0);
		put16(&z->out, 0);
		put32(&z->out, 0);
		put32(&z->out, e->off);
		buf_add(&z->out, e->name, strlen(e->name));
	}
	{
		uint32_t cdsize = (uint32_t)(z->out.n - cdstart);
		uint32_t cdoff = (uint32_t)cdstart;

		put32(&z->out, 0x06054b50u);
		put16(&z->out, 0);
		put16(&z->out, 0);
		put16(&z->out, (uint32_t)z->n);
		put16(&z->out, (uint32_t)z->n);
		put32(&z->out, cdsize);
		put32(&z->out, cdoff);
		put16(&z->out, 0);
	}
	{
		FILE *f = fopen(path, "wb");
		size_t w;

		if (!f)
			return -1;
		w = fwrite(z->out.p, 1, z->out.n, f);
		fclose(f);
		if (w != z->out.n)
			return -1;
	}
	return 0;
}

static void zipw_free(ZipW *z)
{
	int i;

	for (i = 0; i < z->n; i++) {
		free(z->e[i].name);
		if (z->e[i].method == 8)
			free(z->e[i].cdata);
		free(z->e[i].udata);
	}
	free(z->e);
	buf_free(&z->out);
}

/* ---------------- worksheet xml ---------------- */
static void xml_esc(Buf *b, const char *s)
{
	for (; s && *s; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
		case '&': buf_puts(b, "&amp;"); break;
		case '<': buf_puts(b, "&lt;"); break;
		case '>': buf_puts(b, "&gt;"); break;
		case '"': buf_puts(b, "&quot;"); break;
		case '\r': break;
		case '\n': buf_puts(b, "\n"); break;
		default:
			if (c < 0x20 && c != '\t')
				break;
			buf_putc(b, c);
		}
	}
}

static void col_ref(Buf *b, int col)
{
	char t[8];
	int n = 0;
	int c = col + 1;

	while (c > 0) {
		int m = (c - 1) % 26;
		t[n++] = (char)('A' + m);
		c = (c - 1) / 26;
	}
	while (n > 0)
		buf_putc(b, t[--n]);
}

static void build_sheet_xml(Buf *b, const Table *t)
{
	int r, c;

	buf_puts(b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n");
	buf_puts(b, "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");
	buf_puts(b, "<sheetViews><sheetView workbookViewId=\"0\">");
	buf_puts(b, "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>");
	buf_puts(b, "</sheetView></sheetViews><sheetData>");
	for (r = -1; r < t->nrows; r++) {
		buf_printf(b, "<row r=\"%d\">", r + 2);
		for (c = 0; c < t->schema.n; c++) {
			const char *v = (r < 0) ? t->schema.name[c] : t->cell[r][c];

			if (!v || !*v)
				continue;
			buf_puts(b, "<c r=\"");
			col_ref(b, c);
			buf_printf(b, "%d\" t=\"inlineStr\"><is><t xml:space=\"preserve\">", r + 2);
			xml_esc(b, v);
			buf_puts(b, "</t></is></c>");
		}
		buf_puts(b, "</row>");
	}
	buf_puts(b, "</sheetData></worksheet>");
}

int xlsx_write(const Table *t, const char *path, const char *sheetname, char *err, size_t errlen)
{
	ZipW z;
	Buf b;
	int rc;

	if (t->schema.n == 0) {
		snprintf(err, errlen, "nothing to export (empty table)");
		return -1;
	}
	memset(&z, 0, sizeof(z));
	buf_init(&z.out);
	buf_init(&b);

	buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		     "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
		     "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
		     "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
		     "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
		     "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
		     "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
		     "</Types>");
	zipw_add(&z, "[Content_Types].xml", (unsigned char *)b.p, b.n);

	buf_reset(&b);
	buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		     "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
		     "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
		     "</Relationships>");
	zipw_add(&z, "_rels/.rels", (unsigned char *)b.p, b.n);

	buf_reset(&b);
	buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		     "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
		     "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
		     "<sheets><sheet name=\"");
	xml_esc(&b, sheetname && *sheetname ? sheetname : "Sheet1");
	buf_puts(&b, "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");
	zipw_add(&z, "xl/workbook.xml", (unsigned char *)b.p, b.n);

	buf_reset(&b);
	buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		     "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
		     "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
		     "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
		     "</Relationships>");
	zipw_add(&z, "xl/_rels/workbook.xml.rels", (unsigned char *)b.p, b.n);

	buf_reset(&b);
	buf_puts(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
		     "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
		     "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/><family val=\"2\"/></font></fonts>"
		     "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
		     "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
		     "<borders count=\"1\"><border/></borders>"
		     "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
		     "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\" "
		     "applyAlignment=\"1\"><alignment wrapText=\"1\" vertical=\"top\"/></xf></cellXfs>"
		     "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
		     "</styleSheet>");
	zipw_add(&z, "xl/styles.xml", (unsigned char *)b.p, b.n);

	buf_reset(&b);
	build_sheet_xml(&b, t);
	if (b.n > 192u * 1024u * 1024u) {
		snprintf(err, errlen, "table too large to export as xlsx (%lu bytes of xml)",
			 (unsigned long)b.n);
		buf_free(&b);
		zipw_free(&z);
		return -1;
	}
	zipw_add(&z, "xl/worksheets/sheet1.xml", (unsigned char *)b.p, b.n);

	rc = zipw_save(&z, path);
	if (rc != 0)
		snprintf(err, errlen, "cannot write %s", path);
	buf_free(&b);
	zipw_free(&z);
	return rc;
}
