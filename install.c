#include "install.h"
#include "alloc.h"
#include "audit.h"
#include "config.h"
#include "fsutil.h"
#include "git.h"
#include "pacman.h"
#include "proc.h"
#include "review.h"
#include "rpc.h"
#include "srcinfo.h"
#include "tty.h"
#include "validate.h"
#include "vercmp.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <syslog.h>
#include <unistd.h>

typedef enum { ST_PENDING, ST_SKIPPED, ST_REMOVED, ST_FAILED, ST_DONE } NodeState;

typedef struct {
    char *base;
    StrList want;          /* pkgnames from this base that must be installed */
    StrList aliases;       /* dependency names that resolved to this base */
    bool asdeps;
    bool installed;        /* some wanted pkgname is installed right now */
    NodeState state;
    char *why;             /* reason for SKIPPED/FAILED */
    char commit[80];
    char dir[PATH_BUF];
    SrcInfo si;
    StrList built;         /* package files to install */
} Node;

typedef struct {
    Node *v;
    size_t n, cap;
    StrList visited;       /* dependency names already handled */
    StrList in_progress;   /* bases being resolved (cycle guard) */
    const Options *o;
    char arch[80];
} Plan;

static void node_free(Node *n)
{
    free(n->base);
    free(n->why);
    sl_free(&n->want);
    sl_free(&n->aliases);
    sl_free(&n->built);
    srcinfo_free(&n->si);
}

static void plan_free(Plan *p)
{
    for (size_t i = 0; i < p->n; i++)
        node_free(&p->v[i]);
    free(p->v);
    sl_free(&p->visited);
    sl_free(&p->in_progress);
}

static Node *plan_find_base(Plan *p, const char *base)
{
    for (size_t i = 0; i < p->n; i++)
        if (strcmp(p->v[i].base, base) == 0)
            return &p->v[i];
    return NULL;
}

/* ------------------------------------------------------------ repo */

static int prepare_repo(const Options *o, Node *n)
{
    if (!path_join(n->dir, sizeof n->dir, o->cache, n->base))
        return -1;
    char url[PATH_BUF];
    snprintf(url, sizeof url, AUR_GIT_FMT, n->base);

    char gitdir[PATH_BUF];
    bool reuse = false;
    if (path_join(gitdir, sizeof gitdir, n->dir, ".git") && is_dir_nofollow(n->dir) &&
        is_dir_nofollow(gitdir)) {
        StrBuf origin;
        sb_init(&origin, 4096);
        reuse = git_origin_url(n->dir, &origin) == 0 && strcmp(origin.data, url) == 0;
        sb_free(&origin);
    }

    if (reuse) {
        printf("   fetching %s\n", n->base);
        if (git_fetch(n->dir) != 0) {
            fprintf(stderr, "error: git fetch failed for %s\n", n->base);
            return -1;
        }
    } else {
        if (rmtree(n->dir) != 0) {
            fprintf(stderr, "error: could not clear stale cache dir %s\n", n->dir);
            return -1;
        }
        printf("   cloning %s\n", url);
        if (git_clone(url, n->dir) != 0) {
            fprintf(stderr, "error: git clone failed for %s\n", n->base);
            return -1;
        }
    }

    if (git_rev(n->dir, "refs/remotes/origin/master", n->commit, sizeof n->commit) != 0) {
        fprintf(stderr, "error: %s: empty repository (package does not exist in the AUR)\n",
                n->base);
        return -1;
    }

    StrBuf si;
    sb_init(&si, MAX_SCAN_FILE_BYTES);
    if (git_show_file(n->dir, n->commit, ".SRCINFO", &si) != 0 ||
        srcinfo_parse(si.data, &n->si) != 0) {
        fprintf(stderr, "error: %s: missing or malformed .SRCINFO\n", n->base);
        sb_free(&si);
        return -1;
    }
    sb_free(&si);

    const char *b = srcinfo_first(&n->si, "pkgbase");
    if (b == NULL || strcmp(b, n->base) != 0) {
        fprintf(stderr, "error: %s: .SRCINFO pkgbase does not match the AUR record\n", n->base);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------ resolve */

static int resolve(Plan *p, const char *depstr, bool explicit, int depth);

static int resolve_node_deps(Plan *p, Node *n, int depth)
{
    static const char *const keys[] = { "depends", "makedepends", "checkdepends", NULL };
    for (size_t k = 0; keys[k]; k++) {
        StrList deps;
        sl_init(&deps);
        srcinfo_values(&n->si, keys[k], p->arch, &deps);
        for (size_t i = 0; i < deps.n; i++) {
            if (resolve(p, deps.v[i], false, depth + 1) != 0) {
                fprintf(stderr, "   (required by %s)\n", n->base);
                sl_free(&deps);
                return -1;
            }
        }
        sl_free(&deps);
    }
    return 0;
}

static int lookup_aur(const char *name, RpcResult *res, const AurPkg **out)
{
    memset(res, 0, sizeof *res);
    StrList one;
    sl_init(&one);
    sl_push(&one, name);
    int rc = rpc_info(&one, res);
    sl_free(&one);
    if (rc != 0)
        return -1;
    *out = rpc_find(res, name);
    if (*out)
        return 0;

    /* maybe a virtual name provided by some AUR package */
    rpc_result_free(res);
    if (!valid_search_term(name) || rpc_search(name, true, res) != 0)
        return -1;
    if (res->n == 1) {
        *out = &res->pkgs[0];
        printf("   %s is provided by AUR package %s\n", name, (*out)->name);
        return 0;
    }
    if (res->n > 1) {
        fprintf(stderr, "error: several AUR packages provide '%s'; install one explicitly:\n", name);
        for (size_t i = 0; i < res->n && i < 20; i++) {
            fputs("   ", stderr);
            safe_fputs(stderr, res->pkgs[i].name);
            fputc('\n', stderr);
        }
    }
    return -1;
}

static int resolve(Plan *p, const char *depstr, bool explicit, int depth)
{
    char name[MAX_NAME_LEN + 1];
    if (!valid_depstring(depstr, name, sizeof name)) {
        fputs("error: invalid dependency string: ", stderr);
        safe_fputs(stderr, depstr);
        fputc('\n', stderr);
        return -1;
    }
    if (depth > MAX_DEP_DEPTH) {
        fprintf(stderr, "error: dependency chain too deep at %s\n", name);
        return -1;
    }
    if (!sl_push_unique(&p->visited, name))
        return 0;

    if (!explicit) {
        if (pacman_dep_satisfied(depstr))
            return 0;
        if (pacman_dep_in_repos(depstr))
            return 0;   /* makepkg --syncdeps installs it from the repos */
    }

    RpcResult res;
    const AurPkg *pkg = NULL;
    if (lookup_aur(name, &res, &pkg) != 0) {
        fprintf(stderr, "error: target not found in the repositories or the AUR: %s\n", name);
        rpc_result_free(&res);
        return -1;
    }

    Node *existing = plan_find_base(p, pkg->base);
    if (existing) {
        sl_push_unique(&existing->want, pkg->name);
        sl_push_unique(&existing->aliases, name);
        if (explicit)
            existing->asdeps = false;
        rpc_result_free(&res);
        return 0;
    }
    if (sl_contains(&p->in_progress, pkg->base)) {
        rpc_result_free(&res);
        return 0;
    }

    if (explicit && p->o->needed) {
        char *inst = pacman_installed_version(pkg->name);
        if (inst && pkg_vercmp(inst, pkg->version) >= 0) {
            printf("warning: %s-%s is up to date -- skipping\n", pkg->name, inst);
            free(inst);
            rpc_result_free(&res);
            return 0;
        }
        free(inst);
    }
    if (pkg->outofdate)
        printf("%swarning:%s %s is flagged OUT OF DATE in the AUR\n", C_YEL, C_END, pkg->name);
    if (pkg->orphan)
        printf("%swarning:%s %s is ORPHANED (no maintainer): higher takeover risk\n",
               C_YEL, C_END, pkg->name);

    Node n;
    memset(&n, 0, sizeof n);
    n.base = xstrdup(pkg->base);
    sl_init(&n.want);
    sl_init(&n.aliases);
    sl_init(&n.built);
    sl_push(&n.want, pkg->name);
    sl_push(&n.aliases, name);
    n.asdeps = !explicit;
    rpc_result_free(&res);

    sl_push(&p->in_progress, n.base);
    if (prepare_repo(p->o, &n) != 0 || resolve_node_deps(p, &n, depth) != 0) {
        node_free(&n);
        return -1;
    }

    StrList names;
    sl_init(&names);
    srcinfo_pkgnames(&n.si, &names);
    bool listed = sl_contains(&names, n.want.v[0]);
    sl_free(&names);
    if (!listed) {
        fprintf(stderr, "error: %s: .SRCINFO does not declare pkgname %s\n", n.base, n.want.v[0]);
        node_free(&n);
        return -1;
    }

    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->v = xreallocarray(p->v, p->cap, sizeof(Node));
    }
    p->v[p->n++] = n;   /* post-order: dependencies first */
    return 0;
}

/* ------------------------------------------------------------ build */

static int check_pgp_keys(const Node *n)
{
    StrList keys;
    sl_init(&keys);
    srcinfo_values_exact(&n->si, "validpgpkeys", &keys);
    int rc = 0;
    for (size_t i = 0; i < keys.n && rc == 0; i++) {
        const char *fpr = keys.v[i];
        if (!valid_fingerprint(fpr)) {
            fprintf(stderr, "error: %s: validpgpkeys entry is not a full fingerprint\n", n->base);
            rc = -1;
            break;
        }
        const char *list[] = { BIN_GPG, "--batch", "--list-keys", "--", fpr, NULL };
        ProcOpts q;
        memset(&q, 0, sizeof q);
        q.stdin_null = q.stdout_null = q.stderr_null = true;
        if (proc_run(list, &q) == 0)
            continue;

        printf("\n%s:: %s requires source signing key %s%s\n"
               "   This key was chosen by the AUR maintainer, not by Arch. Before importing,\n"
               "   confirm the fingerprint on the upstream project's official site.\n",
               C_YEL, n->base, fpr, C_END);
        if (!ask_approval("Import this key from the keyserver?")) {
            fprintf(stderr, "error: key not imported; signature cannot be verified\n");
            rc = -1;
            break;
        }
        const char *recv[] = { BIN_GPG, "--batch", "--recv-keys", "--", fpr, NULL };
        ProcOpts r;
        memset(&r, 0, sizeof r);
        r.stdin_null = true;
        if (proc_run(recv, &r) != 0 || proc_run(list, &q) != 0) {
            fprintf(stderr, "error: could not import key %s\n", fpr);
            rc = -1;
        }
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "pgp key %s imported for %s", fpr, n->base);
    }
    sl_free(&keys);
    return rc;
}

static void normalize_lines(const char *text, StrList *out)
{
    StrList raw;
    sl_init(&raw);
    sl_split_lines(&raw, text);
    for (size_t i = 0; i < raw.n; i++) {
        char *s = raw.v[i];
        while (*s == ' ' || *s == '\t')
            s++;
        size_t L = strlen(s);
        while (L && (s[L - 1] == ' ' || s[L - 1] == '\t'))
            s[--L] = '\0';
        if (L && s[0] != '#')
            sl_push(out, s);
    }
    sl_free(&raw);
}

/* .SRCINFO is what dependency resolution used; make sure it matches what
 * the (approved) PKGBUILD really declares. */
static int check_srcinfo_consistency(const Node *n)
{
    StrBuf gen;
    sb_init(&gen, MAX_SCAN_FILE_BYTES);
    const char *argv[] = { BIN_MAKEPKG, "--printsrcinfo", NULL };
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.cwd = n->dir;
    o.capture = &gen;
    o.stdin_null = true;
    if (proc_run(argv, &o) != 0) {
        fprintf(stderr, "error: makepkg --printsrcinfo failed for %s\n", n->base);
        sb_free(&gen);
        return -1;
    }
    char path[PATH_BUF];
    StrBuf file;
    sb_init(&file, MAX_SCAN_FILE_BYTES);
    if (!path_join(path, sizeof path, n->dir, ".SRCINFO") ||
        read_file_limited(path, MAX_SCAN_FILE_BYTES, &file) != 0) {
        sb_free(&gen);
        sb_free(&file);
        return -1;
    }
    StrList a, b;
    sl_init(&a);
    sl_init(&b);
    normalize_lines(gen.data, &a);
    normalize_lines(file.data, &b);
    sb_free(&gen);
    sb_free(&file);

    size_t diffs = 0;
    for (size_t i = 0; i < a.n; i++)
        if (!sl_contains(&b, a.v[i])) {
            if (diffs++ == 0)
                printf("\n%s:: %s: .SRCINFO differs from what PKGBUILD declares%s\n",
                       C_YEL, n->base, C_END);
            if (diffs <= 20) {
                fputs("   PKGBUILD only: ", stdout);
                safe_fputs(stdout, a.v[i]);
                fputc('\n', stdout);
            }
        }
    for (size_t i = 0; i < b.n; i++)
        if (!sl_contains(&a, b.v[i])) {
            if (diffs++ == 0)
                printf("\n%s:: %s: .SRCINFO differs from what PKGBUILD declares%s\n",
                       C_YEL, n->base, C_END);
            if (diffs <= 20) {
                fputs("   .SRCINFO only: ", stdout);
                safe_fputs(stdout, b.v[i]);
                fputc('\n', stdout);
            }
        }
    sl_free(&a);
    sl_free(&b);
    if (diffs == 0)
        return 0;
    printf("   A stale .SRCINFO is common, but it can also hide dependencies or sources.\n");
    return ask_approval("Continue with this build anyway?") ? 0 : -1;
}

static char *pkgname_from_file(const char *path)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *ext = strstr(b, ".pkg.tar");
    if (ext == NULL)
        return NULL;
    char *s = xstrndup(b, (size_t)(ext - b));
    for (int i = 0; i < 3; i++) {
        char *d = strrchr(s, '-');
        if (d == NULL) {
            free(s);
            return NULL;
        }
        *d = '\0';
    }
    return s;
}

enum { BUILD_OK = 0, BUILD_FAIL = -1, BUILD_SKIP = 1, BUILD_REMOVE = 2, BUILD_ABORT = 3 };

static int build_node(Plan *p, Node *n)
{
    const Options *o = p->o;
    printf("\n%s:: Building %s @ %.12s%s\n", C_BOLD, n->base, n->commit, C_END);

    if (git_checkout_exact(n->dir, n->commit) != 0) {
        fprintf(stderr, "error: %s: worktree does not match the reviewed commit\n", n->base);
        return -1;
    }
    if (check_pgp_keys(n) != 0 || check_srcinfo_consistency(n) != 0)
        return -1;

    int rc;
    if (o->chroot) {
        size_t cap = 8 + 2 * 64;
        const char **argv = xcalloc(cap, sizeof(char *));
        size_t k = 0;
        argv[k++] = BIN_MAKECHROOTPKG;
        argv[k++] = "-c";
        argv[k++] = "-r";
        argv[k++] = o->chroot;
        for (size_t i = 0; i < p->n && &p->v[i] != n; i++)
            for (size_t j = 0; j < p->v[i].built.n && k + 3 < cap; j++) {
                argv[k++] = "-I";
                argv[k++] = p->v[i].built.v[j];
            }
        argv[k] = NULL;
        ProcOpts po;
        memset(&po, 0, sizeof po);
        po.cwd = n->dir;
        rc = proc_run(argv, &po);
        free(argv);
    } else {
        const char *argv[] = { BIN_MAKEPKG, "--syncdeps", "--cleanbuild", "--clean",
                               o->noconfirm ? "--noconfirm" : NULL, NULL };
        ProcOpts po;
        memset(&po, 0, sizeof po);
        po.cwd = n->dir;
        rc = proc_run(argv, &po);
    }
    if (rc != 0) {
        fprintf(stderr, "error: build of %s failed (exit %d)\n", n->base, rc);
        return -1;
    }

    StrBuf list;
    sb_init(&list, 1u << 20);
    const char *pl[] = { BIN_MAKEPKG, "--packagelist", NULL };
    ProcOpts lo;
    memset(&lo, 0, sizeof lo);
    lo.cwd = n->dir;
    lo.capture = &list;
    lo.stdin_null = true;
    if (proc_run(pl, &lo) != 0) {
        fprintf(stderr, "error: makepkg --packagelist failed for %s\n", n->base);
        sb_free(&list);
        return -1;
    }
    StrList files;
    sl_init(&files);
    sl_split_lines(&files, list.data);
    sb_free(&list);
    for (size_t i = 0; i < files.n; i++) {
        char *pn = pkgname_from_file(files.v[i]);
        if (pn && sl_contains(&n->want, pn) && files.v[i][0] == '/' &&
            is_file_nofollow(files.v[i]))
            sl_push(&n->built, files.v[i]);
        free(pn);
    }
    sl_free(&files);
    if (n->built.n == 0) {
        fprintf(stderr, "error: %s: no built package found for the requested names\n", n->base);
        return -1;
    }

    Report rep;
    report_init(&rep);
    for (size_t i = 0; i < n->built.n; i++)
        audit_package_file(n->built.v[i], &rep);
    printf("\n%s:: Package content audit for %s%s\n", C_BOLD, n->base, C_END);
    report_print(&rep, stdout, use_color());
    Decision d = DEC_APPROVE;
    if (rep.max >= SEV_HIGH)
        d = ask_decision(n->base, "built package: install it?", rep.max, n->installed, false);
    syslog(LOG_AUTHPRIV | LOG_NOTICE, "package audit decision=%d pkgbase=%s commit=%s max_severity=%s",
           (int)d, n->base, n->commit, severity_name(rep.max));
    report_free(&rep);
    switch (d) {
    case DEC_APPROVE: return BUILD_OK;
    case DEC_SKIP:    return BUILD_SKIP;
    case DEC_REMOVE:  return BUILD_REMOVE;
    case DEC_ABORT:   return BUILD_ABORT;
    default:          return BUILD_FAIL;
    }
}

static int install_node(const Options *o, const Node *n)
{
    const char *elev = privilege_elevator();
    if (elev == NULL) {
        fprintf(stderr, "error: sudo or doas (root-owned, setuid) is required to install\n");
        return -1;
    }
    const char **argv = xcalloc(n->built.n + 8, sizeof(char *));
    size_t k = 0;
    argv[k++] = elev;
    argv[k++] = BIN_PACMAN;
    argv[k++] = "-U";
    if (n->asdeps)
        argv[k++] = "--asdeps";
    if (o->noconfirm)
        argv[k++] = "--noconfirm";
    argv[k++] = "--";
    for (size_t i = 0; i < n->built.n; i++)
        argv[k++] = n->built.v[i];
    argv[k] = NULL;
    ProcOpts po;
    memset(&po, 0, sizeof po);
    int rc = proc_run(argv, &po);
    free(argv);
    if (rc == 0)
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "installed pkgbase=%s commit=%s", n->base, n->commit);
    return rc == 0 ? 0 : -1;
}

/* ------------------------------------------------------------ commands */

static void mark(Node *n, NodeState st, const char *why)
{
    n->state = st;
    free(n->why);
    n->why = why ? xstrdup(why) : NULL;
}

/* Does node a need (depends/makedepends/checkdepends) anything node b provides? */
static bool node_needs(const Plan *p, const Node *a, const Node *b)
{
    static const char *const keys[] = { "depends", "makedepends", "checkdepends", NULL };
    bool hit = false;
    for (size_t k = 0; keys[k] && !hit; k++) {
        StrList deps;
        sl_init(&deps);
        srcinfo_values(&a->si, keys[k], p->arch, &deps);
        for (size_t i = 0; i < deps.n && !hit; i++) {
            char nm[MAX_NAME_LEN + 1];
            if (valid_depstring(deps.v[i], nm, sizeof nm) &&
                (sl_contains(&b->want, nm) || sl_contains(&b->aliases, nm)))
                hit = true;
        }
        sl_free(&deps);
    }
    return hit;
}

/* b will not be built: everything pending that needs it is skipped too. */
static void drop_dependents(Plan *p, size_t b)
{
    /* a skipped/failed update leaves the old version installed, which still
     * satisfies its dependents; only a missing or removed package blocks them */
    if (p->v[b].installed && p->v[b].state != ST_REMOVED)
        return;
    for (size_t i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        if (i == b || n->state != ST_PENDING || !node_needs(p, n, &p->v[b]))
            continue;
        char why[PATH_BUF];
        snprintf(why, sizeof why, "needs %s, which was not built", p->v[b].base);
        mark(n, ST_SKIPPED, why);
        printf("%s::%s skipping %s: %s\n", C_YEL, C_END, n->base, why);
        drop_dependents(p, i);
    }
}

static bool node_is_installed(const Node *n)
{
    for (size_t i = 0; i < n->want.n; i++) {
        char *v = pacman_installed_version(n->want.v[i]);
        free(v);
        if (v)
            return true;
    }
    return false;
}

/* pacman -R the installed pkgnames of this base; pacman asks for confirmation. */
static int remove_node(const Options *o, Node *n)
{
    const char *elev = privilege_elevator();
    if (elev == NULL) {
        fprintf(stderr, "error: sudo or doas is required\n");
        return -1;
    }
    const char **argv = xcalloc(n->want.n + 8, sizeof(char *));
    size_t k = 0;
    argv[k++] = elev;
    argv[k++] = BIN_PACMAN;
    argv[k++] = "-R";
    if (o->noconfirm)
        argv[k++] = "--noconfirm";
    argv[k++] = "--";
    size_t first = k;
    for (size_t i = 0; i < n->want.n; i++) {
        char *v = pacman_installed_version(n->want.v[i]);
        if (v)
            argv[k++] = n->want.v[i];
        free(v);
    }
    argv[k] = NULL;
    int rc = 0;
    if (k > first) {
        ProcOpts po;
        memset(&po, 0, sizeof po);
        rc = proc_run(argv, &po) == 0 ? 0 : -1;
    }
    free(argv);
    if (rc == 0)
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "removed pkgbase=%s by user decision", n->base);
    return rc;
}

/* Apply a skip/remove decision and propagate it to dependents. */
static void apply_leave_out(Plan *p, size_t i, bool remove)
{
    Node *n = &p->v[i];
    if (remove) {
        if (remove_node(p->o, n) == 0) {
            mark(n, ST_REMOVED, "removed by you");
        } else {
            printf("%s::%s %s could not be removed (see pacman above); it is skipped\n",
                   C_YEL, C_END, n->base);
            mark(n, ST_SKIPPED, "removal failed, skipped");
        }
    } else {
        mark(n, ST_SKIPPED, "skipped by you");
    }
    drop_dependents(p, i);
}

static int print_summary(const Plan *p, bool aborted)
{
    static const char *const names[] = { "not built", "skipped", "removed", "FAILED", "installed" };
    int failed = 0;
    printf("\n%s:: Summary%s\n", C_BOLD, C_END);
    for (size_t i = 0; i < p->n; i++) {
        const Node *n = &p->v[i];
        const char *col = n->state == ST_DONE ? C_GREEN :
                          n->state == ST_FAILED ? C_RED : C_YEL;
        printf("   %s%-10s%s %s", col, names[n->state], C_END, n->base);
        if (n->why)
            printf("  (%s)", n->why);
        putchar('\n');
        if (n->state == ST_FAILED)
            failed++;
    }
    if (aborted)
        printf("   aborted by you; pending packages were not built\n");
    return (failed || aborted) ? 1 : 0;
}

static int run_plan(Plan *p)
{
    if (p->n == 0) {
        printf(" there is nothing to do\n");
        return 0;
    }
    printf("\n%s:: AUR packages (%zu), in build order:%s\n", C_BOLD, p->n, C_END);
    for (size_t i = 0; i < p->n; i++) {
        printf("   %s @ %.12s  [", p->v[i].base, p->v[i].commit);
        for (size_t j = 0; j < p->v[i].want.n; j++)
            printf("%s%s", j ? " " : "", p->v[i].want.v[j]);
        printf("]%s\n", p->v[i].asdeps ? " (dependency)" : "");
    }
    if (!ask_yesno("Proceed to security review?", true, p->o->noconfirm))
        return 1;

    /* Phase 1: every package is reviewed before anything is built. */
    for (size_t i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        if (n->state != ST_PENDING)
            continue;
        if (git_checkout_exact(n->dir, n->commit) != 0) {
            fprintf(stderr, "error: could not check out %s @ %s\n", n->base, n->commit);
            mark(n, ST_FAILED, "checkout failed");
            drop_dependents(p, i);
            continue;
        }
        n->installed = node_is_installed(n);
        Report rep;
        report_init(&rep);
        audit_repo(n->dir, true, &rep);
        ReviewCtx ctx = { n->dir, n->base, n->commit, p->o->cache, n->installed };
        Decision d = review_package(&ctx, &rep);
        report_free(&rep);
        if (d == DEC_ABORT)
            return print_summary(p, true);
        if (d == DEC_SKIP || d == DEC_REMOVE)
            apply_leave_out(p, i, d == DEC_REMOVE);
    }

    /* Phase 2: build, audit the result, install. Dependencies come first. */
    for (size_t i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        if (n->state != ST_PENDING)
            continue;
        int r = build_node(p, n);
        if (r == BUILD_ABORT)
            return print_summary(p, true);
        if (r == BUILD_SKIP || r == BUILD_REMOVE) {
            apply_leave_out(p, i, r == BUILD_REMOVE);
            continue;
        }
        if (r != BUILD_OK || install_node(p->o, n) != 0) {
            mark(n, ST_FAILED, r != BUILD_OK ? "build failed" : "pacman -U failed");
            drop_dependents(p, i);
            continue;
        }
        mark(n, ST_DONE, NULL);
    }
    return print_summary(p, false);
}

static void plan_init(Plan *p, const Options *o)
{
    memset(p, 0, sizeof *p);
    sl_init(&p->visited);
    sl_init(&p->in_progress);
    p->o = o;
    struct utsname u;
    if (uname(&u) == 0 && valid_pkgname(u.machine))
        snprintf(p->arch, sizeof p->arch, "%s", u.machine);
    else
        snprintf(p->arch, sizeof p->arch, "x86_64");
}

static int sync_aur(const StrList *aur, const Options *o)
{
    Plan p;
    plan_init(&p, o);
    printf(":: Resolving AUR dependencies...\n");
    int rc = 0;
    for (size_t i = 0; i < aur->n && rc == 0; i++)
        if (resolve(&p, aur->v[i], true, 0) != 0)
            rc = 1;
    if (rc == 0)
        rc = run_plan(&p);
    plan_free(&p);
    return rc;
}

int cmd_sync(const StrList *targets, const Options *o)
{
    StrList repo, aur;
    sl_init(&repo);
    sl_init(&aur);
    int rc = 0;
    for (size_t i = 0; i < targets->n; i++) {
        const char *t = targets->v[i];
        bool force_aur = strncmp(t, "aur/", 4) == 0;
        if (force_aur)
            t += 4;
        if (!valid_pkgname(t)) {
            fputs("error: invalid package name: ", stderr);
            safe_fputs(stderr, targets->v[i]);
            fputc('\n', stderr);
            rc = 1;
            goto out;
        }
        if (!force_aur && pacman_dep_in_repos(t))
            sl_push_unique(&repo, t);
        else
            sl_push_unique(&aur, t);
    }

    if (repo.n) {
        const char *elev = privilege_elevator();
        if (elev == NULL) {
            fprintf(stderr, "error: sudo or doas is required\n");
            rc = 1;
            goto out;
        }
        const char **argv = xcalloc(repo.n + 8, sizeof(char *));
        size_t k = 0;
        argv[k++] = elev;
        argv[k++] = BIN_PACMAN;
        argv[k++] = "-S";
        if (o->needed)
            argv[k++] = "--needed";
        if (o->noconfirm)
            argv[k++] = "--noconfirm";
        argv[k++] = "--";
        for (size_t i = 0; i < repo.n; i++)
            argv[k++] = repo.v[i];
        argv[k] = NULL;
        ProcOpts po;
        memset(&po, 0, sizeof po);
        rc = proc_run(argv, &po) == 0 ? 0 : 1;
        free(argv);
        if (rc)
            goto out;
    }
    if (aur.n)
        rc = sync_aur(&aur, o);
out:
    sl_free(&repo);
    sl_free(&aur);
    return rc;
}

int cmd_upgrade(const Options *o, bool sync_repos)
{
    if (sync_repos) {
        const char *elev = privilege_elevator();
        if (elev == NULL) {
            fprintf(stderr, "error: sudo or doas is required\n");
            return 1;
        }
        const char *argv[] = { elev, BIN_PACMAN, "-Syu",
                               o->noconfirm ? "--noconfirm" : NULL, NULL };
        ProcOpts po;
        memset(&po, 0, sizeof po);
        printf(":: Synchronizing package databases and upgrading repositories...\n");
        if (proc_run(argv, &po) != 0)
            return 1;
    }

    StrList names, vers;
    sl_init(&names);
    sl_init(&vers);
    if (pacman_foreign(&names, &vers) != 0) {
        fprintf(stderr, "error: could not list foreign packages\n");
        return 1;
    }
    printf(":: Checking %zu foreign packages against the AUR...\n", names.n);

    StrList upd;
    sl_init(&upd);
    int rc = 0;
    for (size_t off = 0; off < names.n && rc == 0; off += RPC_BATCH) {
        StrList batch;
        sl_init(&batch);
        for (size_t i = off; i < names.n && i < off + RPC_BATCH; i++)
            sl_push(&batch, names.v[i]);
        RpcResult res;
        if (rpc_info(&batch, &res) != 0) {
            rc = 1;
        } else {
            for (size_t i = off; i < names.n && i < off + RPC_BATCH; i++) {
                const AurPkg *a = rpc_find(&res, names.v[i]);
                if (a && pkg_vercmp(a->version, vers.v[i]) > 0) {
                    printf("   %s%-30s%s %s%s%s -> %s%s%s\n", C_BOLD, a->name, C_END,
                           C_RED, vers.v[i], C_END, C_GREEN, a->version, C_END);
                    sl_push(&upd, a->name);
                }
            }
            rpc_result_free(&res);
        }
        sl_free(&batch);
    }
    sl_free(&names);
    sl_free(&vers);

    if (rc == 0 && upd.n == 0)
        printf(" there is nothing to do\n");
    else if (rc == 0 && ask_yesno("Upgrade these AUR packages?", true, o->noconfirm)) {
        Options o2 = *o;
        o2.needed = false;
        Plan p;
        plan_init(&p, &o2);
        int unresolved = 0;
        for (size_t i = 0; i < upd.n; i++) {
            size_t before = p.n;
            if (resolve(&p, upd.v[i], true, 0) != 0) {
                /* drop whatever this target pulled in and continue */
                while (p.n > before)
                    node_free(&p.v[--p.n]);
                printf("%s::%s %s cannot be upgraded right now; continuing with the others\n",
                       C_YEL, C_END, upd.v[i]);
                unresolved++;
            }
        }
        rc = run_plan(&p);
        if (unresolved)
            rc = 1;
        plan_free(&p);
    }
    sl_free(&upd);
    return rc;
}

static int clean_build_files(const char *dir)
{
    int rc = 0;
    char path[PATH_BUF];
    if (path_join(path, sizeof path, dir, "src") && rmtree(path) != 0)
        rc = -1;
    if (path_join(path, sizeof path, dir, "pkg") && rmtree(path) != 0)
        rc = -1;
    DIR *d = opendir(dir);
    if (d == NULL)
        return -1;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strstr(de->d_name, ".pkg.tar") == NULL && strstr(de->d_name, ".src.tar") == NULL)
            continue;
        if (path_join(path, sizeof path, dir, de->d_name) && is_file_nofollow(path) &&
            unlink(path) != 0)
            rc = -1;
    }
    closedir(d);
    return rc;
}

int cmd_clean(const Options *o, bool all)
{
    if (all && !ask_yesno("Remove ALL cloned AUR repositories (and your review history)?",
                          false, o->noconfirm))
        return 1;
    DIR *d = opendir(o->cache);
    if (d == NULL)
        return 1;
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!valid_pkgname(de->d_name))
            continue;   /* skips ".", "..", ".lock", review temp files */
        char path[PATH_BUF];
        if (!path_join(path, sizeof path, o->cache, de->d_name) || !is_dir_nofollow(path))
            continue;
        printf("cleaning %s\n", de->d_name);
        if ((all ? rmtree(path) : clean_build_files(path)) != 0)
            rc = 1;
    }
    closedir(d);
    return rc;
}

int cmd_audit_dir(const char *dir)
{
    char real[PATH_MAX];
    if (realpath(dir, real) == NULL || !is_dir_nofollow(real)) {
        fprintf(stderr, "error: %s is not a directory\n", dir);
        return 4;
    }
    char gitdir[PATH_BUF];
    bool is_git = path_join(gitdir, sizeof gitdir, real, ".git") && is_dir_nofollow(gitdir);
    Report rep;
    report_init(&rep);
    audit_repo(real, is_git, &rep);
    printf("%s:: Audit of %s%s\n", C_BOLD, real, C_END);
    report_print(&rep, stdout, use_color());
    printf("\nhighest severity: %s (%zu findings)\n", severity_name(rep.max), rep.n);
    printf("This is a reading aid, not a verdict: read PKGBUILD and *.install in full.\n");
    int r = (int)rep.max;
    report_free(&rep);
    return r;
}
