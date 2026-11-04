#include "alloc.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die_oom(void)
{
    fputs("error: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (p == NULL)
        die_oom();
    return p;
}

void *xcalloc(size_t n, size_t size)
{
    void *p = calloc(n ? n : 1, size ? size : 1);
    if (p == NULL)
        die_oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (q == NULL)
        die_oom();
    return q;
}

void *xreallocarray(void *p, size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size)
        die_oom();
    return xrealloc(p, n * size);
}

char *xstrndup(const char *s, size_t n)
{
    size_t len = 0;
    while (len < n && s[len] != '\0')
        len++;
    char *d = xmalloc(len + 1);
    memcpy(d, s, len);
    d[len] = '\0';
    return d;
}

char *xstrdup(const char *s)
{
    if (s == NULL)
        return NULL;
    return xstrndup(s, strlen(s));
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0)
        die_oom();

    char *buf = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}
