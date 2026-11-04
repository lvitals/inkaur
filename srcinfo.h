#ifndef SRCINFO_H
#define SRCINFO_H

#include "buf.h"

#include <stddef.h>

/* Parsed .SRCINFO: flat list of key/value pairs in file order. */
typedef struct {
    char *key;
    char *val;
    char *section;   /* "pkgbase" or the pkgname the line belongs to */
} SIEntry;

typedef struct {
    SIEntry *e;
    size_t n, cap;
} SrcInfo;

/* Returns 0, or -1 for a structurally invalid file. */
int srcinfo_parse(const char *text, SrcInfo *out);
void srcinfo_free(SrcInfo *s);
const char *srcinfo_first(const SrcInfo *s, const char *key);
/* Values for key and key_<arch> (arch may be NULL for any arch). */
void srcinfo_values(const SrcInfo *s, const char *key, const char *arch, StrList *out);
/* Values for exactly this key (no arch suffix handling). */
void srcinfo_values_exact(const SrcInfo *s, const char *key, StrList *out);
/* All pkgname values. */
void srcinfo_pkgnames(const SrcInfo *s, StrList *out);

#endif /* SRCINFO_H */
