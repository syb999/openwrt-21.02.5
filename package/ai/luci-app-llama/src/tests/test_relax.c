#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "json.h"

/* 复制 agent.c 里的修复逻辑，单独验证它到底对不对 */
static jval *relax(const char *content) {
    char err[128] = {0};
    jval *reply = json_parse(content, err, sizeof(err));
    const char *ck = reply ? json_str(json_get(reply, "tool")) : NULL;
    if (ck && ck[0]) return reply;

    char *s1 = malloc(strlen(content) + 8);
    char *s2 = malloc(strlen(content) + 8);
    if (!s1 || !s2) { free(s1); free(s2); return reply; }

    size_t fi = 0;
    for (const char *q = content; *q; q++) {
        if (q[0] == '\\' && q[1] == '"') {
            char n = q[2];
            if (n == ':' || n == ',' || n == '}' || n == ']' || n == 0
                || n == '\n' || n == '\r' || n == ' ') {
                s1[fi++] = '"';
                q++;
                continue;
            }
        }
        s1[fi++] = *q;
    }
    s1[fi] = 0;

    fi = 0;
    for (const char *q = s1; *q; q++) {
        if (*q == '"' && q > s1) {
            char prev = q[-1], next = q[1];
            if (isdigit((unsigned char)prev)
                && (next == ',' || next == '}' || next == ']' || next == '\n' || next == 0))
                continue;
        }
        s2[fi++] = *q;
    }
    s2[fi] = 0;

    printf("  修1 → %s\n", s1);
    printf("  修2 → %s\n", s2);

    jval *r1 = json_parse(s1, err, sizeof(err));
    if (r1 && json_str(json_get(r1, "tool")) && json_str(json_get(r1, "tool"))[0]) {
        printf("  ✓ 修1 可用\n");
        free(s1); free(s2);
        return r1;
    }
    jval *r2 = json_parse(s2, err, sizeof(err));
    if (r2 && json_str(json_get(r2, "tool")) && json_str(json_get(r2, "tool"))[0]) {
        printf("  ✓ 修2 可用\n");
        free(s1); free(s2);
        return r2;
    }
    printf("  ✗ 两种修复都失败 (err=%s)\n", err);
    free(s1); free(s2);
    return reply;
}

int main(void) {
    const char *bad =
        "{\"tool\":\"jq\",\"file\":\"/tmp/x.json\",\"path\":\".data.rows\","
        "\"offset\":0,\"limit\\\":3\\\",\"query\":\".article_title\"}";
    printf("原始: %s\n", bad);
    jval *v = relax(bad);
    if (v) {
        const char *t = json_str(json_get(v, "tool"));
        printf("结果: tool=%s\n", t ? t : "NULL");
        const char *l = json_str(json_get(v, "file"));
        printf("      file=%s\n", l ? l : "NULL");
        json_free(v);
    } else {
        printf("结果: 仍然失败\n");
    }
    return 0;
}
