#ifndef PERF_H
#define PERF_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/*
 * Parallelism settings: how many AUR repositories are cloned/fetched at once,
 * how many AUR packages are built at once (dependency order is always kept),
 * and whether AUR repositories are prefetched while pacman -Syu runs.
 *
 * Sources, later ones win: built-in defaults, /etc/inkaur.conf,
 * ~/.config/inkaur/inkaur.conf ($XDG_CONFIG_HOME), command line.
 *
 *   [options]
 *   Parallel  = auto | yes | no     # no: everything one at a time
 *   BuildJobs = auto | 1..64
 *   FetchJobs = auto | 1..64
 *   Prefetch  = auto | yes | no
 *   SpeedTest = auto | always | never
 *
 * "auto" accelerates by default and scales down on few CPUs, little free
 * RAM, or a slow connection measured by `inkaur --speedtest`.
 */

typedef struct {
    int parallel;     /* -1 auto, 0 no, 1 yes */
    int build_jobs;   /* 0 = auto */
    int fetch_jobs;   /* 0 = auto */
    int prefetch;     /* -1 auto, 0 no, 1 yes */
    int speedtest;    /* 0 never, 1 auto (use saved result), 2 always (measure each -Su) */
} PerfConfig;

typedef struct {
    long cpus;
    double mem_gib;          /* MemAvailable */
    bool have_speed;
    double bytes_per_sec;    /* mirror download throughput */
    double latency_ms;       /* AUR time to first byte */
    time_t measured_at;
} PerfFacts;

typedef struct {
    int build_jobs;
    int fetch_jobs;
    bool prefetch;
    char why[256];
} PerfPlan;

#define PERF_MAX_JOBS 64
#define PERF_SPEED_MAX_AGE (7 * 24 * 3600)

void perf_config_defaults(PerfConfig *c);
/* Applies one config file; a missing file is not an error. Returns -1 on
 * unsafe ownership/permissions or a syntax error (message on stderr). */
int perf_config_load(const char *path, PerfConfig *c, bool root_owned);
/* /etc/inkaur.conf then the user's file. */
void perf_config_load_all(const char *home, PerfConfig *c);
/* "auto" -> 0, "1".."64" -> n; -1 if invalid. */
int perf_parse_jobs(const char *s);
/* Parse "Key = Value" lines (exposed for tests). */
int perf_config_parse(const char *text, const char *origin, PerfConfig *c);

/* Hardware facts and the saved speed measurement (if recent). */
void perf_facts(const char *cache, PerfFacts *f);
/* Measures the AUR latency and the first mirror's throughput. */
int perf_measure(const char *pacman_config, PerfFacts *f, bool verbose);
int perf_save_speed(const char *cache, const PerfFacts *f);

void perf_decide(const PerfConfig *c, const PerfFacts *f, bool chroot, PerfPlan *out);

/* inkaur --speedtest */
int cmd_speedtest(const char *cache, const char *pacman_config, const PerfConfig *c);

#endif /* PERF_H */
