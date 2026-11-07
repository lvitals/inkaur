#include "mirrors.h"
#include "alloc.h"
#include "config.h"
#include "fsutil.h"
#include "pacman.h"
#include "proc.h"
#include "tty.h"
#include "validate.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------ root copy */

int root_install(const StrList *srcs, const char *dest, bool dest_is_dir, bool backup)
{
    const char *elev = privilege_elevator();
    if (elev == NULL) {
        fprintf(stderr, "error: sudo or doas (root-owned, setuid) is required\n");
        return -1;
    }
    if (!tool_is_trusted(BIN_INSTALL)) {
        fprintf(stderr, "error: %s is missing or not trusted\n", BIN_INSTALL);
        return -1;
    }
    const char **argv = xcalloc(srcs->n + 16, sizeof(char *));
    size_t k = 0;
    argv[k++] = elev;
    argv[k++] = BIN_INSTALL;
    argv[k++] = "-m";
    argv[k++] = "0644";
    argv[k++] = "-o";
    argv[k++] = "root";
    argv[k++] = "-g";
    argv[k++] = "root";
    if (backup) {
        argv[k++] = "-b";
        argv[k++] = "--suffix=.inkaur-bak";
    }
    if (dest_is_dir) {
        argv[k++] = "-t";
        argv[k++] = dest;
        argv[k++] = "--";
        for (size_t i = 0; i < srcs->n; i++)
            argv[k++] = srcs->v[i];
    } else {
        argv[k++] = "--";
        argv[k++] = srcs->v[0];
        argv[k++] = dest;
    }
    argv[k] = NULL;
    ProcOpts o;
    memset(&o, 0, sizeof o);
    fflush(stdout);
    int rc = proc_run(argv, &o);
    free(argv);
    return rc == 0 ? 0 : -1;
}

/* ------------------------------------------------------------ ParallelDownloads */

long recommended_parallel_downloads(double bps)
{
    double kib = bps / 1024.0;
    if (kib < 128)
        return 1;
    if (kib < 512)
        return 2;
    if (kib < 2048)
        return 3;
    if (kib < 8192)
        return 5;
    return 8;
}

static bool is_parallel_line(const char *l)
{
    while (*l == ' ' || *l == '\t' || *l == '#')
        l++;
    return strncmp(l, "ParallelDownloads", 17) == 0 &&
           (l[17] == ' ' || l[17] == '\t' || l[17] == '=');
}

char *pacman_conf_with_parallel(const char *text, long n)
{
    StrBuf out;
    sb_init(&out, 4u << 20);
    char line[64];
    snprintf(line, sizeof line, "ParallelDownloads = %ld\n", n);
    bool in_options = false, done = false;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) + 1 : strlen(p);
        char cur[1024];
        size_t cl = len < sizeof cur ? len : sizeof cur - 1;
        memcpy(cur, p, cl);
        cur[cl] = '\0';
        const char *t = cur;
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '[') {
            if (in_options && !done) {   /* no line in [options]: add one at its end */
                sb_puts(&out, line);
                done = true;
            }
            in_options = strncmp(t, "[options]", 9) == 0;
        }
        if (in_options && !done && is_parallel_line(cur)) {
            sb_puts(&out, line);
            done = true;
        } else {
            sb_append(&out, p, len);
            if (e == NULL)
                sb_putc(&out, '\n');
        }
        p += len;
    }
    if (!done)
        sb_puts(&out, in_options ? line : "");
    char *r = xstrdup(out.data);
    sb_free(&out);
    return r;
}

int pacman_conf_set_parallel(const char *cache, const char *conf_path, long n)
{
    StrBuf old;
    sb_init(&old, 4u << 20);
    if (read_file_limited(conf_path, 4u << 20, &old) != 0) {
        fprintf(stderr, "error: cannot read %s\n", conf_path);
        sb_free(&old);
        return -1;
    }
    char *neu = pacman_conf_with_parallel(old.data, n);
    int rc = 1;
    if (strcmp(neu, old.data) == 0) {
        printf("   %s already has ParallelDownloads = %ld\n", conf_path, n);
    } else {
        printf("   %s [options]: ParallelDownloads = %ld (backup: %s.inkaur-bak)\n",
               conf_path, n, conf_path);
        if (stdin_is_tty() && ask_yesno("Apply this change?", false, false)) {
            char tmp[PATH_BUF];
            StrList one;
            sl_init(&one);
            if (path_join(tmp, sizeof tmp, cache, ".pacman.conf.new") &&
                write_file_atomic(tmp, neu, strlen(neu)) == 0) {
                sl_push(&one, tmp);
                rc = root_install(&one, conf_path, false, true) == 0 ? 0 : -1;
                unlink(tmp);
                if (rc == 0)
                    printf("   written\n");
            } else {
                rc = -1;
            }
            sl_free(&one);
        }
    }
    free(neu);
    sb_free(&old);
    return rc;
}

/* ------------------------------------------------------------ mirrorlist */

void mirrorlist_servers(const char *text, bool include_commented, StrList *out)
{
    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, text);
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        while (*l == ' ' || *l == '\t')
            l++;
        bool commented = false;
        if (*l == '#') {
            commented = true;
            l++;
            while (*l == ' ' || *l == '\t')
                l++;
        }
        if (commented && !include_commented)
            continue;
        if (strncmp(l, "Server", 6) != 0)
            continue;
        l += 6;
        while (*l == ' ' || *l == '\t')
            l++;
        if (*l != '=')
            continue;
        l++;
        while (*l == ' ' || *l == '\t')
            l++;
        size_t n = strcspn(l, " \t#");
        if (n == 0 || n > 512)
            continue;
        char *url = xstrndup(l, n);
        if (strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0)
            sl_push_unique(out, url);
        free(url);
    }
    sl_free(&lines);
}

#define RANK_HEADER "# Ranked by inkaur"
#define RANK_PREV   "# --- previous entries (commented by inkaur) ---"

char *mirrorlist_rewrite(const char *old_text, const StrList *ranked, const char *date)
{
    StrBuf out;
    sb_init(&out, 8u << 20);
    sb_printf(&out, "%s on %s: fastest up-to-date mirrors first\n", RANK_HEADER, date);
    for (size_t i = 0; i < ranked->n; i++)
        sb_printf(&out, "Server = %s\n", ranked->v[i]);
    sb_printf(&out, "\n%s\n", RANK_PREV);

    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, old_text);
    bool in_ranked = false;
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        if (strncmp(l, RANK_HEADER, strlen(RANK_HEADER)) == 0) {
            in_ranked = true;   /* drop the previous inkaur block */
            continue;
        }
        if (strcmp(l, RANK_PREV) == 0) {
            in_ranked = false;
            continue;
        }
        if (in_ranked)
            continue;
        const char *t = l;
        while (*t == ' ' || *t == '\t')
            t++;
        if (strncmp(t, "Server", 6) == 0)
            sb_printf(&out, "#%s\n", t);
        else
            sb_printf(&out, "%s\n", l);
    }
    sl_free(&lines);
    char *r = xstrdup(out.data);
    sb_free(&out);
    return r;
}

static void subst(const char *tmpl, const char *repo, const char *arch, char *out, size_t sz)
{
    StrBuf b;
    sb_init(&b, sz);
    for (const char *p = tmpl; *p; p++) {
        if (strncmp(p, "$repo", 5) == 0) {
            sb_puts(&b, repo);
            p += 4;
        } else if (strncmp(p, "$arch", 5) == 0) {
            sb_puts(&b, arch);
            p += 4;
        } else {
            sb_putc(&b, *p);
        }
    }
    snprintf(out, sz, "%s", b.data);
    sb_free(&b);
}

/* curl GET; body into cap (if given) or /dev/null; write-out stats into stats. */
static int curl_get(const char *url, const char *maxtime, StrBuf *body, double *time_total,
                    double *speed, double *size)
{
    const char *argv[] = {
        BIN_CURL, "-q", "--silent", "--fail", "--globoff",
        "--proto", "=https,http", "--proto-redir", "=https,http", "--max-redirs", "3",
        "--connect-timeout", "5", "--max-time", maxtime, "--max-filesize", "33554432",
        "--user-agent", "inkaur/" INKAUR_VERSION,
        "--output", body ? "-" : "/dev/null",
        "--write-out", body ? "" : "%{time_total} %{speed_download} %{size_download}",
        "--url", url, NULL
    };
    StrBuf out;
    sb_init(&out, 1u << 16);
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.capture = body ? body : &out;
    o.stdin_null = true;
    o.stderr_null = true;
    int rc = proc_run(argv, &o);
    if (rc == 0 && !body &&
        sscanf(out.data, "%lf %lf %lf", time_total, speed, size) != 3)
        rc = -1;
    sb_free(&out);
    return rc;
}

/* Bytes [0, range_end] of url to /dev/null: time to first byte and speed. */
static int curl_stats(const char *url, const char *maxtime, const char *range, double *ttfb,
                      double *speed, double *size)
{
    const char *argv[] = {
        BIN_CURL, "-q", "--silent", "--fail", "--globoff",
        "--proto", "=https,http", "--proto-redir", "=https,http", "--max-redirs", "3",
        "--connect-timeout", "5", "--max-time", maxtime, "--range", range,
        "--user-agent", "inkaur/" INKAUR_VERSION, "--output", "/dev/null",
        "--write-out", "%{time_starttransfer} %{speed_download} %{size_download}",
        "--url", url, NULL
    };
    StrBuf out;
    sb_init(&out, 4096);
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.capture = &out;
    o.stdin_null = true;
    o.stderr_null = true;
    int rc = proc_run(argv, &o);
    /* 28 = --max-time reached: the transfer was still measured */
    int ok = (rc == 0 || rc == 28) &&
             sscanf(out.data, "%lf %lf %lf", ttfb, speed, size) == 3 && *size > 0;
    sb_free(&out);
    return ok ? 0 : -1;
}

/* "scheme://host/..." -> host */
static void url_host(const char *url, char *out, size_t sz)
{
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    size_t n = strcspn(h, "/");
    snprintf(out, sz, "%.*s", (int)(n < 255 ? n : 255), h);
}

/* One entry per host; https wins over http for the same host (the Arch
 * databases are not signed, so plain http would allow a frozen database). */
static void dedupe_hosts(const StrList *in, StrList *out)
{
    for (size_t i = 0; i < in->n; i++) {
        char hi[256];
        url_host(in->v[i], hi, sizeof hi);
        bool https = strncmp(in->v[i], "https://", 8) == 0, keep = true;
        for (size_t j = 0; j < in->n && keep; j++) {
            char hj[256];
            url_host(in->v[j], hj, sizeof hj);
            if (j == i || strcmp(hi, hj) != 0)
                continue;
            bool other_https = strncmp(in->v[j], "https://", 8) == 0;
            if ((other_https && !https) || (other_https == https && j < i))
                keep = false;
        }
        if (keep)
            sl_push(out, in->v[i]);
    }
}

typedef struct {
    const StrList *servers;
    const char *arch;
} RankCtx;

/* Parallel part (small requests, so they don't skew each other much):
 * latency of core.db and the mirror's lastsync. */
static void rank_job(size_t i, void *vctx, StrBuf *out)
{
    const RankCtx *c = vctx;
    const char *srv = c->servers->v[i];
    char base[1024], url[1100];
    subst(srv, "core", c->arch, base, sizeof base);
    snprintf(url, sizeof url, "%s/core.db", base);
    double ttfb = 0, speed = 0, size = 0;
    if (curl_stats(url, "8", "0-65535", &ttfb, &speed, &size) != 0) {
        sb_puts(out, "fail");
        return;
    }
    long long lastsync = 0;
    const char *r = strstr(srv, "$repo");   /* mirror root = everything before $repo */
    if (r) {
        char root[1024], ls[1100];
        snprintf(root, sizeof root, "%.*s", (int)(r - srv), srv);
        snprintf(ls, sizeof ls, "%slastsync", root);
        StrBuf body;
        sb_init(&body, 64);
        double a, b, d;
        if (curl_get(ls, "5", &body, &a, &b, &d) == 0)
            lastsync = strtoll(body.data, NULL, 10);
        sb_free(&body);
    }
    sb_printf(out, "ok %.4f %lld", ttfb, lastsync);
}

typedef struct {
    const char *url;
    double ttfb, speed;
    long long lastsync;
    bool ok, stale, https, measured;
} RankRow;

static int cmp_latency(const void *a, const void *b)
{
    const RankRow *x = a, *y = b;
    if (x->ok != y->ok)
        return x->ok ? -1 : 1;
    if (x->stale != y->stale)
        return x->stale ? 1 : -1;
    return x->ttfb < y->ttfb ? -1 : x->ttfb > y->ttfb;
}

static int cmp_rank(const void *a, const void *b)
{
    const RankRow *x = a, *y = b;
    if (x->ok != y->ok)
        return x->ok ? -1 : 1;
    if (x->stale != y->stale)
        return x->stale ? 1 : -1;
    if (x->https != y->https)
        return x->https ? -1 : 1;
    if (x->measured != y->measured)
        return x->measured ? -1 : 1;
    if (x->measured && x->speed != y->speed)
        return x->speed > y->speed ? -1 : 1;
    return x->ttfb < y->ttfb ? -1 : x->ttfb > y->ttfb;
}

#define RANK_MEASURE_MAX 15   /* throughput is measured for this many candidates */

static void machine_arch(char *out, size_t sz)
{
    StrList a;
    sl_init(&a);
    pacman_conf_list(NULL, "Architecture", &a);
    struct utsname u;
    if (a.n && strcmp(a.v[0], "auto") != 0 && valid_pkgname(a.v[0]))
        snprintf(out, sz, "%s", a.v[0]);
    else if (uname(&u) == 0 && valid_pkgname(u.machine))
        snprintf(out, sz, "%s", u.machine);
    else
        snprintf(out, sz, "x86_64");
    sl_free(&a);
}

int cmd_rankmirrors(const char *cache, bool all, int count, int jobs, bool noconfirm)
{
    StrBuf text;
    sb_init(&text, 8u << 20);
    if (read_file_limited(MIRRORLIST_PATH, 8u << 20, &text) != 0) {
        fprintf(stderr, "error: cannot read %s\n", MIRRORLIST_PATH);
        sb_free(&text);
        return 1;
    }
    StrList listed, servers;
    sl_init(&listed);
    sl_init(&servers);
    mirrorlist_servers(text.data, all, &listed);
    dedupe_hosts(&listed, &servers);
    if (servers.n == 0) {
        fprintf(stderr, "error: no %sServer lines in %s%s\n", all ? "" : "active ",
                MIRRORLIST_PATH, all ? "" : " (try --all to test the commented ones)");
        sl_free(&listed);
        sl_free(&servers);
        sb_free(&text);
        return 1;
    }
    char arch[80];
    machine_arch(arch, sizeof arch);
    if (jobs < 1)
        jobs = 1;
    printf("%s:: Testing %zu mirrors%s: latency and sync age (%d at a time)%s\n", C_BOLD,
           servers.n, listed.n > servers.n ? " (one per host, https preferred)" : "", jobs, C_END);
    RankCtx ctx = { &servers, arch };
    StrList res;
    sl_init(&res);
    proc_fork_jobs(servers.n, (size_t)jobs, rank_job, &ctx, &res);

    RankRow *rows = xcalloc(servers.n, sizeof(RankRow));
    long long fresh = 0;
    for (size_t i = 0; i < servers.n; i++) {
        rows[i].url = servers.v[i];
        rows[i].https = strncmp(servers.v[i], "https://", 8) == 0;
        rows[i].ok = sscanf(res.v[i], "ok %lf %lld", &rows[i].ttfb, &rows[i].lastsync) == 2;
        if (rows[i].ok && rows[i].lastsync > fresh)
            fresh = rows[i].lastsync;
    }
    for (size_t i = 0; i < servers.n; i++)   /* > 6h behind the freshest = stale */
        rows[i].stale = rows[i].ok && fresh && rows[i].lastsync &&
                        rows[i].lastsync < fresh - 6 * 3600;

    /* Throughput one mirror at a time: measured in parallel they would share
     * (and split) the same link, and the numbers would mean nothing. */
    qsort(rows, servers.n, sizeof(RankRow), cmp_latency);
    size_t cand = 0;
    for (size_t i = 0; i < servers.n; i++)
        cand += rows[i].ok && !rows[i].stale;
    if (cand > RANK_MEASURE_MAX)
        cand = RANK_MEASURE_MAX;
    printf("%s:: Measuring throughput of %zu mirrors, one at a time (4 MiB of extra.db)%s\n",
           C_BOLD, cand, C_END);
    for (size_t i = 0; i < cand; i++) {
        char base[1024], url[1100];
        subst(rows[i].url, "extra", arch, base, sizeof base);
        snprintf(url, sizeof url, "%s/extra.db", base);
        double t, sp, sz;
        printf("\r   %zu/%zu ", i + 1, cand);
        fflush(stdout);
        if (curl_stats(url, "6", "0-4194303", &t, &sp, &sz) == 0 && sz >= 256 * 1024) {
            rows[i].speed = sp;
            rows[i].measured = true;
        }
    }
    printf("\r%20s\r", "");   /* clear the progress counter */
    qsort(rows, servers.n, sizeof(RankRow), cmp_rank);

    printf("   %-4s %8s %11s %7s  %s\n", "rank", "latency", "speed", "behind", "server");
    StrList ranked;
    sl_init(&ranked);
    for (size_t i = 0; i < servers.n; i++) {
        const RankRow *r = &rows[i];
        if (!r->ok) {
            printf("   %-4s %8s %11s %7s  ", "-", "failed", "", "");
        } else {
            char behind[16], speed[24];
            if (!r->lastsync)
                snprintf(behind, sizeof behind, "?");
            else
                snprintf(behind, sizeof behind, "%.1fh", (double)(fresh - r->lastsync) / 3600.0);
            if (r->measured)
                snprintf(speed, sizeof speed, "%.1f MiB/s", r->speed / 1048576.0);
            else
                snprintf(speed, sizeof speed, "-");
            const char *col = r->stale ? C_YEL : C_GREEN;
            printf("   %s%-4zu%s %6.0fms %11s %7s  ", col, i + 1, C_END, r->ttfb * 1000.0, speed,
                   behind);
            if (!r->stale && (count <= 0 || ranked.n < (size_t)count))
                sl_push(&ranked, r->url);
        }
        safe_fputs(stdout, r->url);
        printf("%s%s\n", r->stale ? "  (out of date)" : "",
               r->ok && !r->https ? "  (http: database not protected in transit)" : "");
    }
    free(rows);
    sl_free(&res);

    int rc = 0;
    if (ranked.n == 0) {
        fprintf(stderr, "error: no usable mirror answered\n");
        rc = 1;
    } else {
        printf("\nThe %zu best up-to-date mirrors become active (https first, then by\n"
               "measured speed); the previous entries are kept as comments\n"
               "(backup: %s.inkaur-bak).\n", ranked.n, MIRRORLIST_PATH);
        if (!noconfirm && stdin_is_tty() &&
            ask_yesno("Write the new mirrorlist?", false, false)) {
            char date[32];
            time_t now = time(NULL);
            struct tm tmv;
            strftime(date, sizeof date, "%Y-%m-%d %H:%M", localtime_r(&now, &tmv));
            char *neu = mirrorlist_rewrite(text.data, &ranked, date);
            char tmp[PATH_BUF];
            StrList one;
            sl_init(&one);
            if (path_join(tmp, sizeof tmp, cache, ".mirrorlist.new") &&
                write_file_atomic(tmp, neu, strlen(neu)) == 0) {
                sl_push(&one, tmp);
                rc = root_install(&one, MIRRORLIST_PATH, false, true) == 0 ? 0 : 1;
                unlink(tmp);
                if (rc == 0)
                    printf("written; run 'inkaur -Syyu' to refresh the databases from them\n");
            } else {
                rc = 1;
            }
            sl_free(&one);
            free(neu);
        } else {
            printf("nothing written\n");
        }
    }
    sl_free(&ranked);
    sl_free(&servers);
    sl_free(&listed);
    sb_free(&text);
    return rc;
}

/* ------------------------------------------------------------ multi-mirror download */

typedef struct {
    const StrList *names;     /* file names */
    const StrList *sizes;
    const StrList *repo;      /* repository of each file */
    const StrList *mirrors;   /* "repo\tserver" lines, in pacman's order */
    size_t nmirrors;          /* first n servers of each repo are used */
    const char *staging;
} DlCtx;

static size_t servers_for(const DlCtx *c, const char *repo, const char **out, size_t max)
{
    size_t n = 0, rl = strlen(repo);
    for (size_t i = 0; i < c->mirrors->n && n < max; i++)
        if (strncmp(c->mirrors->v[i], repo, rl) == 0 && c->mirrors->v[i][rl] == '\t')
            out[n++] = c->mirrors->v[i] + rl + 1;
    return n;
}

static void dl_job(size_t i, void *vctx, StrBuf *out)
{
    const DlCtx *c = vctx;
    const char *srv[8];
    size_t ns = servers_for(c, c->repo->v[i], srv, 8);
    if (ns == 0) {
        sb_puts(out, "fail");
        return;
    }
    size_t use = c->nmirrors < ns ? c->nmirrors : ns;
    long long want = strtoll(c->sizes->v[i], NULL, 10);
    char part[PATH_BUF], dest[PATH_BUF], fname[600];
    snprintf(fname, sizeof fname, "%s.part", c->names->v[i]);
    if (!path_join(part, sizeof part, c->staging, fname) ||
        !path_join(dest, sizeof dest, c->staging, c->names->v[i])) {
        sb_puts(out, "fail");
        return;
    }
    char maxsize[32];
    snprintf(maxsize, sizeof maxsize, "%lld", want + 4096);
    /* start at this file's own mirror (spreads the load), then the others */
    for (size_t k = 0; k < ns; k++) {
        size_t m = k < use ? (i + k) % use : k;
        char url[2048];
        if (snprintf(url, sizeof url, "%s/%s", srv[m], c->names->v[i]) >= (int)sizeof url)
            continue;
        unlink(part);
        const char *argv[] = {
            BIN_CURL, "-q", "--silent", "--fail", "--globoff",
            "--proto", "=https,http", "--proto-redir", "=https,http", "--max-redirs", "3",
            "--connect-timeout", "10", "--speed-limit", "1024", "--speed-time", "30",
            "--max-filesize", maxsize, "--user-agent", "inkaur/" INKAUR_VERSION,
            "--output", part, "--url", url, NULL
        };
        ProcOpts o;
        memset(&o, 0, sizeof o);
        o.stdin_null = true;
        o.stdout_null = true;
        o.stderr_null = true;
        struct stat st;
        if (proc_run(argv, &o) == 0 && lstat(part, &st) == 0 && S_ISREG(st.st_mode) &&
            (long long)st.st_size == want && rename(part, dest) == 0) {
            sb_printf(out, "ok %zu", m);
            return;
        }
    }
    unlink(part);
    sb_puts(out, "fail");
}

/* pacman package file names only: no paths, no leading dot. */
static bool safe_filename(const char *s)
{
    if (s[0] == '\0' || s[0] == '.' || s[0] == '-' || strlen(s) > 512)
        return false;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("@._+:-~", *p))
            return false;
    return strstr(s, ".pkg.tar") != NULL;
}

static void clear_staging(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL)
        return;
    struct dirent *de;
    char path[PATH_BUF];
    while ((de = readdir(d)) != NULL)
        if (de->d_name[0] != '.' && path_join(path, sizeof path, dir, de->d_name) &&
            is_file_nofollow(path))
            unlink(path);
    closedir(d);
}

int repo_download_start(RepoDownload *d, const char *cache, const StrList *print_args,
                        const char *cachedir_override, int mirrors, int jobs)
{
    memset(d, 0, sizeof *d);
    d->pid = -1;
    sl_init(&d->files);
    sl_init(&d->sizes);

    /* what pacman would download (no root needed, no lock taken) */
    StrList args;
    sl_init(&args);
    sl_extend(&args, print_args);
    sl_push(&args, "--print");
    sl_push(&args, "--print-format=%r %s %l");
    StrBuf out;
    sb_init(&out, 8u << 20);
    int rc = pacman_capture(&args, &out);
    sl_free(&args);
    StrList lines, repos;
    sl_init(&lines);
    sl_init(&repos);
    if (rc == 0)
        sl_split_lines(&lines, out.data);
    sb_free(&out);
    for (size_t i = 0; i < lines.n; i++) {
        char repo[256], size[32], url[2048];
        if (sscanf(lines.v[i], "%255s %31s %2047s", repo, size, url) != 3 ||
            strncmp(url, "file://", 7) == 0 || strtoll(size, NULL, 10) <= 0 ||
            !valid_pkgname(repo))
            continue;
        const char *fn = strrchr(url, '/');
        if (fn == NULL || !safe_filename(fn + 1))
            continue;
        sl_push(&d->files, fn + 1);
        sl_push(&d->sizes, size);
        sl_push(&repos, repo);
        d->bytes += strtod(size, NULL);
    }
    sl_free(&lines);
    d->nfiles = d->files.n;
    if (d->nfiles == 0) {
        sl_free(&repos);
        return rc == 0 ? 0 : -1;
    }

    /* cache dir and the servers of each repository, in pacman's order */
    StrList cd;
    sl_init(&cd);
    if (cachedir_override)
        sl_push(&cd, cachedir_override);
    else
        pacman_conf_list(NULL, "CacheDir", &cd);
    snprintf(d->cachedir, sizeof d->cachedir, "%s", cd.n ? cd.v[0] : "/var/cache/pacman/pkg/");
    sl_free(&cd);
    StrList mirrorlines, uniq;
    sl_init(&mirrorlines);
    sl_init(&uniq);
    for (size_t i = 0; i < repos.n; i++)
        sl_push_unique(&uniq, repos.v[i]);
    for (size_t i = 0; i < uniq.n; i++) {
        StrList srv;
        sl_init(&srv);
        pacman_conf_repo_servers(uniq.v[i], &srv);
        for (size_t j = 0; j < srv.n; j++) {
            char line[2400];
            snprintf(line, sizeof line, "%s\t%s", uniq.v[i], srv.v[j]);
            sl_push(&mirrorlines, line);
        }
        sl_free(&srv);
    }
    sl_free(&uniq);

    if (!path_join(d->staging, sizeof d->staging, cache, ".pkgdl") ||
        ensure_private_dir(d->staging) != 0) {
        sl_free(&repos);
        sl_free(&mirrorlines);
        return -1;
    }
    clear_staging(d->staging);

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        sl_free(&repos);
        sl_free(&mirrorlines);
        return -1;
    }
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        DlCtx c = { &d->files, &d->sizes, &repos, &mirrorlines,
                    (size_t)(mirrors < 1 ? 1 : mirrors), d->staging };
        StrList res;
        sl_init(&res);
        proc_fork_jobs(d->files.n, (size_t)jobs, dl_job, &c, &res);
        _exit(0);
    }
    d->pid = pid;
    sl_free(&repos);
    sl_free(&mirrorlines);
    return 0;
}

int repo_download_finish(RepoDownload *d)
{
    if (d->pid <= 0) {
        sl_free(&d->files);
        sl_free(&d->sizes);
        return 0;
    }
    if (!proc_has_exited(d->pid))
        printf(":: Waiting for the repository downloads...\n");
    proc_wait_pid(d->pid);
    d->pid = -1;

    StrList done;
    sl_init(&done);
    double bytes = 0;
    for (size_t i = 0; i < d->files.n; i++) {
        char path[PATH_BUF];
        struct stat st;
        if (path_join(path, sizeof path, d->staging, d->files.v[i]) &&
            lstat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (long long)st.st_size == strtoll(d->sizes.v[i], NULL, 10)) {
            sl_push(&done, path);
            bytes += (double)st.st_size;
        }
    }
    int rc = 0;
    if (done.n) {
        printf(":: %zu/%zu repository packages (%.1f MiB) downloaded across mirrors; "
               "copying into %s\n", done.n, d->files.n, bytes / 1048576.0, d->cachedir);
        rc = root_install(&done, d->cachedir, true, false);
        if (rc != 0)
            fprintf(stderr, "warning: could not copy into the pacman cache; pacman will "
                            "download the packages itself\n");
    }
    if (done.n < d->files.n)
        printf("   %zu package(s) left for pacman to download\n", d->files.n - done.n);
    clear_staging(d->staging);
    sl_free(&done);
    sl_free(&d->files);
    sl_free(&d->sizes);
    return rc;
}
