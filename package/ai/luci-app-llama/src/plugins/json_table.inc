/* ============ op: table —— JSON 数组 → 表格 + CSV ============
 * 设计原则（踩了几轮才定下来）：
 *   1. jq 只干一件事：把数组导出成 CSV（全字段、全行、转义正确）。
 *   2. 表格不在 jq 里拼 —— 从 CSV 读前几行派生，两边数据必然一致。
 *   3. 字段一个都不能少。之前为了表格好看按长度筛字段，CSV 跟着丢列，
 *      导出的文件就是废的。
 *
 * 为什么不直接在 jq 里拼 markdown：值里有 tab/换行/引号，
 * @tsv/@csv 一混，C 侧按分隔符拆就错位，改五轮都修不干净。
 */

/* 找 "key" : [ 的数组起点（容忍冒号前后空白，python json.dump 会输出 "rows": [） */
static const char *find_arr_start(const char *json, const char *key) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != ':') { p = q; continue; }
        q++;
        while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q == '[') return q + 1;
        p = q;
    }
    return NULL;
}

/* 解析一行 CSV 成单元格（处理双引号包裹和 "" 转义） */
static int csv_split(const char *line, char cells[][280], int maxc) {
    int c = 0;
    const char *p = line;
    while (*p && c < maxc) {
        char *o = cells[c];
        int oi = 0;
        if (*p == '"') {
            p++;
            while (*p && oi < 275) {
                if (*p == '"') {
                    if (p[1] == '"') { o[oi++] = '"'; p += 2; }
                    else { p++; break; }
                } else o[oi++] = *p++;
            }
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        } else {
            while (*p && *p != ',' && oi < 275) o[oi++] = *p++;
            if (*p == ',') p++;
        }
        o[oi] = 0;
        c++;
    }
    return c;
}

/* 读 CSV 文件 → 生成 markdown 表格（前 showmax 列 × maxrow 行） */
static char *csv_to_markdown(const char *csvpath, int showmax, int maxrow,
                             int *rows_out, int *cols_out) {
    FILE *f = fopen(csvpath, "r");
    if (!f) return NULL;
    char *md = malloc(OUTMAX);
    char *line = malloc(512 * 1024);
    if (!md || !line) { if (md) free(md); if (line) free(line); fclose(f); return NULL; }

    static char cells[96][280];
    int ncol = 0, rows = 0;
    size_t oi = 0;
    int first = 1;

    while (fgets(line, 512 * 1024, f)) {
        size_t L = strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = 0;
        if (L == 0) continue;

        int nc = csv_split(line, cells, 96);
        if (first) {
            ncol = nc;
            int sk = ncol > showmax ? showmax : ncol;
            oi += (size_t)snprintf(md + oi, OUTMAX - oi, "|");
            for (int k = 0; k < sk; k++)
                oi += (size_t)snprintf(md + oi, OUTMAX - oi, " %s |", cells[k]);
            oi += (size_t)snprintf(md + oi, OUTMAX - oi, "\n|");
            for (int k = 0; k < sk; k++)
                oi += (size_t)snprintf(md + oi, OUTMAX - oi, "---|");
            first = 0;
            continue;
        }
        if (rows >= maxrow || oi >= OUTMAX - 500) break;
        int sk = ncol > showmax ? showmax : ncol;
        oi += (size_t)snprintf(md + oi, OUTMAX - oi, "\n|");
        for (int k = 0; k < sk && k < nc; k++) {
            char v[200];
            snprintf(v, sizeof(v), "%s", cells[k]);
            for (char *z = v; *z; z++) if (*z == '|') *z = '/';
            utf8_trunc(v, 90);
            oi += (size_t)snprintf(md + oi, OUTMAX - oi, " %s |", v);
        }
        rows++;
    }
    fclose(f);
    free(line);

    if (ncol > showmax)
        oi += (size_t)snprintf(md + oi, OUTMAX - oi,
            "\n（共 %d 个字段，表格显示前 %d 个；**完整字段在 CSV 里**）\n", ncol, showmax);

    if (rows_out) *rows_out = rows;
    if (cols_out) *cols_out = ncol;
    return md;
}

static char *op_table(const char *json) {
    char fname[768] = {0}, qpath[256] = {0};
    jget(json, "file", fname, sizeof(fname));
    jget(json, "path", qpath, sizeof(qpath));
    long limit = jget_int(json, "limit", 15);
    if (limit <= 0) limit = 15;
    if (limit > 100) limit = 100;

    /* 找文件：和 op_probe 走同一套逻辑（含"自动选的最新文件太旧就拒绝"） */
    char path[900] = {0};
    if (resolve_file(fname, path, sizeof(path)) != 0) {
        if (!fname[0]) return jerr("没有 downloads 目录，先用 curl 抓数据");
        return jerr("文件不存在");
    }
    if (auto_too_old(path, sizeof(path))) return jerr(auto_too_old_msg());
    struct stat st;
    if (stat(path, &st) != 0) return jerr("文件不存在");

    /* 数组路径候选：猜错一个就全盘失败，所以列表逐个试 */
    static char cand[12][300];
    int ncp = 0;
    if (qpath[0]) {
        if (qpath[0] == '.') snprintf(cand[ncp++], 300, "%s", qpath);
        else snprintf(cand[ncp++], 300, ".%s", qpath);
    } else {
        /* 从原文里看有哪些数组键 */
        static const char *keys[] = {"rows", "list", "items", "records", "feeds", "data", NULL};
        char rawhead[65536];
        FILE *hf = fopen(path, "rb");
        size_t hn = hf ? fread(rawhead, 1, sizeof(rawhead) - 1, hf) : 0;
        if (hf) fclose(hf);
        rawhead[hn] = 0;
        for (int i = 0; keys[i] && ncp < 8; i++) {
            if (!find_arr_start(rawhead, keys[i])) continue;
            snprintf(cand[ncp++], 300, ".data.%s", keys[i]);
            snprintf(cand[ncp++], 300, ".%s", keys[i]);
        }
        static const char *extra[] = {".data.rows", ".rows", ".data.list", ".list",
                                      ".data.items", ".items", ".data.records", ".data", NULL};
        for (int i = 0; extra[i] && ncp < 11; i++) {
            int dup = 0;
            for (int j = 0; j < ncp; j++) if (!strcmp(cand[j], extra[i])) { dup = 1; break; }
            if (!dup) snprintf(cand[ncp++], 300, "%s", extra[i]);
        }
    }

    char csvp[900] = {0};
    snprintf(csvp, sizeof(csvp), "%s", path);
    {
        char *dot = strrchr(csvp, '.');
        if (dot) *dot = 0;
        strncat(csvp, ".csv", sizeof(csvp) - strlen(csvp) - 1);
    }

    int have_jq = (access("/usr/bin/jq", X_OK) == 0 && !getenv("AGENT_NOJQ"));
    int ok = 0;
    char used[300] = {0};

    if (have_jq) {
        /* 一次 jq 直接吐完整 CSV（表头 + 数据）。
         * 关键在值处理：tostring（字符串原样、对象/数组转 JSON 文本）
         * + gsub 把换行/tab 清成空格 —— 否则一个多行正文就把整行 CSV 截断，
         * 后面所有行全部错位（实测第 18 行 30 列、第 19 行 45 列）。
         * 表头 ($k|@csv) 和数据行必须分开写：合并写会被再转义一层。 */
        for (int i = 0; i < ncp && !ok; i++) {
            char expr[900];
            snprintf(expr, sizeof(expr), "%s | (map(keys_unsorted)|add|unique) as $k "
                     "| ($k|@csv), (.[]|[$k[] as $f | "
                     "((.[$f] // \"\")|tostring|gsub(\"[\\n\\r\\t]+\";\" \"))]|@csv)",
                     cand[i]);
            static char csvbuf[8 * 1024 * 1024];
            if (run_jq_raw(expr, path, csvbuf, sizeof(csvbuf)) != 0 || !csvbuf[0]) continue;

            /* 看第一行是不是表头（字段名不该带转义引号） */
            if (strstr(csvbuf, "\\\"") == csvbuf) continue;

            FILE *cf = fopen(csvp, "w");
            if (!cf) break;
            fwrite(csvbuf, 1, strlen(csvbuf), cf);
            if (csvbuf[strlen(csvbuf) - 1] != '\n') fputc('\n', cf);
            fclose(cf);
            ok = 1;
            snprintf(used, sizeof(used), "%s", cand[i]);
        }
    }

    /* 没 jq 或 jq 全失败 → C 版兜底（简易扫描，嵌套字段会丢，但能用） */
    if (!ok) {
        FILE *f = fopen(path, "rb");
        if (!f) return jerr("打不开文件");
        long sz = st.st_size;
        if (sz > 16 * 1024 * 1024) { fclose(f); return jerr("文件太大（>16MB）"); }
        char *raw = malloc((size_t)sz + 1);
        if (!raw) { fclose(f); return NULL; }
        size_t rd = fread(raw, 1, (size_t)sz, f);
        raw[rd] = 0;
        fclose(f);

        const char *astart = NULL;
        static const char *kk[] = {"rows", "list", "items", "records", "feeds", "data", NULL};
        for (int i = 0; kk[i] && !astart; i++) astart = find_arr_start(raw, kk[i]);
        if (!astart) { free(raw); return jerr("没找到数组（用 op=jq 探测 path）"); }

        FILE *cf = fopen(csvp, "w");
        if (!cf) { free(raw); return jerr("写不了 CSV"); }
        /* 切元素 + 扫字段（全收，不按长度筛） */
        char keys[80][64];
        int nk = 0;
        {
            const char *q = astart;
            int el = 0;
            while (*q && nk < 80) {
                while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t' || *q == ',') q++;
                if (*q == ']' || !*q) break;
                const char *elem = q;
                int depth = 0, in_str = 0, esc = 0;
                while (*q) {
                    char c = *q;
                    if (in_str) {
                        if (esc) esc = 0;
                        else if (c == '\\') esc = 1;
                        else if (c == '"') in_str = 0;
                    } else {
                        if (c == '"') in_str = 1;
                        else if (c == '{' || c == '[') depth++;
                        else if (c == '}' || c == ']') { depth--; if (depth == 0) { q++; break; } }
                    }
                    q++;
                }
                long elen = (long)(q - elem);
                if (elen <= 0) break;
                /* 扫这条的字段名 */
                for (long i = 0; i + 2 < elen; i++) {
                    if (elem[i] != '"') continue;
                    long j = i + 1;
                    char kb[64];
                    int kl = 0;
                    while (j < elen && elem[j] != '"' && kl < 60) {
                        if (elem[j] == '\\') { j += 2; continue; }
                        kb[kl++] = elem[j++];
                    }
                    if (j >= elen || elem[j] != '"') break;
                    kb[kl] = 0;
                    j++;
                    while (j < elen && (elem[j] == ' ' || elem[j] == '\t')) j++;
                    if (j >= elen || elem[j] != ':') { i = j - 1; continue; }
                    int dup = 0;
                    for (int k = 0; k < nk; k++) if (!strcmp(keys[k], kb)) { dup = 1; break; }
                    if (!dup && nk < 80 && kb[0]) snprintf(keys[nk++], 64, "%s", kb);
                    i = j;
                }
                el++;
                if (el > 5000) break;
            }
        }
        if (nk == 0) { fclose(cf); free(raw); return jerr("没解析出字段"); }
        for (int k = 0; k < nk; k++) fprintf(cf, "%s\"%s\"", k ? "," : "", keys[k]);
        fprintf(cf, "\n");
        /* 值：简单取（值里含逗号会错，没有 jq 时的降级可接受） */
        {
            const char *q = astart;
            int el = 0;
            while (*q && el < 5000) {
                while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t' || *q == ',') q++;
                if (*q == ']' || !*q) break;
                const char *elem = q;
                int depth = 0, in_str = 0, esc = 0;
                while (*q) {
                    char c = *q;
                    if (in_str) {
                        if (esc) esc = 0;
                        else if (c == '\\') esc = 1;
                        else if (c == '"') in_str = 0;
                    } else {
                        if (c == '"') in_str = 1;
                        else if (c == '{' || c == '[') depth++;
                        else if (c == '}' || c == ']') { depth--; if (depth == 0) { q++; break; } }
                    }
                    q++;
                }
                long elen = (long)(q - elem);
                if (elen <= 0) break;
                for (int k = 0; k < nk; k++) {
                    char pat[80], val[400] = "";
                    snprintf(pat, sizeof(pat), "\"%s\"", keys[k]);
                    const char *hit = NULL;
                    for (long i = 0; i + (long)strlen(pat) < elen; i++)
                        if (memcmp(elem + i, pat, strlen(pat)) == 0) { hit = elem + i + strlen(pat); break; }
                    if (hit) {
                        while (hit < elem + elen && (*hit == ' ' || *hit == ':')) hit++;
                        size_t vo = 0;
                        if (*hit == '"') {
                            hit++;
                            while (hit < elem + elen && *hit != '"' && vo < 390) val[vo++] = *hit++;
                        } else {
                            while (hit < elem + elen && *hit != ',' && *hit != '}' && vo < 390)
                                val[vo++] = *hit++;
                        }
                        val[vo] = 0;
                    }
                    if (k) fputc(',', cf);
                    fputc('"', cf);
                    for (char *z = val; *z; z++) {
                        if (*z == '"') { fputc('"', cf); fputc('"', cf); }
                        else if (*z == '\n' || *z == '\r') fputc(' ', cf);
                        else fputc(*z, cf);
                    }
                    fputc('"', cf);
                }
                fprintf(cf, "\n");
                el++;
            }
        }
        fclose(cf);
        free(raw);
        ok = 1;
        snprintf(used, sizeof(used), "C");
    }

    if (!ok) return jerr("生成 CSV 失败");

    /* ===== 子表：主数组每条里的嵌套数组（article_category、tabs 这种）=====
     * 主表只放标量字段，嵌套内容塞进去会变成一坨 JSON 文本，没法看。
     * 每个嵌套数组单独导一个 CSV，带上"行号"关联回主表第几行。 */
    char sheets_json[4096] = {0};
    {
        size_t sj = 0;
        char base[900];
        snprintf(base, sizeof(base), "%s", csvp);
        char *bdot = strrchr(base, '.');
        if (bdot) *bdot = 0;

        /* 探测哪些字段是数组 */
        char subkeys[2048] = {0};
        char expr[900];
        snprintf(expr, sizeof(expr),
            /* 扫所有条目，不能只看第一条（第一条常缺字段，
             * 只探 [0] 会漏掉 article_category 这种只在部分条目里出现的子表） */
            "[%s[] | to_entries[] | select(.value|type==\"array\") | .key] | unique | .[]",
            used[0] ? used : ".data.rows");
        if (run_jq_raw(expr, path, subkeys, sizeof(subkeys)) == 0 && subkeys[0]) {
            char *sv = NULL;
            for (char *k = strtok_r(subkeys, "\n", &sv); k; k = strtok_r(NULL, "\n", &sv)) {
                if (!k[0] || sj + 400 >= sizeof(sheets_json)) continue;
                /* 子表：行号 + 子对象的所有字段 */
                snprintf(expr, sizeof(expr),
                    "%s | to_entries | map(.key as $i | (.value[\"%s\"] // [])[] | "
                    "{row: ($i + 1)} + (if type == \"object\" then . else {value: .} end)) "
                    "| (map(keys_unsorted)|add|unique) as $k "
                    "| ($k|@csv), (.[]|[$k[] as $f | ((.[$f] // \"\")|tostring|gsub(\"[\\n\\r\\t]+\";\" \"))]|@csv)",
                    used[0] ? used : ".data.rows", k);
                static char sbuf[4 * 1024 * 1024];
                if (run_jq_raw(expr, path, sbuf, sizeof(sbuf)) != 0 || !sbuf[0]) continue;

                char subp[1000];
                snprintf(subp, sizeof(subp), "%s_sub_%s.csv", base, k);
                FILE *sf = fopen(subp, "w");
                if (!sf) continue;
                fwrite(sbuf, 1, strlen(sbuf), sf);
                if (strlen(sbuf) && sbuf[strlen(sbuf) - 1] != '\n') fputc('\n', sf);
                fclose(sf);

                sj += (size_t)snprintf(sheets_json + sj, sizeof(sheets_json) - sj,
                    "%s{\"name\":\"%s\",\"path\":\"%s\"}", sj ? "," : "", k, subp);
            }
        }
    }


    int rows = 0, cols = 0;
    char *md = csv_to_markdown(csvp, 12, (int)limit, &rows, &cols);
    if (!md) return jerr("CSV 生成成功但读不回来");

    /* 自动选的源（用户没给 file）要在结果里写明，否则他会以为这是我们瞎编的 */
    if (g_auto_file[0]) {
        size_t need = strlen(md) + 300;
        char *md2 = malloc(need);
        if (md2) {
            snprintf(md2, need, "%s\n\n> 数据源：`%s`（%ld 分钟前，你没指定 file，自动选的）",
                     md, g_auto_file, g_auto_age / 60);
            free(md);
            md = md2;
        }
    }

    char ans[360];
    snprintf(ans, sizeof(ans), "从 %s 取出 %d 条 × %d 个字段%s",
             strrchr(path, '/') ? strrchr(path, '/') + 1 : path, rows, cols,
             have_jq && strcmp(used, "C") ? "（jq）" : "");
    char extra[1200];
    snprintf(extra, sizeof(extra), "\"rows\":%d,\"fields\":%d,\"file\":\"%s\",\"sheets\":[%s]",
             rows, cols, csvp, sheets_json);
    char *r = jpair(md, ans, extra);
    free(md);
    return r;
}
