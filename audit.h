#ifndef AUDIT_H
#define AUDIT_H

#include <stdbool.h>
#include <stdio.h>

/*
 * Static analysis of an AUR repository and of built packages.
 * This is a reading aid with hard stops, not a proof of safety: approving
 * anything HIGH requires typing "yes", CRITICAL requires typing the package
 * name. Every finding also has a plain-language explanation (audit_explain)
 * and the report is summed up in a verdict for people who cannot read
 * shell scripts.
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

/* Plain-language explanation of a finding message for non-experts, or NULL. */
const char *audit_explain(const char *msg);

/* Overall verdict, from the highest severity. */
typedef enum { VERDICT_OK, VERDICT_CAUTION, VERDICT_DANGER, VERDICT_BLOCK } Verdict;
Verdict audit_verdict(const Report *r);
const char *verdict_title(Verdict v);   /* "No warning signs found", ... */
const char *verdict_advice(Verdict v);  /* what to do, in plain words */

/*
 * Plain-language summary: one line per kind of finding (duplicates grouped,
 * most severe first) with what it means and where it was seen.
 */
void report_print_plain(const Report *r, FILE *fp, bool color);

/* AUR metadata used as trust signals (all optional; known=false: none). */
typedef struct {
    bool known;
    long votes;
    double popularity;
    long first_submitted;   /* unix time, 0 unknown */
    long last_modified;     /* unix time, 0 unknown */
    long outofdate;         /* unix time flagged, 0 not flagged */
    bool orphan;
} AurMeta;

/* Adds findings for weak trust signals (brand-new package with no votes,
 * orphaned, out of date). now = current unix time. */
void audit_aur_meta(Report *r, const AurMeta *m, long now);

/* Downloads declared in .SRCINFO and how each one is verified. */
typedef struct {
    size_t total;       /* every source entry */
    size_t remote;      /* downloaded files (not VCS, not signatures) */
    size_t strong;      /* ... with a SHA-256/384/512 or BLAKE2 checksum */
    size_t weak;        /* ... with only MD5/SHA-1/cksum */
    size_t unchecked;   /* ... with no checksum (SKIP) */
    size_t vcs;         /* git/svn/hg/... sources */
    size_t vcs_pinned;  /* ... pinned to an exact commit */
    size_t signatures;  /* .sig/.asc files */
} SourceStats;
void audit_source_stats(const char *srcinfo_text, SourceStats *st);

/*
 * Audit a package source tree.
 *   dir      directory with PKGBUILD/.SRCINFO
 *   is_git   when true, file modes are taken from "git ls-tree HEAD"
 *            (symlinks/submodules) and makepkg build dirs are not skipped
 */
int audit_repo(const char *dir, bool is_git, Report *r);

/* Audit a built package archive (setuid files, sensitive paths, hooks). */
int audit_package_file(const char *pkgfile, Report *r);

/*
 * Full audit of a built package: the file listing above, plus the real
 * install scriptlet inside it (.INSTALL, which runs as root) and its
 * .PKGINFO, whose pkgname/pkgbase must be the expected ones (a package file
 * named foo-1-1 could otherwise install itself as "sudo").
 */
int audit_built_package(const char *pkgfile, const char *expect_name,
                        const char *expect_base, Report *r);

/* Exposed for tests: .PKGINFO identity check. */
void audit_pkginfo_text(Report *r, const char *where, const char *text,
                        const char *expect_name, const char *expect_base);

/* Exposed for tests: scan one text buffer as if it were file `name`. */
void audit_scan_text(Report *r, const char *name, const char *text, size_t len);
/* Exposed for tests: run .SRCINFO structural checks. */
void audit_srcinfo_text(Report *r, const char *text);

#endif /* AUDIT_H */
