#include "../test_helper.h"
#include "perf.h"

void run_perf_tests(void);

void run_perf_tests(void)
{
    PerfConfig c;
    perf_config_defaults(&c);
    ASSERT(c.parallel == -1 && c.build_jobs == 0 && c.fetch_jobs == 0, "defaults are auto");

    ASSERT(perf_parse_jobs("auto") == 0 && perf_parse_jobs("4") == 4, "jobs parse");
    ASSERT(perf_parse_jobs("0") < 0 && perf_parse_jobs("65") < 0 && perf_parse_jobs("3x") < 0 &&
           perf_parse_jobs("-1") < 0 && perf_parse_jobs("") < 0, "bad jobs refused");

    const char *conf = "# comment\n[options]\nParallel = yes\nBuildJobs = 3   # inline\n"
                       "FetchJobs=auto\nRepoDownload = mirrors\nMirrors = 4\nSpeedTest = always\n";
    ASSERT(perf_config_parse(conf, "test", &c) == 0, "config parses");
    ASSERT(c.parallel == 1 && c.build_jobs == 3 && c.fetch_jobs == 0 && c.repo_download == 2 && c.mirrors == 4 &&
           c.speedtest == 2, "config values");
    PerfConfig bad;
    perf_config_defaults(&bad);
    ASSERT(perf_config_parse("BuildJobs = lots\n", "test", &bad) != 0, "bad value refused");
    ASSERT(perf_config_parse("Bogus = 1\n", "test", &bad) != 0, "unknown key refused");
    ASSERT(perf_config_parse("Parallel\n", "test", &bad) != 0, "missing '=' refused");

    PerfFacts f;
    memset(&f, 0, sizeof f);
    PerfPlan p;
    PerfConfig a;
    perf_config_defaults(&a);

    /* fast machine, speed not measured: accelerate */
    f.cpus = 16;
    f.mem_gib = 30;
    perf_decide(&a, &f, false, &p);
    ASSERT(p.fetch_jobs == 4 && p.build_jobs == 4 && p.repo_download == 1, "auto accelerates by default");

    /* slow computer */
    f.cpus = 2;
    f.mem_gib = 1.5;
    perf_decide(&a, &f, false, &p);
    ASSERT(p.build_jobs == 1, "few CPUs / little RAM: one build at a time");

    /* slow connection measured */
    f.cpus = 8;
    f.mem_gib = 16;
    f.have_speed = true;
    f.bytes_per_sec = 60 * 1024;
    perf_decide(&a, &f, false, &p);
    ASSERT(p.fetch_jobs == 1 && p.repo_download == 0 && p.build_jobs == 2, "slow link: serial fetch");
    f.bytes_per_sec = 20.0 * 1024 * 1024;
    perf_decide(&a, &f, false, &p);
    ASSERT(p.fetch_jobs == 8 && p.repo_download == 1, "fast link: more fetch jobs");

    /* explicit settings, serial mode, chroot */
    a.build_jobs = 6;
    perf_decide(&a, &f, false, &p);
    ASSERT(p.build_jobs == 6, "configured build jobs win");
    perf_decide(&a, &f, true, &p);
    ASSERT(p.build_jobs == 1, "chroot builds are serial");
    a.parallel = 0;
    perf_decide(&a, &f, false, &p);
    ASSERT(p.build_jobs == 1 && p.fetch_jobs == 1 && p.repo_download == 0, "Parallel = no");

    printf("perf: ok\n");
}
