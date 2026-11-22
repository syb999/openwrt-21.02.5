/* plugin.h — agent 插件统一接口
 *
 * 每个插件是一个 .so，导出唯一的 plugin_entry()。
 * 框架负责：dlopen 目录下所有 .so → 调 plugin_entry() 注册 → 按模型请求 dispatch。
 *
 * 约定：
 *   - 名称唯一（music / math / ppt / table / doc ...）
 *   - run() 的入参、返回值都是 UTF-8 JSON 字符串
 *   - 返回的字符串用 malloc 分配，框架负责 free
 *   - 出错也要返回合法 JSON：{"error":"..."}
 *
 * 新增一个插件只需要：
 *   1. 写 plugins/xxx.c，实现 run()
 *   2. 加进 Makefile 的 PLUGINS 列表
 *   3. make → 生成的 xxx.so 扔进 /opt/llama/agent/plugins/
 * 框架不用改一行。
 */
#ifndef AGENT_PLUGIN_H
#define AGENT_PLUGIN_H

#define PLUGIN_API_VERSION 1

typedef struct {
    int api_version;            /* 必须是 PLUGIN_API_VERSION */
    const char *name;           /* 唯一名，会被写进工具的 enum */
    const char *description;    /* 一句话说明（给模型看） */
    const char *params;         /* 参数说明，JSON 片段或纯文字 */
    /* 执行：in 是 JSON（含 path/args 等），返回 malloc 的 JSON */
    char *(*run)(const char *json_in);
    /* 可选别名，逗号分隔。模型常把子功能名当工具名（把 op=calc 写成
     * tool=calc），声明后框架会把这些名字路由到本插件。
     * 现有插件按位置初始化时会自动置 NULL，不用改。 */
    const char *aliases;
} plugin_desc_t;

/* 每个 .so 必须导出这个符号 */
plugin_desc_t *plugin_entry(void);

/* ============================================================
 * 自动化插件（第二类插件，可选）
 *
 * 上面 plugin_desc_t 描述的是「被动工具」：模型决定调用一次，
 * run() 返回一段数据就结束。
 *
 * 自动化插件是另一回事——它自己跑一整套流程（巡检、批量处理、
 * 定时任务、长时间采集…），不需要模型一步一步指挥。这类插件
 * 单独导出 plugin_auto_entry()，框架用 dlsym 探测：
 *
 *   有 plugin_entry()      → 作为工具挂给模型（可以被模型调度）
 *   有 plugin_auto_entry() → 作为自动化能力（由用户/定时/事件触发）
 *   两个都有               → 既能被模型调用，也能独立跑
 *
 * 为什么用独立符号而不是往 plugin_desc_t 里加字段：
 * 加字段会改变结构体大小，已经编译好的旧 .so 返回的是小结构体，
 * 框架按大结构体读就越界了。独立符号能做到零影响。
 *
 * 实现约定：
 *   - start() 在框架 fork 出的子进程里执行，可以长时间跑
 *   - 进度直接 printf 到 stdout（CGI 会重定向进 job 日志，前端轮询可见）
 *   - 中止靠 SIGTERM（框架/CGI 发），插件应捕获后清理退出
 *   - status()/stop() 是给需要精细控制的插件留的，可实现，也可置 NULL
 * ============================================================ */
#define PLUGIN_AUTO_API_VERSION 1

typedef struct {
    int api_version;            /* 必须是 PLUGIN_AUTO_API_VERSION */
    const char *name;           /* 自动化能力名，如 "sysmon" */
    const char *description;    /* 给人和模型看的说明 */
    const char *triggers;       /* 触发方式声明：
                                 *   "manual"           手动
                                 *   "cron:0 3 * * *"   定时
                                 *   "event:boot"       开机
                                 *   "event:ifup"       网络就绪
                                 * 多个用逗号分隔 */
    const char *params;         /* 参数说明（JSON 片段） */

    /* 启动一次流程。在子进程里同步执行，可以长时间运行。
     * 返回 malloc 的 JSON 作为结论（{"ok":1,...} 或 {"error":"..."}）；
     * 返回 NULL 表示异常。 */
    char *(*start)(const char *json_in);

    /* 可选：查询进度。job 是框架给的 id。
     * 返回 {"state":"running|done|failed","progress":0-100,"text":"..."} */
    char *(*status)(const char *job);

    /* 可选：请求中止。返回 0 表示已受理。 */
    int (*stop)(const char *job);
} plugin_auto_t;

/* 自动化插件导出这个符号（可选，和 plugin_entry 互不冲突） */
plugin_auto_t *plugin_auto_entry(void);

#endif
