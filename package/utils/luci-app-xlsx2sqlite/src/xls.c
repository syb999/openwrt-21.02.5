/* xls.c - legacy .xls (OLE2 / BIFF8) reader */
#include "x2s.h"
#include <math.h>

static uint16_t g16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t g32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

/* ---------------- CFB (OLE2) ---------------- */
typedef struct {
	unsigned char *d;
	size_t len;
	unsigned int ssz, mssz, cutoff;
	uint32_t *fat;
	uint32_t nfat;
	uint32_t *mfat;
	uint32_t nmfat;
	uint32_t first_dir;
	uint32_t nfatsec;
	uint32_t first_difat;
	uint32_t ndifat;
} Cfb;

static int cfb_sector(const Cfb *c, uint32_t sec, const unsigned char **p)
{
	size_t off = ((size_t)sec + 1) * c->ssz;
	if (off + c->ssz > c->len)
		return -1;
	*p = c->d + off;
	return 0;
}

static int cfb_open(Cfb *c, unsigned char *d, size_t len)
{
	unsigned int ssz_bits;

	memset(c, 0, sizeof(*c));
	c->d = d;
	c->len = len;
	if (len < 512 || g32(d) != 0xE011CFD0u || g32(d + 4) != 0xE11AB1A1u)
		return -1;
	ssz_bits = g16(d + 0x1E);
	if (ssz_bits < 7 || ssz_bits > 16)
		return -1;
	c->ssz = 1u << ssz_bits;
	c->mssz = 1u << g16(d + 0x20);
	c->nfatsec = g32(d + 0x2C);
	c->first_dir = g32(d + 0x30);
	c->cutoff = g32(d + 0x38);
	c->nmfat = g32(d + 0x40);
	c->first_difat = g32(d + 0x44);
	c->ndifat = g32(d + 0x48);
	if (c->ssz != 512 && c->ssz != 4096)
		return -1;
	if (c->nfatsec == 0 || c->nfatsec > 100000)
		return -1;
	c->nfat = c->nfatsec * (c->ssz / 4);
	c->fat = xmalloc(sizeof(uint32_t) * c->nfat);
	{
		uint32_t got = 0;
		uint32_t sec = 0;
		int i;
		unsigned char *tmp = xmalloc(c->ssz);

		for (i = 0; i < 109 && got < c->nfatsec; i++) {
			uint32_t fs = g32(d + 0x4C + i * 4);
			const unsigned char *p;
			if (fs == 0xFFFFFFFFu)
				break;
			if (cfb_sector(c, fs, &p) != 0)
				break;
			memcpy(tmp + got * c->ssz, p, c->ssz);
			got++;
		}
		sec = c->first_difat;
		while (got < c->nfatsec && sec != 0xFFFFFFFFu && sec != 0xFFFFFFFEu) {
			const unsigned char *p;
			uint32_t k;
			if (cfb_sector(c, sec, &p) != 0)
				break;
			for (k = 0; k + 4 < c->ssz && got < c->nfatsec; k += 4) {
				uint32_t fs = g32(p + k);
				const unsigned char *q;
				if (fs == 0xFFFFFFFFu)
					continue;
				if (cfb_sector(c, fs, &q) != 0)
					continue;
				memcpy(tmp + got * c->ssz, q, c->ssz);
				got++;
			}
			sec = g32(p + c->ssz - 4);
		}
		memcpy(c->fat, tmp, (size_t)got * c->ssz);
		for (; got < c->nfatsec; got++)
			memset((unsigned char *)c->fat + (size_t)got * c->ssz, 0xFF, c->ssz);
		free(tmp);
	}
	if (c->nmfat) {
		uint32_t maxsz = c->nmfat * (c->ssz / 4);
		c->mfat = xmalloc(sizeof(uint32_t) * maxsz);
		{
			uint32_t sec = g32(d + 0x3C);
			uint32_t got = 0;
			unsigned char *tmp = xmalloc(c->ssz * (size_t)c->nmfat + 1);

			while (sec != 0xFFFFFFFFu && sec != 0xFFFFFFFEu && got < c->nmfat) {
				const unsigned char *p;
				if (cfb_sector(c, sec, &p) != 0)
					break;
				memcpy(tmp + (size_t)got * c->ssz, p, c->ssz);
				got++;
				sec = c->fat[(size_t)sec + 0];
			}
			memcpy(c->mfat, tmp, (size_t)got * c->ssz);
			free(tmp);
			c->nmfat = got * (c->ssz / 4);
		}
	}
	return 0;
}

static void cfb_close(Cfb *c)
{
	free(c->fat);
	free(c->mfat);
	memset(c, 0, sizeof(*c));
}

typedef struct {
	char name[64];
	int type;
	uint32_t start;
	uint32_t size;
} DirEnt;

static int cfb_read_chain(const Cfb *c, const uint32_t *fat, uint32_t nfat,
		uint32_t start, unsigned char *out, size_t size,
		const unsigned char *base, unsigned int ssz, size_t baselen)
{
	uint32_t sec = start;
	size_t done = 0;
	uint32_t guard = 0;
	unsigned int secsz = base ? ssz : c->ssz;
	while (done < size && sec != 0xFFFFFFFFu && sec != 0xFFFFFFFEu) {
		const unsigned char *p;
		size_t n;

		if (sec >= nfat || ++guard > nfat + 16)
			return -1;
		if (base) {
			size_t off = ((size_t)sec) * secsz;

			if (off >= baselen)
				return -1;
			p = base + off;
			if (baselen - off < secsz && size - done > baselen - off)
				return -1;
		} else {
			size_t off = ((size_t)sec + 1) * secsz;
			if (off + secsz > c->len)
				return -1;
			p = c->d + off;
		}
		n = size - done;
		if (n > secsz)
			n = secsz;
		memcpy(out + done, p, n);
		done += n;
		sec = fat[sec];
	}
	return done == size ? 0 : -1;
}

int xls_read(const char *path, Sheet **out, int *nout, char *err, size_t errlen)
{
	unsigned char *data = NULL;
	size_t len = 0;
	Cfb c;
	DirEnt *ents = NULL;
	int nents = 0;
	DirEnt *root = NULL, *wb = NULL;
	unsigned char *mini = NULL, *stream = NULL;
	int i;
	int biff8 = 0;
	size_t pos;

	*out = NULL;
	*nout = 0;
	if (slurp(path, &data, &len, err, errlen) != 0)
		return -1;
	if (cfb_open(&c, data, len) != 0) {
		snprintf(err, errlen, "not an OLE2 .xls file");
		free(data);
		return -1;
	}
	{
		size_t dsize = c.ssz;
		unsigned char *dir = NULL;
		uint32_t sec = c.first_dir;
		size_t have = 0;
		uint32_t guard = 0;
		Buf all;

		buf_init(&all);
		while (sec != 0xFFFFFFFFu && sec != 0xFFFFFFFEu) {
			const unsigned char *p;
			if (cfb_sector(&c, sec, &p) != 0 || ++guard > c.nfat + 16)
				break;
			buf_add(&all, p, c.ssz);
			sec = c.fat[sec];
		}
		dsize = all.n;
		(void)have;
		dir = (unsigned char *)buf_detach(&all);
		if (dsize < 128) {
			free(dir);
			snprintf(err, errlen, "xls: empty directory");
			cfb_close(&c);
			free(data);
			return -1;
		}
		nents = (int)(dsize / 128);
		ents = xmalloc(sizeof(DirEnt) * (size_t)nents);
		for (i = 0; i < nents; i++) {
			unsigned char *e = dir + (size_t)i * 128;
			size_t nl = g16(e + 0x40);
			size_t k;
			int j = 0;

			ents[i].type = e[0x42];
			ents[i].start = g32(e + 0x74);
			ents[i].size = g32(e + 0x78);
			ents[i].name[0] = 0;
			if (nl > 64)
				nl = 64;
			for (k = 0; k + 1 < nl; k += 2) {
				uint32_t ch = (uint32_t)e[k] | ((uint32_t)e[k + 1] << 8);
				if (!ch)
					break;
				if (ch < 128)
					ents[i].name[j++] = (char)ch;
			}
			ents[i].name[j] = 0;
			if (ents[i].type == 5)
				root = &ents[i];
		}
		free(dir);
	}
	for (i = 0; i < nents; i++) {
		if (ents[i].type != 2)
			continue;
		if (!strncmp(ents[i].name, "Workbook", 9) || !strncmp(ents[i].name, "Book", 5)) {
			wb = &ents[i];
			break;
		}
	}
	if (!wb || !root) {
		snprintf(err, errlen, "xls: no Workbook stream");
		free(ents);
		cfb_close(&c);
		free(data);
		return -1;
	}
	if (root->size && c.nmfat) {
		mini = xmalloc(root->size ? root->size : 1);
		if (cfb_read_chain(&c, c.fat, c.nfat, root->start, mini, root->size, NULL, 0, 0) != 0) {
			free(mini);
			mini = NULL;
		}
	}
	stream = xmalloc(wb->size ? wb->size : 1);
	if (wb->size >= c.cutoff) {
		if (cfb_read_chain(&c, c.fat, c.nfat, wb->start, stream, wb->size, NULL, 0, 0) != 0) {
			snprintf(err, errlen, "xls: cannot read workbook stream");
			goto fail;
		}
	} else {
		if (!mini) {
			snprintf(err, errlen, "xls: missing mini stream");
			goto fail;
		}
		if (cfb_read_chain(&c, c.mfat, c.nmfat, wb->start, stream, wb->size, mini,
				   c.mssz, root->size) != 0) {
			snprintf(err, errlen, "xls: cannot read mini workbook stream");
			goto fail;
		}
	}
	{
		/* version check */
		if (wb->size < 8 || g16(stream) != 0x0809) {
			snprintf(err, errlen, "xls: bad BIFF header");
			goto fail;
		}
		biff8 = (g16(stream + 4) >= 0x0600);
		if (!biff8) {
			snprintf(err, errlen, "xls: only BIFF8 (Excel 97+) is supported");
			goto fail;
		}
	}
	/* pass 1: collect BOUNDSHEET list and SST */
	{
		typedef struct { uint32_t pos; char *name; } BSheet;
		BSheet *bs = NULL;
		int nbs = 0, capbs = 0;
		char **shared = NULL;
		int nshared = 0;
		/* SST virtual stream */
		Buf sst;
		size_t *bst = NULL;
		int nblk = 0, capblk = 0;

		buf_init(&sst);
		pos = 0;
		while (pos + 4 <= wb->size) {
			uint16_t rid = g16(stream + pos);
			uint16_t rlen = g16(stream + pos + 2);
			const unsigned char *r = stream + pos + 4;

			if (pos + 4 + rlen > wb->size)
				break;
			if (rid == 0x0085) { /* BOUNDSHEET */
				uint32_t off = g32(r);
				int type = r[5] & 0x0F;
				unsigned int cch = r[6];
				int high = r[7] & 1;
				Buf nb;

				buf_init(&nb);
				if (type == 0) {
					if (high)
						utf16le_to_utf8(&nb, r + 8, cch);
					else {
						size_t k;
						for (k = 0; k < cch && 8 + k < (size_t)rlen; k++)
							utf8_put(&nb, r[8 + k]);
					}
					if (nbs == capbs) {
						capbs = capbs ? capbs * 2 : 8;
						bs = xrealloc(bs, sizeof(BSheet) * (size_t)capbs);
					}
					bs[nbs].pos = off;
					bs[nbs].name = buf_detach(&nb);
					nbs++;
				} else {
					buf_free(&nb);
				}
			} else if (rid == 0x00FC) { /* SST */
				/* gather payload plus following CONTINUE records */
				if (sst.n)
					buf_reset(&sst);
				nblk = 0;
				buf_add(&sst, r, rlen);
				if (nblk == capblk) {
					capblk = 8;
					bst = xrealloc(bst, sizeof(size_t) * (size_t)capblk);
				}
				bst[nblk++] = 0;
				{
					size_t q = pos + 4 + rlen;
					while (q + 4 <= wb->size && g16(stream + q) == 0x003C) {
						uint16_t cl = g16(stream + q + 2);
						if (q + 4 + cl > wb->size)
							break;
						if (nblk == capblk) {
							capblk *= 2;
							bst = xrealloc(bst, sizeof(size_t) * (size_t)capblk);
						}
						bst[nblk++] = sst.n;
						buf_add(&sst, stream + q + 4, cl);
						q += 4 + cl;
					}
				}
				/* parse strings */
				{
					size_t sp = 8; /* skip cstTotal, cstUnique */
					int idx;

					for (idx = 0; idx < 65535 && sp + 3 <= sst.n; idx++) {
						unsigned int cch = g16((const unsigned char *)sst.p + sp);
						unsigned char fl = (unsigned char)sst.p[sp + 2];
						int fHigh = fl & 1;
						int fRich = (fl & 8) != 0;
						int fExt = (fl & 4) != 0;
						unsigned int crun = 0;
						uint32_t cbExt = 0;
						unsigned int k;
						Buf sb;
						int blk = 0;

						sp += 3;
						if (fRich) {
							if (sp + 2 > sst.n)
								break;
							crun = g16((const unsigned char *)sst.p + sp);
							sp += 2;
						}
						if (fExt) {
							if (sp + 4 > sst.n)
								break;
							cbExt = g32((const unsigned char *)sst.p + sp);
							sp += 4;
						}
						buf_init(&sb);
						for (k = 0; k < cch; k++) {
							while (blk + 1 < nblk && sp >= bst[blk + 1])
								blk++;
							if (blk + 1 < nblk && sp == bst[blk + 1]) {
								fl = (unsigned char)sst.p[sp];
								fHigh = fl & 1;
								sp++;
								blk++;
							}
							while (blk + 1 < nblk && sp >= bst[blk + 1])
								blk++;
							if (fHigh) {
								uint32_t cp;
								if (sp + 2 > sst.n)
									break;
								cp = g16((const unsigned char *)sst.p + sp);
								sp += 2;
								if (cp >= 0xD800 && cp <= 0xDBFF && sp + 2 <= sst.n) {
									uint32_t c2 = g16((const unsigned char *)sst.p + sp);
									if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
										cp = 0x10000 + ((cp - 0xD800) << 10) + (c2 - 0xDC00);
										sp += 2;
									}
								}
								if (cp)
									utf8_put(&sb, cp);
							} else {
								if (sp + 1 > sst.n)
									break;
								utf8_put(&sb, (unsigned char)sst.p[sp]);
								sp += 1;
							}
						}
						sp += (size_t)crun * 4 + cbExt;
						shared = xrealloc(shared, sizeof(char *) * (size_t)(nshared + 1));
						shared[nshared++] = buf_detach(&sb);
					}
				}
				pos += 4 + rlen;
				/* skip the CONTINUE records we consumed */
				while (pos + 4 <= wb->size && g16(stream + pos) == 0x003C)
					pos += 4 + g16(stream + pos + 2);
				continue;
			}
			pos += 4 + rlen;
		}

		if (nbs == 0) {
			snprintf(err, errlen, "xls: no worksheets found");
			for (i = 0; i < nshared; i++)
				free(shared[i]);
			free(shared);
			free(bs);
			buf_free(&sst);
			free(bst);
			goto fail;
		}
		*out = xmalloc(sizeof(Sheet) * (size_t)nbs);
		for (i = 0; i < nbs; i++) {
			size_t p = bs[i].pos;

			sheet_init(&(*out)[i], bs[i].name);
			*nout = i + 1;
			while (p + 4 <= wb->size) {
				uint16_t rid = g16(stream + p);
				uint16_t rlen = g16(stream + p + 2);
				const unsigned char *r = stream + p + 4;

				if (p + 4 + rlen > wb->size)
					break;
				if (rid == 0x000A) /* EOF */
					break;
				if (rid == 0x00FD && rlen >= 10) { /* LABELSST */
					uint32_t isst = g32(r + 6);
					if (isst < (uint32_t)nshared)
						sheet_set(&(*out)[i], g16(r), g16(r + 2), shared[isst]);
				} else if (rid == 0x0204 && rlen >= 8) { /* LABEL */
					unsigned int cch = g16(r + 6);
					int high = r[8] & 1;
					Buf t;
					buf_init(&t);
					if (high)
						utf16le_to_utf8(&t, r + 9, cch);
					else {
						unsigned int k;
						for (k = 0; k < cch && 9 + k < (unsigned int)rlen; k++)
							utf8_put(&t, r[9 + k]);
					}
					sheet_set(&(*out)[i], g16(r), g16(r + 2), buf_str(&t));
					buf_free(&t);
				} else if (rid == 0x0203 && rlen >= 14) { /* NUMBER */
					double v;
					uint64_t bits = (uint64_t)g32(r + 6) | ((uint64_t)g32(r + 10) << 32);
					Buf t;
					memcpy(&v, &bits, 8);
					buf_init(&t);
					buf_printf(&t, "%.15g", v);
					sheet_set(&(*out)[i], g16(r), g16(r + 2), buf_str(&t));
					buf_free(&t);
				} else if (rid == 0x027E && rlen >= 10) { /* RK */
					uint32_t rk = g32(r + 6);
					double v;
					Buf t;
					if (rk & 2) {
						int32_t iv = (int32_t)rk >> 2;
						v = (double)iv;
					} else {
						uint64_t bits = ((uint64_t)(rk & 0xFFFFFFFCu)) << 32;
						memcpy(&v, &bits, 8);
					}
					if (rk & 1)
						v /= 100.0;
					buf_init(&t);
					buf_printf(&t, "%.15g", v);
					sheet_set(&(*out)[i], g16(r), g16(r + 2), buf_str(&t));
					buf_free(&t);
				} else if (rid == 0x00BD && rlen >= 6) { /* MULRK */
					unsigned int row = g16(r);
					unsigned int c0 = g16(r + 2);
					unsigned int n = (unsigned int)((rlen - 6) / 6);
					unsigned int k;
					for (k = 0; k < n; k++) {
						uint32_t rk = g32(r + 4 + k * 6 + 2);
						double v;
						Buf t;
						if (rk & 2) {
							int32_t iv = (int32_t)rk >> 2;
							v = (double)iv;
						} else {
							uint64_t bits = ((uint64_t)(rk & 0xFFFFFFFCu)) << 32;
							memcpy(&v, &bits, 8);
						}
						if (rk & 1)
							v /= 100.0;
						buf_init(&t);
						buf_printf(&t, "%.15g", v);
						sheet_set(&(*out)[i], (int)row, (int)c0 + (int)k, buf_str(&t));
						buf_free(&t);
					}
				} else if (rid == 0x0006 && rlen >= 20) { /* FORMULA */
					if ((uint16_t)g16(r + 12) == 0xFFFF && r[6] == 0) {
						/* string result in the next STRING record */
						size_t q = p + 4 + rlen;
						while (q + 4 <= wb->size) {
							uint16_t sid = g16(stream + q);
							uint16_t sl = g16(stream + q + 2);
							if (q + 4 + sl > wb->size)
								break;
							if (sid == 0x0207) {
								unsigned int cch = g16(stream + q + 4);
								int high = stream[q + 6] & 1;
								Buf t;
								buf_init(&t);
								if (high)
									utf16le_to_utf8(&t, stream + q + 7, cch);
								else {
									unsigned int k;
									for (k = 0; k < cch && 7 + k < sl; k++)
										utf8_put(&t, stream[q + 7 + k]);
								}
								sheet_set(&(*out)[i], g16(r), g16(r + 2), buf_str(&t));
								buf_free(&t);
								break;
							}
							if (sid != 0x003C && sid != 0x0207)
								break;
							q += 4 + sl;
						}
					} else if ((uint16_t)g16(r + 12) != 0xFFFF) {
						double v;
						uint64_t bits = (uint64_t)g32(r + 6) | ((uint64_t)g32(r + 10) << 32);
						Buf t;
						memcpy(&v, &bits, 8);
						buf_init(&t);
						buf_printf(&t, "%.15g", v);
						sheet_set(&(*out)[i], g16(r), g16(r + 2), buf_str(&t));
						buf_free(&t);
					}
				}
				p += 4 + rlen;
			}
		}
		for (i = 0; i < nshared; i++)
			free(shared[i]);
		free(shared);
		for (i = 0; i < nbs; i++)
			free(bs[i].name);
		free(bs);
		buf_free(&sst);
		free(bst);
	}
	free(stream);
	free(mini);
	free(ents);
	cfb_close(&c);
	free(data);
	return 0;

fail:
	free(stream);
	free(mini);
	free(ents);
	cfb_close(&c);
	free(data);
	return -1;
}
