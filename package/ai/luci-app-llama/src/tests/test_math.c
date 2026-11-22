/* test_math.c — math 插件回归测试
 *
 * 直接 dlopen 插件调 run()，不走框架、不碰模型。
 * 每项都写清了"输入 → 期望值"，答案错就红。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "plugin.h"

static const char *g_so = "./plugins/math.so";
static char *(*g_run)(const char *) = NULL;

static int fails, total;

/* 检查输出里是否包含 needle */
static void want(const char *json_in, const char *needle, const char *what) {
    total++;
    char *out = g_run(json_in);
    int ok = out && strstr(out, needle) != NULL;
    if (!ok) {
        fails++;
        printf("  ✗ %-34s 期望含 %-22s 实际: %.160s\n", what, needle, out ? out : "(null)");
    } else {
        printf("  ✓ %-34s %s\n", what, needle);
    }
    free(out);
}

/* 检查不含 needle（用于错误路径） */
static void want_not(const char *json_in, const char *needle, const char *what) {
    total++;
    char *out = g_run(json_in);
    int ok = out && !strstr(out, needle) && strstr(out, "\"error\"") == NULL;
    if (!ok) {
        fails++;
        printf("  ✗ %-34s 不该含 %-18s 实际: %.160s\n", what, needle, out ? out : "(null)");
    } else {
        printf("  ✓ %-34s 正常\n", what);
    }
    free(out);
}

int main(int argc, char **argv) {
    if (argc > 1) g_so = argv[1];
    void *h = dlopen(g_so, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", g_so, dlerror()); return 1; }
    plugin_desc_t *(*entry)(void) = (plugin_desc_t *(*)(void))dlsym(h, "plugin_entry");
    if (!entry) { fprintf(stderr, "没有 plugin_entry\n"); return 1; }
    plugin_desc_t *p = entry();
    g_run = p->run;
    printf("插件: %s — %s\n\n", p->name, p->description);

    printf("--- calc：表达式求值 ---\n");
    want("{\"op\":\"calc\",\"expr\":\"1+2*3\"}", "\"value\":7", "1+2*3 = 7（优先级）");
    want("{\"op\":\"calc\",\"expr\":\"(1+2)*3\"}", "\"value\":9", "(1+2)*3 = 9（括号）");
    want("{\"op\":\"calc\",\"expr\":\"2^10\"}", "\"value\":1024", "2^10 = 1024（乘方）");
    want("{\"op\":\"calc\",\"expr\":\"2^3^2\"}", "\"value\":512", "2^3^2 = 512（右结合）");
    want("{\"op\":\"calc\",\"expr\":\"-3^2\"}", "\"value\":-9", "-3^2 = -9");
    want("{\"op\":\"calc\",\"expr\":\"10%3\"}", "\"value\":1", "10%3 = 1");
    want("{\"op\":\"calc\",\"expr\":\"sqrt(16)+abs(-3)\"}", "\"value\":7", "sqrt(16)+abs(-3) = 7");
    want("{\"op\":\"calc\",\"expr\":\"min(3,5)+max(2,9)\"}", "\"value\":12", "min(3,5)+max(2,9) = 12");
    want("{\"op\":\"calc\",\"expr\":\"round(3.6)\"}", "\"value\":4", "round(3.6) = 4");
    want("{\"op\":\"calc\",\"expr\":\"1/4\"}", "\"value\":0.25", "1/4 = 0.25（小数）");
    want("{\"op\":\"calc\",\"expr\":\"(2+3)*(4-1)/5\"}", "\"value\":3", "(2+3)*(4-1)/5 = 3");
    want("{\"op\":\"calc\",\"expr\":\"1/0\"}", "error", "1/0 → 报错");
    want("{\"op\":\"calc\",\"expr\":\"1+\"}", "error", "1+ → 报错");

    printf("\n--- 各种写法的根号 / 常数 / 运算符 ---\n");
    want("{\"op\":\"calc\",\"expr\":\"sqrt(3)+2\"}", "\"value\":3.7320508", "sqrt(3)+2 = 3.732");
    want("{\"op\":\"calc\",\"expr\":\"√3+2\"}", "\"value\":3.7320508", "√3+2 = 3.732（unicode 根号）");
    want("{\"op\":\"calc\",\"expr\":\"根号3+2\"}", "\"value\":3.7320508", "根号3+2 = 3.732（中文）");
    want("{\"op\":\"calc\",\"expr\":\"√(3+1)\"}", "\"value\":2", "√(3+1) = 2（括号内先算）");
    want("{\"op\":\"calc\",\"expr\":\"√{16}\"}", "\"value\":4", "√{16} = 4（LaTeX 花括号）");
    want("{\"op\":\"calc\",\"expr\":\"2√3\"}", "\"value\":3.4641016", "2√3 = 3.464（隐式乘法）");
    want("{\"op\":\"calc\",\"expr\":\"π\"}", "\"value\":3.14159265", "π = 3.14159");
    want("{\"op\":\"calc\",\"expr\":\"2π+1\"}", "\"value\":7.2831853", "2π+1 = 7.283");
    want("{\"op\":\"calc\",\"expr\":\"6÷2\"}", "\"value\":3", "6÷2 = 3（unicode 除号）");
    want("{\"op\":\"calc\",\"expr\":\"2×3\"}", "\"value\":6", "2×3 = 6（unicode 乘号）");
    want("{\"op\":\"calc\",\"expr\":\"3−1\"}", "\"value\":2", "3−1 = 2（unicode 减号）");
    want("{\"op\":\"calc\",\"expr\":\"\\u221a3+2\"}", "\"value\":3.7320508", "\\uXXXX 转义能解开");

    printf("\n--- 中文数字 / 中文分数 ---\n");
    want("{\"op\":\"calc\",\"expr\":\"七百二十\"}", "\"value\":720", "七百二十 = 720");
    want("{\"op\":\"calc\",\"expr\":\"一千二百\"}", "\"value\":1200", "一千二百 = 1200");
    want("{\"op\":\"calc\",\"expr\":\"十二\"}", "\"value\":12", "十二 = 12");
    want("{\"op\":\"calc\",\"expr\":\"八十+五\"}", "\"value\":85", "八十+五 = 85");
    want("{\"op\":\"calc\",\"expr\":\"三分之二\"}", "\"value\":0.6666666667", "三分之二 ≈ 0.667");

    printf("\n--- solve：一元方程 ---\n");
    want("{\"op\":\"solve\",\"equation\":\"3x+5=20\"}", "\"answer\":\"x = 5\"", "3x+5=20 → x=5");
    want("{\"op\":\"solve\",\"equation\":\"5+3x=20\"}", "\"answer\":\"x = 5\"", "5+3x=20 → x=5（换写法）");
    want("{\"op\":\"solve\",\"equation\":\"2(x-3)=x+7\"}", "\"answer\":\"x = 13\"", "2(x-3)=x+7 → x=13");
    want("{\"op\":\"solve\",\"equation\":\"x/2+3=7\"}", "\"answer\":\"x = 8\"", "x/2+3=7 → x=8");
    want("{\"op\":\"solve\",\"equation\":\"x^2-5x+6=0\"}", "\"answer\":\"x = 2 或 3\"", "x²-5x+6=0 → 两根");
    want("{\"op\":\"solve\",\"equation\":\"x^2-5x+6=0\"}", "x₁ = 2", "  └ 有根 2");
    want("{\"op\":\"solve\",\"equation\":\"x^2+1=0\"}", "无实数解", "x²+1=0 → 无实数解");
    want("{\"op\":\"solve\",\"equation\":\"x+1=x+1\"}", "error", "恒等式 → 报错");
    want("{\"op\":\"solve\",\"equation\":\"x+1=x+2\"}", "error", "矛盾式 → 报错");

    printf("\n--- system：二元一次方程组 ---\n");
    want("{\"op\":\"system\",\"eqs\":[\"x+y=10\",\"x-y=2\"]}", "x = 6", "x+y=10, x-y=2 → x=6");
    want("{\"op\":\"system\",\"eqs\":[\"x+y=10\",\"x-y=2\"]}", "y = 4", "  └ y=4");
    want("{\"op\":\"system\",\"eqs\":[\"2x+3y=12\",\"x-y=1\"]}", "x = 3", "2x+3y=12, x-y=1 → x=3");

    printf("\n--- plan：计划vs实际 ---\n");
    want("{\"op\":\"plan\",\"task\":\"筑路队原计划每天修720米，实际每天多修80米，还差1200米时提前3天完成。全长多少米？\"}",
         "10800 米", "720/多80/还差1200/提前3 → 10800");
    want("{\"op\":\"plan\",\"task\":\"原计划每天修七百二十米，实际每天多修八十米，还差一千二百米时提前三天完成\"}",
         "10800 米", "同上但全中文数字 → 10800");
    want("{\"op\":\"plan\",\"task\":\"原计划每天修720米，实际每天少修80米，还差1200米时延后3天完成\"}",
         "28080 米", "少修+延后 → 28080");
    want("{\"op\":\"plan\",\"task\":\"原计划每天修720米，实际每天多修80米，提前3天完成\"}",
         "21600 米", "只提前无剩余 720t=800(t-3) → 21600");

    printf("\n--- meet：行程（相遇/追及/往返）---\n");
    want("{\"op\":\"meet\",\"task\":\"甲、乙两人相距420米，甲每分钟走80米，乙每分钟走60米，两人同时从两地出发，相遇后继续前进，到达对方出发点后立即返回，再次相遇。两次相遇地点之间相距多少米？\"}",
         "相距 120 米", "往返相遇 420/80/60 → 120");
    want("{\"op\":\"meet\",\"task\":\"甲、乙两人相距100米，甲每分钟走60米，乙每分钟走40米，相向出发，到达对方出发点后立即返回，再次相遇。两次相遇地点相距多少米？\"}",
         "相距 40 米", "往返相遇 100/60/40 → 40");
    want("{\"op\":\"meet\",\"task\":\"甲乙相距420米，甲每分钟走80米，乙每分钟走60米，同时出发相向而行，多久相遇？\"}",
         "甲出发点 240 米", "单次相遇 420/80/60 → 距甲 240");
    want("{\"op\":\"meet\",\"task\":\"甲乙相距420米，甲每分钟走80米，乙每分钟走60米，同时出发相向而行，多久相遇？\"}",
         "3 分钟后相遇", "  └ 时间 3 分钟");

    printf("\n--- ring / train：环形跑道、火车过桥 ---\n");
    want("{\"task\":\"环形跑道一圈400米，甲每分钟走100米，乙每分钟走60米，同时同地同向出发，甲第一次追上乙需要多少分钟？\"}",
         "10 分钟后", "环形同向 400/(100-60) → 10 分");
    want("{\"task\":\"环形跑道一圈400米，甲每分钟走100米，乙每分钟走60米，同时同地反向而行，多久相遇？\"}",
         "2.5 分钟后", "环形反向 400/(100+60) → 2.5 分");
    want("{\"task\":\"环形跑道一圈400米，甲每分钟走100米，乙每分钟走60米，同向出发，甲第二次追上乙需要多少分钟？\"}",
         "第 2 次：20 分钟", "  └ 第2次同向 2×400/40 → 20 分");
    want("{\"task\":\"环形跑道一圈400米，甲每分钟走100米，乙每分钟走60米，反向而行，第3次相遇需要多久？\"}",
         "第 3 次：7.5 分钟", "  └ 第3次反向 3×400/160 → 7.5 分");
    want("{\"task\":\"一列火车长200米，以每秒20米的速度通过一座长600米的桥，需要多长时间？\"}",
         "需要 40 秒", "火车过桥 (200+600)/20 → 40 秒");
    want("{\"task\":\"一列火车长180米，通过一根电线杆用了9秒，这列火车速度是多少？\"}",
         "20 米/秒", "火车过电线杆 180/9 → 20 米/秒");

    printf("\n--- work / chicken：工程合作、鸡兔同笼 ---\n");
    want("{\"task\":\"一项工程甲单独做12天完成，乙单独做15天完成，两人合作几天完成？\"}",
         "20/3", "工程合作 12+15 → 20/3 天");
    want("{\"task\":\"一项工程甲单独做6天完成，乙单独做3天完成，两人合作几天完成？\"}",
         "2 天", "  └ 6+3 → 2 天（整数不显示分数）");
    want("{\"task\":\"鸡兔同笼，共35个头，94只脚，鸡和兔各多少只？\"}",
         "鸡 23 只，兔 12 只", "鸡兔同笼 35头94脚 → 鸡23兔12");
    want("{\"task\":\"鸡兔同笼，共10个头，28只脚，鸡和兔各多少只？\"}",
         "鸡 6 只，兔 4 只", "  └ 10头28脚 → 鸡6兔4");

    printf("\n--- stat：统计 ---\n");
    want("{\"op\":\"stat\",\"values\":[1,2,3,4]}", "平均 | 2.5", "[1,2,3,4] 平均 2.5");
    want("{\"op\":\"stat\",\"values\":[1,2,3,4]}", "中位数 | 2.5", "  └ 中位数 2.5");
    want("{\"op\":\"stat\",\"values\":[1,2,3,4]}", "和 | 10", "  └ 和 10");
    want("{\"op\":\"stat\",\"values\":[1,2,3,4]}", "方差 | 1.25", "  └ 方差 1.25");
    want("{\"op\":\"stat\",\"values\":[1,2,3,4]}", "最小值 | 1", "  └ 最小 1");
    want("{\"op\":\"stat\",\"values\":[1,2,3,4]}", "最大值 | 4", "  └ 最大 4");
    want("{\"op\":\"stat\",\"values\":[5,3,9,1]}", "中位数 | 4", "未排序 [5,3,9,1] 中位数 4");

    printf("\n--- num：数论 ---\n");
    want("{\"op\":\"num\",\"sub\":\"gcd\",\"values\":[12,18]}", "\"value\":6", "gcd(12,18) = 6");
    want("{\"op\":\"num\",\"sub\":\"lcm\",\"values\":[12,18]}", "\"value\":36", "lcm(12,18) = 36");
    want("{\"op\":\"num\",\"sub\":\"factorial\",\"values\":[5]}", "\"value\":120", "5! = 120");
    want("{\"op\":\"num\",\"sub\":\"comb\",\"values\":[5,2]}", "\"value\":10", "C(5,2) = 10");
    want("{\"op\":\"num\",\"sub\":\"perm\",\"values\":[5,2]}", "\"value\":20", "P(5,2) = 20");
    want("{\"op\":\"num\",\"sub\":\"fib\",\"values\":[10]}", "\"value\":55", "fib(10) = 55");
    want("{\"op\":\"num\",\"sub\":\"factor\",\"values\":[12]}", "2,2,3", "12 = 2×2×3");
    want("{\"op\":\"num\",\"sub\":\"factor\",\"values\":[97]}", "\"factors\":[97]", "97 是质数");

    printf("\n--- 自动判断（不给 op）---\n");
    want("{\"expr\":\"12*15\"}", "\"value\":180", "只有 expr 数字 → calc");
    want("{\"expr\":\"3x=15\"}", "\"answer\":\"x = 5\"", "expr 带等号 → solve");
    want("{\"task\":\"原计划每天修720米，实际每天多修80米，还差1200米时提前3天完成\"}",
         "10800", "只有 task → plan");

    printf("\n%s（%d 项，失败 %d）\n", fails ? "有失败" : "全部通过", total, fails);
    dlclose(h);
    return fails ? 1 : 0;
}
