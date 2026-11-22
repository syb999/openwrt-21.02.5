/* test_json.c — JSON 解析器回归测试（各类格式） */
#include <stdio.h>
#include <string.h>
#include "json.h"

static int fails = 0;

#define CHECK(cond, msg) do { \
    if (cond) printf("  ✓ %s\n", msg); \
    else { printf("  ✗ %s\n", msg); fails++; } \
} while (0)

int main(void) {
    char err[256];

    printf("--- 基本对象 ---\n");
    jval *v = json_parse("{\"a\":1,\"b\":\"x\"}", err, sizeof(err));
    CHECK(v && json_int(json_get(v, "a"), -1) == 1, "整数");
    CHECK(v && json_str(json_get(v, "b")) && !strcmp(json_str(json_get(v, "b")), "x"), "字符串");
    json_free(v);

    printf("--- 嵌套 ---\n");
    v = json_parse("{\"a\":{\"b\":[1,2,{\"c\":\"deep\"}]}}", err, sizeof(err));
    if (v) {
        jval *arr = json_get(json_get(v, "a"), "b");
        jval *third = json_at(arr, 2);
        CHECK(arr && arr->n == 3, "数组长度 3");
        CHECK(third && !strcmp(json_str(json_get(third, "c")), "deep"), "三层嵌套取值");
    } else { CHECK(0, "嵌套解析"); }
    json_free(v);

    printf("--- 字符串转义 ---\n");
    v = json_parse("{\"s\":\"a\\nb\\tc\\\"d\\\\e\"}", err, sizeof(err));
    CHECK(v && !strcmp(json_str(json_get(v, "s")), "a\nb\tc\"d\\e"), "\\n \\t \\\" \\\\");
    json_free(v);

    printf("--- \\uXXXX（中文）---\n");
    v = json_parse("{\"s\":\"\\u4e2d\\u6587\\u6d4b\\u8bd5\"}", err, sizeof(err));
    CHECK(v && !strcmp(json_str(json_get(v, "s")), "中文测试"), "4e2d 6587 6d4b 8bd5 → 中文测试");
    json_free(v);

    printf("--- 代理对（emoji）---\n");
    v = json_parse("{\"s\":\"\\ud83d\\ude00\"}", err, sizeof(err));
    CHECK(v && strlen(json_str(json_get(v, "s"))) == 4, "U+1F600 → 4 字节 UTF-8");
    json_free(v);

    printf("--- 数字/布尔/null ---\n");
    v = json_parse("{\"n\":-1.5e3,\"b\":true,\"f\":false,\"z\":null}", err, sizeof(err));
    CHECK(v && json_num(json_get(v, "n"), 0) == -1500.0, "-1.5e3 = -1500");
    CHECK(v && json_bool(json_get(v, "b"), 0) == 1, "true");
    CHECK(v && json_bool(json_get(v, "f"), 1) == 0, "false");
    CHECK(v && json_get(v, "z") && json_get(v, "z")->type == J_NULL, "null");
    json_free(v);

    printf("--- 顶层数组 ---\n");
    v = json_parse("[{\"pitch\":69,\"dur\":1},{\"pitch\":57,\"dur\":2}]", err, sizeof(err));
    CHECK(v && v->type == J_ARR && v->n == 2, "数组 2 个元素");
    CHECK(v && json_int(json_get(json_at(v, 1), "pitch"), -1) == 57, "第二元素 pitch=57");
    json_free(v);

    printf("--- llama-server 响应结构 ---\n");
    v = json_parse("{\"choices\":[{\"index\":0,\"message\":{\"role\":\"assistant\","
                   "\"content\":\"{\\\"tool\\\":\\\"final\\\",\\\"answer\\\":\\\"中文答案\\\"}\"}}],"
                   "\"usage\":{\"completion_tokens\":12}}", err, sizeof(err));
    if (v) {
        jval *msg = json_get(json_at(json_get(v, "choices"), 0), "message");
        const char *content = json_str(json_get(msg, "content"));
        CHECK(content != NULL, "取到 content");
        /* content 里又是一层 JSON */
        jval *inner = json_parse(content, err, sizeof(err));
        CHECK(inner && !strcmp(json_str(json_get(inner, "tool")), "final"), "content 内层 tool");
        CHECK(inner && !strcmp(json_str(json_get(inner, "answer")), "中文答案"), "content 内层中文 answer");
        json_free(inner);
    } else { CHECK(0, "llama 响应解析"); }
    json_free(v);

    printf("--- 键名冲突（strstr 会错的场景）---\n");
    v = json_parse("{\"note\":\"里面提到 \\\"tool\\\":\\\"x\\\" 字样\",\"tool\":\"real\"}", err, sizeof(err));
    CHECK(v && !strcmp(json_str(json_get(v, "tool")), "real"), "字符串里的 tool 不干扰");
    json_free(v);

    printf("--- 非法输入 ---\n");
    v = json_parse("{\"a\":}", err, sizeof(err));
    CHECK(v == NULL, "缺值 → 报错");
    if (v) json_free(v);
    v = json_parse("{\"a\":\"未闭合", err, sizeof(err));
    CHECK(v == NULL, "未闭合字符串 → 报错");
    if (v) json_free(v);

    printf("\n%s（%d 项失败）\n", fails ? "有失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
