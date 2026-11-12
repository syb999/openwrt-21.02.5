/* xlsx.c - .xlsx / .xlsm reader (zip + xml) */
#include "x2s.h"
#include <ctype.h>
#include <math.h>

/* ---------------- tiny xml scanner ---------------- */
typedef struct {
	const char *p;
	const char *end;
} Xml;

enum { XT_TEXT = 0, XT_OPEN, XT_CLOSE, XT_EMPTY, XT_SKIP };

typedef struct {
	int type;
	const char *name;
	size_t namelen;
	const char *attr;
	const char *attr_end;
	int selfclose;
} Tok;

static int xml_next(Xml *x, Tok *t)
{
	const char *p = x->p;

	memset(t, 0, sizeof(*t));
	if (p >= x->end)
		return 0;

	if (*p != '<') {
		const char *q = p;
		while (q < x->end && *q != '<')
			q++;
		t->type = XT_TEXT;
		t->name = p;
		t->namelen = (size_t)(q - p);
		x->p = q;
		return 1;
	}

	if (x->end - p >= 4 && !memcmp(p, "<!--", 4)) {
		const char *q = p + 4;
		while (q + 3 <= x->end && memcmp(q, "-->", 3))
			q++;
		x->p = (q + 3 <= x->end) ? q + 3 : x->end;
		t->type = XT_SKIP;
		return 1;
	}
	if (x->end - p >= 9 && !memcmp(p, "<![CDATA[", 9)) {
		const char *q = p + 9;
		while (q + 3 <= x->end && memcmp(q, "]]>", 3))
			q++;
		t->type = XT_TEXT;
		t->name = p + 9;
		t->namelen = (size_t)(q - p - 9);
		x->p = (q + 3 <= x->end) ? q + 3 : x->end;
		return 1;
	}
	if (x->end - p >= 2 && p[1] == '?') {
		const char *q = p + 2;
		while (q + 2 <= x->end && memcmp(q, "?>", 2))
			q++;
		x->p = (q + 2 <= x->end) ? q + 2 : x->end;
		t->type = XT_SKIP;
		return 1;
	}
	if (x->end - p >= 2 && p[1] == '!') {
		const char *q = p + 2;
		while (q < x->end && *q != '>')
			q++;
		x->p = (q < x->end) ? q + 1 : x->end;
		t->type = XT_SKIP;
		return 1;
	}

	{
		int closing = (x->end - p >= 2 && p[1] == '/');
		const char *q = p + (closing ? 2 : 1);
		const char *ns = q;
		const char *gt;
		char quote = 0;

		while (q < x->end && !isspace((unsigned char)*q) && *q != '>' && *q != '/')
			q++;
		t->name = ns;
		t->namelen = (size_t)(q - ns);
		t->attr = q;
		while (q < x->end) {
			if (quote) {
				if (*q == quote)
					quote = 0;
			} else if (*q == '"' || *q == '\'') {
				quote = *q;
			} else if (*q == '>') {
				break;
			}
			q++;
		}
		gt = (q < x->end) ? q : x->end;
		t->attr_end = gt;
		x->p = (gt < x->end) ? gt + 1 : x->end;
		if (closing) {
			t->type = XT_CLOSE;
			t->attr = t->attr_end = NULL;
		} else {
			const char *b = gt;
			while (b > p && isspace((unsigned char)b[-1]))
				b--;
			t->selfclose = (b > p && b[-1] == '/');
			t->type = t->selfclose ? XT_EMPTY : XT_OPEN;
		}
		return 1;
	}
}

static int tok_is(const Tok *t, const char *name)
{
	size_t n = strlen(name);
	return t->namelen == n && !memcmp(t->name, name, n);
}

static int tok_attr(const Tok *t, const char *name, char **val, size_t *vlen)
{
	const char *p = t->attr, *e = t->attr_end;
	size_t n = strlen(name);

	if (!p || !e || p >= e)
		return 0;
	while (p < e) {
		const char *ns, *ne, *vs, *ve;
		char q;

		while (p < e && (isspace((unsigned char)*p) || *p == '/'))
			p++;
		if (p >= e || *p == '>')
			break;
		ns = p;
		while (p < e && *p != '=' && *p != '>' && *p != '/' &&
		       !isspace((unsigned char)*p))
			p++;
		ne = p;
		while (p < e && isspace((unsigned char)*p))
			p++;
		if (p >= e || *p != '=') {
			if (p < e && *p == '>')
				break;
			continue;
		}
		p++;
		while (p < e && isspace((unsigned char)*p))
			p++;
		if (p >= e || (*p != '"' && *p != '\''))
			continue;
		q = *p++;
		vs = p;
		while (p < e && *p != q)
			p++;
		ve = p;
		if (p < e)
			p++;
		if ((size_t)(ne - ns) == n && !strncmp(ns, name, n)) {
			*val = (char *)vs;
			*vlen = (size_t)(ve - vs);
			return 1;
		}
	}
	return 0;
}

static void decode_entities(Buf *out, const char *s, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (s[i] == '&') {
			const char *semi = memchr(s + i, ';', n - i);
			if (semi && semi - (s + i) <= 10) {
				size_t l = (size_t)(semi - (s + i)) - 1;
				const char *e = s + i + 1;
				if (l == 3 && !memcmp(e, "amp", 3)) {
					buf_putc(out, '&');
					i += 4;
					continue;
				}
				if (l == 2 && !memcmp(e, "lt", 2)) {
					buf_putc(out, '<');
					i += 3;
					continue;
				}
				if (l == 2 && !memcmp(e, "gt", 2)) {
					buf_putc(out, '>');
					i += 3;
					continue;
				}
				if (l == 4 && !memcmp(e, "quot", 4)) {
					buf_putc(out, '"');
					i += 5;
					continue;
				}
				if (l == 4 && !memcmp(e, "apos", 4)) {
					buf_putc(out, '\'');
					i += 5;
					continue;
				}
				if (l >= 2 && e[0] == '#') {
					uint32_t cp = 0;
					size_t k = 1;
					int hex = 0;

					if (e[1] == 'x' || e[1] == 'X') {
						hex = 1;
						k = 2;
					}
					for (; k < l + 0; k++) {
						char c = e[k];
						int d;
						if (hex && isxdigit((unsigned char)c))
							d = isdigit((unsigned char)c) ? c - '0' :
							    (tolower((unsigned char)c) - 'a' + 10);
						else if (!hex && isdigit((unsigned char)c))
							d = c - '0';
						else
							break;
						cp = cp * (hex ? 16 : 10) + (uint32_t)d;
					}
					if (cp) {
						utf8_put(out, cp);
						i += l + 1;
						continue;
					}
				}
			}
		}
		buf_putc(out, s[i]);
	}
}

/* ---------------- shared strings ---------------- */
static char **read_shared(Zip *z, int *count)
{
	unsigned char *data = NULL;
	size_t len = 0;
	char **tab = NULL;
	int n = 0, cap = 0;
	Xml x;
	Tok t;
	Buf cur, tmp;
	int in_si = 0, in_rph = 0, in_t = 0;

	*count = 0;
	if (zip_extract(z, "xl/sharedStrings.xml", &data, &len) != 0)
		return NULL;
	buf_init(&cur);
	buf_init(&tmp);
	x.p = (const char *)data;
	x.end = (const char *)data + len;

	while (xml_next(&x, &t)) {
		if (t.type == XT_OPEN || t.type == XT_EMPTY) {
			if (tok_is(&t, "si")) {
				if (t.type == XT_OPEN) {
					in_si = 1;
					buf_reset(&cur);
				} else {
					if (n == cap) {
						cap = cap ? cap * 2 : 64;
						tab = xrealloc(tab, sizeof(char *) * (size_t)cap);
					}
					tab[n++] = xstrdup(buf_str(&cur));
				}
			} else if (tok_is(&t, "rPh")) {
				in_rph = 1;
			} else if (tok_is(&t, "t")) {
				in_t = 1;
				if (t.type == XT_EMPTY && in_si && !in_rph) {
					/* empty <t/> */
				}
			}
		} else if (t.type == XT_CLOSE) {
			if (tok_is(&t, "si")) {
				if (in_si) {
					if (n == cap) {
						cap = cap ? cap * 2 : 64;
						tab = xrealloc(tab, sizeof(char *) * (size_t)cap);
					}
					tab[n++] = xstrdup(buf_str(&cur));
				}
				in_si = 0;
			} else if (tok_is(&t, "rPh")) {
				in_rph = 0;
			} else if (tok_is(&t, "t")) {
				in_t = 0;
			}
		} else if (t.type == XT_TEXT) {
			if (in_si && in_t && !in_rph) {
				buf_reset(&tmp);
				decode_entities(&tmp, t.name, t.namelen);
				buf_add(&cur, buf_str(&tmp), tmp.n);
			}
		}
	}
	buf_free(&cur);
	buf_free(&tmp);
	free(data);
	*count = n;
	return tab;
}

/* ---------------- styles: which xf indexes are dates ---------------- */
static int fmt_code_is_date(const char *c)
{
	int in_q = 0, in_b = 0;
	int has_y = 0, has_d = 0, has_h = 0, has_s = 0, has_m = 0;

	for (; *c; c++) {
		char ch = *c;
		if (ch == '"') {
			in_q = !in_q;
			continue;
		}
		if (in_q)
			continue;
		if (ch == '\\') {
			c++;
			continue;
		}
		if (ch == '[') {
			in_b = 1;
			continue;
		}
		if (ch == ']') {
			in_b = 0;
			continue;
		}
		if (in_b) {
			if (ch == 'h' || ch == 'm' || ch == 's')
				return 1; /* elapsed time format */
			continue;
		}
		switch (tolower((unsigned char)ch)) {
		case 'y': has_y = 1; break;
		case 'd': has_d = 1; break;
		case 'h': has_h = 1; break;
		case 's': has_s = 1; break;
		case 'm': has_m = 1; break;
		default: break;
		}
	}
	if (has_y || has_d)
		return 1;
	if (has_m && (has_h || has_s))
		return 1;
	return 0;
}

static int builtin_date(uint32_t id)
{
	return (id >= 14 && id <= 22) || (id >= 27 && id <= 36) ||
	       (id >= 45 && id <= 47) || (id >= 50 && id <= 58);
}

static unsigned char *read_styles(Zip *z, int *numxf)
{
	unsigned char *data = NULL;
	size_t len = 0;
	Xml x;
	Tok t;
	unsigned char *map = NULL;
	uint32_t *custom_ids = NULL;
	int ncustom = 0, capcustom = 0;
	char **custom_date = NULL;
	int in_cellxfs = 0, n = 0, cap = 0;

	*numxf = 0;
	if (zip_extract(z, "xl/styles.xml", &data, &len) != 0)
		return NULL;
	x.p = (const char *)data;
	x.end = (const char *)data + len;

	while (xml_next(&x, &t)) {
		if ((t.type == XT_OPEN || t.type == XT_EMPTY) && tok_is(&t, "numFmt")) {
			char *v;
			size_t vl;

			if (tok_attr(&t, "numFmtId", &v, &vl)) {
				char idbuf[24];
				int id;
				size_t cpl = vl < 23 ? vl : 23;

				memcpy(idbuf, v, cpl);
				idbuf[cpl] = 0;
				id = atoi(idbuf);
				if (tok_attr(&t, "formatCode", &v, &vl)) {
					char *code = xstrndup(v, vl);
					int isdate = fmt_code_is_date(code);

					free(code);
					if (ncustom == capcustom) {
						capcustom = capcustom ? capcustom * 2 : 16;
						custom_ids = xrealloc(custom_ids, sizeof(uint32_t) * (size_t)capcustom);
						custom_date = xrealloc(custom_date, sizeof(char *) * (size_t)capcustom);
					}
					custom_ids[ncustom] = (uint32_t)id;
					custom_date[ncustom] = (char *)(long)isdate;
					ncustom++;
				}
			}
			continue;
		}
		if (t.type == XT_OPEN || t.type == XT_EMPTY) {
			if (tok_is(&t, "cellXfs")) {
				in_cellxfs = 1;
				continue;
			}
			if (in_cellxfs && tok_is(&t, "xf")) {
				char *v;
				size_t vl;
				uint32_t fid = 0;
				int isdate = 0;
				int i;

				if (tok_attr(&t, "numFmtId", &v, &vl)) {
					char b[24];
					size_t cpl = vl < 23 ? vl : 23;
					memcpy(b, v, cpl);
					b[cpl] = 0;
					fid = (uint32_t)atoi(b);
				}
				isdate = builtin_date(fid);
				for (i = 0; i < ncustom && !isdate; i++)
					if (custom_ids[i] == fid && custom_date[i])
						isdate = 1;
				if (n == cap) {
					cap = cap ? cap * 2 : 32;
					map = xrealloc(map, (size_t)cap);
				}
				map[n++] = (unsigned char)isdate;
			}
		} else if (t.type == XT_CLOSE && tok_is(&t, "cellXfs")) {
			in_cellxfs = 0;
		}
	}
	free(data);
	free(custom_ids);
	free(custom_date);
	*numxf = n;
	if (!n) {
		free(map);
		return NULL;
	}
	return map;
}

/* ---------------- date conversion ---------------- */
static void civil_from_days(long z, int *yy, int *mm, int *dd)
{
	long era, doe, yoe, y, doy, mp, d, m;

	z += 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	d = doy - (153 * mp + 2) / 5 + 1;
	m = mp + (mp < 10 ? 3 : -9);
	*yy = (int)(y + (m <= 2));
	*mm = (int)m;
	*dd = (int)d;
}

static void date_to_text(Buf *out, double serial, int date1904)
{
	double whole = floor(serial);
	double frac = serial - whole;
	long days;
	int y, m, d, hh, mi, ss;

	if (date1904) {
		days = (long)whole - 24107L;
	} else {
		if (whole <= 0)
			days = (long)whole - 25568L;
		else if (whole < 60)
			days = (long)whole - 25568L;
		else
			days = (long)whole - 25569L;
	}
	if (frac < 0)
		frac = 0;
	if (frac > 0.9999999)
		frac = 0;
	civil_from_days(days, &y, &m, &d);
	hh = (int)(frac * 24.0 + 1e-9);
	mi = (int)((frac * 24.0 - hh) * 60.0 + 1e-9);
	ss = (int)((((frac * 24.0 - hh) * 60.0) - mi) * 60.0 + 0.5);
	if (ss >= 60)
		ss = 59;
	if (frac <= 0) {
		buf_printf(out, "%04d-%02d-%02d", y, m, d);
	} else {
		buf_printf(out, "%04d-%02d-%02d %02d:%02d:%02d", y, m, d, hh, mi, ss);
	}
}

static void num_to_text(Buf *out, const char *raw, size_t rlen, int isdate, int date1904)
{
	char b[64];
	size_t n = rlen < sizeof(b) - 1 ? rlen : sizeof(b) - 1;
	double v;

	memcpy(b, raw, n);
	b[n] = 0;
	str_trim(b);
	if (!b[0])
		return;
	if (isdate) {
		v = strtod(b, NULL);
		date_to_text(out, v, date1904);
		return;
	}
	if (!strchr(b, '.') && !strchr(b, 'e') && !strchr(b, 'E')) {
		buf_puts(out, b);
		return;
	}
	v = strtod(b, NULL);
	if (v == floor(v) && fabs(v) < 1e15) {
		buf_printf(out, "%.0f", v);
	} else {
		buf_printf(out, "%.15g", v);
	}
}

/* ---------------- worksheet ---------------- */
static int col_from_ref(const char *s, size_t n, int *col)
{
	int c = 0;
	size_t i = 0;

	while (i < n && (s[i] == ' '))
		i++;
	while (i < n && isalpha((unsigned char)s[i])) {
		c = c * 26 + (toupper((unsigned char)s[i]) - 'A' + 1);
		i++;
	}
	if (c <= 0)
		return -1;
	*col = c - 1;
	return 0;
}

static int parse_sheet_xml(const unsigned char *data, size_t len, Sheet *sh,
		char **shared, int nshared, const unsigned char *styles, int nxf,
		int date1904)
{
	Xml x;
	Tok t;
	int rowidx = -1;
	int colidx = 0;
	int incell = 0;
	char ctype = 0;
	int cellstyle = -1;
	Buf v, it;
	int cap = 0; /* 0 none, 1 v, 2 t, 3 is */
	Buf *target = NULL;

	buf_init(&v);
	buf_init(&it);
	x.p = (const char *)data;
	x.end = (const char *)data + len;

	while (xml_next(&x, &t)) {
		if (t.type == XT_OPEN || t.type == XT_EMPTY) {
			if (tok_is(&t, "row")) {
				char *val;
				size_t vl;
				int empty = (t.type == XT_EMPTY);

				if (tok_attr(&t, "r", &val, &vl)) {
					char b[16];
					size_t n = vl < 15 ? vl : 15;
					memcpy(b, val, n);
					b[n] = 0;
					rowidx = atoi(b) - 1;
				} else {
					rowidx++;
				}
				(void)empty;
		} else if (tok_is(&t, "c")) {
				char *val;
				size_t vl;

				incell = 1;
				ctype = 0;
				cellstyle = -1;
				buf_reset(&v);
				buf_reset(&it);
				cap = 0;
				target = NULL;
				if (tok_attr(&t, "r", &val, &vl))
					col_from_ref(val, vl, &colidx);
				if (tok_attr(&t, "t", &val, &vl) && vl > 0)
					ctype = val[0];
				if (tok_attr(&t, "s", &val, &vl)) {
					char b[16];
					size_t n = vl < 15 ? vl : 15;
					memcpy(b, val, n);
					b[n] = 0;
					cellstyle = atoi(b);
				}
			} else if (tok_is(&t, "v") && incell) {
				cap = 1;
				target = &v;
			} else if (tok_is(&t, "is") && incell) {
				cap = 3;
				target = &it;
			} else if (tok_is(&t, "t") && incell) {
				if (cap != 2)
					target = &it;
				cap = 2;
			}
			continue;
		}
		if (t.type == XT_CLOSE) {
			if (tok_is(&t, "c")) {
				const char *txt = NULL;
				Buf out;

				buf_init(&out);
				if (ctype == 's') {
					int idx = atoi(buf_str(&v));
					if (idx >= 0 && idx < nshared)
						txt = shared[idx];
				} else if (ctype == 'i' || ctype == 'I') {
					/* inlineStr handled below via it */
				}
				if (ctype == 's') {
					if (txt)
						buf_puts(&out, txt);
				} else if (ctype == 0 || ctype == 'n') {
					int isdate = 0;
					if (styles && cellstyle >= 0 && cellstyle < nxf)
						isdate = styles[cellstyle];
					num_to_text(&out, buf_str(&v), v.n, isdate, date1904);
				} else if (ctype == 's' && !txt) {
					;
				} else if (ctype == 'b') {
					buf_puts(&out, strcmp(buf_str(&v), "1") == 0 ? "TRUE" : "FALSE");
				} else if (ctype == 'e') {
					buf_puts(&out, buf_str(&v));
				} else {
					if (it.n)
						buf_add(&out, buf_str(&it), it.n);
					else
						buf_add(&out, buf_str(&v), v.n);
				}
				if (rowidx >= 0 && out.n)
					sheet_set(sh, rowidx, colidx, buf_str(&out));
				buf_free(&out);
				incell = 0;
				cap = 0;
				target = NULL;
			} else if (tok_is(&t, "v") || tok_is(&t, "is") || tok_is(&t, "t")) {
				target = NULL;
			}
			continue;
		}
		if (t.type == XT_TEXT && target) {
			Buf tmp;
			buf_init(&tmp);
			if (t.namelen)
				decode_entities(&tmp, t.name, t.namelen);
			buf_add(target, buf_str(&tmp), tmp.n);
			buf_free(&tmp);
		}
	}
	buf_free(&v);
	buf_free(&it);
	return 0;
}

typedef struct {
	char *name;
	char *target;
	int is_sheet;
} WbSheet;

int xlsx_read(const char *path, Sheet **out, int *nout, char *err, size_t errlen)
{
	unsigned char *data = NULL;
	size_t len = 0;
	Zip z;
	unsigned char *wb = NULL, *rels = NULL;
	size_t wblen = 0, relslen = 0;
	char **shared = NULL;
	int nshared = 0;
	unsigned char *styles = NULL;
	int nxf = 0;
	WbSheet *ws = NULL;
	int nws = 0, capws = 0;
	int date1904 = 0;
	Xml x;
	Tok t;
	int i;

	*out = NULL;
	*nout = 0;
	if (slurp(path, &data, &len, err, errlen) != 0)
		return -1;
	if (zip_open(&z, data, len, err, errlen) != 0) {
		free(data);
		return -1;
	}
	if (zip_extract_soft(&z, "xl/workbook.xml", &wb, &wblen) != 0) {
		snprintf(err, errlen, "not an xlsx workbook (no xl/workbook.xml)");
		zip_close(&z);
		return -1;
	}
	zip_extract_soft(&z, "xl/_rels/workbook.xml.rels", &rels, &relslen);

	x.p = (const char *)wb;
	x.end = (const char *)wb + wblen;
	while (xml_next(&x, &t)) {
		if (t.type != XT_OPEN && t.type != XT_EMPTY)
			continue;
		if (tok_is(&t, "workbookPr")) {
			char *v;
			size_t vl;
			if (tok_attr(&t, "date1904", &v, &vl) && vl && (v[0] == '1' || v[0] == 't'))
				date1904 = 1;
		}
		if (tok_is(&t, "sheet")) {
			char *nm = NULL, *rid = NULL;
			size_t nml = 0, rl = 0;

			if (!tok_attr(&t, "name", &nm, &nml))
				continue;
			if (!tok_attr(&t, "r:id", &rid, &rl)) {
				if (!tok_attr(&t, "id", &rid, &rl))
					rid = NULL;
			}
			if (nws == capws) {
				capws = capws ? capws * 2 : 8;
				ws = xrealloc(ws, sizeof(WbSheet) * (size_t)capws);
			}
			ws[nws].name = xstrndup(nm, nml);
			ws[nws].target = rid ? xstrndup(rid, rl) : NULL;
			ws[nws].is_sheet = 0;
			nws++;
		}
	}
	if (rels) {
		x.p = (const char *)rels;
		x.end = (const char *)rels + relslen;
		while (xml_next(&x, &t)) {
			char *id = NULL, *tg = NULL, *ty = NULL;
			size_t idl = 0, tgl = 0, tyl = 0;

			if (t.type != XT_OPEN && t.type != XT_EMPTY)
				continue;
			if (!tok_is(&t, "Relationship"))
				continue;
			if (!tok_attr(&t, "Id", &id, &idl) || !tok_attr(&t, "Target", &tg, &tgl))
				continue;
			tok_attr(&t, "Type", &ty, &tyl);
			for (i = 0; i < nws; i++) {
				if (ws[i].target && strlen(ws[i].target) == idl &&
				    !strncmp(ws[i].target, id, idl)) {
					int issheet = 1;
					if (ty && tyl >= 9) {
						size_t k;
						for (k = 0; k + 9 <= tyl; k++)
							if (!memcmp(ty + k, "worksheet", 9))
								issheet = 1;
					}
					ws[i].is_sheet = issheet;
					if (ws[i].target) {
						Buf b;

						buf_init(&b);
						if (tg[0] == '/') {
							buf_add(&b, tg + 1, tgl - 1);
						} else {
							buf_puts(&b, "xl/");
							buf_add(&b, tg, tgl);
						}
						free(ws[i].target);
						ws[i].target = buf_detach(&b);
					}
				}
			}
		}
	}
	if (nws == 0) {
		snprintf(err, errlen, "workbook has no sheets");
		goto fail;
	}
	shared = read_shared(&z, &nshared);
	styles = read_styles(&z, &nxf);

	*out = xmalloc(sizeof(Sheet) * (size_t)nws);
	for (i = 0; i < nws; i++) {
		unsigned char *sd = NULL;
		size_t sdlen = 0;

		sheet_init(&(*out)[i], ws[i].name ? ws[i].name : "Sheet");
		if (!ws[i].target)
			continue;
		if (zip_extract_soft(&z, ws[i].target, &sd, &sdlen) != 0) {
			char alt[512];
			snprintf(alt, sizeof(alt), "xl/%s", ws[i].target);
			if (zip_extract_soft(&z, alt, &sd, &sdlen) != 0)
				continue;
		}
		parse_sheet_xml(sd, sdlen, &(*out)[i], shared, nshared, styles, nxf, date1904);
		free(sd);
		*nout = i + 1;
	}
	for (i = 0; i < nshared; i++)
		free(shared[i]);
	free(shared);
	free(styles);
	for (i = 0; i < nws; i++) {
		free(ws[i].name);
		free(ws[i].target);
	}
	free(ws);
	free(wb);
	free(rels);
	zip_close(&z);
	return 0;

fail:
	for (i = 0; i < nws; i++) {
		free(ws[i].name);
		free(ws[i].target);
	}
	free(ws);
	free(wb);
	free(rels);
	zip_close(&z);
	return -1;
}
