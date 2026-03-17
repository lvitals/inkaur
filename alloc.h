#ifndef ALLOC_H
#define ALLOC_H

#include <stdlib.h>

void *safe_malloc(size_t s);
void *safe_calloc(int n, size_t s);
void *safe_realloc(void *ptr, size_t s);
char *safe_strdup(const char *str);

#endif  /* ALLOC_H */
