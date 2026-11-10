#include "install.h"
#include "integrity.h"
#include "alloc.h"
#include "audit.h"
#include "config.h"
#include "fsutil.h"
#include "git.h"
#include "mirrors.h"
#include "pacman.h"
#include "perf.h"
#include "proc.h"
#include "review.h"
#include "rpc.h"
#include "sha256.h"
#include "srcinfo.h"
#include "tty.h"
#include "validate.h"
#include "vercmp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
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
    StrList built;         /* staged package files to install (private copies) */
    StrList sha;           /* SHA-256 of each built[] file, taken when it was staged */
    StrList built_names;   /* pkgname of each built[] file */
    AurMeta meta;          /* AUR trust signals */
    bool running;          /* parallel build in progress */
    pid_t pid;
    time_t started;
    char log[PATH_BUF];
    /* progress of a background build, read from its log */
    off_t log_off;
    const char *stage;     /* plain-language stage, NULL before the first one */
    int percent;           /* -1 unknown */
    char detail[96];       /* file being downloaded */
    char carry[512];       /* unfinished log line */
    size_t ncarry;
} Node;

typedef struct {
    Node *v;
    size_t n, cap;
    StrList visited;       /* dependency names already handled */
    StrList in_progress;   /* bases being resolved (cycle guard) */
    StrList dep_ok;        /* dependency strings already satisfied or in the repos */
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
    sl_free(&n->sha);
    sl_free(&n->built_names);
    srcinfo_free(&n->si);
}

/* <cache>/.staged: audited, hash-pinned copies of the built packages. */
static bool staging_root(const char *cache, char *buf, size_t sz)
{
    return cache && path_join(buf, sz, cache, ".staged");
}

static void plan_free(Plan *p)
{
    char st[PATH_BUF];
    if (p->n && staging_root(p->o->cache, st, sizeof st))
        rmtree(st);
    for (size_t i = 0; i < p->n; i++)
        node_free(&p->v[i]);
    free(p->v);
    sl_free(&p->visited);
    sl_free(&p->in_progress);
    sl_free(&p->dep_ok);
}

static Node *plan_find_base(Plan *p, const char *base)
{
    for (size_t i = 0; i < p->n; i++)
        if (strcmp(p->v[i].base, base) == 0)
            return &p->v[i];
    return NULL;
}

/* ------------------------------------------------------------ repo */

static bool repo_reusable(const char *dir, const char *url)
{
    char gitdir[PATH_BUF];
    if (!path_join(gitdir, sizeof gitdir, dir, ".git") || !is_dir_nofollow(dir) ||
        !is_dir_nofollow(gitdir))
        return false;
    StrBuf origin;
    sb_init(&origin, 4096);
    bool reuse = git_origin_url(dir, &origin) == 0 && strcmp(origin.data, url) == 0;
    sb_free(&origin);
    return reuse;
}

/*
 * Clone/fetch several AUR repositories at once (FetchJobs at a time).
 * Successful ones are recorded in o->fetched so prepare_repo() does not hit
 * the network again; failures are simply retried there, serially, with the
 * error visible.
 */
static void prefetch_bases(const Options *o, const StrList *bases, bool quiet)
{
    if (o->fetch_jobs <= 1 || o->fetched == NULL || o->cache == NULL)
        return;
    StrList todo;
    sl_init(&todo);
    for (size_t i = 0; i < bases->n; i++)
        if (valid_pkgname(bases->v[i]) && !sl_contains(o->fetched, bases->v[i]))
            sl_push_unique(&todo, bases->v[i]);
    if (todo.n < 2) {
        sl_free(&todo);
        return;
    }
    size_t jobs = (size_t)o->fetch_jobs;
    if (!quiet)
        printf(":: Fetching %zu AUR repositories (%zu at a time)...\n", todo.n,
               jobs < todo.n ? jobs : todo.n);
    struct { pid_t pid; size_t idx; } run[PERF_MAX_JOBS];
    size_t nrun = 0, next = 0, ok = 0;
    while (next < todo.n || nrun) {
        while (nrun < jobs && next < todo.n) {
            size_t i = next++;
            char dir[PATH_BUF], url[PATH_BUF];
            if (!path_join(dir, sizeof dir, o->cache, todo.v[i]))
                continue;
            snprintf(url, sizeof url, AUR_GIT_FMT, todo.v[i]);
            pid_t pid;
            if (repo_reusable(dir, url))
                pid = git_spawn_fetch(dir);
            else
                pid = rmtree(dir) == 0 ? git_spawn_clone(url, dir) : -1;
            if (pid > 0) {
                run[nrun].pid = pid;
                run[nrun].idx = i;
                nrun++;
            }
        }
        if (nrun == 0)
            break;
        int rc = -1;
        pid_t set[PERF_MAX_JOBS];
        for (size_t k = 0; k < nrun; k++)
            set[k] = run[k].pid;
        pid_t w = proc_wait_set(set, nrun, &rc);
        if (w < 0)
            break;
        for (size_t k = 0; k < nrun; k++) {
            if (run[k].pid != w)
                continue;
            if (rc == 0) {
                sl_push(o->fetched, todo.v[run[k].idx]);
                ok++;
            }
            run[k] = run[--nrun];
            break;
        }
    }
    if (!quiet)
        printf("   %zu/%zu fetched\n", ok, todo.n);
    sl_free(&todo);
}

/* AUR bases of the given exact package names (one RPC request per batch). */
static void bases_of(const StrList *names, StrList *bases)
{
    for (size_t off = 0; off < names->n; off += RPC_BATCH) {
        StrList batch;
        sl_init(&batch);
        for (size_t i = off; i < names->n && i < off + RPC_BATCH; i++)
            sl_push(&batch, names->v[i]);
        RpcResult res;
        if (rpc_info(&batch, &res) == 0) {
            for (size_t i = 0; i < res.n; i++)
                sl_push_unique(bases, res.pkgs[i].base);
            rpc_result_free(&res);
        }
        sl_free(&batch);
    }
}

static int prepare_repo(const Options *o, Node *n)
{
    if (!path_join(n->dir, sizeof n->dir, o->cache, n->base))
        return -1;
    char url[PATH_BUF];
    snprintf(url, sizeof url, AUR_GIT_FMT, n->base);

    bool reuse = repo_reusable(n->dir, url);
    if (reuse && o->fetched && sl_contains(o->fetched, n->base)) {
        /* already fetched in parallel during this run */
    } else if (reuse) {
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
    if (p->o->fetch_jobs > 1) {
        /* fetch this node's AUR dependencies all at once before descending */
        StrList cand;
        sl_init(&cand);
        for (size_t k = 0; keys[k]; k++) {
            StrList deps;
            sl_init(&deps);
            srcinfo_values(&n->si, keys[k], p->arch, &deps);
            for (size_t i = 0; i < deps.n; i++) {
                char nm[MAX_NAME_LEN + 1];
                if (!valid_depstring(deps.v[i], nm, sizeof nm) || sl_contains(&p->visited, nm) ||
                    sl_contains(&p->dep_ok, deps.v[i]))
                    continue;
                if (pacman_dep_satisfied(deps.v[i]) || pacman_dep_in_repos(deps.v[i]))
                    sl_push(&p->dep_ok, deps.v[i]);
                else
                    sl_push_unique(&cand, nm);
            }
            sl_free(&deps);
        }
        if (cand.n > 1) {
            StrList bases;
            sl_init(&bases);
            bases_of(&cand, &bases);
            prefetch_bases(p->o, &bases, false);
            sl_free(&bases);
        }
        sl_free(&cand);
    }
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
        if (sl_contains(&p->dep_ok, depstr))
            return 0;
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
    sl_init(&n.sha);
    sl_init(&n.built_names);
    sl_push(&n.want, pkg->name);
    n.meta.known = true;
    n.meta.votes = pkg->votes;
    n.meta.popularity = pkg->popularity;
    n.meta.first_submitted = pkg->first_submitted;
    n.meta.last_modified = pkg->last_modified;
    n.meta.outofdate = pkg->outofdate;
    n.meta.orphan = pkg->orphan;
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

/* Checks that need the terminal: exact commit, PGP keys, .SRCINFO consistency. */
static int build_prepare(Node *n)
{
    if (git_checkout_exact(n->dir, n->commit) != 0) {
        fprintf(stderr, "error: %s: worktree does not match the reviewed commit\n", n->base);
        return -1;
    }
    if (check_pgp_keys(n) != 0 || check_srcinfo_consistency(n) != 0)
        return -1;
    return 0;
}

/* After a successful build: find the package files and audit them. */
static int build_collect(Plan *p, Node *n)
{
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

    /* Stage a private copy of every package, hashing it while copying. The
     * copy is what gets audited, and pacman -U installs it only if its hash
     * still matches, so nothing can be swapped in after the audit. */
    char root[PATH_BUF], stage[PATH_BUF];
    if (!staging_root(p->o->cache, root, sizeof root) || ensure_private_dir(root) != 0 ||
        !path_join(stage, sizeof stage, root, n->base) || rmtree(stage) != 0 ||
        ensure_private_dir(stage) != 0) {
        fprintf(stderr, "error: %s: could not create the staging directory\n", n->base);
        sl_free(&files);
        return -1;
    }
    sl_free(&n->built);
    sl_free(&n->sha);
    sl_free(&n->built_names);
    StrList names;
    sl_init(&names);
    for (size_t i = 0; i < files.n; i++) {
        char *pn = pkgname_from_file(files.v[i]);
        if (pn && sl_contains(&n->want, pn) && files.v[i][0] == '/' &&
            is_file_nofollow(files.v[i])) {
            const char *b = strrchr(files.v[i], '/') + 1;
            char dst[PATH_BUF], hex[SHA256_HEX_LEN + 1];
            if (!path_join(dst, sizeof dst, stage, b) || sha256_copy(files.v[i], dst, hex) != 0) {
                fprintf(stderr, "error: %s: could not copy %s for verification\n", n->base, b);
                free(pn);
                sl_free(&files);
                sl_free(&names);
                return -1;
            }
            sl_push(&n->built, dst);
            sl_push(&n->sha, hex);
            sl_push(&names, pn);
            sl_push(&n->built_names, pn);
        }
        free(pn);
    }
    sl_free(&files);
    if (n->built.n == 0) {
        fprintf(stderr, "error: %s: no built package found for the requested names\n", n->base);
        sl_free(&names);
        return -1;
    }

    Report rep;
    report_init(&rep);
    for (size_t i = 0; i < n->built.n; i++)
        audit_built_package(n->built.v[i], names.v[i], n->base, &rep);
    sl_free(&names);
    printf("\n%s:: Checking the built package of %s before installing%s\n", C_BOLD, n->base,
           C_END);
    for (size_t i = 0; i < n->built.n; i++) {
        printf("   %s\n      SHA-256 %s\n", strrchr(n->built.v[i], '/') + 1, n->sha.v[i]);
        syslog(LOG_AUTHPRIV | LOG_NOTICE, "built pkgbase=%s commit=%s file=%s sha256=%s",
               n->base, n->commit, strrchr(n->built.v[i], '/') + 1, n->sha.v[i]);
    }
    printf("   %s%s%s\n", rep.max >= SEV_HIGH ? C_RED : rep.max == SEV_WARN ? C_YEL : C_GREEN,
           verdict_title(audit_verdict(&rep)), C_END);
    report_print_plain(&rep, stdout, use_color());
    Decision d = DEC_APPROVE;
    if (rep.max >= SEV_HIGH) {
        printf("\n%s:: Technical details%s\n", C_BOLD, C_END);
        report_print(&rep, stdout, use_color());
        d = ask_decision(n->base, "built package: install it?", rep.max, n->installed, NULL);
    }
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

static int build_node(Plan *p, Node *n)
{
    const Options *o = p->o;
    printf("\n%s:: Building %s @ %.12s%s\n", C_BOLD, n->base, n->commit, C_END);
    if (build_prepare(n) != 0)
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
    return build_collect(p, n);
}

/* The staged files must still be exactly what was audited. */
static int verify_staged(const Node *n)
{
    if (n->built.n == 0 || n->sha.n != n->built.n)
        return -1;
    for (size_t i = 0; i < n->built.n; i++) {
        char hex[SHA256_HEX_LEN + 1];
        if (sha256_file(n->built.v[i], hex) != 0 || strcmp(hex, n->sha.v[i]) != 0) {
            fprintf(stderr, "%serror: %s CHANGED after it was checked (SHA-256 mismatch).%s\n"
                            "   Nothing was installed. Another program modified the file; "
                            "this can be an attack.\n",
                    C_RED, strrchr(n->built.v[i], '/') + 1, C_END);
            syslog(LOG_AUTHPRIV | LOG_WARNING, "SHA-256 MISMATCH pkgbase=%s file=%s expected=%s",
                   n->base, n->built.v[i], n->sha.v[i]);
            return -1;
        }
    }
    printf("   %sverified%s: SHA-256 of %zu package file%s matches what was checked\n",
           C_GREEN, C_END, n->built.n, n->built.n == 1 ? "" : "s");
    return 0;
}

static int install_node(const Options *o, const Node *n)
{
    if (verify_staged(n) != 0)
        return -1;
    StrList args;
    sl_init(&args);
    sl_push(&args, "--upgrade");
    sl_extend(&args, o->common);
    sl_extend(&args, o->xact);
    if (n->asdeps || o->asdeps)
        sl_push(&args, "--asdeps");
    else if (o->asexplicit)
        sl_push(&args, "--asexplicit");
    int rc = pacman_run(true, &args, &n->built);
    sl_free(&args);
    if (rc == 0) {
        for (size_t i = 0; i < n->built.n; i++) {
            syslog(LOG_AUTHPRIV | LOG_NOTICE, "installed pkgbase=%s commit=%s file=%s sha256=%s",
                   n->base, n->commit, strrchr(n->built.v[i], '/') + 1, n->sha.v[i]);
            /* record for inkaur --integrity */
            InstallRecord rec;
            memset(&rec, 0, sizeof rec);
            char *ver = i < n->built_names.n ? pacman_installed_version(n->built_names.v[i]) : NULL;
            if (ver) {
                snprintf(rec.name, sizeof rec.name, "%s", n->built_names.v[i]);
                snprintf(rec.base, sizeof rec.base, "%s", n->base);
                snprintf(rec.version, sizeof rec.version, "%s", ver);
                snprintf(rec.commit, sizeof rec.commit, "%s", n->commit);
                snprintf(rec.sha256, sizeof rec.sha256, "%s", n->sha.v[i]);
                rec.installed = (long)time(NULL);
                if (integrity_record(&rec) != 0)
                    fprintf(stderr, "warning: could not save the install record of %s\n",
                            rec.name);
            }
            free(ver);
        }
        for (size_t i = 0; o->touched && i < n->want.n; i++)
            sl_push_unique(o->touched, n->want.v[i]);
    }
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
    StrList args, names;
    sl_init(&args);
    sl_init(&names);
    for (size_t i = 0; i < n->want.n; i++) {
        char *v = pacman_installed_version(n->want.v[i]);
        if (v)
            sl_push(&names, n->want.v[i]);
        free(v);
    }
    int rc = 0;
    if (names.n) {
        sl_push(&args, "--remove");
        sl_extend(&args, o->common);
        rc = pacman_run(true, &args, &names) == 0 ? 0 : -1;
    }
    sl_free(&args);
    sl_free(&names);
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

static int print_summary(const Plan *p, const char *aborted)
{
    static const char *const names[] = { "not built", "skipped", "removed", "FAILED", "installed" };
    int failed = 0;
    printf("\n%s:: AUR build summary%s\n", C_BOLD, C_END);
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
        printf("   %s; pending packages were not built\n", aborted);
    return (failed || aborted) ? 1 : 0;
}

/* ------------------------------------------------------------ parallel builds */

static void print_log_tail(const char *path, size_t lines)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return;
    struct stat st;
    char buf[16384];
    ssize_t r = 0;
    if (fstat(fd, &st) == 0) {
        off_t from = st.st_size > (off_t)sizeof buf - 1 ? st.st_size - (off_t)sizeof buf + 1 : 0;
        if (lseek(fd, from, SEEK_SET) >= 0)
            r = read(fd, buf, sizeof buf - 1);
    }
    close(fd);
    if (r <= 0)
        return;
    size_t n = (size_t)r, seen = 0, start = 0;
    for (size_t i = n; i > 0; i--)
        if (buf[i - 1] == '\n' && i != n && ++seen == lines) {
            start = i;
            break;
        }
    StrBuf out;
    sb_init(&out, sizeof buf * 8);
    sanitize_append(&out, buf + start, n - start, true);
    fputs(out.data, stderr);
    if (out.len && out.data[out.len - 1] != '\n')
        fputc('\n', stderr);
    sb_free(&out);
}

/* Every plan node that a needs is finished (installed, or left out). */
static bool node_ready(const Plan *p, size_t a)
{
    for (size_t b = 0; b < p->n; b++)
        if (b != a && p->v[b].state == ST_PENDING && node_needs(p, &p->v[a], &p->v[b]))
            return false;
    return true;
}

static bool provided_by_plan(const Plan *p, const char *name)
{
    for (size_t i = 0; i < p->n; i++)
        if (sl_contains(&p->v[i].want, name) || sl_contains(&p->v[i].aliases, name))
            return true;
    return false;
}

/* makepkg --syncdeps cannot run concurrently (pacman lock, prompts), so the
 * repository dependencies of all pending builds are installed up front. */
static int install_repo_deps(Plan *p)
{
    static const char *const keys[] = { "depends", "makedepends", "checkdepends", NULL };
    StrList need;
    sl_init(&need);
    for (size_t i = 0; i < p->n; i++) {
        if (p->v[i].state != ST_PENDING)
            continue;
        for (size_t k = 0; keys[k]; k++) {
            StrList deps;
            sl_init(&deps);
            srcinfo_values(&p->v[i].si, keys[k], p->arch, &deps);
            for (size_t j = 0; j < deps.n; j++) {
                char nm[MAX_NAME_LEN + 1];
                if (valid_depstring(deps.v[j], nm, sizeof nm) && !provided_by_plan(p, nm) &&
                    !sl_contains(&need, deps.v[j]) && !pacman_dep_satisfied(deps.v[j]))
                    sl_push(&need, deps.v[j]);
            }
            sl_free(&deps);
        }
    }
    int rc = 0;
    if (need.n) {
        printf("\n%s:: Installing %zu repository dependencies for the parallel builds%s\n",
               C_BOLD, need.n, C_END);
        StrList args;
        sl_init(&args);
        sl_push(&args, "--sync");
        sl_push(&args, "--asdeps");
        sl_push(&args, "--needed");
        sl_extend(&args, p->o->common);
        rc = pacman_run(true, &args, &need) == 0 ? 0 : -1;
        sl_free(&args);
    }
    sl_free(&need);
    return rc;
}

static pid_t spawn_build(const Plan *p, Node *n)
{
    char logdir[PATH_BUF];
    if (!path_join(logdir, sizeof logdir, p->o->cache, ".logs") || ensure_private_dir(logdir) != 0)
        return -1;
    char fname[MAX_NAME_LEN + 8];
    snprintf(fname, sizeof fname, "%s.log", n->base);
    if (!path_join(n->log, sizeof n->log, logdir, fname))
        return -1;
    int fd = open(n->log, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    const char *argv[] = { BIN_MAKEPKG, "--cleanbuild", "--clean", "--noprogressbar",
                           p->o->noconfirm ? "--noconfirm" : NULL, NULL };
    ProcOpts po;
    memset(&po, 0, sizeof po);
    po.cwd = n->dir;
    pid_t pid = proc_spawn(argv, &po, fd);
    close(fd);
    return pid;
}

static void stop_builds(Plan *p)
{
    for (size_t i = 0; i < p->n; i++)
        if (p->v[i].running)
            kill(p->v[i].pid, SIGTERM);
    for (size_t i = 0; i < p->n; i++)
        if (p->v[i].running) {
            proc_wait_pid(p->v[i].pid);
            p->v[i].running = false;
        }
}

static void fmt_elapsed(char *buf, size_t sz, time_t secs)
{
    if (secs >= 60)
        snprintf(buf, sz, "%ldm%02lds", (long)secs / 60, (long)secs % 60);
    else
        snprintf(buf, sz, "%lds", (long)secs);
}

/* ------------------------------------------------------------ build progress */

/*
 * What each background build is doing, in plain words, read from the
 * "==> " stage messages makepkg writes to the log. The log is read
 * incrementally (only the new bytes on every tick).
 */
static const struct {
    const char *key;
    const char *stage;
} stages[] = {
    { "Making package", "starting" },
    { "Checking runtime dependencies", "checking dependencies" },
    { "Checking buildtime dependencies", "checking dependencies" },
    { "Retrieving sources", "downloading the source code" },
    { "Validating source files", "checking download fingerprints" },
    { "Verifying source file signatures", "checking signatures" },
    { "Extracting sources", "unpacking the source code" },
    { "Removing existing $srcdir", "unpacking the source code" },
    { "Starting prepare()", "preparing" },
    { "Starting pkgver()", "detecting the version" },
    { "Starting build()", "compiling" },
    { "Starting check()", "running the package's tests" },
    { "Entering fake root", "packaging" },
    { "Starting package()", "packaging" },
    { "Tidying install", "packaging" },
    { "Checking for packaging issues", "packaging" },
    { "Creating package", "creating the package file" },
    { "Leaving fake root", "creating the package file" },
    { "Finished making", "finished, checking the result" },
};

/* Drops "ESC [ ... letter" color codes, turns tabs into spaces. */
static size_t strip_ansi(const char *in, size_t n, char *out, size_t outsz)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 1 < outsz; i++) {
        if (in[i] == '\033') {
            if (i + 1 < n && in[i + 1] == '[') {
                i += 2;
                while (i < n && !((in[i] >= 'A' && in[i] <= 'Z') || (in[i] >= 'a' && in[i] <= 'z')))
                    i++;
            }
            continue;
        }
        out[k++] = in[i] == '\t' ? ' ' : in[i];
    }
    out[k] = '\0';
    return k;
}

/* Percentage shown by common tools on a progress line, or -1:
 * cmake/make "[ 47%]", ninja "[123/456]", curl " 70  542.7k  70 ...". */
static int line_percent(const char *l, bool downloading)
{
    while (*l == ' ')
        l++;
    char *e;
    if (*l == '[') {
        long a = strtol(l + 1 + strspn(l + 1, " "), &e, 10);
        if (e[0] == '%' && e[1] == ']' && a >= 0 && a <= 100)
            return (int)a;
        if (e[0] == '/') {
            long b = strtol(e + 1, &e, 10);
            if (e[0] == ']' && b > 0 && a >= 0 && a <= b)
                return (int)(a * 100 / b);
        }
        return -1;
    }
    if (downloading && *l >= '0' && *l <= '9') {
        long a = strtol(l, &e, 10);
        if (*e == ' ' && a >= 0 && a <= 100 && strchr(e, ':') != NULL)
            return (int)a;   /* curl's progress meter (has a time column) */
    }
    return -1;
}

static void progress_line(Node *n, const char *line)
{
    const char *m = strstr(line, "==> ");
    if (m) {
        m += 4;
        if (strncmp(m, "ERROR", 5) == 0 || strncmp(m, "WARNING", 7) == 0)
            return;
        for (size_t i = 0; i < sizeof stages / sizeof stages[0]; i++)
            if (strncmp(m, stages[i].key, strlen(stages[i].key)) == 0) {
                n->stage = stages[i].stage;
                n->percent = -1;
                n->detail[0] = '\0';
                return;
            }
        return;
    }
    const char *d = strstr(line, "-> Downloading ");
    if (d) {   /* "  -> Downloading foo-1.0.tar.gz..." */
        d += 15;
        size_t len = strcspn(d, " ");
        while (len && d[len - 1] == '.')
            len--;
        StrBuf b;
        sb_init(&b, 0);
        sanitize_append(&b, d, len, false);
        snprintf(n->detail, sizeof n->detail, "%s", b.data);
        sb_free(&b);
        n->percent = -1;
        return;
    }
    int pc = line_percent(line, n->stage && strstr(n->stage, "downloading") != NULL);
    if (pc >= 0)
        n->percent = pc;
}

/* Reads what the build wrote since the last tick and updates its stage. */
static void progress_read(Node *n)
{
    int fd = open(n->log, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
        return;
    if (lseek(fd, n->log_off, SEEK_SET) < 0) {
        close(fd);
        return;
    }
    char buf[65536];
    for (int rounds = 0; rounds < 64; rounds++) {   /* at most 4 MiB per tick */
        ssize_t r = read(fd, buf, sizeof buf);
        if (r <= 0)
            break;
        n->log_off += r;
        for (ssize_t i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {   /* curl redraws its meter with \r */
                char clean[sizeof n->carry];
                strip_ansi(n->carry, n->ncarry, clean, sizeof clean);
                if (clean[0])
                    progress_line(n, clean);
                n->ncarry = 0;
            } else if (n->ncarry < sizeof n->carry - 1) {
                n->carry[n->ncarry++] = c;
            }
        }
    }
    close(fd);
    /* an unfinished line can already hold a progress meter */
    if (n->ncarry) {
        char clean[sizeof n->carry];
        strip_ansi(n->carry, n->ncarry, clean, sizeof clean);
        int pc = line_percent(clean, n->stage && strstr(n->stage, "downloading") != NULL);
        if (pc >= 0)
            n->percent = pc;
    }
}

/* Appends s to out using at most *room terminal columns (UTF-8 aware). */
static void append_cols(StrBuf *out, const char *s, size_t *room)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) {   /* start of a character: costs one column */
            if (*room == 0)
                break;
            (*room)--;
        }
        sb_putc(out, (char)*p);
    }
}

/*
 * Live progress while builds run in the background. On a terminal a small
 * block is redrawn every second: a header and one line per build with its
 * stage in plain words, a percentage when the tools print one, and the
 * elapsed time. Without a terminal the same lines are printed every minute.
 */
typedef struct {
    bool tty;
    size_t lines;      /* lines of the block currently on screen */
    unsigned tick;
    time_t last_plain;
} Progress;

static void progress_clear(Progress *g)
{
    if (g->lines) {
        printf("\033[%zuA\r\033[J", g->lines);
        fflush(stdout);
        g->lines = 0;
    }
}

static void progress_show(Progress *g, Plan *p, size_t done, size_t total)
{
    time_t now = time(NULL);
    for (size_t i = 0; i < p->n; i++)
        if (p->v[i].running)
            progress_read(&p->v[i]);
    if (!g->tty) {
        if (now - g->last_plain < 60)
            return;
        g->last_plain = now;
    }
    size_t nrun = 0, waiting = 0, width = 0;
    for (size_t i = 0; i < p->n; i++) {
        const Node *n = &p->v[i];
        if (n->running) {
            nrun++;
            if (strlen(n->base) > width)
                width = strlen(n->base);
        } else if (n->state == ST_PENDING) {
            waiting++;
        }
    }
    if (nrun == 0)
        return;
    if (width > 32)
        width = 32;

    StrList out;
    sl_init(&out);
    static const char spin[] = "|/-\\";
    StrBuf l;
    sb_init(&l, 0);
    sb_printf(&l, "   %c %zu of %zu finished, %zu building now", g->tty ? spin[g->tick++ % 4] : '*',
              done, total, nrun);
    if (waiting)
        sb_printf(&l, ", %zu waiting to start", waiting);
    sl_push(&out, l.data);
    for (size_t i = 0; i < p->n; i++) {
        const Node *n = &p->v[i];
        if (!n->running)
            continue;
        char el[32];
        fmt_elapsed(el, sizeof el, now - n->started);
        sb_reset(&l);
        sb_printf(&l, "     %-*.*s  %-7s %s", (int)width, (int)width, n->base, el,
                  n->stage ? n->stage : "starting");
        if (n->percent >= 0)
            sb_printf(&l, " %d%%", n->percent);
        if (n->detail[0] && n->stage && strstr(n->stage, "downloading"))
            sb_printf(&l, " (%s)", n->detail);
        sl_push(&out, l.data);
    }
    sb_free(&l);

    if (g->tty) {
        progress_clear(g);
        size_t cols = tty_columns();
        for (size_t i = 0; i < out.n; i++) {
            size_t room = cols > 1 ? cols - 1 : 1;   /* never wrap: keeps the redraw exact */
            StrBuf cut;
            sb_init(&cut, 0);
            append_cols(&cut, out.v[i], &room);
            printf("%s\n", cut.data);
            sb_free(&cut);
        }
        g->lines = out.n;
    } else {
        for (size_t i = 0; i < out.n; i++)
            printf("%s\n", out.v[i]);
    }
    fflush(stdout);
    sl_free(&out);
}

/*
 * Up to BuildJobs makepkg processes at once. A package starts only when every
 * plan package it depends on is installed, so the dependency order is kept.
 * Builds run without a terminal (output in ~/.cache/inkaur/.logs); everything
 * interactive (key checks, audit decisions, pacman -U) stays in this process,
 * one at a time. Returns 1 if the user aborted.
 */
static int run_parallel(Plan *p)
{
    size_t total = 0;
    for (size_t i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        if (n->state != ST_PENDING)
            continue;
        printf("\n%s:: Preparing %s @ %.12s%s\n", C_BOLD, n->base, n->commit, C_END);
        if (build_prepare(n) != 0) {
            mark(n, ST_FAILED, "pre-build checks failed");
            drop_dependents(p, i);
            continue;
        }
        total++;
    }
    if (total == 0)
        return 0;
    if (install_repo_deps(p) != 0) {
        fprintf(stderr, "error: could not install the repository dependencies\n");
        for (size_t i = 0; i < p->n; i++)
            if (p->v[i].state == ST_PENDING)
                mark(&p->v[i], ST_FAILED, "repository dependencies missing");
        return 0;
    }

    size_t jobs = (size_t)p->o->build_jobs, nrun = 0, done = 0;
    printf("\n%s:: Building %zu AUR packages, up to %zu at a time (dependency order kept)%s\n",
           C_BOLD, total, jobs, C_END);
    printf("   this can take a while; what each build is doing is shown below\n"
           "   (complete output in the log files listed)\n");
    Progress prog;
    memset(&prog, 0, sizeof prog);
    prog.tty = isatty(STDOUT_FILENO) != 0;
    prog.last_plain = time(NULL);
    for (;;) {
        for (size_t i = 0; i < p->n && nrun < jobs; i++) {
            Node *n = &p->v[i];
            if (n->state != ST_PENDING || n->running || !node_ready(p, i))
                continue;
            progress_clear(&prog);
            n->pid = spawn_build(p, n);
            if (n->pid < 0) {
                mark(n, ST_FAILED, "could not start makepkg");
                drop_dependents(p, i);
                continue;
            }
            n->running = true;
            n->started = time(NULL);
            n->log_off = 0;
            n->stage = NULL;
            n->percent = -1;
            n->detail[0] = '\0';
            n->ncarry = 0;
            nrun++;
            printf("   %s->%s building %s  (log: %s)\n", C_BLUE, C_END, n->base, n->log);
        }
        if (nrun == 0)
            break;

        int rc = -1;
        pid_t *set = xcalloc(p->n, sizeof(pid_t));
        size_t ns = 0;
        for (size_t k = 0; k < p->n; k++)
            if (p->v[k].running)
                set[ns++] = p->v[k].pid;
        pid_t w = proc_wait_set_timeout(set, ns, &rc, 1000);
        free(set);
        if (w == 0) {
            progress_show(&prog, p, done, total);
            continue;
        }
        progress_clear(&prog);
        if (w < 0)
            break;
        size_t i = p->n;
        for (size_t k = 0; k < p->n; k++)
            if (p->v[k].running && p->v[k].pid == w)
                i = k;
        if (i == p->n)
            continue;
        Node *n = &p->v[i];
        n->running = false;
        nrun--;
        done++;
        char el[32];
        fmt_elapsed(el, sizeof el, time(NULL) - n->started);
        if (rc != 0) {
            printf("   %s[%zu/%zu] FAILED%s %s after %s (exit %d); last lines of the log:\n",
                   C_RED, done, total, C_END, n->base, el, rc);
            print_log_tail(n->log, 25);
            mark(n, ST_FAILED, "build failed");
            drop_dependents(p, i);
            continue;
        }
        printf("   %s[%zu/%zu] built%s %s in %s\n", C_GREEN, done, total, C_END, n->base, el);
        int r = build_collect(p, n);
        if (r == BUILD_ABORT) {
            stop_builds(p);
            return 1;
        }
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
    for (size_t i = 0; i < p->n; i++)
        if (p->v[i].state == ST_PENDING)
            mark(&p->v[i], ST_FAILED, "could not be scheduled");
    return 0;
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
        audit_aur_meta(&rep, &n->meta, (long)time(NULL));
        ReviewCtx ctx = { n->dir, n->base, n->commit, p->o->cache, n->installed,
                          p->o->full_review, &n->meta };
        Decision d = review_package(&ctx, &rep);
        report_free(&rep);
        if (d == DEC_ABORT)
            return print_summary(p, "aborted by you");
        if (d == DEC_SKIP || d == DEC_REMOVE)
            apply_leave_out(p, i, d == DEC_REMOVE);
    }

    /* Phase 2: build, audit the result, install. Dependencies come first. */
    size_t pending = 0;
    for (size_t i = 0; i < p->n; i++)
        pending += p->v[i].state == ST_PENDING;
    if (pending && p->o->before_builds && p->o->before_builds(p->o->hook_ctx) != 0)
        return print_summary(p, "the repository upgrade did not complete, so nothing was "
                                "built against a partially upgraded system");
    if (p->o->build_jobs > 1 && pending > 1 && p->o->chroot == NULL)
        return print_summary(p, run_parallel(p) != 0 ? "aborted by you" : NULL);
    for (size_t i = 0; i < p->n; i++) {
        Node *n = &p->v[i];
        if (n->state != ST_PENDING)
            continue;
        int r = build_node(p, n);
        if (r == BUILD_ABORT)
            return print_summary(p, "aborted by you");
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
    return print_summary(p, NULL);
}

static void plan_init(Plan *p, const Options *o)
{
    memset(p, 0, sizeof *p);
    sl_init(&p->visited);
    sl_init(&p->in_progress);
    sl_init(&p->dep_ok);
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
    if (o->perf_note)
        printf(":: Parallel: %d fetch / %d build jobs (%s)\n", o->fetch_jobs, o->build_jobs,
               o->perf_note);
    StrList bases;
    sl_init(&bases);
    bases_of(aur, &bases);
    prefetch_bases(o, &bases, false);
    sl_free(&bases);
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

/* "name", "repo/name" or "aur/name"; on success *name points at the bare name. */
static bool split_target(const char *t, char *repo, size_t rsz, const char **name)
{
    const char *sl = strchr(t, '/');
    repo[0] = '\0';
    *name = t;
    if (sl == NULL)
        return valid_pkgname(t);
    size_t n = (size_t)(sl - t);
    if (n == 0 || n >= rsz)
        return false;
    memcpy(repo, t, n);
    repo[n] = '\0';
    *name = sl + 1;
    return valid_pkgname(repo) && valid_pkgname(*name);
}

int cmd_sync(const StrList *targets, const Options *o)
{
    StrList repo, aur;
    sl_init(&repo);
    sl_init(&aur);
    int rc = 0;
    for (size_t i = 0; i < targets->n; i++) {
        const char *t = targets->v[i], *name;
        char rname[MAX_NAME_LEN + 1];
        if (!split_target(t, rname, sizeof rname, &name)) {
            fputs("error: invalid package name: ", stderr);
            safe_fputs(stderr, t);
            fputc('\n', stderr);
            rc = 1;
            goto out;
        }
        bool force_aur = strcmp(rname, "aur") == 0;
        bool force_repo = rname[0] && !force_aur;
        if ((force_aur && o->repo_only) || (force_repo && o->aur_only)) {
            fputs("error: target conflicts with --aur/--repo: ", stderr);
            safe_fputs(stderr, t);
            fputc('\n', stderr);
            rc = 1;
            goto out;
        }
        if (force_repo || o->repo_only || (!force_aur && !o->aur_only && pacman_dep_in_repos(name)))
            sl_push_unique(&repo, t);
        else
            sl_push_unique(&aur, name);
    }

    if (repo.n) {
        StrList args;
        sl_init(&args);
        sl_push(&args, "--sync");
        sl_extend(&args, o->common);
        sl_extend(&args, o->xact);
        if (o->needed)
            sl_push(&args, "--needed");
        if (o->asdeps)
            sl_push(&args, "--asdeps");
        if (o->asexplicit)
            sl_push(&args, "--asexplicit");
        rc = pacman_run(true, &args, &repo) == 0 ? 0 : 1;
        sl_free(&args);
        for (size_t i = 0; rc == 0 && o->touched && i < repo.n; i++) {
            const char *sl = strchr(repo.v[i], '/');
            sl_push_unique(o->touched, sl ? sl + 1 : repo.v[i]);
        }
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

static bool is_ignored(const Options *o, const StrList *conf_ignore, const char *name)
{
    return (o->ignore && sl_contains(o->ignore, name)) || sl_contains(conf_ignore, name);
}

/* Foreign packages with a newer AUR version. Prints them unless quiet
 * (then only names). Returns 0, or -1 if the AUR could not be queried. */
static int find_aur_updates(const Options *o, StrList *upd, StrList *bases, bool quiet)
{
    StrList names, vers, conf_ignore;
    sl_init(&names);
    sl_init(&vers);
    sl_init(&conf_ignore);
    if (pacman_foreign(&names, &vers) != 0) {
        fprintf(stderr, "error: could not list foreign packages\n");
        return -1;
    }
    pacman_conf_list(o->config, "IgnorePkg", &conf_ignore);
    if (!quiet)
        printf(":: Checking %zu foreign packages against the AUR...\n", names.n);

    int rc = 0;
    for (size_t off = 0; off < names.n && rc == 0; off += RPC_BATCH) {
        StrList batch;
        sl_init(&batch);
        for (size_t i = off; i < names.n && i < off + RPC_BATCH; i++)
            sl_push(&batch, names.v[i]);
        RpcResult res;
        if (rpc_info(&batch, &res) != 0) {
            rc = -1;
        } else {
            for (size_t i = off; i < names.n && i < off + RPC_BATCH; i++) {
                const AurPkg *a = rpc_find(&res, names.v[i]);
                if (a == NULL || pkg_vercmp(a->version, vers.v[i]) <= 0)
                    continue;
                if (is_ignored(o, &conf_ignore, a->name)) {
                    if (!quiet)
                        printf("%swarning:%s %s: ignoring package upgrade (%s => %s)\n",
                               C_YEL, C_END, a->name, vers.v[i], a->version);
                    continue;
                }
                if (quiet)
                    printf("%s\n", a->name);
                else
                    printf("   %s%-30s%s %s%s%s -> %s%s%s\n", C_BOLD, a->name, C_END,
                           C_RED, vers.v[i], C_END, C_GREEN, a->version, C_END);
                sl_push(upd, a->name);
                if (bases)
                    sl_push_unique(bases, a->base);
            }
            rpc_result_free(&res);
        }
        sl_free(&batch);
    }
    sl_free(&names);
    sl_free(&vers);
    sl_free(&conf_ignore);
    return rc;
}

int cmd_aur_updates(const Options *o, bool quiet)
{
    StrList upd;
    sl_init(&upd);
    int rc = find_aur_updates(o, &upd, NULL, quiet);
    int n = rc == 0 ? (int)upd.n : -1;
    sl_free(&upd);
    return n;
}

static void upgrade_args(const Options *o, StrList *args, size_t refresh)
{
    sl_push(args, "--sync");
    for (size_t i = 0; i < refresh; i++)
        sl_push(args, "--refresh");
    for (size_t i = 0; i < o->upgrade_level; i++)
        sl_push(args, "--sysupgrade");
    sl_extend(args, o->common);
    sl_extend(args, o->xact);
}

static const char *cachedir_option(const Options *o)
{
    for (size_t i = 0; o->common && i < o->common->n; i++)
        if (strncmp(o->common->v[i], "--cachedir=", 11) == 0)
            return o->common->v[i] + 11;
    return NULL;
}

/* State of the overlapped repository upgrade (-Sy, background download, -Su). */
typedef struct {
    const Options *o;
    bool done;
    int rc;
    RepoDownload dl;
} RepoStep;

/* Runs once: finish the background download, then the real pacman -Su. */
static int repo_step_finish(void *vctx)
{
    RepoStep *r = vctx;
    if (r->done)
        return r->rc;
    r->done = true;
    if (r->o->repo_download == 2)
        repo_download_finish(&r->dl);
    pacman_bg_wait();
    printf("\n%s:: Upgrading repository packages...%s\n", C_BOLD, C_END);
    StrList args;
    sl_init(&args);
    upgrade_args(r->o, &args, 0);
    if (r->o->needed)
        sl_push(&args, "--needed");
    r->rc = pacman_run(true, &args, NULL) == 0 ? 0 : 1;
    sl_free(&args);
    return r->rc;
}

/* Starts the background download of the repository upgrade. */
static void repo_step_start(RepoStep *r)
{
    const Options *o = r->o;
    StrList args;
    sl_init(&args);
    upgrade_args(o, &args, 0);
    if (o->repo_download == 2) {
        if (repo_download_start(&r->dl, o->cache, &args, cachedir_option(o), o->mirrors,
                                o->fetch_jobs) == 0 && r->dl.nfiles)
            printf(":: Downloading %zu repository packages (%.1f MiB) in the background, "
                   "split across %d mirrors\n", r->dl.nfiles, r->dl.bytes / 1048576.0,
                   o->mirrors);
    } else {
        char logdir[PATH_BUF], log[PATH_BUF];
        static char logpath[PATH_BUF];
        sl_push(&args, "--downloadonly");
        sl_push(&args, "--noconfirm");
        sl_push(&args, "--noprogressbar");
        if (path_join(logdir, sizeof logdir, o->cache, ".logs") &&
            ensure_private_dir(logdir) == 0 &&
            path_join(log, sizeof log, logdir, "repo-download.log")) {
            int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
            if (fd >= 0) {
                pid_t pid = pacman_spawn_root(&args, fd);
                close(fd);
                if (pid > 0) {
                    snprintf(logpath, sizeof logpath, "%s", log);
                    pacman_bg_set(pid, logpath);
                    printf(":: Downloading the repository upgrade in the background\n");
                }
            }
        }
    }
    sl_free(&args);
}

static int aur_upgrade(const Options *o, RepoStep *step)
{
    if (o->perf_note)
        printf(":: Parallel: %d fetch / %d build jobs (%s)\n", o->fetch_jobs, o->build_jobs,
               o->perf_note);
    StrList upd, bases;
    sl_init(&upd);
    sl_init(&bases);
    int rc = find_aur_updates(o, &upd, &bases, false) == 0 ? 0 : 1;
    if (rc == 0)
        prefetch_bases(o, &bases, false);
    sl_free(&bases);

    if (rc == 0 && upd.n == 0)
        printf(" there is nothing to do\n");
    else if (rc == 0 && ask_yesno("Upgrade these AUR packages?", true, o->noconfirm)) {
        Options o2 = *o;
        o2.needed = false;
        o2.asdeps = o2.asexplicit = false;   /* upgrades keep the install reason */
        if (step) {
            o2.before_builds = repo_step_finish;
            o2.hook_ctx = step;
        }
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

/*
 * -Su / -Syu.
 *
 * Overlapped (RepoDownload = pacman | mirrors): pacman -Sy, then the
 * repository packages are downloaded in the background while the AUR side
 * is looked up, fetched and reviewed; pacman -Su then installs them from the
 * cache right before the first AUR build (AUR packages are never built
 * against a system that is not upgraded yet). Plain: pacman -Syu, then AUR.
 */
int cmd_upgrade(const Options *o)
{
    bool overlap = !o->aur_only && !o->repo_only && o->repo_download > 0 && o->cache;
    if (!overlap) {
        if (!o->aur_only) {
            StrList args;
            sl_init(&args);
            upgrade_args(o, &args, o->refresh_level);
            if (o->needed)
                sl_push(&args, "--needed");
            if (o->refresh_level)
                printf(":: Synchronizing package databases and upgrading repositories...\n");
            else
                printf(":: Upgrading repository packages...\n");
            int r = pacman_run(true, &args, NULL);
            sl_free(&args);
            if (r != 0)
                return 1;
        }
        if (o->repo_only)
            return 0;
        return aur_upgrade(o, NULL);
    }

    if (o->refresh_level) {
        printf(":: Synchronizing package databases...\n");
        StrList args;
        sl_init(&args);
        sl_push(&args, "--sync");
        for (size_t i = 0; i < o->refresh_level; i++)
            sl_push(&args, "--refresh");
        sl_extend(&args, o->common);
        int r = pacman_run(true, &args, NULL);
        sl_free(&args);
        if (r != 0)
            return 1;
    } else if (o->repo_download == 1) {
        pacman_refresh_credentials();   /* the background sudo -n needs them */
    }
    RepoStep step;
    memset(&step, 0, sizeof step);
    step.o = o;
    step.dl.pid = -1;
    repo_step_start(&step);
    int rc = aur_upgrade(o, &step);
    if (repo_step_finish(&step) != 0)
        rc = 1;
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
    if (!o->aur_only) {
        StrList args;
        sl_init(&args);
        sl_push(&args, "--sync");
        sl_push(&args, "--clean");
        if (all)
            sl_push(&args, "--clean");
        sl_extend(&args, o->common);
        int r = pacman_run(true, &args, NULL);
        sl_free(&args);
        if (r != 0)
            return 1;
    }
    if (o->repo_only)
        return 0;
    printf("\n:: AUR build cache: %s\n", o->cache);
    char staged[PATH_BUF];
    if (staging_root(o->cache, staged, sizeof staged))
        rmtree(staged);
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
    Verdict v = audit_verdict(&rep);
    printf("%s:: Audit of %s%s\n", C_BOLD, real, C_END);
    printf("   %s%s%s\n   %s\n\n", v >= VERDICT_DANGER ? C_RED : v == VERDICT_CAUTION ? C_YEL
                                                                                  : C_GREEN,
           verdict_title(v), C_END, verdict_advice(v));
    report_print_plain(&rep, stdout, use_color());
    if (rep.n) {
        printf("\n%s:: Technical details%s\n", C_BOLD, C_END);
        report_print(&rep, stdout, use_color());
    }
    printf("\nhighest severity: %s (%zu findings)\n", severity_name(rep.max), rep.n);
    printf("This is a reading aid, not a verdict: read PKGBUILD and *.install in full.\n");
    int r = (int)rep.max;
    report_free(&rep);
    return r;
}
