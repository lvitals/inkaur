#ifndef AUDIT_H
#define AUDIT_H

#include <stdbool.h>
#include <stdio.h>

/*
 * Static analysis of an AUR repository and of built packages.
 * This is a reading aid with hard stops, not a verdict: anything HIGH or
 * CRITICAL forces the user to type the package name to proceed.
 */
typedef enum { SEV_INFO = 0, SEV_WARN = 1, SEV_HIGH = 2, SEV_CRIT = 3 } Severity;

typedef struct {
    Severity sev;
    char *file;
    long line;        /* 0 = whole file */
    char *msg;
    char *excerpt;    /* may be NULL; sanitized on print */
} Finding;

typedef struct {
    Finding *f;
    size_t n, cap;
    Severity max;
} Report;

void report_init(Report *r);
void report_add(Report *r, Severity s, const char *file, long line,
                const char *msg, const char *excerpt);
void report_print(const Report *r, FILE *fp, bool color);
void report_free(Report *r);
const char *severity_name(Severity s);

/*
 * Audit a package source tree.
 *   dir      directory with PKGBUILD/.SRCINFO
 *   is_git   when true, file modes are taken from "git ls-tree HEAD"
 *            (symlinks/submodules) and makepkg build dirs are not skipped
 */
int audit_repo(const char *dir, bool is_git, Report *r);

/* Audit a built package archive (setuid files, sensitive paths, hooks). */
int audit_package_file(const char *pkgfile, Report *r);

/* Exposed for tests: scan one text buffer as if it were file `name`. */
void audit_scan_text(Report *r, const char *name, const char *text, size_t len);
/* Exposed for tests: run .SRCINFO structural checks. */
void audit_srcinfo_text(Report *r, const char *text);

#endif /* AUDIT_H */
