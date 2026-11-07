#ifndef BUF_H
#define BUF_H

#include <stdbool.h>
#include <stddef.h>

/* Growable, always NUL-terminated byte buffer with a hard size cap. */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t max;   /* maximum len allowed; appends beyond it fail */
} StrBuf;

void sb_init(StrBuf *sb, size_t max);
int sb_append(StrBuf *sb, const char *data, size_t n);   /* -1 on overflow */
int sb_puts(StrBuf *sb, const char *s);
int sb_putc(StrBuf *sb, char c);
int sb_printf(StrBuf *sb, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void sb_reset(StrBuf *sb);
void sb_free(StrBuf *sb);

/* Vector of owned strings. */
typedef struct {
    char **v;
    size_t n;
    size_t cap;
} StrList;

void sl_init(StrList *l);
void sl_push(StrList *l, const char *s);            /* copies s */
void sl_push_owned(StrList *l, char *s);            /* takes ownership */
bool sl_contains(const StrList *l, const char *s);
bool sl_push_unique(StrList *l, const char *s);     /* true if added */
void sl_extend(StrList *l, const StrList *from);  /* copies every string */
void sl_sort(StrList *l);
bool sl_bsearch(const StrList *l, const char *s);   /* list must be sorted */
void sl_free(StrList *l);

/* Split text into lines (without '\n'); empty lines are skipped. */
void sl_split_lines(StrList *out, const char *text);

#endif /* BUF_H */
