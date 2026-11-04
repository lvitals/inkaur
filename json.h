#ifndef JSON_H
#define JSON_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Strict RFC 8259 JSON parser for untrusted input.
 *  - nesting depth, string length and container sizes are bounded
 *  - duplicate object keys are rejected
 *  - \u0000 and lone surrogates are rejected (C strings stay intact)
 *  - trailing garbage is rejected
 * Accessors are NULL-safe and type-checked, so a malformed server response
 * can never cause a NULL dereference.
 */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
    JType t;
    union {
        bool b;
        double n;
        char *s;
        struct { JVal **items; size_t n; } a;
        struct { char **keys; JVal **vals; size_t n; } o;
    } u;
};

#define JSON_MAX_DEPTH  64
#define JSON_MAX_ITEMS  200000
#define JSON_MAX_STRING (1u << 20)

JVal *json_parse(const char *s, size_t len, char *err, size_t errsz);
void json_free(JVal *v);

const JVal *json_get(const JVal *obj, const char *key);  /* NULL if absent */
const JVal *json_at(const JVal *arr, size_t i);
size_t json_len(const JVal *v);                          /* 0 if not array */
const char *json_str(const JVal *v);                     /* NULL if not string */
const char *json_get_str(const JVal *obj, const char *key);
bool json_get_num(const JVal *obj, const char *key, double *out);

#endif /* JSON_H */
