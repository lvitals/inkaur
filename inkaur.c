#include "alloc.h"
#include "buf.h"
#include "cli.h"
#include "config.h"
#include "fsutil.h"
#include "install.h"
#include "pacman.h"
#include "mirrors.h"
#include "perf.h"
#include "proc.h"
#include "search.h"
#include "tty.h"
#include "validate.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

/* ------------------------------------------------------------ help */

static void usage_main(void)
{
    puts("usage:  inkaur <operation> [...]\n"
         "operations:\n"
         "    inkaur {-h --help}\n"
         "    inkaur {-V --version}\n"
         "    inkaur {-D --database} <options> <package(s)>\n"
         "    inkaur {-F --files}    [options] [file(s)]\n"
         "    inkaur {-Q --query}    [options] [package(s)]\n"
         "    inkaur {-R --remove}   [options] <package(s)>\n"
         "    inkaur {-S --sync}     [options] [package(s)]\n"
         "    inkaur {-T --deptest}  [options] [package(s)]\n"
         "    inkaur {-U --upgrade}  [options] <file(s)>\n"
         "    inkaur {-A --audit}    <directory>          (inkaur only)\n"
         "    inkaur --speedtest                          (inkaur only)\n"
         "    inkaur --rankmirrors [--all] [--count <n>]  (inkaur only)\n"
         "\nevery pacman operation and option is accepted with the same letters and\n"
         "names. inkaur handles the AUR side and passes the rest to pacman unchanged.\n"
         "\nuse 'inkaur {-h --help}' with an operation for available options");
}

static void usage_common(bool xact, bool upgrade)
{
    if (xact)
        puts("  -d, --nodeps         skip dependency version checks (-dd to skip all checks)\n"
             "      --assume-installed <package=version>\n"
             "                       add a virtual package to satisfy dependencies\n"
             "      --dbonly         only modify database entries, not package files\n"
             "      --noprogressbar  do not show a progress bar when downloading files\n"
             "      --noscriptlet    do not execute the install scriptlet if one exists\n"
             "  -p, --print          print the targets instead of performing the operation\n"
             "      --print-format <string>\n"
             "                       specify how the targets should be printed");
    if (upgrade)
        puts("      --asdeps         install packages as non-explicitly installed\n"
             "      --asexplicit     install packages as explicitly installed\n"
             "      --ignore <pkg>   ignore a package upgrade (repos and AUR; can be used more than once)\n"
             "      --ignoregroup <grp>\n"
             "                       ignore a group upgrade (can be used more than once)\n"
             "      --needed         do not reinstall up to date packages\n"
             "      --overwrite <glob>\n"
             "                       overwrite conflicting files (can be used more than once)\n"
             "  -w, --downloadonly   download packages but do not install/upgrade anything");
    puts("  -b, --dbpath <path>  set an alternate database location\n"
         "  -r, --root <path>    set an alternate installation root\n"
         "  -v, --verbose        be verbose\n"
         "      --arch <arch>    set an alternate architecture\n"
         "      --sysroot        operate on a mounted guest system (root-only)\n"
         "      --cachedir <dir> set an alternate package cache location\n"
         "      --hookdir <dir>  set an alternate hook location\n"
         "      --color <when>   colorize the output (always, never, auto)\n"
         "      --config <path>  set an alternate configuration file\n"
         "      --debug          display debug messages\n"
         "      --gpgdir <path>  set an alternate home directory for GnuPG\n"
         "      --logfile <path> set an alternate log file\n"
         "      --noconfirm      do not ask for any confirmation\n"
         "                       (inkaur security reviews are NEVER skipped)\n"
         "      --confirm        always ask for confirmation\n"
         "      --disable-download-timeout\n"
         "                       use relaxed timeouts for download\n"
         "      --disable-sandbox\n"
         "                       disable the sandbox used for the downloader process");
}

static void usage_sync(void)
{
    puts("usage:  inkaur {-S --sync} [options] [package(s)]\n"
         "options:\n"
         "  -c, --clean          remove old packages from cache directory (-cc for all)\n"
         "                       and build files from the AUR cache (-cc: all clones)\n"
         "  -g, --groups         view all members of a package group\n"
         "                       (-gg to view all groups and members)\n"
         "  -i, --info           view package information (repos and AUR; -ii extended)\n"
         "  -l, --list <repo>    view a list of packages in a repo\n"
         "  -q, --quiet          show less information for query and search\n"
         "  -s, --search <regex> search remote repositories and the AUR\n"
         "  -u, --sysupgrade     upgrade installed packages, repos and AUR (-uu enables downgrades)\n"
         "  -y, --refresh        download fresh package databases from the server\n"
         "                       (-yy to force a refresh even if up to date)\n"
         "inkaur options:\n"
         "      --aur            only the AUR (e.g. -Su --aur: AUR upgrades only)\n"
         "      --repo           only the repositories (plain pacman)\n"
         "      --chroot <dir>   build in a clean devtools chroot (<dir>/root,\n"
         "                       create with: mkarchroot <dir>/root base-devel)\n"
         "parallelism (inkaur; defaults in ~/.config/inkaur/inkaur.conf):\n"
         "      --jobs <n|auto>  AUR packages built at the same time; a package starts\n"
         "                       only after its AUR dependencies are installed\n"
         "      --fetch-jobs <n|auto>\n"
         "                       AUR git repositories cloned/fetched at the same time\n"
         "      --serial         one thing at a time (slow or metered connections,\n"
         "                       slow computers)\n"
         "      --parallel       force parallel mode even if the config says no\n"
         "      --repo-download <auto|pacman|mirrors|no>\n"
         "                       -Syu: download repository packages in the background\n"
         "                       while the AUR is reviewed (pacman -Suw), or split them\n"
         "                       across several mirrors (inkaur; pacman still verifies\n"
         "                       every signature); no = plain pacman -Syu first\n"
         "      --mirrors <n>    mirrors per repository for --repo-download=mirrors\n"
         "                       'auto' accelerates by default and scales with CPUs,\n"
         "                       free RAM and the speed saved by 'inkaur --speedtest'\n"
         "  targets may be prefixed with a repository ('extra/pkg') or 'aur/'");
    usage_common(true, true);
    puts("\nsecurity: every AUR package (and every AUR dependency) is audited and\n"
         "shown for review at an exact git commit before anything is built;\n"
         "updates show only the diff since your last approval. After every\n"
         "transaction inkaur prints a summary of the changes.");
}

static void usage_query(void)
{
    puts("usage:  inkaur {-Q --query} [options] [package(s)]\n"
         "options:\n"
         "  -c, --changelog      view the changelog of a package\n"
         "  -d, --deps           list packages installed as dependencies [filter]\n"
         "  -e, --explicit       list packages explicitly installed [filter]\n"
         "  -g, --groups         view all members of a package group\n"
         "  -i, --info           view package information (-ii for backup files)\n"
         "  -k, --check          check that package files exist (-kk for file properties)\n"
         "  -l, --list           list the files owned by the queried package\n"
         "  -m, --foreign        list installed packages not found in sync db(s) [filter]\n"
         "  -n, --native         list installed packages only found in sync db(s) [filter]\n"
         "  -o, --owns <file>    query the package that owns <file>\n"
         "  -p, --file <package> query a package file instead of the database\n"
         "  -q, --quiet          show less information for query and search\n"
         "  -s, --search <regex> search locally-installed packages for matching strings\n"
         "  -t, --unrequired     list packages not (optionally) required by any\n"
         "                       package (-tt to ignore optdepends) [filter]\n"
         "  -u, --upgrades       list outdated packages, repos and AUR [filter]\n"
         "inkaur options:\n"
         "      --aur            only foreign/AUR packages (same as -m; -Qu: AUR only)\n"
         "      --repo           only native packages (same as -n; -Qu: repos only)");
    usage_common(false, false);
}

static void usage_remove(void)
{
    puts("usage:  inkaur {-R --remove} [options] <package(s)>\n"
         "options:\n"
         "  -c, --cascade        remove packages and all packages that depend on them\n"
         "  -n, --nosave         remove configuration files\n"
         "  -s, --recursive      remove unnecessary dependencies\n"
         "                       (-ss includes explicitly installed dependencies)\n"
         "  -u, --unneeded       remove unneeded packages");
    usage_common(true, false);
}

static void usage_upgrade(void)
{
    puts("usage:  inkaur {-U --upgrade} [options] <file(s)>\n"
         "options:");
    usage_common(true, true);
}

static void usage_database(void)
{
    puts("usage:  inkaur {-D --database} <options> <package(s)>\n"
         "options:\n"
         "      --asdeps         mark packages as non-explicitly installed\n"
         "      --asexplicit     mark packages as explicitly installed\n"
         "  -k, --check          test local database for validity (-kk for sync databases)\n"
         "  -q, --quiet          suppress output of success messages");
    usage_common(false, false);
}

static void usage_files(void)
{
    puts("usage:  inkaur {-F --files} [options] [file(s)]\n"
         "options:\n"
         "  -l, --list           list the files owned by the queried package\n"
         "  -q, --quiet          show less information for query and search\n"
         "  -x, --regex          enable searching using regular expressions\n"
         "  -y, --refresh        download fresh package databases from the server\n"
         "                       (-yy to force a refresh even if up to date)\n"
         "      --machinereadable\n"
         "                       produce machine-readable output");
    usage_common(false, false);
}

static void usage_deptest(void)
{
    puts("usage:  inkaur {-T --deptest} [options] [package(s)]\n"
         "options:");
    usage_common(false, false);
}

static void usage_speedtest(void)
{
    puts("usage:  inkaur --speedtest\n"
         "measures the AUR latency and the download speed of your first pacman\n"
         "mirror, shows CPUs/RAM and pacman's ParallelDownloads, and prints the\n"
         "fetch/build jobs inkaur will use. The result is saved for 'auto' (7 days).\n"
         "\nconfiguration (/etc/inkaur.conf, then ~/.config/inkaur/inkaur.conf):\n"
         "    [options]\n"
         "    Parallel  = auto      # auto | yes | no\n"
         "    BuildJobs = auto      # auto | 1..64\n"
         "    FetchJobs = auto      # auto | 1..64\n"
         "    RepoDownload = auto   # auto | pacman | mirrors | no\n"
         "    Mirrors   = auto      # mirrors per repo for RepoDownload = mirrors\n"
         "    SpeedTest = auto      # auto (use saved result) | always | never");
}

static void usage_rankmirrors(void)
{
    puts("usage:  inkaur --rankmirrors [--all] [--count <n>]\n"
         "tests every Server in " MIRRORLIST_PATH " (downloads core.db, checks\n"
         "how far behind each mirror is via its lastsync file) and ranks them.\n"
         "With confirmation, the fastest up-to-date ones become active and the old\n"
         "entries are kept as comments (backup: mirrorlist.inkaur-bak).\n"
         "  --all          also test the commented (#Server) entries\n"
         "  --count <n>    how many mirrors to activate (default 10)");
}

static void usage_audit(void)
{
    puts("usage:  inkaur {-A --audit} <directory>\n"
         "static security audit of a local PKGBUILD directory / AUR clone.\n"
         "exit status: 0 none/info, 1 warn, 2 high, 3 critical, 4 error");
}

static void version(void)
{
    printf("inkaur %s\n", INKAUR_VERSION);
    puts("Copyright (C) 2026 Leandro V. Catarin\n"
         "built without third-party libraries (C99 + POSIX libc only)");
}

/* ------------------------------------------------------------ helpers */

static int setup_cache(const char *home, char *cache, size_t sz)
{
    char dotcache[PATH_BUF];
    if (!path_join(dotcache, sizeof dotcache, home, ".cache"))
        return -1;
    struct stat st;
    if (stat(dotcache, &st) != 0) {
        if (errno != ENOENT || mkdir(dotcache, 0700) != 0) {
            fprintf(stderr, "error: cannot create %s\n", dotcache);
            return -1;
        }
    } else if (!S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        fprintf(stderr, "error: %s is not a directory owned by you\n", dotcache);
        return -1;
    }
    if (!path_join(cache, sz, dotcache, "inkaur"))
        return -1;
    return ensure_private_dir(cache);
}

static bool bad_target(const char *t)
{
    fputs("error: invalid package name: ", stderr);
    safe_fputs(stderr, t);
    fputc('\n', stderr);
    return false;
}

/* Names ("-" = read from stdin, as in pacman); with repo_prefix also "repo/name". */
static bool targets_are_names(const StrList *t, bool repo_prefix)
{
    for (size_t i = 0; i < t->n; i++) {
        const char *s = t->v[i];
        if (strcmp(s, "-") == 0 || valid_pkgname(s))
            continue;
        const char *sl = repo_prefix ? strchr(s, '/') : NULL;
        if (sl && sl != s && strchr(sl + 1, '/') == NULL && valid_pkgname(sl + 1)) {
            char repo[MAX_NAME_LEN + 1];
            size_t n = (size_t)(sl - s);
            if (n < sizeof repo) {
                memcpy(repo, s, n);
                repo[n] = '\0';
                if (valid_pkgname(repo))
                    continue;
            }
        }
        return bad_target(s);
    }
    return true;
}

/* Base pacman argument list for this command line: --<op> <flags> <options>. */
static void base_args(const Cli *c, StrList *args, bool with_opflags)
{
    char op[32];
    snprintf(op, sizeof op, "--%s", cli_op_name(c->op));
    sl_push(args, op);
    if (with_opflags)
        sl_extend(args, &c->opflags);
    sl_extend(args, &c->common);
    sl_extend(args, &c->xact);
    if (c->needed)
        sl_push(args, "--needed");
    if (c->asdeps)
        sl_push(args, "--asdeps");
    if (c->asexplicit)
        sl_push(args, "--asexplicit");
}

/* Hand the whole command to pacman. */
static int passthrough(const Cli *c, bool as_root, const StrList *extra)
{
    StrList args;
    sl_init(&args);
    base_args(c, &args, true);
    sl_extend(&args, extra);
    int rc = pacman_run(as_root, &args, &c->targets);
    sl_free(&args);
    return rc < 0 ? 1 : rc;
}

static size_t count_ops(const Cli *c, const char *const *names)
{
    size_t n = 0;
    for (size_t i = 0; names[i]; i++)
        n += cli_has(c, names[i]);
    return n;
}

/* ------------------------------------------------------------ operations */

static int op_query(const Cli *c, const Options *o)
{
    bool special = cli_has(c, "owns") || cli_has(c, "file") || cli_has(c, "search");
    if (!special && !targets_are_names(&c->targets, false))
        return 1;
    StrList extra;
    sl_init(&extra);
    if (c->aur_only)
        sl_push(&extra, "--foreign");
    if (c->repo_only)
        sl_push(&extra, "--native");

    /* -Qu: repo updates from pacman, AUR updates from the AUR */
    bool only_u = cli_has(c, "upgrades") && c->targets.n == 0 &&
                  c->opflags.n == cli_count(c, "upgrades") + cli_count(c, "quiet");
    if (!only_u || c->repo_only) {
        int rc = passthrough(c, false, &extra);
        sl_free(&extra);
        return rc;
    }
    sl_free(&extra);
    bool quiet = cli_has(c, "quiet");
    int found = 0;
    if (!c->aur_only) {
        int rc = passthrough(c, false, NULL);
        if (rc == 0)
            found = 1;
        else if (rc != 1)
            return rc;
    }
    int n = cmd_aur_updates(o, quiet);
    if (n < 0)
        return 1;
    return (found || n > 0) ? 0 : 1;
}

static int op_search(const Cli *c)
{
    bool quiet = cli_has(c, "quiet");
    if (!c->repo_only) {
        if (c->targets.n == 0 && c->aur_only) {
            fputs("error: no targets specified\n", stderr);
            return 1;
        }
        for (size_t i = 0; i < c->targets.n; i++)
            if (!valid_search_term(c->targets.v[i])) {
                fputs("error: invalid search term (2-128 printable chars)\n", stderr);
                return 1;
            }
    }
    int found = 0, err = 0;
    if (!c->aur_only) {
        int rc = passthrough(c, false, NULL);
        if (rc == 0)
            found = 1;
        else if (rc != 1)
            err = 1;
    }
    if (!c->repo_only && c->targets.n) {
        if (cmd_search(&c->targets, quiet) == 0)
            found = 1;
    }
    return err ? 1 : found ? 0 : 1;
}

static int op_info(const Cli *c)
{
    if (!targets_are_names(&c->targets, true))
        return 1;
    if (c->targets.n == 0) {
        if (c->aur_only) {
            fputs("error: no targets specified\n", stderr);
            return 1;
        }
        return passthrough(c, false, NULL);
    }
    StrList repo, aur;
    sl_init(&repo);
    sl_init(&aur);
    for (size_t i = 0; i < c->targets.n; i++) {
        const char *t = c->targets.v[i];
        const char *sl = strchr(t, '/');
        if (sl && strncmp(t, "aur/", 4) == 0)
            sl_push(&aur, sl + 1);
        else if (c->repo_only || sl || (!c->aur_only && pacman_in_sync_db(t)))
            sl_push(&repo, t);
        else
            sl_push(&aur, t);
    }
    int rc = 0;
    if (repo.n) {
        StrList args;
        sl_init(&args);
        base_args(c, &args, true);
        int r = pacman_run(false, &args, &repo);
        sl_free(&args);
        if (r != 0)
            rc = 1;
    }
    if (aur.n && cmd_info(&aur) != 0)
        rc = 1;
    sl_free(&repo);
    sl_free(&aur);
    return rc;
}

static int op_sync(const Cli *c, Options *o, const char *home, const PerfConfig *pc)
{
    if (cli_has(c, "print") || cli_has(c, "downloadonly")) {
        if (c->aur_only) {
            fputs("error: --print and --downloadonly apply to repository packages only\n",
                  stderr);
            return 1;
        }
        if (!targets_are_names(&c->targets, true))
            return 1;
        return passthrough(c, !cli_has(c, "print"), NULL);
    }

    static const char *const excl[] = { "search", "info", "list", "groups", "clean", NULL };
    bool upg = cli_has(c, "sysupgrade") || cli_has(c, "refresh");
    if (count_ops(c, excl) + upg > 1) {
        fputs("error: invalid option combination (see 'inkaur -Sh')\n", stderr);
        return 1;
    }
    if (cli_has(c, "search"))
        return op_search(c);
    if (cli_has(c, "info"))
        return op_info(c);
    if (cli_has(c, "list") || cli_has(c, "groups")) {
        if (!targets_are_names(&c->targets, false))
            return 1;
        return passthrough(c, false, NULL);
    }
    if (cli_has(c, "refresh") && !cli_has(c, "sysupgrade")) {
        fputs("error: '-Sy' without '-u' would be a partial upgrade; use -Syu "
              "(or -Syyu to force a refresh of every database)\n", stderr);
        return 1;
    }
    if (cli_has(c, "clean") && c->targets.n) {
        fputs("error: this operation takes no targets\n", stderr);
        return 1;
    }
    bool upgrade = cli_has(c, "sysupgrade");
    if (!upgrade && !cli_has(c, "clean") && c->targets.n == 0) {
        fputs("error: no targets specified (use -h for help)\n", stderr);
        return 1;
    }
    if (c->targets.n && !targets_are_names(&c->targets, true))
        return 1;

    char chroot_real[PATH_BUF];
    if (c->chroot) {
        char rootdir[PATH_BUF];
        if (realpath(c->chroot, chroot_real) == NULL ||
            !path_join(rootdir, sizeof rootdir, chroot_real, "root") || !is_dir_nofollow(rootdir)) {
            fprintf(stderr, "error: --chroot: '%s/root' not found; create it with\n"
                            "   mkarchroot %s/root base-devel\n", c->chroot, c->chroot);
            return 1;
        }
        if (!tool_is_trusted(BIN_MAKECHROOTPKG)) {
            fputs("error: --chroot needs devtools (makechrootpkg)\n", stderr);
            return 1;
        }
        o->chroot = chroot_real;
    }

    char cache[PATH_BUF];
    if (setup_cache(home, cache, sizeof cache) != 0)
        return 1;
    o->cache = cache;
    int lock = acquire_lock(cache);
    if (lock < 0) {
        fputs("error: another inkaur instance is running (lock in cache dir)\n", stderr);
        return 1;
    }

    /* parallelism: config + command line, scaled to this machine/connection */
    PerfFacts facts;
    perf_facts(cache, &facts);
    if (pc->speedtest == 2 && pc->parallel != 0 && upgrade && !c->repo_only) {
        printf(":: Measuring the connection (SpeedTest = always)...\n");
        if (perf_measure(c->config, &facts, true) == 0)
            perf_save_speed(cache, &facts);
    }
    PerfPlan plan;
    perf_decide(pc, &facts, o->chroot != NULL, &plan);
    StrList fetched;
    sl_init(&fetched);
    o->fetch_jobs = plan.fetch_jobs;
    o->build_jobs = plan.build_jobs;
    o->repo_download = plan.repo_download;
    o->mirrors = plan.mirrors;
    o->fetched = &fetched;
    o->perf_note = plan.why;

    int rc;
    if (cli_has(c, "clean")) {
        rc = cmd_clean(o, cli_count(c, "clean") > 1);
    } else if (upgrade) {
        rc = cmd_upgrade(o);
        if (rc == 0 && c->targets.n) {
            Options o2 = *o;
            o2.refresh_level = 0;
            rc = cmd_sync(&c->targets, &o2);
        }
    } else {
        rc = cmd_sync(&c->targets, o);
    }
    close(lock);
    sl_free(&fetched);
    o->fetched = NULL;
    o->perf_note = NULL;
    o->chroot = NULL;
    o->cache = NULL;
    return rc;
}

/* Does this command line change installed packages? (summary afterwards) */
static bool is_transaction(const Cli *c)
{
    if (cli_has(c, "print"))
        return false;
    switch (c->op) {
    case 'R':
    case 'U':
        return !cli_has(c, "downloadonly");
    case 'S': {
        static const char *const ro[] = { "search", "info", "list", "groups", "clean",
                                          "downloadonly", NULL };
        return count_ops(c, ro) == 0;
    }
    default:
        return false;
    }
}

static int dispatch(const Cli *c, Options *o, const char *home, const PerfConfig *pc)
{
    switch (c->op) {
    case 'S':
        return op_sync(c, o, home, pc);
    case 'Q':
        return op_query(c, o);
    case 'R':
        if (c->targets.n == 0) {
            fputs("error: no targets specified (use -h for help)\n", stderr);
            return 1;
        }
        if (!targets_are_names(&c->targets, false))
            return 1;
        return passthrough(c, !cli_has(c, "print"), NULL);
    case 'D':
        if (!targets_are_names(&c->targets, false))
            return 1;
        return passthrough(c, c->asdeps || c->asexplicit, NULL);
    case 'F':
        return passthrough(c, cli_has(c, "refresh"), NULL);
    case 'T':
        for (size_t i = 0; i < c->targets.n; i++) {
            char nm[MAX_NAME_LEN + 1];
            if (!valid_depstring(c->targets.v[i], nm, sizeof nm))
                return !bad_target(c->targets.v[i]);
        }
        return passthrough(c, false, NULL);
    case 'U':
        return passthrough(c, !cli_has(c, "print"), NULL);
    default:
        return 1;
    }
}

int main(int argc, char **argv)
{
    umask(077);

    if (geteuid() == 0 || getuid() != geteuid() || getgid() != getegid()) {
        fputs("error: do not run inkaur as root or setuid; it calls sudo only for pacman\n",
              stderr);
        return 1;
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw == NULL || pw->pw_dir == NULL || pw->pw_dir[0] != '/' || pw->pw_name == NULL) {
        fputs("error: cannot determine your home directory\n", stderr);
        return 1;
    }
    char *home = xstrdup(pw->pw_dir);
    char *user = xstrdup(pw->pw_name);
    proc_set_identity(home, user);
    openlog("inkaur", LOG_PID, LOG_AUTHPRIV);

    Cli c;
    cli_init(&c);
    int rc = 0;
    char err[512];
    if (cli_parse(&c, argc, argv, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        rc = 1;
        goto done;
    }
    if (c.color >= 0)
        tty_set_color(c.color);
    pacman_set_context(&c.ctx);

    if (c.op == 'V') {
        version();
        goto done;
    }
    if (c.help || c.op == 0) {
        switch (c.op) {
        case 'S': usage_sync(); break;
        case 'Q': usage_query(); break;
        case 'R': usage_remove(); break;
        case 'U': usage_upgrade(); break;
        case 'D': usage_database(); break;
        case 'F': usage_files(); break;
        case 'T': usage_deptest(); break;
        case 'A': usage_audit(); break;
        case 'N': usage_speedtest(); break;
        case 'M': usage_rankmirrors(); break;
        default: usage_main(); break;
        }
        if (c.op == 0 && !c.help) {
            fputs("error: no operation specified (use -h for help)\n", stderr);
            rc = 1;
        }
        goto done;
    }
    if (c.op == 'A') {
        if (c.targets.n != 1 || c.seen.n != cli_count(&c, "color")) {
            usage_audit();
            rc = 4;
        } else {
            rc = cmd_audit_dir(c.targets.v[0]);
        }
        goto done;
    }

    PerfConfig pc;
    perf_config_defaults(&pc);
    perf_config_load_all(home, &pc);
    if (c.parallel >= 0)
        pc.parallel = c.parallel;
    if (c.jobs || c.fetch_jobs) {
        int bj = c.jobs ? perf_parse_jobs(c.jobs) : pc.build_jobs;
        int fj = c.fetch_jobs ? perf_parse_jobs(c.fetch_jobs) : pc.fetch_jobs;
        if (bj < 0 || fj < 0) {
            fprintf(stderr, "error: --jobs/--fetch-jobs take 'auto' or 1..%d\n", PERF_MAX_JOBS);
            rc = 1;
            goto done;
        }
        pc.build_jobs = bj;
        pc.fetch_jobs = fj;
        if (c.parallel < 0 && ((c.jobs && bj != 1) || (c.fetch_jobs && fj != 1)))
            pc.parallel = 1;   /* explicit job counts win over 'Parallel = no' */
    }
    if (c.repo_download) {
        int v = perf_parse_repo_download(c.repo_download);
        if (v == -2) {
            fputs("error: --repo-download takes auto, pacman, mirrors or no\n", stderr);
            rc = 1;
            goto done;
        }
        pc.repo_download = v;
    }
    if (c.mirrors) {
        int v = perf_parse_jobs(c.mirrors);
        if (v < 0 || v > 8) {
            fputs("error: --mirrors takes auto or 1..8\n", stderr);
            rc = 1;
            goto done;
        }
        pc.mirrors = v;
    }
    pacman_set_config_path(c.config);
    if (c.op == 'M') {
        int count = c.count ? perf_parse_jobs(c.count) : 10;
        if (count <= 0) {
            fprintf(stderr, "error: --count takes 1..%d\n", PERF_MAX_JOBS);
            rc = 1;
            goto done;
        }
        char cache[PATH_BUF];
        PerfFacts f;
        PerfPlan plan;
        if (setup_cache(home, cache, sizeof cache) != 0) {
            rc = 1;
            goto done;
        }
        perf_facts(cache, &f);
        perf_decide(&pc, &f, false, &plan);
        rc = cmd_rankmirrors(cache, cli_has(&c, "all"), count,
                             plan.fetch_jobs < 4 ? 4 : plan.fetch_jobs, c.noconfirm);
        goto done;
    }
    if (c.op == 'N') {
        char cache[PATH_BUF];
        rc = setup_cache(home, cache, sizeof cache) == 0 ? cmd_speedtest(cache, c.config, &pc) : 1;
        goto done;
    }

    StrList touched;
    sl_init(&touched);
    Options o;
    memset(&o, 0, sizeof o);
    o.noconfirm = c.noconfirm;
    o.needed = c.needed;
    o.asdeps = c.asdeps;
    o.asexplicit = c.asexplicit;
    o.aur_only = c.aur_only;
    o.repo_only = c.repo_only;
    o.upgrade_level = cli_count(&c, "sysupgrade");
    o.refresh_level = cli_count(&c, "refresh");
    o.config = c.config;
    o.common = &c.common;
    o.xact = &c.xact;
    o.ignore = &c.ignore;
    o.touched = &touched;

    /* printed only if pacman actually ran a transaction (not on bad input) */
    StrList before;
    sl_init(&before);
    bool summary = is_transaction(&c) && pacman_snapshot(&before) == 0;

    rc = dispatch(&c, &o, home, &pc);

    if (summary && pacman_ran_as_root()) {
        StrList after;
        sl_init(&after);
        if (pacman_snapshot(&after) == 0)
            pacman_print_changes(&before, &after, &touched);
        sl_free(&after);
    }
    sl_free(&before);
    sl_free(&touched);

done:
    cli_free(&c);
    free(home);
    free(user);
    closelog();
    return rc;
}
