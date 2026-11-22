/* json.h — 完整 JSON 解析器（递归下降）
 *
 * 为什么不用 strstr 找 key：模型输出里嵌套对象、数组、字符串内出现
 * \"tool\" 这类内容时，strstr 会匹配到错误的位置。这里做真正的解析。
 *
 * 支持：对象 / 数组 / 字符串（含 \" \\ \/ \b \f \n \r \t \uXXXX 含代理对）/
 *       数字（整数/小数/指数/负数）/ true / false / null
 */
#ifndef AGENT_JSON_H
#define AGENT_JSON_H

#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
    jtype type;
    char *s;              /* J_STR 的值（UTF-8，malloc） */
    double num;           /* J_NUM */
    int b;                /* J_BOOL */
    char **keys;          /* J_OBJ 的键（malloc） */
    struct jval **items;  /* J_ARR / J_OBJ 的值 */
    int n;
} jval;

/* 解析整段文本；失败返回 NULL 并写 err */
jval *json_parse(const char *text, char *err, size_t errcap);
void json_free(jval *v);

/* 取值：拿不到返回 NULL */
jval *json_get(const jval *obj, const char *key);     /* 对象里按 key */
jval *json_at(const jval *arr, int i);                /* 数组下标 */
const char *json_str(const jval *v);                  /* J_STR → 字符串，其它 NULL */
int json_int(const jval *v, int dflt);                /* J_NUM/J_BOOL → int */
double json_num(const jval *v, double dflt);
int json_bool(const jval *v, int dflt);

#endif
