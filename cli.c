#include "cli.h"
#include "alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_D = 1u << 0, OP_F = 1u << 1, OP_Q = 1u << 2, OP_R = 1u << 3,
    OP_S = 1u << 4, OP_T = 1u << 5, OP_U = 1u << 6, OP_A = 1u << 7,
    OP_M = 1u << 8, OP_I = 1u << 9,
    OP_PAC = OP_D | OP_F | OP_Q | OP_R | OP_S | OP_T | OP_U,
    OP_XACT = OP_R | OP_S | OP_U,
    OP_UPG = OP_S | OP_U
};

typedef enum { K_COMMON, K_XACT, K_OP, K_LOCAL } Kind;

typedef struct {
    const char *lng;
    char shrt;          /* 0 = long only */
    unsigned ops;
    bool arg;
    Kind kind;
    bool ctx;           /* also passed to inkaur's own pacman queries */
} OptDef;

/* pacman(8) option set, grouped as in the manual. */
static const OptDef OPTS[] = {
    /* options for every operation */
    { "dbpath", 'b', OP_PAC, true, K_COMMON, true },
    { "root", 'r', OP_PAC, true, K_COMMON, true },
    { "verbose", 'v', OP_PAC, false, K_COMMON, false },
    { "arch", 0, OP_PAC, true, K_COMMON, true },
    { "cachedir", 0, OP_PAC, true, K_COMMON, false },
    { "color", 0, OP_PAC | OP_A | OP_M | OP_I, true, K_COMMON, false },
    { "config", 0, OP_PAC, true, K_COMMON, true },
    { "debug", 0, OP_PAC, false, K_COMMON, false },
    { "gpgdir", 0, OP_PAC, true, K_COMMON, false },
    { "hookdir", 0, OP_PAC, true, K_COMMON, false },
    { "logfile", 0, OP_PAC, true, K_COMMON, false },
    { "noconfirm", 0, OP_PAC, false, K_COMMON, false },
    { "confirm", 0, OP_PAC, false, K_COMMON, false },
    { "disable-download-timeout", 0, OP_PAC, false, K_COMMON, false },
    { "disable-sandbox", 0, OP_PAC, false, K_COMMON, false },
    { "disable-sandbox-filesystem", 0, OP_PAC, false, K_COMMON, false },
    { "disable-sandbox-syscalls", 0, OP_PAC, false, K_COMMON, false },
    { "sysroot", 0, OP_PAC, true, K_COMMON, true },
    /* transaction options (-R -S -U) */
    { "nodeps", 'd', OP_XACT, false, K_XACT, false },
    { "assume-installed", 0, OP_XACT, true, K_XACT, false },
    { "dbonly", 0, OP_XACT, false, K_XACT, false },
    { "noprogressbar", 0, OP_XACT, false, K_XACT, false },
    { "noscriptlet", 0, OP_XACT, false, K_XACT, false },
    { "print", 'p', OP_XACT, false, K_OP, false },
    { "print-format", 0, OP_XACT, true, K_XACT, false },
    /* upgrade options (-S -U) */
    { "asdeps", 0, OP_UPG | OP_D, false, K_LOCAL, false },
    { "asexplicit", 0, OP_UPG | OP_D, false, K_LOCAL, false },
    { "ignore", 0, OP_UPG, true, K_XACT, false },
    { "ignoregroup", 0, OP_UPG, true, K_XACT, false },
    { "needed", 0, OP_UPG, false, K_LOCAL, false },
    { "overwrite", 0, OP_UPG, true, K_XACT, false },
    { "downloadonly", 'w', OP_UPG, false, K_OP, false },
    /* query (-Q) */
    { "changelog", 'c', OP_Q, false, K_OP, false },
    { "deps", 'd', OP_Q, false, K_OP, false },
    { "explicit", 'e', OP_Q, false, K_OP, false },
    { "groups", 'g', OP_Q | OP_S, false, K_OP, false },
    { "info", 'i', OP_Q | OP_S, false, K_OP, false },
    { "check", 'k', OP_Q | OP_D, false, K_OP, false },
    { "list", 'l', OP_Q | OP_S | OP_F, false, K_OP, false },
    { "foreign", 'm', OP_Q, false, K_OP, false },
    { "native", 'n', OP_Q, false, K_OP, false },
    { "owns", 'o', OP_Q, false, K_OP, false },
    { "file", 'p', OP_Q, false, K_OP, false },
    { "quiet", 'q', OP_Q | OP_S | OP_F | OP_D, false, K_OP, false },
    { "search", 's', OP_Q | OP_S, false, K_OP, false },
    { "unrequired", 't', OP_Q, false, K_OP, false },
    { "upgrades", 'u', OP_Q, false, K_OP, false },
    /* remove (-R) */
    { "cascade", 'c', OP_R, false, K_OP, false },
    { "nosave", 'n', OP_R, false, K_OP, false },
    { "recursive", 's', OP_R, false, K_OP, false },
    { "unneeded", 'u', OP_R, false, K_OP, false },
    /* sync (-S) */
    { "clean", 'c', OP_S, false, K_OP, false },
    { "sysupgrade", 'u', OP_S, false, K_OP, false },
    { "refresh", 'y', OP_S | OP_F, false, K_OP, false },
    /* files (-F) */
    { "regex", 'x', OP_F, false, K_OP, false },
    { "machinereadable", 0, OP_F, false, K_OP, false },
    /* inkaur additions */
    { "aur", 0, OP_S | OP_Q, false, K_LOCAL, false },
    { "repo", 0, OP_S | OP_Q, false, K_LOCAL, false },
    { "chroot", 0, OP_S, true, K_LOCAL, false },
    { "jobs", 0, OP_S, true, K_LOCAL, false },
    { "fetch-jobs", 0, OP_S, true, K_LOCAL, false },
    { "serial", 0, OP_S, false, K_LOCAL, false },
    { "parallel", 0, OP_S, false, K_LOCAL, false },
    { "repo-download", 0, OP_S, true, K_LOCAL, false },
    { "mirrors", 0, OP_S, true, K_LOCAL, false },
    { "review", 0, OP_S, true, K_LOCAL, false },
    { "all", 0, OP_M, false, K_LOCAL, false },
    { "count", 0, OP_M, true, K_LOCAL, false },
};
#define NOPTS (sizeof OPTS / sizeof OPTS[0])

static const struct { char op; const char *name; } OPS[] = {
    { 'D', "database" }, { 'F', "files" }, { 'Q', "query" }, { 'R', "remove" },
    { 'S', "sync" }, { 'T', "deptest" }, { 'U', "upgrade" }, { 'V', "version" },
    { 'A', "audit" }, { 'N', "speedtest" }, { 'M', "rankmirrors" }, { 'I', "integrity" },
};
#define NOPS (sizeof OPS / sizeof OPS[0])

const char *cli_op_name(char op)
{
    for (size_t i = 0; i < NOPS; i++)
        if (OPS[i].op == op)
            return OPS[i].name;
    return NULL;
}

static unsigned op_bit(char op)
{
    switch (op) {
    case 'D': return OP_D;
    case 'F': return OP_F;
    case 'Q': return OP_Q;
    case 'R': return OP_R;
    case 'S': return OP_S;
    case 'T': return OP_T;
    case 'U': return OP_U;
    case 'A': return OP_A;
    case 'M': return OP_M;
    case 'I': return OP_I;
    default:  return 0;
    }
}

void cli_init(Cli *c)
{
    memset(c, 0, sizeof *c);
    sl_init(&c->seen);
    sl_init(&c->common);
    sl_init(&c->xact);
    sl_init(&c->opflags);
    sl_init(&c->ctx);
    sl_init(&c->targets);
    sl_init(&c->ignore);
    c->color = -1;
    c->parallel = -1;
}

void cli_free(Cli *c)
{
    sl_free(&c->seen);
    sl_free(&c->common);
    sl_free(&c->xact);
    sl_free(&c->opflags);
    sl_free(&c->ctx);
    sl_free(&c->targets);
    sl_free(&c->ignore);
}

size_t cli_count(const Cli *c, const char *lng)
{
    size_t n = 0;
    for (size_t i = 0; i < c->seen.n; i++)
        n += strcmp(c->seen.v[i], lng) == 0;
    return n;
}

static const OptDef *find_short(char ch, unsigned opbit)
{
    for (size_t i = 0; i < NOPTS; i++)
        if (OPTS[i].shrt == ch && (OPTS[i].ops & opbit))
            return &OPTS[i];
    return NULL;
}

/* Exact long name, else a unique prefix (like getopt_long). */
static const OptDef *find_long(const char *name, size_t len, unsigned opbit, bool *ambiguous)
{
    const OptDef *hit = NULL;
    size_t nhit = 0;
    *ambiguous = false;
    for (size_t i = 0; i < NOPTS; i++) {
        if (!(OPTS[i].ops & opbit) || strncmp(OPTS[i].lng, name, len) != 0)
            continue;
        if (OPTS[i].lng[len] == '\0')
            return &OPTS[i];
        hit = &OPTS[i];
        nhit++;
    }
    if (nhit > 1) {
        *ambiguous = true;
        return NULL;
    }
    return hit;
}

static char find_long_op(const char *name, size_t len)
{
    char hit = 0;
    size_t nhit = 0;
    if (strncmp("help", name, len) == 0 && len > 0) {
        if (len == 4)
            return 'h';
        hit = 'h';
        nhit++;
    }
    for (size_t i = 0; i < NOPS; i++) {
        if (strncmp(OPS[i].name, name, len) != 0)
            continue;
        if (OPS[i].name[len] == '\0')
            return OPS[i].op;
        hit = OPS[i].op;
        nhit++;
    }
    return nhit == 1 ? hit : 0;
}

static void split_ignore(Cli *c, const char *v)
{
    char *dup = xstrdup(v);
    char *save = NULL;
    for (char *t = strtok_r(dup, ", ", &save); t; t = strtok_r(NULL, ", ", &save))
        sl_push(&c->ignore, t);
    free(dup);
}

static void apply(Cli *c, const OptDef *d, const char *val)
{
    sl_push(&c->seen, d->lng);
    char *canon;
    if (d->arg) {
        size_t n = strlen(d->lng) + strlen(val) + 4;
        canon = xmalloc(n);
        snprintf(canon, n, "--%s=%s", d->lng, val);
    } else {
        size_t n = strlen(d->lng) + 3;
        canon = xmalloc(n);
        snprintf(canon, n, "--%s", d->lng);
    }
    /* context options go only to ctx: pacman.c prepends them to every call */
    switch (d->ctx ? K_LOCAL : d->kind) {
    case K_COMMON: sl_push(&c->common, canon); break;
    case K_XACT:   sl_push(&c->xact, canon); break;
    case K_OP:     sl_push(&c->opflags, canon); break;
    case K_LOCAL:  break;
    }
    if (d->ctx)
        sl_push(&c->ctx, canon);
    free(canon);

    if (strcmp(d->lng, "noconfirm") == 0) c->noconfirm = true;
    else if (strcmp(d->lng, "confirm") == 0) c->noconfirm = false;
    else if (strcmp(d->lng, "needed") == 0) c->needed = true;
    else if (strcmp(d->lng, "asdeps") == 0) c->asdeps = true;
    else if (strcmp(d->lng, "asexplicit") == 0) c->asexplicit = true;
    else if (strcmp(d->lng, "aur") == 0) c->aur_only = true;
    else if (strcmp(d->lng, "repo") == 0) c->repo_only = true;
    else if (strcmp(d->lng, "chroot") == 0) c->chroot = val;
    else if (strcmp(d->lng, "jobs") == 0) c->jobs = val;
    else if (strcmp(d->lng, "fetch-jobs") == 0) c->fetch_jobs = val;
    else if (strcmp(d->lng, "serial") == 0) c->parallel = 0;
    else if (strcmp(d->lng, "parallel") == 0) c->parallel = 1;
    else if (strcmp(d->lng, "repo-download") == 0) c->repo_download = val;
    else if (strcmp(d->lng, "mirrors") == 0) c->mirrors = val;
    else if (strcmp(d->lng, "count") == 0) c->count = val;
    else if (strcmp(d->lng, "review") == 0) c->review = val;
    else if (strcmp(d->lng, "config") == 0) c->config = val;
    else if (strcmp(d->lng, "ignore") == 0) split_ignore(c, val);
    else if (strcmp(d->lng, "color") == 0)
        c->color = strcmp(val, "never") == 0 ? 0 : strcmp(val, "always") == 0 ? 1 : -1;
}

/* Pass 1 (like pacman): find the operation anywhere on the command line. */
static int find_op(Cli *c, int argc, char **argv, char *err, size_t errsz)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0)
            break;
        if (a[0] != '-' || a[1] == '\0')
            continue;
        char got = 0;
        if (a[1] == '-') {
            const char *l = a + 2;
            size_t len = strcspn(l, "=");
            got = find_long_op(l, len);
            if (got == 0) {
                /* skip the value of a separate-argument long option */
                bool amb;
                const OptDef *d = find_long(l, len, OP_PAC | OP_A | OP_M, &amb);
                if (d && d->arg && l[len] != '=')
                    i++;
                continue;
            }
            if (got == 'h') {
                c->help = true;
                continue;
            }
            if (c->op && c->op != got) {
                snprintf(err, errsz, "only one operation may be used at a time");
                return -1;
            }
            c->op = got;
            continue;
        }
        for (const char *ch = a + 1; *ch; ch++) {
            if (*ch == 'h') {
                c->help = true;
            } else if (strchr("DFQRSTUVA", *ch)) {
                if (c->op && c->op != *ch) {
                    snprintf(err, errsz, "only one operation may be used at a time");
                    return -1;
                }
                c->op = *ch;
            } else if (*ch == 'b' || *ch == 'r') {
                if (ch[1] == '\0')
                    i++;   /* value is the next argument */
                break;     /* or the rest of this one */
            }
        }
    }
    return 0;
}

int cli_parse(Cli *c, int argc, char **argv, char *err, size_t errsz)
{
    if (find_op(c, argc, argv, err, errsz) != 0)
        return -1;
    unsigned opbit = op_bit(c->op);
    if (c->op == 0 || c->op == 'V' || c->op == 'N')
        opbit = OP_PAC | OP_A | OP_M;   /* accept anything; only help/version are used */

    bool end_opts = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (end_opts || a[0] != '-' || a[1] == '\0') {
            sl_push(&c->targets, a);
            continue;
        }
        if (strcmp(a, "--") == 0) {
            end_opts = true;
            continue;
        }
        if (a[1] == '-') {
            const char *l = a + 2;
            size_t len = strcspn(l, "=");
            if (find_long_op(l, len))
                continue;   /* handled in pass 1 */
            bool amb;
            const OptDef *d = find_long(l, len, opbit, &amb);
            if (d == NULL) {
                snprintf(err, errsz, "%s option '%s'%s", amb ? "ambiguous" : "unrecognized", a,
                         c->op && !amb ? " for this operation" : "");
                return -1;
            }
            const char *val = NULL;
            if (d->arg) {
                if (l[len] == '=')
                    val = l + len + 1;
                else if (i + 1 < argc)
                    val = argv[++i];
                else {
                    snprintf(err, errsz, "option '--%s' requires an argument", d->lng);
                    return -1;
                }
            } else if (l[len] == '=') {
                snprintf(err, errsz, "option '--%s' doesn't allow an argument", d->lng);
                return -1;
            }
            apply(c, d, val);
            continue;
        }
        for (const char *ch = a + 1; *ch; ch++) {
            if (*ch == 'h' || strchr("DFQRSTUVA", *ch))
                continue;
            const OptDef *d = find_short(*ch, opbit);
            if (d == NULL) {
                snprintf(err, errsz, "invalid option -- '%c'", *ch);
                return -1;
            }
            if (!d->arg) {
                apply(c, d, NULL);
                continue;
            }
            const char *val = ch + 1;
            if (*val == '\0') {
                if (i + 1 >= argc) {
                    snprintf(err, errsz, "option requires an argument -- '%c'", *ch);
                    return -1;
                }
                val = argv[++i];
            }
            apply(c, d, val);
            break;
        }
    }
    if (c->aur_only && c->repo_only) {
        snprintf(err, errsz, "--aur and --repo cannot be used together");
        return -1;
    }
    return 0;
}
