/* memcheck.c — 分配记账（链接期 --wrap，零侵入）
 *
 * 用途：--memcheck 模式下每步打印 alloc/free/live 与 RSS，
 *       跑几十个任务后看 live 是否收敛。
 *
 * 已知边界（必须理解，否则会误读数字）：
 *   1. --wrap 只拦截**本可执行文件**里对 malloc/free 的调用。
 *      插件是独立 .so，它内部的 malloc 不走这里；但插件返回给框架的字符串
 *      由框架 free，会记一次 free 而没有对应 alloc。
 *      → 所以 live 每调用一次这类插件会 -1（偏负），这是记账口径问题，不是泄漏。
 *   2. 判据不是 live 的绝对值，而是「同一路径重复执行时 live 是否线性增长」。
 *   3. RSS（从 /proc/self/statm 读）是跨边界有效的交叉验证：真正的泄漏会推高 RSS。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t);
void *__real_calloc(size_t, size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
char *__real_strdup(const char *);

static long n_alloc, n_free, n_realloc, live;

void *__wrap_malloc(size_t n) {
    void *p = __real_malloc(n);
    if (p) { n_alloc++; live++; }
    return p;
}

void *__wrap_calloc(size_t a, size_t b) {
    void *p = __real_calloc(a, b);
    if (p) { n_alloc++; live++; }
    return p;
}

char *__wrap_strdup(const char *s) {
    char *p = __real_strdup(s);
    if (p) { n_alloc++; live++; }
    return p;
}

void *__wrap_realloc(void *old, size_t n) {
    void *p = __real_realloc(old, n);
    if (p) {
        n_realloc++;
        if (!old) { n_alloc++; live++; }
    }
    return p;
}

void __wrap_free(void *p) {
    if (p) { n_free++; live--; }
    __real_free(p);
}

/* 当前 RSS / 峰值 RSS（KB） */
static long proc_rss_kb(long *peak) {
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) { if (peak) *peak = 0; return 0; }
    long size = 0, resident = 0;
    if (fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0;
    fclose(f);
    long kb = resident * 4;              /* 页 4KB */
    if (peak) *peak = kb;
    return kb;
}

void memcheck_report(const char *where) {
    long peak = 0;
    long rss = proc_rss_kb(&peak);
    fprintf(stderr, "[mem] %-12s alloc=%ld free=%ld realloc=%ld live=%ld rss=%ldKB\n",
            where ? where : "", n_alloc, n_free, n_realloc, live, rss);
}
