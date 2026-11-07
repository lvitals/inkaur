#ifndef SEARCH_H
#define SEARCH_H

#include "buf.h"

#include <stdbool.h>
#include <stddef.h>

size_t levenshtein(const char *a, const char *b);
int cmd_search(const StrList *terms, bool quiet);
int cmd_info(const StrList *names);

#endif /* SEARCH_H */
