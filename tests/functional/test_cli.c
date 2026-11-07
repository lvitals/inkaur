#include "../test_helper.h"
#include "fsutil.h"

#include <stdbool.h>
#include <sys/wait.h>

void run_cli_tests(void);
const char *make_fixture(int evil);

/* Runs a shell command line (test-only), captures output and exit code. */
static int run(const char *cmd, char *out, size_t sz)
{
    FILE *fp = popen(cmd, "r");
    if (fp == NULL)
        return -1;
    size_t n = fread(out, 1, sz - 1, fp);
    out[n] = '\0';
    int st = pclose(fp);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

void run_cli_tests(void)
{
    static char out[1 << 16];

    ASSERT(run("./inkaur -V", out, sizeof out) == 0, "-V exit 0");
    ASSERT(strstr(out, "inkaur 0.3.0") && strstr(out, "Leandro V. Catarin"), "version text");

    ASSERT(run("./inkaur -h", out, sizeof out) == 0 && strstr(out, "usage:  inkaur"), "-h");
    ASSERT(run("./inkaur -Sh", out, sizeof out) == 0 && strstr(out, "--chroot"), "-Sh");

    /* malicious names are rejected before any network/filesystem access */
    ASSERT(run("./inkaur -S ../.. 2>&1", out, sizeof out) == 1 &&
           strstr(out, "invalid package name"), "-S ../.. rejected");
    ASSERT(run("./inkaur -S -- -rf 2>&1", out, sizeof out) == 1, "-S -- -rf rejected");
    ASSERT(run("./inkaur -Si 'a&type=search' 2>&1", out, sizeof out) == 1 &&
           strstr(out, "invalid package name"), "-Si URL injection rejected");
    ASSERT(run("./inkaur -R '../x' 2>&1", out, sizeof out) == 1, "-R bad name rejected");
    ASSERT(run("./inkaur -Ss a 2>&1", out, sizeof out) == 1, "1-char search rejected");

    ASSERT(run("./inkaur -Sy 2>&1", out, sizeof out) == 1 && strstr(out, "partial upgrade"),
           "-Sy refused");
    ASSERT(run("./inkaur -Ssi foo 2>&1", out, sizeof out) == 1, "conflicting ops refused");
    ASSERT(run("./inkaur -Qx 2>&1", out, sizeof out) == 1, "unknown query flag refused");
    ASSERT(run("./inkaur --bogus 2>&1", out, sizeof out) == 1, "unknown long option");
    ASSERT(run("./inkaur -SQ 2>&1", out, sizeof out) == 1 && strstr(out, "one operation"),
           "two operations refused");
    ASSERT(run("./inkaur 2>&1", out, sizeof out) == 1 && strstr(out, "no operation"),
           "no operation");
    ASSERT(run("./inkaur -Rh", out, sizeof out) == 0 && strstr(out, "--cascade") &&
           strstr(out, "--dbpath"), "-Rh lists pacman options");
    ASSERT(run("./inkaur -Qh", out, sizeof out) == 0 && strstr(out, "--foreign"), "-Qh");
    ASSERT(run("./inkaur -Uh", out, sizeof out) == 0 && strstr(out, "--overwrite"), "-Uh");
    ASSERT(run("./inkaur -Dh", out, sizeof out) == 0 && strstr(out, "--asdeps"), "-Dh");
    ASSERT(run("./inkaur -Fh", out, sizeof out) == 0 && strstr(out, "--regex"), "-Fh");
    ASSERT(run("./inkaur -S --aur --repo x 2>&1", out, sizeof out) == 1, "--aur --repo refused");
    ASSERT(run("./inkaur -S extra/../x 2>&1", out, sizeof out) == 1 &&
           strstr(out, "invalid package name"), "repo/ prefix validated");
    ASSERT(run("./inkaur -D --asdeps ../x 2>&1", out, sizeof out) == 1, "-D bad name rejected");
    ASSERT(run("./inkaur -T 'a;b' 2>&1", out, sizeof out) == 1, "-T bad dep rejected");
    ASSERT(run("./inkaur -S --jobs 0 x 2>&1", out, sizeof out) == 1 &&
           strstr(out, "--jobs"), "--jobs 0 refused");
    ASSERT(run("./inkaur -S --fetch-jobs=lots x 2>&1", out, sizeof out) == 1, "bad --fetch-jobs");
    ASSERT(run("./inkaur -Q --jobs 2 2>&1", out, sizeof out) == 1, "--jobs only for -S");
    ASSERT(run("./inkaur --speedtest -h", out, sizeof out) == 0 && strstr(out, "BuildJobs"),
           "--speedtest help");
    ASSERT(run("./inkaur -Sh", out, sizeof out) == 0 && strstr(out, "--serial") &&
           strstr(out, "--fetch-jobs"), "-Sh documents parallelism");
    ASSERT(run("./inkaur -Syu --repo-download=fast 2>&1", out, sizeof out) == 1 &&
           strstr(out, "--repo-download"), "bad --repo-download");
    ASSERT(run("./inkaur -Syu --mirrors 9 2>&1", out, sizeof out) == 1, "--mirrors 9 refused");
    ASSERT(run("./inkaur --rankmirrors -h", out, sizeof out) == 0 && strstr(out, "lastsync"),
           "--rankmirrors help");
    ASSERT(run("./inkaur --rankmirrors --count 0 2>&1", out, sizeof out) == 1, "--count 0");
    ASSERT(run("./inkaur -S ../.. 2>&1", out, sizeof out) == 1 &&
           strstr(out, "Summary of changes") == NULL, "no summary when nothing ran");

    char cmd[1024];
    const char *evil = make_fixture(1);
    char evild[512];
    snprintf(evild, sizeof evild, "%s", evil);
    snprintf(cmd, sizeof cmd, "NO_COLOR=1 ./inkaur -A '%s' 2>&1", evild);
    ASSERT(run(cmd, out, sizeof out) == 3 && strstr(out, "CRITICAL"), "-A malicious exits 3");
    ASSERT(strchr(out, '\033') == NULL, "audit output has no raw escapes");
    rmtree(evild);

    const char *good = make_fixture(0);
    char goodd[512];
    snprintf(goodd, sizeof goodd, "%s", good);
    snprintf(cmd, sizeof cmd, "./inkaur -A '%s' >/dev/null 2>&1", goodd);
    ASSERT(run(cmd, out, sizeof out) <= 1, "-A clean exits 0/1");
    rmtree(goodd);

    /* approvals refuse non-interactive stdin: piping 'yes' must not work */
    ASSERT(run("./inkaur -A /nonexistent 2>&1", out, sizeof out) == 4, "-A missing dir");
    printf("cli: ok\n");
}
