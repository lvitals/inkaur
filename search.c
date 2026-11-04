#include "search.h"
#include "alloc.h"
#include "config.h"
#include "pacman.h"
#include "rpc.h"
#include "tty.h"
#include "validate.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Bounded, heap-based edit distance (no VLA, no stack blowup). */
size_t levenshtein(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    if (la > 256)
        la = 256;
    if (lb > 256)
        lb = 256;
    size_t *col = xcalloc(la + 1, sizeof(size_t));
    for (size_t y = 0; y <= la; y++)
        col[y] = y;
    for (size_t x = 1; x <= lb; x++) {
        col[0] = x;
        size_t last = x - 1;
        for (size_t y = 1; y <= la; y++) {
            size_t old = col[y];
            size_t cost = (a[y - 1] == b[x - 1]) ? 0 : 1;
            size_t v = col[y] + 1;
            if (col[y - 1] + 1 < v)
                v = col[y - 1] + 1;
            if (last + cost < v)
                v = last + cost;
            col[y] = v;
            last = old;
        }
    }
    size_t r = col[la];
    free(col);
    return r;
}

static bool contains_ci(const char *hay, const char *needle)
{
    if (hay == NULL)
        return false;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == n)
            return true;
    }
    return false;
}

typedef struct {
    const AurPkg *p;
    size_t dist;
} Hit;

static int cmp_hit(const void *a, const void *b)
{
    const Hit *x = a, *y = b;
    if (x->dist != y->dist)
        return x->dist > y->dist ? -1 : 1;   /* best match printed last */
    return strcmp(y->p->name, x->p->name);
}

int cmd_search(const StrList *terms)
{
    if (terms->n == 0) {
        fprintf(stderr, "error: no targets specified\n");
        return 1;
    }
    for (size_t i = 0; i < terms->n; i++)
        if (!valid_search_term(terms->v[i])) {
            fprintf(stderr, "error: invalid search term (2-128 printable chars)\n");
            return 1;
        }

    RpcResult res;
    if (rpc_search(terms->v[0], false, &res) != 0)
        return 1;

    StrList installed;
    sl_init(&installed);
    pacman_installed_names(&installed);

    Hit *hits = xcalloc(res.n ? res.n : 1, sizeof(Hit));
    size_t nh = 0;
    for (size_t i = 0; i < res.n; i++) {
        bool ok = true;
        for (size_t t = 1; t < terms->n && ok; t++)
            ok = contains_ci(res.pkgs[i].name, terms->v[t]) ||
                 contains_ci(res.pkgs[i].desc, terms->v[t]);
        if (ok) {
            hits[nh].p = &res.pkgs[i];
            hits[nh].dist = levenshtein(res.pkgs[i].name, terms->v[0]);
            nh++;
        }
    }
    qsort(hits, nh, sizeof(Hit), cmp_hit);

    for (size_t i = 0; i < nh; i++) {
        const AurPkg *p = hits[i].p;
        printf("%saur/%s", C_BLUE, C_END);
        printf("%s%s%s %s%s%s", C_BOLD, p->name, C_END, C_GREEN, p->version, C_END);
        if (p->outofdate)
            printf(" %s(OUT OF DATE)%s", C_RED, C_END);
        if (p->orphan)
            printf(" %s(ORPHAN)%s", C_YEL, C_END);
        if (sl_bsearch(&installed, p->name))
            printf(" %s[installed]%s", C_BLUE, C_END);
        printf("\n    ");
        safe_fputs(stdout, p->desc ? p->desc : "");
        printf("\n");
    }
    free(hits);
    sl_free(&installed);
    rpc_result_free(&res);
    return nh ? 0 : 1;
}

static void print_field(const JVal *pkg, const char *label, const char *key)
{
    const JVal *v = json_get(pkg, key);
    printf("%-16s: ", label);
    if (v == NULL || v->t == J_NULL) {
        puts("None");
        return;
    }
    if (v->t == J_STR) {
        safe_fputs(stdout, v->u.s);
    } else if (v->t == J_NUM) {
        if (strstr(key, "Date") || strcmp(key, "FirstSubmitted") == 0 ||
            strcmp(key, "LastModified") == 0 || strcmp(key, "OutOfDate") == 0) {
            time_t t = (time_t)v->u.n;
            struct tm tmv;
            char buf[32];
            if (localtime_r(&t, &tmv) && strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv))
                fputs(buf, stdout);
            else
                printf("%.0f", v->u.n);
        } else {
            printf("%g", v->u.n);
        }
    } else if (v->t == J_ARR) {
        if (v->u.a.n == 0)
            fputs("None", stdout);
        for (size_t i = 0; i < v->u.a.n; i++) {
            if (i)
                fputs("  ", stdout);
            safe_fputs(stdout, json_str(v->u.a.items[i]));
        }
    } else {
        fputs("?", stdout);
    }
    putchar('\n');
}

int cmd_info(const StrList *names)
{
    if (names->n == 0) {
        fprintf(stderr, "error: no targets specified\n");
        return 1;
    }
    for (size_t i = 0; i < names->n; i++)
        if (!valid_pkgname(names->v[i])) {
            fputs("error: invalid package name: ", stderr);
            safe_fputs(stderr, names->v[i]);
            fputc('\n', stderr);
            return 1;
        }

    int rc = 0;
    for (size_t off = 0; off < names->n; off += RPC_BATCH) {
        StrList batch;
        sl_init(&batch);
        for (size_t i = off; i < names->n && i < off + RPC_BATCH; i++)
            sl_push(&batch, names->v[i]);
        RpcResult res;
        if (rpc_info(&batch, &res) != 0) {
            sl_free(&batch);
            return 1;
        }
        for (size_t i = 0; i < batch.n; i++) {
            const AurPkg *p = rpc_find(&res, batch.v[i]);
            if (p == NULL) {
                fprintf(stderr, "error: package '%s' was not found\n", batch.v[i]);
                rc = 1;
                continue;
            }
            const JVal *j = p->raw;
            printf("Repository      : aur\n");
            print_field(j, "Name", "Name");
            print_field(j, "Package Base", "PackageBase");
            print_field(j, "Version", "Version");
            print_field(j, "Description", "Description");
            print_field(j, "URL", "URL");
            print_field(j, "Licenses", "License");
            print_field(j, "Provides", "Provides");
            print_field(j, "Depends On", "Depends");
            print_field(j, "Make Deps", "MakeDepends");
            print_field(j, "Conflicts With", "Conflicts");
            print_field(j, "Maintainer", "Maintainer");
            print_field(j, "Co-Maintainers", "CoMaintainers");
            print_field(j, "Votes", "NumVotes");
            print_field(j, "Popularity", "Popularity");
            print_field(j, "First Submitted", "FirstSubmitted");
            print_field(j, "Last Modified", "LastModified");
            print_field(j, "Out Of Date", "OutOfDate");
            printf("AUR URL         : https://aur.archlinux.org/packages/%s\n\n", p->name);
        }
        rpc_result_free(&res);
        sl_free(&batch);
    }
    return rc;
}
