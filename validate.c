#include "validate.h"
#include "config.h"

#include <string.h>

static bool is_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static bool is_hex(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool name_char(unsigned char c)
{
    return is_alnum(c) || c == '@' || c == '.' || c == '_' || c == '+' ||
           c == '-';
}

static bool name_span_ok(const char *s, size_t n)
{
    if (n == 0 || n > MAX_NAME_LEN)
        return false;
    if (s[0] == '-' || s[0] == '.')
        return false;
    for (size_t i = 0; i < n; i++)
        if (!name_char((unsigned char)s[i]))
            return false;
    return true;
}

bool valid_pkgname(const char *s)
{
    return s != NULL && name_span_ok(s, strlen(s));
}

bool valid_version(const char *s)
{
    if (s == NULL)
        return false;
    size_t n = strlen(s);
    if (n == 0 || n > MAX_VERSION_LEN || s[0] == '-')
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(is_alnum(c) || c == '.' || c == '_' || c == '+' || c == ':' ||
              c == '~' || c == '-'))
            return false;
    }
    return true;
}

bool valid_depstring(const char *s, char *name_out, size_t name_sz)
{
    if (s == NULL)
        return false;
    size_t n = strcspn(s, "<>=");
    if (!name_span_ok(s, n) || n + 1 > name_sz)
        return false;

    const char *op = s + n;
    if (*op != '\0') {
        const char *ver = op;
        if (strncmp(op, ">=", 2) == 0 || strncmp(op, "<=", 2) == 0)
            ver = op + 2;
        else if (*op == '<' || *op == '>' || *op == '=')
            ver = op + 1;
        if (!valid_version(ver))
            return false;
    }
    memcpy(name_out, s, n);
    name_out[n] = '\0';
    return true;
}

bool valid_fingerprint(const char *s)
{
    if (s == NULL)
        return false;
    size_t n = strlen(s);
    if (n != 40 && n != 64)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!is_hex((unsigned char)s[i]))
            return false;
    return true;
}

bool valid_commit(const char *s)
{
    if (s == NULL)
        return false;
    size_t n = strlen(s);
    if (n != 40 && n != 64)
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

bool valid_search_term(const char *s)
{
    if (s == NULL)
        return false;
    size_t n = strlen(s);
    if (n < 2 || n > 128 || s[0] == '-')
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

size_t utf8_decode(const unsigned char *s, size_t n, uint32_t *cp)
{
    if (n == 0)
        return 0;
    unsigned char c = s[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    size_t len;
    uint32_t v, min;
    if ((c & 0xE0) == 0xC0) {
        len = 2; v = c & 0x1F; min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3; v = c & 0x0F; min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4; v = c & 0x07; min = 0x10000;
    } else {
        return 0;
    }
    if (n < len)
        return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return 0;
        v = (v << 6) | (uint32_t)(s[i] & 0x3F);
    }
    if (v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF))
        return 0;   /* overlong, out of range or surrogate */
    *cp = v;
    return len;
}

bool is_deceptive_codepoint(uint32_t cp)
{
    return (cp >= 0x200B && cp <= 0x200F) ||   /* zero-width, LRM, RLM */
           (cp >= 0x202A && cp <= 0x202E) ||   /* bidi embeddings/overrides */
           (cp >= 0x2060 && cp <= 0x2064) ||   /* word joiner, invisibles */
           (cp >= 0x2066 && cp <= 0x2069) ||   /* bidi isolates */
           cp == 0xFEFF || cp == 0x180E || cp == 0x061C || cp == 0x00AD;
}

void sanitize_append(StrBuf *out, const char *str, size_t n, bool keep_newlines)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c == '\n' || c == '\t') {
            if (keep_newlines)
                sb_putc(out, (char)c);
            else
                sb_putc(out, ' ');
            i++;
        } else if (c < 0x20 || c == 0x7f) {
            sb_putc(out, '^');
            sb_putc(out, c == 0x7f ? '?' : (char)(c + 0x40));
            i++;
        } else if (c < 0x80) {
            sb_putc(out, (char)c);
            i++;
        } else {
            uint32_t cp;
            size_t len = utf8_decode(s + i, n - i, &cp);
            if (len == 0) {
                sb_printf(out, "\\x%02X", c);
                i++;
            } else if ((cp >= 0x80 && cp <= 0x9F) || is_deceptive_codepoint(cp)) {
                sb_printf(out, "<U+%04X>", (unsigned)cp);
                i += len;
            } else {
                sb_append(out, (const char *)s + i, len);
                i += len;
            }
        }
    }
}

void safe_fputs(FILE *fp, const char *s)
{
    if (s == NULL)
        return;
    StrBuf b;
    sb_init(&b, 0);
    sanitize_append(&b, s, strlen(s), false);
    fputs(b.data, fp);
    sb_free(&b);
}
