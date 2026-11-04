#include "srcinfo.h"
#include "alloc.h"

#include <stdlib.h>
#include <string.h>

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r'))
        s[--n] = '\0';
    return s;
}

int srcinfo_parse(const char *text, SrcInfo *out)
{
    memset(out, 0, sizeof *out);
    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, text);

    char *section = NULL;
    int rc = 0;
    for (size_t i = 0; i < lines.n; i++) {
        char *line = trim(lines.v[i]);
        if (*line == '\0' || *line == '#')
            continue;
        char *eq = strstr(line, " = ");
        if (eq == NULL) {
            /* "key =" with empty value is legal */
            size_t L = strlen(line);
            if (L >= 2 && strcmp(line + L - 2, " =") == 0) {
                eq = line + L - 2;
            } else {
                rc = -1;
                break;
            }
        }
        *eq = '\0';
        char *key = trim(line);
        char *val = (eq[1] == '=' && eq[2] == ' ') ? trim(eq + 3) : (char *)"";
        if (*key == '\0') {
            rc = -1;
            break;
        }
        if (strcmp(key, "pkgbase") == 0) {
            free(section);
            section = xstrdup("pkgbase");
        } else if (strcmp(key, "pkgname") == 0) {
            free(section);
            section = xstrdup(val);
        } else if (section == NULL) {
            rc = -1;   /* data before pkgbase */
            break;
        }
        if (out->n == out->cap) {
            out->cap = out->cap ? out->cap * 2 : 64;
            out->e = xreallocarray(out->e, out->cap, sizeof(SIEntry));
        }
        out->e[out->n].key = xstrdup(key);
        out->e[out->n].val = xstrdup(val);
        out->e[out->n].section = xstrdup(section);
        out->n++;
    }
    free(section);
    sl_free(&lines);
    if (rc == 0 && srcinfo_first(out, "pkgbase") == NULL)
        rc = -1;
    if (rc != 0)
        srcinfo_free(out);
    return rc;
}

void srcinfo_free(SrcInfo *s)
{
    for (size_t i = 0; i < s->n; i++) {
        free(s->e[i].key);
        free(s->e[i].val);
        free(s->e[i].section);
    }
    free(s->e);
    memset(s, 0, sizeof *s);
}

const char *srcinfo_first(const SrcInfo *s, const char *key)
{
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->e[i].key, key) == 0)
            return s->e[i].val;
    return NULL;
}

void srcinfo_values(const SrcInfo *s, const char *key, const char *arch, StrList *out)
{
    size_t kl = strlen(key);
    for (size_t i = 0; i < s->n; i++) {
        const char *k = s->e[i].key;
        if (strncmp(k, key, kl) != 0)
            continue;
        if (k[kl] == '\0' ||
            (k[kl] == '_' && (arch == NULL || strcmp(k + kl + 1, arch) == 0)))
            if (s->e[i].val[0] != '\0')
                sl_push(out, s->e[i].val);
    }
}

void srcinfo_values_exact(const SrcInfo *s, const char *key, StrList *out)
{
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->e[i].key, key) == 0)
            sl_push(out, s->e[i].val);
}

void srcinfo_pkgnames(const SrcInfo *s, StrList *out)
{
    srcinfo_values_exact(s, "pkgname", out);
}
