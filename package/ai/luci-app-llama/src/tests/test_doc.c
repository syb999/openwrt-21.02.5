/* 直接调 doc.so 的读接口，验证纯 C 提取（不走模型、不走 agent） */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "plugin.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("用法: test_doc <文件>\n"); return 2; }
    void *h = dlopen("./plugins/doc.so", RTLD_NOW);
    if (!h) { printf("dlopen 失败: %s\n", dlerror()); return 1; }
    plugin_desc_t *(*entry)(void) = (plugin_desc_t *(*)(void))dlsym(h, "plugin_entry");
    if (!entry) { printf("没有 plugin_entry\n"); return 1; }
    plugin_desc_t *p = entry();
    char req[600];
    snprintf(req, sizeof(req), "{\"path\":\"%s\"}", argv[1]);
    char *r = p->run(req);
    if (!r) { printf("(返回空)\n"); return 1; }
    printf("%s\n", r);
    free(r);
    dlclose(h);
    return 0;
}
