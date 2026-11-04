#include "pacman.h"
#include "alloc.h"
#include "config.h"
#include "proc.h"
#include "validate.h"

#include <stdlib.h>
#include <string.h>

static int run_quiet(const char *const argv[], StrBuf *cap)
{
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.stdin_null = true;
    o.stderr_null = true;
    if (cap)
        o.capture = cap;
    else
        o.stdout_null = true;
    return proc_run(argv, &o);
}

bool pacman_dep_satisfied(const char *dep)
{
    const char *argv[] = { BIN_PACMAN, "-T", "--", dep, NULL };
    return run_quiet(argv, NULL) == 0;
}

bool pacman_dep_in_repos(const char *dep)
{
    const char *argv[] = { BIN_PACMAN, "-Sp", "--noconfirm",
                           "--print-format", "%n", "--", dep, NULL };
    return run_quiet(argv, NULL) == 0;
}

char *pacman_installed_version(const char *name)
{
    StrBuf out;
    sb_init(&out, 4096);
    const char *argv[] = { BIN_PACMAN, "-Q", "--", name, NULL };
    char *ver = NULL;
    if (run_quiet(argv, &out) == 0) {
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

int pacman_foreign(StrList *names, StrList *versions)
{
    StrBuf out;
    sb_init(&out, MAX_GIT_OUTPUT);
    const char *argv[] = { BIN_PACMAN, "-Qm", NULL };
    int rc = run_quiet(argv, &out);
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
        if (valid_pkgname(lines.v[i]) && valid_version(sp + 1)) {
            sl_push(names, lines.v[i]);
            sl_push(versions, sp + 1);
        }
    }
    sl_free(&lines);
    sb_free(&out);
    return 0;
}

int pacman_installed_names(StrList *out)
{
    StrBuf buf;
    sb_init(&buf, MAX_GIT_OUTPUT);
    const char *argv[] = { BIN_PACMAN, "-Qq", NULL };
    int rc = run_quiet(argv, &buf);
    if (rc == 0) {
        sl_split_lines(out, buf.data);
        sl_sort(out);
    }
    sb_free(&buf);
    return rc == 0 ? 0 : -1;
}
