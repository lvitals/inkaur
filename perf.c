#include "perf.h"
#include "buf.h"
#include "config.h"
#include "fsutil.h"
#include "pacman.h"
#include "proc.h"
#include "tty.h"
#include "validate.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SYSTEM_CONF "/etc/inkaur.conf"

void perf_config_defaults(PerfConfig *c)
{
    c->parallel = -1;
    c->build_jobs = 0;
    c->fetch_jobs = 0;
    c->prefetch = -1;
    c->speedtest = 1;
}

int perf_parse_jobs(const char *s)
{
    if (s == NULL)
        return -1;
    if (strcmp(s, "auto") == 0)
        return 0;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > PERF_MAX_JOBS)
        return -1;
    return (int)v;
}

static int parse_tristate(const char *v)
{
    if (strcmp(v, "auto") == 0)
        return -1;
    if (strcmp(v, "yes") == 0 || strcmp(v, "true") == 0 || strcmp(v, "1") == 0)
        return 1;
    if (strcmp(v, "no") == 0 || strcmp(v, "false") == 0 || strcmp(v, "0") == 0)
        return 0;
    return -2;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
    return s;
}

int perf_config_parse(const char *text, const char *origin, PerfConfig *c)
{
    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, text);
    int rc = 0;
    for (size_t i = 0; i < lines.n; i++) {
        char *l = lines.v[i];
        char *hash = strchr(l, '#');
        if (hash)
            *hash = '\0';
        l = trim(l);
        if (*l == '\0' || *l == '[')
            continue;
        char *eq = strchr(l, '=');
        if (eq == NULL) {
            fprintf(stderr, "error: %s: expected 'Key = Value': %s\n", origin, l);
            rc = -1;
            continue;
        }
        *eq = '\0';
        char *k = trim(l), *v = trim(eq + 1);
        int t;
        if (strcmp(k, "Parallel") == 0 && (t = parse_tristate(v)) != -2) {
            c->parallel = t;
        } else if (strcmp(k, "Prefetch") == 0 && (t = parse_tristate(v)) != -2) {
            c->prefetch = t;
        } else if (strcmp(k, "BuildJobs") == 0 && (t = perf_parse_jobs(v)) >= 0) {
            c->build_jobs = t;
        } else if (strcmp(k, "FetchJobs") == 0 && (t = perf_parse_jobs(v)) >= 0) {
            c->fetch_jobs = t;
        } else if (strcmp(k, "SpeedTest") == 0 &&
                   (strcmp(v, "never") == 0 || strcmp(v, "auto") == 0 ||
                    strcmp(v, "always") == 0)) {
            c->speedtest = v[0] == 'n' ? 0 : v[0] == 'a' && v[1] == 'u' ? 1 : 2;
        } else {
            fprintf(stderr, "error: %s: invalid setting '%s = %s'\n", origin, k, v);
            rc = -1;
        }
    }
    sl_free(&lines);
    return rc;
}

int perf_config_load(const char *path, PerfConfig *c, bool root_owned)
{
    struct stat st;
    if (lstat(path, &st) != 0)
        return 0;
    if (!S_ISREG(st.st_mode) || (st.st_mode & (S_IWGRP | S_IWOTH)) ||
        (root_owned ? st.st_uid != 0 : (st.st_uid != getuid() && st.st_uid != 0))) {
        fprintf(stderr, "warning: ignoring %s: must be a regular file owned by %s and "
                        "not writable by group/others\n", path, root_owned ? "root" : "you");
        return -1;
    }
    StrBuf b;
    sb_init(&b, 65536);
    int rc = read_file_limited(path, 65536, &b);
    if (rc == 0) {
        PerfConfig tmp = *c;
        rc = perf_config_parse(b.data, path, &tmp);
        if (rc == 0)
            *c = tmp;
        else
            fprintf(stderr, "warning: %s ignored because of the errors above\n", path);
    }
    sb_free(&b);
    return rc;
}

void perf_config_load_all(const char *home, PerfConfig *c)
{
    perf_config_load(SYSTEM_CONF, c, true);
    char path[PATH_BUF];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    bool ok = (xdg && xdg[0] == '/')
                  ? snprintf(path, sizeof path, "%s/inkaur/inkaur.conf", xdg) < (int)sizeof path
                  : snprintf(path, sizeof path, "%s/.config/inkaur/inkaur.conf", home) <
                        (int)sizeof path;
    if (ok)
        perf_config_load(path, c, false);
}

/* ------------------------------------------------------------ facts */

static double mem_available_gib(void)
{
    StrBuf b;
    sb_init(&b, 1u << 16);
    double gib = 0;
    if (read_file_limited("/proc/meminfo", 1u << 16, &b) == 0) {
        const char *p = strstr(b.data, "MemAvailable:");
        if (p)
            gib = strtod(p + 13, NULL) / (1024.0 * 1024.0);   /* kB -> GiB */
    }
    sb_free(&b);
    return gib;
}

static bool speed_path(const char *cache, char *out, size_t sz)
{
    return cache && path_join(out, sz, cache, ".netspeed");
}

void perf_facts(const char *cache, PerfFacts *f)
{
    memset(f, 0, sizeof *f);
    f->cpus = cpu_count();
    f->mem_gib = mem_available_gib();
    char path[PATH_BUF];
    if (!speed_path(cache, path, sizeof path))
        return;
    StrBuf b;
    sb_init(&b, 4096);
    if (read_file_limited(path, 4096, &b) == 0) {
        long long t;
        double bps, lat;
        if (sscanf(b.data, "v1 %lld %lf %lf", &t, &bps, &lat) == 3 && bps >= 0 &&
            (time_t)t <= time(NULL) && time(NULL) - (time_t)t < PERF_SPEED_MAX_AGE) {
            f->have_speed = true;
            f->bytes_per_sec = bps;
            f->latency_ms = lat;
            f->measured_at = (time_t)t;
        }
    }
    sb_free(&b);
}

int perf_save_speed(const char *cache, const PerfFacts *f)
{
    char path[PATH_BUF], line[128];
    if (!f->have_speed || !speed_path(cache, path, sizeof path))
        return -1;
    int n = snprintf(line, sizeof line, "v1 %lld %.0f %.1f\n", (long long)f->measured_at,
                     f->bytes_per_sec, f->latency_ms);
    return write_file_atomic(path, line, (size_t)n);
}

/* curl to /dev/null; fills size (bytes), speed (B/s), connect and ttfb (s). */
static int probe(const char *url, const char *maxtime, double *size, double *speed,
                 double *conn, double *ttfb)
{
    const char *argv[] = {
        BIN_CURL, "-q", "--silent", "--globoff",
        "--proto", "=https,http", "--proto-redir", "=https,http",
        "--max-redirs", "3", "--connect-timeout", "5", "--max-time", maxtime,
        "--user-agent", "inkaur/" INKAUR_VERSION,
        "--output", "/dev/null",
        "--write-out", "%{http_code} %{size_download} %{speed_download} "
                       "%{time_connect} %{time_starttransfer}",
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
    int code = 0;
    int ok = (rc == 0 || rc == 28) &&   /* 28: --max-time reached, still measured */
             sscanf(out.data, "%d %lf %lf %lf %lf", &code, size, speed, conn, ttfb) == 5 &&
             code >= 200 && code < 400 && *size > 0;
    sb_free(&out);
    return ok ? 0 : -1;
}

static int first_mirror(const char *pacman_config, const char *repo, char *out, size_t sz)
{
    char cfg[PATH_BUF], rarg[128];
    const char *argv[7];
    size_t k = 0;
    argv[k++] = BIN_PACMAN_CONF;
    if (pacman_config) {
        snprintf(cfg, sizeof cfg, "--config=%s", pacman_config);
        argv[k++] = cfg;
    }
    snprintf(rarg, sizeof rarg, "--repo=%s", repo);
    argv[k++] = rarg;
    argv[k++] = "--";
    argv[k++] = "Server";
    argv[k] = NULL;
    StrBuf b;
    sb_init(&b, 1u << 20);
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.capture = &b;
    o.stdin_null = true;
    o.stderr_null = true;
    int rc = -1;
    if (proc_run(argv, &o) == 0) {
        size_t n = strcspn(b.data, "\n");
        if (n > 0 && n < sz && (strncmp(b.data, "https://", 8) == 0 ||
                                strncmp(b.data, "http://", 7) == 0)) {
            memcpy(out, b.data, n);
            out[n] = '\0';
            rc = 0;
        }
    }
    sb_free(&b);
    return rc;
}

int perf_measure(const char *pacman_config, PerfFacts *f, bool verbose)
{
    double size, speed, conn, ttfb;
    f->have_speed = false;
    if (probe(AUR_RPC_BASE "/info?arg[]=pacman", "10", &size, &speed, &conn, &ttfb) == 0) {
        f->latency_ms = ttfb * 1000.0;
        if (verbose)
            printf("   %-26s connect %.0f ms, first byte %.0f ms\n", "AUR (aur.archlinux.org)",
                   conn * 1000.0, ttfb * 1000.0);
    } else if (verbose) {
        printf("   %-26s %sunreachable%s\n", "AUR (aur.archlinux.org)", C_RED, C_END);
    }

    static const char *const repos[] = { "extra", "core", NULL };
    for (size_t i = 0; repos[i]; i++) {
        char server[PATH_BUF], url[PATH_BUF];
        if (first_mirror(pacman_config, repos[i], server, sizeof server) != 0)
            continue;
        if (snprintf(url, sizeof url, "%s/%s.db", server, repos[i]) >= (int)sizeof url)
            continue;
        if (probe(url, "8", &size, &speed, &conn, &ttfb) != 0) {
            if (verbose) {
                printf("   mirror ");
                safe_fputs(stdout, server);
                printf(": %sfailed%s\n", C_RED, C_END);
            }
            continue;
        }
        f->have_speed = true;
        f->bytes_per_sec = speed;
        f->measured_at = time(NULL);
        if (verbose) {
            printf("   %-26s ", "mirror download");
            printf("%.2f MiB/s (%.1f Mbit/s), %.1f MiB, connect %.0f ms\n",
                   speed / 1048576.0, speed * 8 / 1e6, size / 1048576.0, conn * 1000.0);
            printf("   %-26s ", "");
            safe_fputs(stdout, server);
            putchar('\n');
        }
        break;
    }
    return f->have_speed ? 0 : -1;
}

/* ------------------------------------------------------------ decision */

static int auto_fetch_jobs(const PerfFacts *f)
{
    if (!f->have_speed)
        return 4;   /* not measured: accelerate */
    double kib = f->bytes_per_sec / 1024.0;
    if (kib < 128)
        return 1;
    if (kib < 1024)
        return 2;
    if (kib < 4096)
        return 4;
    return 8;
}

static int auto_build_jobs(const PerfFacts *f)
{
    if (f->cpus < 4 || (f->mem_gib > 0 && f->mem_gib < 3))
        return 1;
    /* many AUR builds are -bin/python/repackaging work bound by I/O, not CPU */
    long by_cpu = f->cpus / 4 < 2 ? 2 : f->cpus / 4;
    long by_mem = f->mem_gib > 0 ? (long)(f->mem_gib / 3) : by_cpu;
    long n = by_cpu < by_mem ? by_cpu : by_mem;
    if (n > 4)
        n = 4;
    return n < 1 ? 1 : (int)n;
}

void perf_decide(const PerfConfig *c, const PerfFacts *f, bool chroot, PerfPlan *out)
{
    memset(out, 0, sizeof *out);
    if (c->parallel == 0) {
        out->build_jobs = out->fetch_jobs = 1;
        snprintf(out->why, sizeof out->why, "serial: Parallel = no or --serial");
        return;
    }
    out->fetch_jobs = c->fetch_jobs > 0 ? c->fetch_jobs : auto_fetch_jobs(f);
    out->build_jobs = c->build_jobs > 0 ? c->build_jobs : auto_build_jobs(f);
    if (chroot)
        out->build_jobs = 1;   /* one devtools chroot copy at a time */
    bool fast = !f->have_speed || f->bytes_per_sec >= 1024.0 * 1024.0;
    out->prefetch = c->prefetch == 1 || (c->prefetch == -1 && out->fetch_jobs > 1 && fast);

    char speed[64];
    if (f->have_speed) {
        struct tm tmv;
        char date[16] = "?";
        if (localtime_r(&f->measured_at, &tmv))
            strftime(date, sizeof date, "%Y-%m-%d", &tmv);
        snprintf(speed, sizeof speed, "%.1f MiB/s measured %s", f->bytes_per_sec / 1048576.0,
                 date);
    } else {
        snprintf(speed, sizeof speed, "speed not measured");
    }
    snprintf(out->why, sizeof out->why, "%ld CPUs, %.1f GiB free RAM, %s%s", f->cpus,
             f->mem_gib, speed, chroot ? ", chroot builds run one at a time" : "");
}

/* ------------------------------------------------------------ --speedtest */

int cmd_speedtest(const char *cache, const char *pacman_config, const PerfConfig *c)
{
    PerfFacts f;
    perf_facts(cache, &f);
    printf("%s:: Connection analysis%s\n", C_BOLD, C_END);
    int rc = perf_measure(pacman_config, &f, true);
    if (rc == 0 && perf_save_speed(cache, &f) != 0)
        fprintf(stderr, "warning: could not save the measurement\n");

    printf("\n%s:: Computer%s\n", C_BOLD, C_END);
    printf("   %-26s %ld\n", "CPUs", f.cpus);
    printf("   %-26s %.1f GiB\n", "RAM available", f.mem_gib);
    StrList pd;
    sl_init(&pd);
    pacman_conf_list(pacman_config, "ParallelDownloads", &pd);
    long pdn = pd.n ? strtol(pd.v[0], NULL, 10) : 1;
    printf("   %-26s %ld\n", "pacman ParallelDownloads", pdn);
    sl_free(&pd);

    PerfPlan plan;
    perf_decide(c, &f, false, &plan);
    printf("\n%s:: inkaur will use%s\n", C_BOLD, C_END);
    printf("   fetch jobs  %-3d %s\n", plan.fetch_jobs,
           c->fetch_jobs ? "(configured)" : "(auto) concurrent git clone/fetch of AUR repos");
    printf("   build jobs  %-3d %s\n", plan.build_jobs,
           c->build_jobs ? "(configured)" : "(auto) concurrent AUR builds, dependency order kept");
    printf("   prefetch    %-3s AUR repos fetched while pacman -Syu runs\n",
           plan.prefetch ? "yes" : "no");
    printf("   based on: %s\n", plan.why);

    if (f.have_speed) {
        double kib = f.bytes_per_sec / 1024.0;
        printf("\n%s:: Suggestions%s\n", C_BOLD, C_END);
        if (kib < 256)
            printf("   slow connection: consider 'Parallel = no' in inkaur.conf and\n"
                   "   'ParallelDownloads = 1' or 2 in /etc/pacman.conf\n");
        else if (pdn <= 1 && kib >= 1024)
            printf("   fast connection: set 'ParallelDownloads = 5' in /etc/pacman.conf\n"
                   "   so pacman also downloads in parallel\n");
        else
            printf("   current settings fit this connection\n");
    }
    if (rc == 0)
        printf("\nmeasurement saved; 'auto' uses it for %d days (SpeedTest = always\n"
               "measures before every upgrade). Settings: ~/.config/inkaur/inkaur.conf\n",
               PERF_SPEED_MAX_AGE / 86400);
    else
        printf("\n%swarning:%s could not measure the connection; 'auto' keeps the default "
               "(accelerated) settings\n", C_YEL, C_END);
    return rc == 0 ? 0 : 1;
}
