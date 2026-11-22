/* test_plugin.c — 单独测一个插件（不启动 llama-server）
 *   ./test_plugin plugins/math.so '{"tool":"math","task":"..."}'
 */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include "plugin.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <plugin.so> '<json>'\n", argv[0]);
        return 1;
    }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) {
        fprintf(stderr, "dlopen 失败: %s\n", dlerror());
        return 1;
    }
    plugin_desc_t *(*entry)(void) =
        (plugin_desc_t *(*)(void))dlsym(h, "plugin_entry");
    if (!entry) {
        fprintf(stderr, "没有 plugin_entry\n");
        return 1;
    }
    plugin_desc_t *p = entry();
    printf("插件: %s\n说明: %s\n参数: %s\n", p->name, p->description, p->params);
    char *out = p->run(argv[2]);
    printf("输出: %s\n", out ? out : "(null)");
    free(out);
    return 0;
}
