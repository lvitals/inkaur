#include "json.h"
#include "alloc.h"
#include "buf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t i, n;
    int depth;
    size_t items;
    char *err;
    size_t errsz;
} Parser;

static JVal *parse_value(Parser *p);

static void fail(Parser *p, const char *msg)
{
    if (p->err && p->err[0] == '\0')
        snprintf(p->err, p->errsz, "%s at offset %zu", msg, p->i);
}

static void skip_ws(Parser *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->i++;
        else
            break;
    }
}

static JVal *new_val(JType t)
{
    JVal *v = xcalloc(1, sizeof *v);
    v->t = t;
    return v;
}

void json_free(JVal *v)
{
    if (v == NULL)
        return;
    switch (v->t) {
    case J_STR:
        free(v->u.s);
        break;
    case J_ARR:
        for (size_t i = 0; i < v->u.a.n; i++)
            json_free(v->u.a.items[i]);
        free(v->u.a.items);
        break;
    case J_OBJ:
        for (size_t i = 0; i < v->u.o.n; i++) {
            free(v->u.o.keys[i]);
            json_free(v->u.o.vals[i]);
        }
        free(v->u.o.keys);
        free(v->u.o.vals);
        break;
    default:
        break;
    }
    free(v);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(Parser *p, uint32_t *out)
{
    if (p->n - p->i < 4)
        return false;
    uint32_t v = 0;
    for (int k = 0; k < 4; k++) {
        int h = hexval(p->s[p->i + (size_t)k]);
        if (h < 0)
            return false;
        v = (v << 4) | (uint32_t)h;
    }
    p->i += 4;
    *out = v;
    return true;
}

static void put_utf8(StrBuf *b, uint32_t cp)
{
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    sb_append(b, tmp, n);
}

/* Parses a string starting at the opening quote; returns malloc'd text. */
static char *parse_string_raw(Parser *p)
{
    if (p->i >= p->n || p->s[p->i] != '"') {
        fail(p, "expected string");
        return NULL;
    }
    p->i++;
    StrBuf b;
    sb_init(&b, JSON_MAX_STRING);
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i];
        if (c == '"') {
            p->i++;
            return b.data;
        }
        if (c < 0x20) {
            fail(p, "control character in string");
            goto bad;
        }
        if (c != '\\') {
            if (sb_putc(&b, (char)c) != 0) {
                fail(p, "string too long");
                goto bad;
            }
            p->i++;
            continue;
        }
        p->i++;
        if (p->i >= p->n)
            break;
        char e = p->s[p->i++];
        int r = 0;
        switch (e) {
        case '"':  r = sb_putc(&b, '"'); break;
        case '\\': r = sb_putc(&b, '\\'); break;
        case '/':  r = sb_putc(&b, '/'); break;
        case 'b':  r = sb_putc(&b, '\b'); break;
        case 'f':  r = sb_putc(&b, '\f'); break;
        case 'n':  r = sb_putc(&b, '\n'); break;
        case 'r':  r = sb_putc(&b, '\r'); break;
        case 't':  r = sb_putc(&b, '\t'); break;
        case 'u': {
            uint32_t cp;
            if (!read_hex4(p, &cp)) {
                fail(p, "bad \\u escape");
                goto bad;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                uint32_t lo;
                if (p->n - p->i < 2 || p->s[p->i] != '\\' || p->s[p->i + 1] != 'u') {
                    fail(p, "lone high surrogate");
                    goto bad;
                }
                p->i += 2;
                if (!read_hex4(p, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
                    fail(p, "bad low surrogate");
                    goto bad;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                fail(p, "lone low surrogate");
                goto bad;
            }
            if (cp == 0) {
                fail(p, "NUL in string");
                goto bad;
            }
            size_t before = b.len;
            put_utf8(&b, cp);
            if (b.len == before)
                r = -1;
            break;
        }
        default:
            fail(p, "invalid escape");
            goto bad;
        }
        if (r != 0) {
            fail(p, "string too long");
            goto bad;
        }
    }
    fail(p, "unterminated string");
bad:
    sb_free(&b);
    return NULL;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static JVal *parse_number(Parser *p)
{
    size_t start = p->i;
    if (p->i < p->n && p->s[p->i] == '-')
        p->i++;
    if (p->i >= p->n) {
        fail(p, "bad number");
        return NULL;
    }
    if (p->s[p->i] == '0') {
        p->i++;
    } else if (is_digit(p->s[p->i])) {
        while (p->i < p->n && is_digit(p->s[p->i]))
            p->i++;
    } else {
        fail(p, "bad number");
        return NULL;
    }
    if (p->i < p->n && p->s[p->i] == '.') {
        p->i++;
        if (p->i >= p->n || !is_digit(p->s[p->i])) {
            fail(p, "bad fraction");
            return NULL;
        }
        while (p->i < p->n && is_digit(p->s[p->i]))
            p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-'))
            p->i++;
        if (p->i >= p->n || !is_digit(p->s[p->i])) {
            fail(p, "bad exponent");
            return NULL;
        }
        while (p->i < p->n && is_digit(p->s[p->i]))
            p->i++;
    }
    size_t len = p->i - start;
    if (len > 64) {
        fail(p, "number too long");
        return NULL;
    }
    char tmp[65];
    memcpy(tmp, p->s + start, len);
    tmp[len] = '\0';
    JVal *v = new_val(J_NUM);
    v->u.n = strtod(tmp, NULL);
    return v;
}

static bool match_lit(Parser *p, const char *lit)
{
    size_t l = strlen(lit);
    if (p->n - p->i < l || memcmp(p->s + p->i, lit, l) != 0)
        return false;
    p->i += l;
    return true;
}

static bool count_item(Parser *p)
{
    if (++p->items > JSON_MAX_ITEMS) {
        fail(p, "too many items");
        return false;
    }
    return true;
}

static JVal *parse_array(Parser *p)
{
    p->i++; /* [ */
    JVal *v = new_val(J_ARR);
    size_t cap = 0;
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') {
        p->i++;
        return v;
    }
    for (;;) {
        if (!count_item(p))
            goto bad;
        JVal *item = parse_value(p);
        if (item == NULL)
            goto bad;
        if (v->u.a.n == cap) {
            cap = cap ? cap * 2 : 8;
            v->u.a.items = xreallocarray(v->u.a.items, cap, sizeof(JVal *));
        }
        v->u.a.items[v->u.a.n++] = item;
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == ']') {
            p->i++;
            return v;
        }
        fail(p, "expected , or ]");
        goto bad;
    }
bad:
    json_free(v);
    return NULL;
}

static JVal *parse_object(Parser *p)
{
    p->i++; /* { */
    JVal *v = new_val(J_OBJ);
    size_t cap = 0;
    skip_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') {
        p->i++;
        return v;
    }
    for (;;) {
        if (!count_item(p))
            goto bad;
        skip_ws(p);
        char *key = parse_string_raw(p);
        if (key == NULL)
            goto bad;
        for (size_t k = 0; k < v->u.o.n; k++) {
            if (strcmp(v->u.o.keys[k], key) == 0) {
                fail(p, "duplicate key");
                free(key);
                goto bad;
            }
        }
        skip_ws(p);
        if (p->i >= p->n || p->s[p->i] != ':') {
            fail(p, "expected :");
            free(key);
            goto bad;
        }
        p->i++;
        JVal *val = parse_value(p);
        if (val == NULL) {
            free(key);
            goto bad;
        }
        if (v->u.o.n == cap) {
            cap = cap ? cap * 2 : 8;
            v->u.o.keys = xreallocarray(v->u.o.keys, cap, sizeof(char *));
            v->u.o.vals = xreallocarray(v->u.o.vals, cap, sizeof(JVal *));
        }
        v->u.o.keys[v->u.o.n] = key;
        v->u.o.vals[v->u.o.n] = val;
        v->u.o.n++;
        skip_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') {
            p->i++;
            continue;
        }
        if (p->i < p->n && p->s[p->i] == '}') {
            p->i++;
            return v;
        }
        fail(p, "expected , or }");
        goto bad;
    }
bad:
    json_free(v);
    return NULL;
}

static JVal *parse_value(Parser *p)
{
    skip_ws(p);
    if (p->i >= p->n) {
        fail(p, "unexpected end");
        return NULL;
    }
    if (++p->depth > JSON_MAX_DEPTH) {
        fail(p, "nesting too deep");
        return NULL;
    }
    JVal *v = NULL;
    char c = p->s[p->i];
    if (c == '{') {
        v = parse_object(p);
    } else if (c == '[') {
        v = parse_array(p);
    } else if (c == '"') {
        char *s = parse_string_raw(p);
        if (s) {
            v = new_val(J_STR);
            v->u.s = s;
        }
    } else if (c == '-' || is_digit(c)) {
        v = parse_number(p);
    } else if (match_lit(p, "true")) {
        v = new_val(J_BOOL);
        v->u.b = true;
    } else if (match_lit(p, "false")) {
        v = new_val(J_BOOL);
        v->u.b = false;
    } else if (match_lit(p, "null")) {
        v = new_val(J_NULL);
    } else {
        fail(p, "unexpected character");
    }
    p->depth--;
    return v;
}

JVal *json_parse(const char *s, size_t len, char *err, size_t errsz)
{
    Parser p;
    memset(&p, 0, sizeof p);
    p.s = s;
    p.n = len;
    p.err = err;
    p.errsz = errsz;
    if (err && errsz)
        err[0] = '\0';
    if (s == NULL) {
        fail(&p, "no input");
        return NULL;
    }
    if (memchr(s, '\0', len) != NULL) {
        fail(&p, "NUL byte in input");
        return NULL;
    }
    JVal *v = parse_value(&p);
    if (v == NULL)
        return NULL;
    skip_ws(&p);
    if (p.i != p.n) {
        fail(&p, "trailing data");
        json_free(v);
        return NULL;
    }
    return v;
}

const JVal *json_get(const JVal *obj, const char *key)
{
    if (obj == NULL || obj->t != J_OBJ)
        return NULL;
    for (size_t i = 0; i < obj->u.o.n; i++)
        if (strcmp(obj->u.o.keys[i], key) == 0)
            return obj->u.o.vals[i];
    return NULL;
}

const JVal *json_at(const JVal *arr, size_t i)
{
    if (arr == NULL || arr->t != J_ARR || i >= arr->u.a.n)
        return NULL;
    return arr->u.a.items[i];
}

size_t json_len(const JVal *v)
{
    return (v && v->t == J_ARR) ? v->u.a.n : 0;
}

const char *json_str(const JVal *v)
{
    return (v && v->t == J_STR) ? v->u.s : NULL;
}

const char *json_get_str(const JVal *obj, const char *key)
{
    return json_str(json_get(obj, key));
}

bool json_get_num(const JVal *obj, const char *key, double *out)
{
    const JVal *v = json_get(obj, key);
    if (v == NULL || v->t != J_NUM)
        return false;
    *out = v->u.n;
    return true;
}
