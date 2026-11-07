#include "pacman.h"
#include "alloc.h"
#include "config.h"
#include "proc.h"
#include "tty.h"
#include "validate.h"
#include "vercmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const StrList *g_ctx;
static bool g_ran_root;
static const char *g_conf;
static pid_t g_bg = -1;
static const char *g_bg_log;

void pacman_set_config_path(const char *path)
{
    g_conf = path;
}

void pacman_bg_set(pid_t pid, const char *log)
{
    g_bg = pid;
    g_bg_log = log;
}

int pacman_bg_wait(void)
{
    if (g_bg <= 0)
        return 0;
    if (!proc_has_exited(g_bg))
        printf(":: Waiting for the background repository download...\n");
    int rc = proc_wait_pid(g_bg);
    g_bg = -1;
    if (rc != 0)
        printf("%s::%s background download did not finish (see %s); pacman downloads "
               "what is missing\n", C_YEL, C_END, g_bg_log ? g_bg_log : "its log");
    return rc;
}

bool pacman_ran_as_root(void)
{
    return g_ran_root;
}

void pacman_set_context(const StrList *ctx)
{
    g_ctx = ctx;
}

/* argv = [elev] pacman <ctx...> <head...> [-- targets...]; caller frees the array. */
static const char **build_argv(const char *elev, const char *const *head,
                               const StrList *args, const StrList *targets)
{
    size_t n = 4 + (g_ctx ? g_ctx->n : 0) + (args ? args->n : 0) + (targets ? targets->n : 0);
    for (size_t i = 0; head && head[i]; i++)
        n++;
    const char **argv = xcalloc(n, sizeof(char *));
    size_t k = 0;
    if (elev)
        argv[k++] = elev;
    argv[k++] = BIN_PACMAN;
    for (size_t i = 0; g_ctx && i < g_ctx->n; i++)
        argv[k++] = g_ctx->v[i];
    for (size_t i = 0; head && head[i]; i++)
        argv[k++] = head[i];
    for (size_t i = 0; args && i < args->n; i++)
        argv[k++] = args->v[i];
    if (targets) {
        argv[k++] = "--";
        for (size_t i = 0; i < targets->n; i++)
            argv[k++] = targets->v[i];
    }
    argv[k] = NULL;
    return argv;
}

static int run_quiet(const char *const head[], StrBuf *cap)
{
    const char **argv = build_argv(NULL, head, NULL, NULL);
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.stdin_null = true;
    o.stderr_null = true;
    if (cap)
        o.capture = cap;
    else
        o.stdout_null = true;
    int rc = proc_run(argv, &o);
    free(argv);
    return rc;
}

bool pacman_dep_satisfied(const char *dep)
{
    const char *head[] = { "-T", "--", dep, NULL };
    return run_quiet(head, NULL) == 0;
}

bool pacman_dep_in_repos(const char *dep)
{
    const char *head[] = { "-Sp", "--noconfirm", "--print-format", "%n", "--", dep, NULL };
    return run_quiet(head, NULL) == 0;
}

bool pacman_in_sync_db(const char *name)
{
    const char *head[] = { "-Si", "--", name, NULL };
    return run_quiet(head, NULL) == 0;
}

char *pacman_installed_version(const char *name)
{
    StrBuf out;
    sb_init(&out, 4096);
    const char *head[] = { "-Q", "--", name, NULL };
    char *ver = NULL;
    if (run_quiet(head, &out) == 0) {
        char *sp = strchr(out.data, ' ');
        if (sp) {
            char *nl = strchr(sp + 1, '\n');
            if (nl)
                *nl = '\0';
            if (valid_version(sp + 1))
                ver = xstrdup(sp + 1);
        }
    }
    sb_free(&out);
    return ver;
}

/* Parses "name version" lines; keeps only well-formed ones. */
static int query_lines(const char *flag, StrList *names, StrList *versions, StrList *lines_out)
{
    StrBuf out;
    sb_init(&out, MAX_GIT_OUTPUT);
    const char *head[] = { flag, NULL };
    int rc = run_quiet(head, &out);
    /* pacman -Qm exits 1 when there are no foreign packages */
    if (rc != 0 && rc != 1) {
        sb_free(&out);
        return -1;
    }
    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, out.data);
    for (size_t i = 0; i < lines.n; i++) {
        char *sp = strchr(lines.v[i], ' ');
        if (!sp)
            continue;
        *sp = '\0';
        if (!valid_pkgname(lines.v[i]) || !valid_version(sp + 1))
            continue;
        if (names)
            sl_push(names, lines.v[i]);
        if (versions)
            sl_push(versions, sp + 1);
        if (lines_out) {
            *sp = ' ';
            sl_push(lines_out, lines.v[i]);
        }
    }
    sl_free(&lines);
    sb_free(&out);
    return 0;
}

int pacman_foreign(StrList *names, StrList *versions)
{
    return query_lines("-Qm", names, versions, NULL);
}

int pacman_installed_names(StrList *out)
{
    StrBuf buf;
    sb_init(&buf, MAX_GIT_OUTPUT);
    const char *head[] = { "-Qq", NULL };
    int rc = run_quiet(head, &buf);
    if (rc == 0) {
        sl_split_lines(out, buf.data);
        sl_sort(out);
    }
    sb_free(&buf);
    return rc == 0 ? 0 : -1;
}

int pacman_capture(const StrList *args, StrBuf *out)
{
    const char **argv = build_argv(NULL, NULL, args, NULL);
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.stdin_null = true;
    o.stderr_null = true;
    o.capture = out;
    int rc = proc_run(argv, &o);
    free(argv);
    return rc;
}

int pacman_conf_repo_servers(const char *repo, StrList *out)
{
    char cfg[PATH_BUF], rarg[300];
    const char *argv[7];
    size_t k = 0;
    argv[k++] = BIN_PACMAN_CONF;
    if (g_conf) {
        snprintf(cfg, sizeof cfg, "--config=%s", g_conf);
        argv[k++] = cfg;
    }
    snprintf(rarg, sizeof rarg, "--repo=%s", repo);
    argv[k++] = rarg;
    argv[k++] = "--";
    argv[k++] = "Server";
    argv[k] = NULL;
    StrBuf buf;
    sb_init(&buf, 1u << 20);
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.stdin_null = true;
    o.stderr_null = true;
    o.capture = &buf;
    int rc = proc_run(argv, &o);
    if (rc == 0) {
        StrList l;
        sl_init(&l);
        sl_split_lines(&l, buf.data);
        for (size_t i = 0; i < l.n; i++)
            if (strncmp(l.v[i], "https://", 8) == 0 || strncmp(l.v[i], "http://", 7) == 0)
                sl_push(out, l.v[i]);
        sl_free(&l);
    }
    sb_free(&buf);
    return rc == 0 ? 0 : -1;
}

int pacman_conf_list(const char *config, const char *key, StrList *out)
{
    char cfg[PATH_BUF];
    const char *argv[5];
    if (config == NULL)
        config = g_conf;
    size_t k = 0;
    argv[k++] = BIN_PACMAN_CONF;
    if (config) {
        snprintf(cfg, sizeof cfg, "--config=%s", config);
        argv[k++] = cfg;
    }
    argv[k++] = "--";
    argv[k++] = key;
    argv[k] = NULL;
    StrBuf buf;
    sb_init(&buf, 1u << 20);
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.stdin_null = true;
    o.stderr_null = true;
    o.capture = &buf;
    int rc = proc_run(argv, &o);
    if (rc == 0)
        sl_split_lines(out, buf.data);
    sb_free(&buf);
    return rc == 0 ? 0 : -1;
}

int pacman_run(bool as_root, const StrList *args, const StrList *targets)
{
    const char *elev = NULL;
    if (as_root) {
        elev = privilege_elevator();
        if (elev == NULL) {
            fprintf(stderr, "error: sudo or doas (root-owned, setuid) is required\n");
            return -1;
        }
    }
    const char **argv = build_argv(elev, NULL, args, targets);
    ProcOpts o;
    memset(&o, 0, sizeof o);
    fflush(stdout);
    if (as_root) {
        pacman_bg_wait();   /* the background pacman holds the lock */
        g_ran_root = true;
    }
    int rc = proc_run(argv, &o);
    free(argv);
    return rc;
}

pid_t pacman_spawn_root(const StrList *args, int log_fd)
{
    const char *elev = privilege_elevator();
    if (elev == NULL)
        return -1;
    const char **inner = build_argv(NULL, NULL, args, NULL);
    size_t n = 0;
    while (inner[n])
        n++;
    const char **argv = xcalloc(n + 3, sizeof(char *));
    argv[0] = elev;
    argv[1] = "-n";   /* sudo and doas: non-interactive */
    for (size_t i = 0; i < n; i++)
        argv[2 + i] = inner[i];
    argv[n + 2] = NULL;
    ProcOpts o;
    memset(&o, 0, sizeof o);
    pid_t pid = proc_spawn(argv, &o, log_fd);
    free(argv);
    free(inner);
    if (pid > 0)
        g_ran_root = true;
    return pid;
}

void pacman_refresh_credentials(void)
{
    const char *elev = privilege_elevator();
    if (elev == NULL || strcmp(elev, BIN_SUDO) != 0)
        return;
    const char *argv[] = { elev, "-v", NULL };
    ProcOpts o;
    memset(&o, 0, sizeof o);
    proc_run(argv, &o);
}

int pacman_snapshot(StrList *out)
{
    if (query_lines("-Q", NULL, NULL, out) != 0)
        return -1;
    sl_sort(out);   /* ' ' sorts below every name byte: this is a sort by name */
    return 0;
}

/* ------------------------------------------------------------ summary */

typedef struct {
    const char *name;
    size_t nlen;
    const char *ver;
} Entry;

static Entry entry(const char *line)
{
    Entry e;
    const char *sp = strchr(line, ' ');
    e.name = line;
    e.nlen = sp ? (size_t)(sp - line) : strlen(line);
    e.ver = sp ? sp + 1 : "";
    return e;
}

static int cmp_name(const Entry *a, const Entry *b)
{
    size_t n = a->nlen < b->nlen ? a->nlen : b->nlen;
    int c = memcmp(a->name, b->name, n);
    if (c)
        return c;
    return a->nlen < b->nlen ? -1 : a->nlen > b->nlen;
}

enum { CH_UPGRADED, CH_INSTALLED, CH_REINSTALLED, CH_DOWNGRADED, CH_REMOVED, CH_N };

typedef struct {
    Entry e;
    const char *old;   /* previous version (upgraded/downgraded) */
} Change;

typedef struct {
    Change *v;
    size_t n, cap;
} ChangeList;

static void push(ChangeList *l, Entry e, const char *old)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xreallocarray(l->v, l->cap, sizeof(Change));
    }
    l->v[l->n].e = e;
    l->v[l->n].old = old;
    l->n++;
}

static bool touched_has(const StrList *touched, const Entry *e)
{
    for (size_t i = 0; touched && i < touched->n; i++)
        if (strlen(touched->v[i]) == e->nlen && memcmp(touched->v[i], e->name, e->nlen) == 0)
            return true;
    return false;
}

size_t pacman_print_changes(const StrList *before, const StrList *after, const StrList *touched)
{
    ChangeList ch[CH_N];
    memset(ch, 0, sizeof ch);
    size_t i = 0, j = 0;
    while (i < before->n || j < after->n) {
        Entry a = entry(i < before->n ? before->v[i] : "");
        Entry b = entry(j < after->n ? after->v[j] : "");
        int c = i >= before->n ? 1 : j >= after->n ? -1 : cmp_name(&a, &b);
        if (c < 0) {
            push(&ch[CH_REMOVED], a, NULL);
            i++;
        } else if (c > 0) {
            push(&ch[CH_INSTALLED], b, NULL);
            j++;
        } else {
            int v = pkg_vercmp(b.ver, a.ver);
            if (v > 0)
                push(&ch[CH_UPGRADED], b, a.ver);
            else if (v < 0)
                push(&ch[CH_DOWNGRADED], b, a.ver);
            else if (touched_has(touched, &b))
                push(&ch[CH_REINSTALLED], b, NULL);
            i++;
            j++;
        }
    }

    static const char *const title[CH_N] = {
        "Upgraded", "Installed", "Reinstalled", "Downgraded", "Removed"
    };
    const char *col[CH_N] = { C_GREEN, C_GREEN, C_BLUE, C_YEL, C_RED };
    size_t total = 0, width = 0;
    for (int k = 0; k < CH_N; k++) {
        total += ch[k].n;
        for (size_t m = 0; m < ch[k].n; m++)
            if (ch[k].v[m].e.nlen > width)
                width = ch[k].v[m].e.nlen;
    }
    if (width > 40)
        width = 40;

    printf("\n%s:: Summary of changes%s\n", C_BOLD, C_END);
    if (total == 0)
        printf("   no packages were changed\n");
    for (int k = 0; k < CH_N; k++) {
        if (ch[k].n == 0)
            continue;
        printf("%s%s (%zu)%s\n", col[k], title[k], ch[k].n, C_END);
        for (size_t m = 0; m < ch[k].n; m++) {
            const Change *x = &ch[k].v[m];
            printf("   %s%-*.*s%s  ", C_BOLD, (int)width, (int)x->e.nlen, x->e.name, C_END);
            if (x->old)
                printf("%s -> %s%s%s\n", x->old, col[k], x->e.ver, C_END);
            else
                printf("%s\n", x->e.ver);
        }
        free(ch[k].v);
    }
    if (total)
        printf("%s   total: %zu upgraded, %zu installed, %zu reinstalled, %zu downgraded, "
               "%zu removed%s\n", C_BOLD, ch[CH_UPGRADED].n, ch[CH_INSTALLED].n,
               ch[CH_REINSTALLED].n, ch[CH_DOWNGRADED].n, ch[CH_REMOVED].n, C_END);
    return total;
}
