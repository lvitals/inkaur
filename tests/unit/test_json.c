#include "../test_helper.h"
#include "json.h"
#include "rpc.h"

#include <stdint.h>

void run_json_tests(void);

static JVal *P(const char *s)
{
    char err[128];
    return json_parse(s, strlen(s), err, sizeof err);
}

static void reject(const char *s, const char *msg)
{
    JVal *v = P(s);
    ASSERT(v == NULL, msg);
}

void run_json_tests(void)
{
    JVal *v = P("{\"type\":\"multiinfo\",\"resultcount\":1,\"results\":[{\"Name\":\"aide\","
                "\"PackageBase\":\"aide\",\"Version\":\"0.18-1\",\"Depends\":[\"zlib\"],"
                "\"Maintainer\":null,\"OutOfDate\":null,\"Desc\":\"a\\u00e9\\ud83d\\ude00\"}]}");
    ASSERT(v != NULL, "valid RPC document");
    const JVal *r0 = json_at(json_get(v, "results"), 0);
    ASSERT_STR_EQ(json_get_str(r0, "Name"), "aide", "nested access");
    ASSERT_STR_EQ(json_get_str(r0, "Desc"), "a\xc3\xa9\xf0\x9f\x98\x80", "unicode + surrogate pair");
    ASSERT(json_get_str(r0, "Missing") == NULL, "missing key is NULL");
    ASSERT(json_get_str(r0, "Depends") == NULL, "type-checked accessor");
    ASSERT(json_len(json_get(r0, "Depends")) == 1, "array len");
    ASSERT(json_at(json_get(r0, "Depends"), 5) == NULL, "out of range index");
    ASSERT(json_get(NULL, "x") == NULL && json_at(NULL, 0) == NULL, "NULL-safe");
    json_free(v);

    reject("", "empty");
    reject("{", "unterminated object");
    reject("{\"a\":1,}", "trailing comma");
    reject("[1,2", "unterminated array");
    reject("\"abc", "unterminated string");
    reject("\"a\\u0000b\"", "NUL escape");
    reject("\"\\ud800\"", "lone surrogate");
    reject("\"\\x41\"", "invalid escape");
    reject("\"a\tb\"", "raw control char");
    reject("{\"a\":1,\"a\":2}", "duplicate key");
    reject("01", "leading zero");
    reject("1.", "bad fraction");
    reject("tru", "bad literal");
    reject("{} {}", "trailing data");
    reject("{\"a\" 1}", "missing colon");

    /* nesting bomb */
    char deep[JSON_MAX_DEPTH * 2 + 10];
    size_t k = 0;
    for (int i = 0; i < JSON_MAX_DEPTH + 2; i++)
        deep[k++] = '[';
    deep[k] = '\0';
    reject(deep, "depth limit");

    /* embedded NUL in input */
    {
        char err[64];
        const char nul[] = "{\"a\":\"b\0c\"}";
        ASSERT(json_parse(nul, sizeof nul - 1, err, sizeof err) == NULL, "NUL byte in input");
    }

    /* deterministic fuzz: must never crash (ASan/UBSan would abort) */
    uint32_t seed = 12345;
    const char alphabet[] = "{}[],:\"\\u0123456789abcdefnulltruefalse-+.eE \t\n";
    char buf[257];
    for (int iter = 0; iter < 20000; iter++) {
        size_t len = (size_t)(iter % 256);
        for (size_t i = 0; i < len; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = alphabet[(seed >> 16) % (sizeof alphabet - 1)];
        }
        buf[len] = '\0';
        char err[64];
        json_free(json_parse(buf, len, err, sizeof err));
    }
    ASSERT(1, "fuzz loop survived");
    printf("json: ok\n");
}
