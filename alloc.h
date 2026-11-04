#ifndef ALLOC_H
#define ALLOC_H

#include <stddef.h>

/*
 * Allocation wrappers: abort the process on OOM and on size overflow so
 * callers never operate on a short buffer.
 */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
void *xreallocarray(void *p, size_t n, size_t size);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#endif /* ALLOC_H */
