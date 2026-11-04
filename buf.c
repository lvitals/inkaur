#include "buf.h"
#include "alloc.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sb_init(StrBuf *sb, size_t max)
{
    sb->cap = 64;
    sb->len = 0;
    sb->max = max ? max : (SIZE_MAX / 4);
    sb->data = xmalloc(sb->cap);
    sb->data[0] = '\0';
}

int sb_append(StrBuf *sb, const char *data, size_t n)
{
    if (n > sb->max || sb->len > sb->max - n)
        return -1;
    if (sb->len + n + 1 > sb->cap) {
        size_t ncap = sb->cap;
        while (ncap < sb->len + n + 1) {
            if (ncap > SIZE_MAX / 2)
                return -1;
            ncap *= 2;
        }
        sb->data = xrealloc(sb->data, ncap);
        sb->cap = ncap;
    }
    if (n)
        memcpy(sb->data + sb->len, data, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_puts(StrBuf *sb, const char *s)
{
    return sb_append(sb, s, strlen(s));
}

int sb_putc(StrBuf *sb, char c)
{
    return sb_append(sb, &c, 1);
}

int sb_printf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    char *tmp = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(tmp, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int r = sb_append(sb, tmp, (size_t)n);
    free(tmp);
    return r;
}

void sb_reset(StrBuf *sb)
{
    sb->len = 0;
    sb->data[0] = '\0';
}

void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

void sl_init(StrList *l)
{
    l->v = NULL;
    l->n = 0;
    l->cap = 0;
}

void sl_push_owned(StrList *l, char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = xreallocarray(l->v, l->cap, sizeof(char *));
    }
    l->v[l->n++] = s;
}

void sl_push(StrList *l, const char *s)
{
    sl_push_owned(l, xstrdup(s));
}

bool sl_contains(const StrList *l, const char *s)
{
    for (size_t i = 0; i < l->n; i++)
        if (strcmp(l->v[i], s) == 0)
            return true;
    return false;
}

bool sl_push_unique(StrList *l, const char *s)
{
    if (sl_contains(l, s))
        return false;
    sl_push(l, s);
    return true;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void sl_sort(StrList *l)
{
    if (l->n > 1)
        qsort(l->v, l->n, sizeof(char *), cmp_str);
}

bool sl_bsearch(const StrList *l, const char *s)
{
    if (l->n == 0)
        return false;
    return bsearch(&s, l->v, l->n, sizeof(char *), cmp_str) != NULL;
}

void sl_free(StrList *l)
{
    for (size_t i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    sl_init(l);
}

void sl_split_lines(StrList *out, const char *text)
{
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n > 0 && p[n - 1] == '\r')
            n--;
        if (n > 0)
            sl_push_owned(out, xstrndup(p, n));
        if (!e)
            break;
        p = e + 1;
    }
}
