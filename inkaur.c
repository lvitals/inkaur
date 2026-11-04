#include "alloc.h"
#include "buf.h"
#include "config.h"
#include "fsutil.h"
#include "install.h"
#include "proc.h"
#include "search.h"
#include "validate.h"

#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

static void usage_main(void)
{
    puts("usage:  inkaur <operation> [...]\n"
         "operations:\n"
         "    inkaur {-h --help}\n"
         "    inkaur {-V --version}\n"
         "    inkaur {-S --sync}     [options] [package(s)]\n"
         "    inkaur {-Q --query}    [options] [package(s)]\n"
         "    inkaur {-R --remove}   [options] <package(s)>\n"
         "    inkaur {-A --audit}    <directory>\n"
         "\nuse 'inkaur {-h --help}' with an operation for available options");
}

static void usage_sync(void)
{
    puts("usage:  inkaur {-S --sync} [options] [package(s)]\n"
         "options:\n"
         "  -s, --search       search the AUR (name and description)\n"
         "  -i, --info         show AUR package information\n"
         "  -u, --sysupgrade   upgrade installed AUR packages (-Su: AUR only)\n"
         "  -y, --refresh      with -u: run 'pacman -Syu' first (-Syu)\n"
         "  -c, --clean        remove build files from the cache (-cc: everything)\n"
         "      --needed       skip targets that are already up to date\n"
         "      --noconfirm    do not ask pacman/makepkg questions\n"
         "                     (security reviews are NEVER skipped)\n"
         "      --chroot <dir> build in a clean devtools chroot (<dir>/root,\n"
         "                     create with: mkarchroot <dir>/root base-devel)\n"
         "  prefix a target with 'aur/' to skip the repositories\n"
         "\nsecurity: every AUR package (and every AUR dependency) is audited and\n"
         "shown for review at an exact git commit before anything is built;\n"
         "updates show only the diff since your last approval.");
}

static void usage_query(void)
{
    puts("usage:  inkaur {-Q --query} [options] [package(s)]\n"
         "lists foreign (AUR) packages, like 'pacman -Qm'\n"
         "options:\n"
         "  -d, --deps         installed as dependencies\n"
         "  -e, --explicit     explicitly installed\n"
         "  -t, --unrequired   not required by any package\n"
         "  -q, --quiet        names only");
}

static void usage_remove(void)
{
    puts("usage:  inkaur {-R --remove} [options] <package(s)>\n"
         "options (passed to pacman):\n"
         "  -n, --nosave       remove configuration files\n"
         "  -s, --recursive    remove unneeded dependencies\n"
         "  -c, --cascade      remove packages that depend on the targets\n"
         "  -u, --unneeded     remove only unneeded targets");
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

static bool letters_ok(const char *s, const char *allowed)
{
    for (; *s; s++)
        if (strchr(allowed, *s) == NULL)
            return false;
    return true;
}

static size_t count_char(const char *s, char c)
{
    size_t n = 0;
    for (; *s; s++)
        n += (*s == c);
    return n;
}

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

static int pass_to_pacman(char op, const char *letters, const StrList *targets, bool noconfirm)
{
    for (size_t i = 0; i < targets->n; i++)
        if (!valid_pkgname(targets->v[i])) {
            fputs("error: invalid package name: ", stderr);
            safe_fputs(stderr, targets->v[i]);
            fputc('\n', stderr);
            return 1;
        }
    char flag[16];
    if (op == 'Q')
        snprintf(flag, sizeof flag, "-Qm%s", letters);
    else
        snprintf(flag, sizeof flag, "-R%s", letters);

    const char **argv = xcalloc(targets->n + 8, sizeof(char *));
    size_t k = 0;
    if (op == 'R') {
        const char *elev = privilege_elevator();
        if (elev == NULL) {
            fprintf(stderr, "error: sudo or doas is required\n");
            free(argv);
            return 1;
        }
        argv[k++] = elev;
    }
    argv[k++] = BIN_PACMAN;
    argv[k++] = flag;
    if (op == 'R' && noconfirm)
        argv[k++] = "--noconfirm";
    argv[k++] = "--";
    for (size_t i = 0; i < targets->n; i++)
        argv[k++] = targets->v[i];
    argv[k] = NULL;
    ProcOpts o;
    memset(&o, 0, sizeof o);
    int rc = proc_run(argv, &o);
    free(argv);
    return rc < 0 ? 1 : rc;
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

    char op = 0;
    char letters[32] = "";
    size_t nl = 0;
    bool help = false, end_opts = false;
    Options opt;
    memset(&opt, 0, sizeof opt);
    StrList targets;
    sl_init(&targets);

#define ADD_LETTER(c) do { if (nl + 1 >= sizeof letters) goto bad_args; \
                           letters[nl++] = (c); letters[nl] = '\0'; } while (0)

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (end_opts || a[0] != '-' || a[1] == '\0') {
            sl_push(&targets, a);
        } else if (strcmp(a, "--") == 0) {
            end_opts = true;
        } else if (a[1] == '-') {
            const char *l = a + 2;
            if (strcmp(l, "help") == 0) help = true;
            else if (strcmp(l, "version") == 0) { if (op) goto bad_args; op = 'V'; }
            else if (strcmp(l, "sync") == 0) { if (op) goto bad_args; op = 'S'; }
            else if (strcmp(l, "query") == 0) { if (op) goto bad_args; op = 'Q'; }
            else if (strcmp(l, "remove") == 0) { if (op) goto bad_args; op = 'R'; }
            else if (strcmp(l, "audit") == 0) { if (op) goto bad_args; op = 'A'; }
            else if (strcmp(l, "noconfirm") == 0) opt.noconfirm = true;
            else if (strcmp(l, "needed") == 0) opt.needed = true;
            else if (strncmp(l, "chroot=", 7) == 0) opt.chroot = l + 7;
            else if (strcmp(l, "chroot") == 0 && i + 1 < argc) opt.chroot = argv[++i];
            else if (strcmp(l, "search") == 0) ADD_LETTER('s');
            else if (strcmp(l, "info") == 0) ADD_LETTER('i');
            else if (strcmp(l, "sysupgrade") == 0) ADD_LETTER('u');
            else if (strcmp(l, "refresh") == 0) ADD_LETTER('y');
            else if (strcmp(l, "clean") == 0) ADD_LETTER('c');
            else if (strcmp(l, "deps") == 0) ADD_LETTER('d');
            else if (strcmp(l, "explicit") == 0) ADD_LETTER('e');
            else if (strcmp(l, "unrequired") == 0) ADD_LETTER('t');
            else if (strcmp(l, "quiet") == 0) ADD_LETTER('q');
            else if (strcmp(l, "nosave") == 0) ADD_LETTER('n');
            else if (strcmp(l, "recursive") == 0) ADD_LETTER('s');
            else if (strcmp(l, "cascade") == 0) ADD_LETTER('c');
            else if (strcmp(l, "unneeded") == 0) ADD_LETTER('u');
            else {
                fprintf(stderr, "error: unknown option '%s'\n", a);
                goto fail;
            }
        } else {
            for (const char *c = a + 1; *c; c++) {
                if (*c == 'h') {
                    help = true;
                } else if (strchr("SQRAV", *c) && op == 0) {
                    op = *c;
                } else if (op && *c >= 'a' && *c <= 'z') {
                    ADD_LETTER(*c);
                } else {
                    goto bad_args;
                }
            }
        }
    }

    int rc = 0;
    if (op == 'V') {
        version();
        goto done;
    }
    if (help || op == 0) {
        switch (op) {
        case 'S': usage_sync(); break;
        case 'Q': usage_query(); break;
        case 'R': usage_remove(); break;
        case 'A': usage_audit(); break;
        default: usage_main(); break;
        }
        rc = (op == 0 && !help) ? 1 : 0;
        goto done;
    }

    if (op == 'A') {
        if (targets.n != 1 || nl) {
            usage_audit();
            rc = 4;
        } else {
            rc = cmd_audit_dir(targets.v[0]);
        }
        goto done;
    }
    if (op == 'Q') {
        if (!letters_ok(letters, "deqt"))
            goto bad_args;
        rc = pass_to_pacman('Q', letters, &targets, false);
        goto done;
    }
    if (op == 'R') {
        if (!letters_ok(letters, "nscu") || targets.n == 0)
            goto bad_args;
        rc = pass_to_pacman('R', letters, &targets, opt.noconfirm);
        goto done;
    }

    /* op == 'S' */
    if (!letters_ok(letters, "siyuc"))
        goto bad_args;
    bool s = strchr(letters, 's'), i = strchr(letters, 'i');
    bool y = strchr(letters, 'y'), u = strchr(letters, 'u');
    size_t c = count_char(letters, 'c');
    if ((s + i + (u || y) + (c > 0)) > 1)
        goto bad_args;
    if (s) { rc = cmd_search(&targets); goto done; }
    if (i) { rc = cmd_info(&targets); goto done; }
    if (y && !u) {
        fputs("error: '-Sy' without '-u' would be a partial upgrade; use -Syu\n", stderr);
        goto fail;
    }
    if ((u || c) && targets.n) {
        fputs("error: this operation takes no targets\n", stderr);
        goto fail;
    }
    if (!u && !c && targets.n == 0) {
        fputs("error: no targets specified\n", stderr);
        goto fail;
    }

    char chroot_real[PATH_BUF];
    if (opt.chroot) {
        char rootdir[PATH_BUF];
        if (realpath(opt.chroot, chroot_real) == NULL ||
            !path_join(rootdir, sizeof rootdir, chroot_real, "root") || !is_dir_nofollow(rootdir)) {
            fprintf(stderr, "error: --chroot: '%s/root' not found; create it with\n"
                            "   mkarchroot %s/root base-devel\n", opt.chroot, opt.chroot);
            goto fail;
        }
        if (!tool_is_trusted(BIN_MAKECHROOTPKG)) {
            fputs("error: --chroot needs devtools (makechrootpkg)\n", stderr);
            goto fail;
        }
        opt.chroot = chroot_real;
    }

    char cache[PATH_BUF];
    if (setup_cache(home, cache, sizeof cache) != 0)
        goto fail;
    opt.cache = cache;
    int lock = acquire_lock(cache);
    if (lock < 0) {
        fputs("error: another inkaur instance is running (lock in cache dir)\n", stderr);
        goto fail;
    }

    if (c)
        rc = cmd_clean(&opt, c > 1);
    else if (u)
        rc = cmd_upgrade(&opt, y);
    else
        rc = cmd_sync(&targets, &opt);
    close(lock);
    goto done;

bad_args:
    fputs("error: invalid option combination (see 'inkaur -h')\n", stderr);
fail:
    rc = 1;
done:
    sl_free(&targets);
    free(home);
    free(user);
    closelog();
    return rc;
}
