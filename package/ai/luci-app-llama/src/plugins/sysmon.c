/* sysmon.c — 自动化插件示例：系统巡检
 *
 * 它同时导出两个符号：
 *   plugin_entry()       → 挂成被动工具，模型问"系统负载怎么样"可以调
 *   plugin_auto_entry()  → 挂成自动化能力，跑一整套巡检流程
 *
 * 自动化那半边是重点：一次调用里自己循环采样 N 轮、判定阈值、汇总结论，
 * 全程 printf 进度（CGI 把 stdout 重定向进 job 日志，前端轮询就能看到）。
 * 收到 SIGTERM 会在当前轮次后干净退出（stop 语义）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "plugin.h"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ---------------- 取值 ---------------- */
static int jget_int(const char *js, const char *key, int dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(js, pat);
    return p ? atoi(p + strlen(pat)) : dflt;
}

static double read_load1(void) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1;
    double v = -1;
    if (fscanf(f, "%lf", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static double read_mem_pct(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    double total = 0, avail = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long kb;
        if (sscanf(line, "MemTotal: %lu kB", &kb) == 1) total = (double)kb;
        else if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1) avail = (double)kb;
    }
    fclose(f);
    if (total <= 0) return -1;
    if (avail <= 0) return -1;
    return (total - avail) / total * 100.0;
}

static double read_temp(void) {
    const char *paths[] = {
        "/sys/class/thermal/thermal_zone0/temp",
        "/sys/class/hwmon/hwmon0/temp1_input",
        NULL
    };
    for (int i = 0; paths[i]; i++) {
        FILE *f = fopen(paths[i], "r");
        if (!f) continue;
        long v = 0;
        int ok = fscanf(f, "%ld", &v) == 1;
        fclose(f);
        if (ok && v > 0) return (double)v / 1000.0;    /* 毫度 → 度 */
    }
    return -1;
}

/* ---------------- 被动工具：一次性快照 ---------------- */
static char *snapshot(void) {
    double l = read_load1(), m = read_mem_pct(), t = read_temp();
    char ans[256];
    snprintf(ans, sizeof(ans), "负载 %.2f，内存 %.1f%%%s", l,
             m, t > 0 ? "" : "");
    char val[512];
    snprintf(val, sizeof(val),
        "| 项 | 值 |\n|---|---|\n| 1 分钟负载 | %.2f |\n| 内存占用 | %.1f%% |\n| 温度 | %s%.1f |\n",
        l, m, t > 0 ? "" : "-", t > 0 ? t : 0);

    /* 拼 JSON（value=表格，answer=一句话） */
    char *o = malloc(strlen(val) + strlen(ans) + 128);
    if (!o) return NULL;
    size_t i = 0;
    i += (size_t)snprintf(o + i, strlen(val) + strlen(ans) + 128, "{\"value\":\"");
    for (const char *p = val; *p; p++) {
        if (*p == '"' || *p == '\\') { o[i++] = '\\'; o[i++] = *p; }
        else if (*p == '\n') { o[i++] = '\\'; o[i++] = 'n'; }
        else o[i++] = *p;
    }
    i += (size_t)snprintf(o + i, strlen(val) + strlen(ans) + 128 - i, "\",\"answer\":\"%s\"}", ans);
    return o;
}

static char *tool_run(const char *json_in) {
    (void)json_in;
    return snapshot();
}

/* ---------------- 自动化：整套巡检 ---------------- */
static char *auto_start(const char *json_in) {
    /* rounds 默认 0 = 一直跑（守护模式）；传正数就只跑那么多轮 */
    int interval = jget_int(json_in, "interval", 60);
    int rounds = jget_int(json_in, "rounds", 0);
    int cpu_warn = jget_int(json_in, "load_warn", 4);      /* 负载阈值（×100 比较） */
    int mem_warn = jget_int(json_in, "mem_warn", 85);

    /* 前台一次性运行时框架会设 AGENT_AUTO_FG：
     * 这时不该一直循环，收敛成几轮、间隔调短，十几秒内出结果。
     * （守护模式则保持 60 秒一轮、一直跑） */
    if (getenv("AGENT_AUTO_FG")) {
        if (rounds == 0) rounds = 5;
        if (interval >= 60) interval = 2;
    }

    if (interval < 1) interval = 1;
    if (interval > 3600) interval = 3600;
    if (rounds < 0) rounds = 0;
    if (rounds > 5000) rounds = 5000;
    int infinite = (rounds == 0);
    if (cpu_warn <= 0) cpu_warn = 4;
    if (mem_warn <= 0) mem_warn = 85;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    if (infinite)
        printf("开始系统巡检：持续运行，每 %ds 一轮（停止请按页面「停止」或发 SIGTERM）\n\n", interval);
    else
        printf("开始系统巡检：%d 轮，间隔 %ds（要中止发 SIGTERM）\n\n", rounds, interval);
    fflush(stdout);

    double worst_load = 0, worst_mem = 0, max_temp = 0;
    int warns = 0;

    for (int i = 0; (infinite || i < rounds) && !g_stop; i++) {
        double l = read_load1(), m = read_mem_pct(), t = read_temp();
        if (l > worst_load) worst_load = l;
        if (m > worst_mem) worst_mem = m;
        if (t > max_temp) max_temp = t;

        if (infinite)
            printf("[#%d] 负载 %.2f  内存 %.1f%%  温度 %s%.1f\n",
                   i + 1, l, m, t > 0 ? "" : "-", t > 0 ? t : 0);
        else
            printf("[%d/%d] 负载 %.2f  内存 %.1f%%  温度 %s%.1f\n",
                   i + 1, rounds, l, m, t > 0 ? "" : "-", t > 0 ? t : 0);
        if (l * 100 > cpu_warn * 100) {
            printf("  ⚠ 负载偏高（%d 核参考阈值 %.2f）\n", 4, (double)cpu_warn);
            warns++;
        }
        if (m > mem_warn) {
            printf("  ⚠ 内存占用 %.1f%% 超过 %d%%\n", m, mem_warn);
            warns++;
        }
        if (t > 75) {
            printf("  ⚠ 温度 %.1f 度偏高\n", t);
            warns++;
        }
        fflush(stdout);

        if ((infinite || i + 1 < rounds) && !g_stop) {
            for (int s = 0; s < interval && !g_stop; s++) sleep(1);
        }
    }

    printf("\n巡检结束%s\n", g_stop ? "（收到中止信号）" : "");
    printf("峰值：负载 %.2f  内存 %.1f%%  温度 %s%.1f  告警 %d 次\n",
           worst_load, worst_mem, max_temp > 0 ? "" : "-", max_temp > 0 ? max_temp : 0, warns);

    char *o = malloc(512);
    if (!o) return NULL;
    snprintf(o, 512,
        "{\"ok\":1,\"answer\":\"巡检完成：峰值负载 %.2f、内存 %.1f%%、温度 %.1f度，告警 %d 次\","
        "\"warns\":%d,\"worst_load\":%.2f,\"worst_mem\":%.1f,\"max_temp\":%.1f}",
        worst_load, worst_mem, max_temp > 0 ? max_temp : 0, warns,
        warns, worst_load, worst_mem, max_temp > 0 ? max_temp : 0);
    return o;
}

/* ---------------- 注册 ---------------- */
static plugin_desc_t desc = {
    PLUGIN_API_VERSION,
    "sysmon",
    "系统快照：1 分钟负载、内存占用、芯片温度（自动巡检能力是 sysmon-auto）",
    "{}",
    tool_run,
    NULL
};

static plugin_auto_t adesc = {
    PLUGIN_AUTO_API_VERSION,
    "sysmon-auto",
    "系统巡检：持续采样负载/内存/温度，超阈值告警（默认 60 秒一轮、一直跑，手动启动/停止）",
    "manual,cron:0 * * * *",
    "{\"interval\":60,\"rounds\":0,\"load_warn\":4,\"mem_warn\":85}",
    auto_start,
    NULL,
    NULL
};

plugin_desc_t *plugin_entry(void) { return &desc; }
plugin_auto_t *plugin_auto_entry(void) { return &adesc; }
