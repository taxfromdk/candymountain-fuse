/*
 * test_json - the JSON parser: values, nesting, escapes, numbers, errors,
 * and a real server answer shape.
 */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks_run;

#define CHECK(cond) do {                                                  \
    checks_run++;                                                         \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        exit(1);                                                          \
    }                                                                     \
} while (0)

static json_t *parse(const char *s)
{
    return json_parse(s, strlen(s), NULL);
}

int main(void)
{
    json_t *j;

    /* scalars */
    j = parse("  42 "); CHECK(j && json_type(j) == JSON_NUMBER && json_int(j, 0) == 42); json_free(j);
    j = parse("-3.5e2"); CHECK(j && json_num(j, 0) == -350.0 && json_int(j, 0) == -350); json_free(j);
    j = parse("12345678901234"); CHECK(j && json_int(j, 0) == 12345678901234LL); json_free(j);
    j = parse("true"); CHECK(j && json_bool(j, 0) == 1); json_free(j);
    j = parse("false"); CHECK(j && json_type(j) == JSON_BOOL && json_bool(j, 1) == 0); json_free(j);
    j = parse("null"); CHECK(j && json_type(j) == JSON_NULL); json_free(j);
    j = parse("\"a\\\"b\\\\c\\nd\\u00e9\\u20ac\\ud83d\\ude00\"");
    CHECK(j && strcmp(json_str(j, ""), "a\"b\\c\nd\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80") == 0);
    json_free(j);

    /* containers and lookups */
    j = parse("{\"a\":[1,2,{\"b\":\"x\"}],\"c\":{},\"d\":[],\"e\":null}");
    CHECK(j != NULL && json_type(j) == JSON_OBJECT && json_len(j) == 4);
    CHECK(json_len(json_get(j, "a")) == 3);
    CHECK(json_int(json_at(json_get(j, "a"), 1), 0) == 2);
    CHECK(strcmp(json_str(json_get(json_at(json_get(j, "a"), 2), "b"), ""), "x") == 0);
    CHECK(json_len(json_get(j, "c")) == 0 && json_len(json_get(j, "d")) == 0);
    CHECK(json_get(j, "zz") == NULL && json_at(json_get(j, "a"), 3) == NULL);
    CHECK(strcmp(json_key_at(j, 2), "d") == 0);
    /* defaults on missing / mistyped */
    CHECK(json_int(json_get(j, "zz"), 7) == 7 && json_str(json_get(j, "a"), "d") != NULL);
    CHECK(json_bool(json_get(j, "e"), 3) == 3);
    json_free(j);

    /* errors */
    {
        const char *bad[] = { "", "{", "[1,]", "{\"a\" 1}", "\"abc", "tru", "1 2",
                              "{\"a\":1,}", "\"\\x\"", "[", "nul", "\"\\u12\"", "01x" };
        size_t pos;

        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
            pos = 999;
            j = json_parse(bad[i], strlen(bad[i]), &pos);
            CHECK(j == NULL);
            CHECK(pos != 999);
        }
    }
    /* deep nesting is refused rather than overflowing */
    {
        char deep[200];

        memset(deep, '[', 100);
        memset(deep + 100, ']', 100);
        CHECK(json_parse(deep, 200, NULL) == NULL);
    }

    /* a server answer shape */
    {
        const char *ans = "{\"source\":\"00222cfc\",\"view\":\"P\",\"media\":\"stereo\","
            "\"frames\":539,\"pts_first\":721465083,\"pts_last\":723077083,"
            "\"duration_s\":17.911,\"from\":0,\"count\":2,\"width\":7680,\"height\":2160,"
            "\"items\":[{\"i\":0,\"l\":0,\"r\":1,\"pts_l\":721465083,\"pts_r\":721465083,"
            "\"key_offset\":0},{\"i\":1,\"l\":1,\"r\":2,\"pts_l\":721468083,"
            "\"pts_r\":721468083,\"key_offset\":1}]}";

        j = parse(ans);
        CHECK(j != NULL);
        CHECK(json_int(json_get(j, "frames"), 0) == 539);
        CHECK(json_int(json_get(j, "pts_last"), 0) == 723077083);
        CHECK(json_num(json_get(j, "duration_s"), 0) > 17.9);
        CHECK(json_len(json_get(j, "items")) == 2);
        CHECK(json_int(json_get(json_at(json_get(j, "items"), 1), "pts_r"), 0) == 721468083);
        json_free(j);
    }
    /* a large array parses and frees cleanly */
    {
        size_t n = 50000, cap = n * 40 + 16, o = 0;
        char *big = malloc(cap);

        big[o++] = '[';
        for (size_t i = 0; i < n; i++)
            o += (size_t)snprintf(big + o, cap - o, "%s{\"i\":%zu,\"p\":%zu}", i ? "," : "", i, i * 3003);
        big[o++] = ']';
        j = json_parse(big, o, NULL);
        CHECK(j != NULL && json_len(j) == n);
        CHECK(json_int(json_get(json_at(j, n - 1), "p"), 0) == (int64_t)((n - 1) * 3003));
        json_free(j);
        free(big);
    }
    printf("test_json: PASS (%d checks)\n", checks_run);
    return 0;
}
