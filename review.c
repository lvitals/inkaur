#include "review.h"
#include "alloc.h"
#include "buf.h"
#include "config.h"
#include "fsutil.h"
#include "git.h"
#include "proc.h"
#include "tty.h"
#include "validate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static bool reviewed_path(const char *dir, char *buf, size_t sz)
{
    return path_join(buf, sz, dir, ".git/inkaur-reviewed");
}

bool review_last(const char *dir, char *out, size_t outsz)
{
    char p[PATH_BUF];
    if (!reviewed_path(dir, p, sizeof p))
        return false;
    StrBuf b;
    sb_init(&b, 256);
    bool ok = false;
    if (read_file_limited(p, 256, &b) == 0) {
        char *nl = strchr(b.data, '\n');
        if (nl)
            *nl = '\0';
        if (valid_commit(b.data) && strlen(b.data) < outsz) {
            memcpy(out, b.data, strlen(b.data) + 1);
            ok = true;
        }
    }
    sb_free(&b);
    return ok;
}

int review_record(const char *dir, const char *commit)
{
    char p[PATH_BUF];
    if (!reviewed_path(dir, p, sizeof p) || !valid_commit(commit))
        return -1;
    char line[80];
    int n = snprintf(line, sizeof line, "%s\n", commit);
    return write_file_atomic(p, line, (size_t)n);
}

void show_paged(const char *cache, const char *text, size_t len)
{
    if (!isatty(STDOUT_FILENO) || !tool_is_trusted(BIN_LESS)) {
        fwrite(text, 1, len, stdout);
        return;
    }
    char tmpl[PATH_BUF];
    if (!path_join(tmpl, sizeof tmpl, cache, ".review-XXXXXX")) {
        fwrite(text, 1, len, stdout);
        return;
    }
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        fwrite(text, 1, len, stdout);
        return;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    close(fd);

    /* LESSSECURE disables shell escapes, editors, pipes and lesskey files;
     * LESSOPEN/LESSCLOSE never reach the child (environment allow-list). */
    static const char *const env[] = { "LESSSECURE=1", "LESSHISTFILE=-", "LESSCHARSET=utf-8", NULL };
    const char *argv[] = { BIN_LESS, "--", tmpl, NULL };
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.extra_env = env;
    proc_run(argv, &o);
    unlink(tmpl);
}

static void append_numbered(StrBuf *out, const char *text, size_t len)
{
    StrBuf clean;
    sb_init(&clean, 0);
    sanitize_append(&clean, text, len, true);
    long ln = 1;
    const char *p = clean.data;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        sb_printf(out, "%5ld | ", ln++);
        sb_append(out, p, n);
        sb_putc(out, '\n');
        if (!e)
            break;
        p = e + 1;
    }
    sb_free(&clean);
}

static int file_rank(const char *path)
{
    if (strcmp(path, "PKGBUILD") == 0)
        return 0;
    size_t n = strlen(path);
    if (n > 8 && strcmp(path + n - 8, ".install") == 0)
        return 1;
    if (strcmp(path, ".SRCINFO") == 0)
        return 3;
    return 2;
}

static int cmp_rank(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int d = file_rank(x) - file_rank(y);
    return d ? d : strcmp(x, y);
}

static void build_full_view(const ReviewCtx *c, StrBuf *view)
{
    StrBuf ls;
    sb_init(&ls, MAX_GIT_OUTPUT);
    const char *a[] = { "ls-tree", "-r", "-z", "--name-only", "--full-tree", "HEAD", NULL };
    StrList files;
    sl_init(&files);
    if (git_run(c->dir, a, &ls) == 0) {
        const char *p = ls.data, *end = ls.data + ls.len;
        while (p < end) {
            size_t n = strnlen(p, (size_t)(end - p));
            if (n)
                sl_push_owned(&files, xstrndup(p, n));
            p += n + 1;
        }
    }
    sb_free(&ls);
    if (files.n > 1)
        qsort(files.v, files.n, sizeof(char *), cmp_rank);

    sb_printf(view, "==== %s @ %s — FULL REVIEW (%zu files) ====\n"
                    "Read every file. PKGBUILD functions run as YOU; *.install runs as ROOT.\n\n",
              c->pkgbase, c->commit, files.n);
    for (size_t i = 0; i < files.n; i++) {
        char path[PATH_BUF];
        sb_puts(view, "======================================================================\n");
        sb_puts(view, "== ");
        sanitize_append(view, files.v[i], strlen(files.v[i]), false);
        sb_puts(view, "\n======================================================================\n");
        if (!path_join(path, sizeof path, c->dir, files.v[i])) {
            sb_puts(view, "[path too long]\n\n");
            continue;
        }
        StrBuf f;
        sb_init(&f, MAX_SCAN_FILE_BYTES);
        if (read_file_limited(path, MAX_SCAN_FILE_BYTES, &f) != 0) {
            sb_puts(view, "[not a regular file or too large — inspect manually]\n\n");
        } else if (memchr(f.data, '\0', f.len) != NULL) {
            sb_printf(view, "[binary file, %zu bytes, not shown]\n\n", f.len);
        } else {
            append_numbered(view, f.data, f.len);
            sb_putc(view, '\n');
        }
        sb_free(&f);
    }
    sl_free(&files);
}

Decision ask_decision(const char *pkgbase, const char *what, Severity max,
                      bool installed, const char *view_label)
{
    if (!stdin_is_tty()) {
        fprintf(stderr, "error: a decision about %s requires an interactive terminal; aborting\n",
                pkgbase);
        return DEC_ABORT;
    }
    Report tmp;
    report_init(&tmp);
    tmp.max = max;
    Verdict v = audit_verdict(&tmp);
    for (;;) {
        printf("\n%s:: %s%s %s\n", C_BOLD, pkgbase, C_END, what);
        printf("   safety check: %s%s%s (%s)\n",
               max >= SEV_HIGH ? C_RED : (max == SEV_WARN ? C_YEL : C_GREEN),
               verdict_title(v), C_END, severity_name(max));
        printf("   [a] approve%s\n", max >= SEV_HIGH ? "" : "  (build and install it)");
        printf("   [s] skip %s and continue with the other packages%s%s\n", pkgbase,
               installed ? " (installed version is kept)" : "",
               max >= SEV_HIGH ? "  <- recommended" : "");
        if (installed)
            printf("   [r] remove %s from the system and continue with the others\n", pkgbase);
        if (view_label)
            printf("   [v] %s\n", view_label);
        printf("   [x] abort everything\n");
        discard_typeahead();
        char buf[MAX_NAME_LEN + 2];
        do {   /* bare ENTER just asks again */
            printf("%schoice:%s ", C_BLUE, C_END);
            if (!read_line(buf, sizeof buf))
                return DEC_ABORT;
        } while (buf[0] == '\0');
        char c = buf[0];
        if (buf[0] != '\0' && buf[1] != '\0')
            c = '?';   /* only single-letter answers */

        switch (c) {
        case 'a': case 'A':
            if (max < SEV_HIGH)
                return DEC_APPROVE;
            if (max >= SEV_CRIT) {
                printf("%s   %s has CRITICAL findings. To install it anyway, type the package "
                       "name (%s);\n   anything else goes back:%s ", C_RED, pkgbase, pkgbase, C_END);
                discard_typeahead();
                if (read_line(buf, sizeof buf) && strcmp(buf, pkgbase) == 0)
                    return DEC_APPROVE;
            } else {
                printf("%s   %s has %s findings. Type the word yes to confirm approval "
                       "(anything else goes back):%s ", C_RED, pkgbase, severity_name(max), C_END);
                discard_typeahead();
                if (read_line(buf, sizeof buf) && strcmp(buf, "yes") == 0)
                    return DEC_APPROVE;
            }
            printf("   approval not confirmed\n");
            continue;
        case 's': case 'S':
            return DEC_SKIP;
        case 'r': case 'R':
            if (installed)
                return DEC_REMOVE;
            break;
        case 'v': case 'V':
            if (view_label)
                return DEC_VIEW;
            break;
        case 'x': case 'X':
            return DEC_ABORT;
        default:
            break;
        }
        printf("   invalid choice\n");
    }
}

static const char *decision_name(Decision d)
{
    switch (d) {
    case DEC_APPROVE: return "APPROVED";
    case DEC_SKIP:    return "SKIPPED";
    case DEC_REMOVE:  return "REMOVE";
    case DEC_VIEW:    return "VIEW";
    case DEC_ABORT:   return "ABORTED";
    }
    return "?";
}

static void fmt_date(char *buf, size_t sz, long t)
{
    time_t tt = (time_t)t;
    struct tm tm;
    if (t <= 0 || gmtime_r(&tt, &tm) == NULL || strftime(buf, sz, "%Y-%m-%d", &tm) == 0)
        snprintf(buf, sz, "unknown");
}

static void print_about(const AurMeta *m)
{
    if (m == NULL || !m->known)
        return;
    char first[32], last[32];
    fmt_date(first, sizeof first, m->first_submitted);
    fmt_date(last, sizeof last, m->last_modified);
    printf("   About it (AUR): %ld vote%s, popularity %.2f, first published %s, "
           "last updated %s%s%s\n",
           m->votes, m->votes == 1 ? "" : "s", m->popularity, first, last,
           m->orphan ? ", NO MAINTAINER" : "", m->outofdate ? ", flagged out of date" : "");
}

/* How the downloads are verified, in plain words. */
static void print_sources(const char *dir)
{
    char path[PATH_BUF];
    StrBuf b;
    sb_init(&b, MAX_SCAN_FILE_BYTES);
    if (!path_join(path, sizeof path, dir, ".SRCINFO") ||
        read_file_limited(path, MAX_SCAN_FILE_BYTES, &b) != 0) {
        sb_free(&b);
        return;
    }
    SourceStats st;
    audit_source_stats(b.data, &st);
    sb_free(&b);
    if (st.remote == 0 && st.vcs == 0) {
        printf("   Downloads: none (everything is in the reviewed repository)\n");
        return;
    }
    if (st.remote) {
        if (st.strong == st.remote)
            printf("   Downloads: %s%zu file%s, all checked against a SHA-256 (or stronger) "
                   "fingerprint%s\n", C_GREEN, st.remote, st.remote == 1 ? "" : "s", C_END);
        else
            printf("   Downloads: %zu file%s: %zu with a strong fingerprint, %s%zu weak, "
                   "%zu NOT checked%s\n", st.remote, st.remote == 1 ? "" : "s", st.strong,
                   st.unchecked ? C_YEL : "", st.weak, st.unchecked, st.unchecked ? C_END : "");
        if (st.signatures)
            printf("              plus %zu signature file%s checked by makepkg\n",
                   st.signatures, st.signatures == 1 ? "" : "s");
    }
    if (st.vcs) {
        if (st.vcs_pinned == st.vcs)
            printf("   Code from %zu developer repositor%s, pinned to an exact version\n",
                   st.vcs, st.vcs == 1 ? "y" : "ies");
        else
            printf("   Code from %zu developer repositor%s: %s%zu always take%s the newest, "
                   "unreviewed code%s\n", st.vcs, st.vcs == 1 ? "y" : "ies", C_YEL,
                   st.vcs - st.vcs_pinned, st.vcs - st.vcs_pinned == 1 ? "s" : "", C_END);
    }
}

/* "N files changed, +A -B lines" of a unified diff. */
static void diff_stats(const char *d, size_t len, size_t *files, size_t *add, size_t *del)
{
    *files = *add = *del = 0;
    const char *p = d, *end = d + len;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        size_t n = e ? (size_t)(e - p) : (size_t)(end - p);
        if (n >= 11 && strncmp(p, "diff --git ", 11) == 0)
            (*files)++;
        else if (n >= 1 && p[0] == '+' && !(n >= 3 && strncmp(p, "+++", 3) == 0))
            (*add)++;
        else if (n >= 1 && p[0] == '-' && !(n >= 3 && strncmp(p, "---", 3) == 0))
            (*del)++;
        if (!e)
            break;
        p = e + 1;
    }
}

static void print_safety_summary(const ReviewCtx *c, const Report *rep, bool update,
                          size_t files, size_t add, size_t del)
{
    Verdict v = audit_verdict(rep);
    const char *col = v >= VERDICT_DANGER ? C_RED : v == VERDICT_CAUTION ? C_YEL : C_GREEN;
    printf("\n%s:: Safety check of %s @ %.12s%s\n", C_BOLD, c->pkgbase, c->commit, C_END);
    printf("   %s%s%s\n   %s\n\n", col, verdict_title(v), C_END, verdict_advice(v));
    print_about(c->meta);
    print_sources(c->dir);
    if (update)
        printf("   Update: %zu file%s changed (+%zu / -%zu lines) since the version you "
               "approved before\n", files, files == 1 ? "" : "s", add, del);
    else
        printf("   First install: the whole package is new to you\n");
    printf("\n   What the automatic check found (! serious, * check, - note):\n");
    report_print_plain(rep, stdout, use_color());
    printf("\n   PKGBUILD runs as you while building; an install script runs as administrator.\n"
           "   Reading the code is optional: you can decide from this summary, or choose\n"
           "   [v] to read the PKGBUILD yourself before deciding.\n");
}

Decision review_package(const ReviewCtx *c, const Report *rep)
{
    char prev[80];
    bool have_prev = review_last(c->dir, prev, sizeof prev);

    if (have_prev && strcmp(prev, c->commit) == 0 && rep->max < SEV_HIGH) {
        printf("%s::%s %s @ %.12s was already reviewed and approved by you; no changes.\n",
               C_BLUE, C_END, c->pkgbase, c->commit);
        return DEC_APPROVE;
    }

    StrBuf view;
    sb_init(&view, 0);
    StrBuf diff;
    sb_init(&diff, MAX_GIT_OUTPUT);
    bool update = false;
    size_t files = 0, add = 0, del = 0;
    if (have_prev && strcmp(prev, c->commit) != 0 && git_diff(c->dir, prev, c->commit, &diff) == 0) {
        update = true;
        diff_stats(diff.data, diff.len, &files, &add, &del);
        sb_printf(&view, "==== %s: changes since your last approved review ====\n"
                         "==== %.12s -> %.12s ====\n\n", c->pkgbase, prev, c->commit);
        sanitize_append(&view, diff.data, diff.len, true);
    } else {
        build_full_view(c, &view);
    }
    sb_free(&diff);

    print_safety_summary(c, rep, update, files, add, del);

    bool show = c->full;
    if (show) {
        printf("\n%s::%s Opening the code of %s (q to leave the pager)...\n", C_BLUE, C_END,
               c->pkgbase);
        if (stdin_is_tty() && isatty(STDOUT_FILENO)) {
            printf("   press ENTER to continue ");
            char tmp[8];
            read_line(tmp, sizeof tmp);
        }
    }

    Decision d;
    for (;;) {
        if (show) {
            show_paged(c->cache, view.data, view.len);
            if (rep->n) {
                printf("\n%s:: Technical audit findings for %s%s\n", C_BOLD, c->pkgbase, C_END);
                report_print(rep, stdout, use_color());
            }
        }
        char what[128];
        snprintf(what, sizeof what, "@ %.12s: build and install this version?", c->commit);
        d = ask_decision(c->pkgbase, what, rep->max, c->installed,
                         update ? "review what changed in the build files (optional)"
                                : "review the build files: PKGBUILD and scripts (optional)");
        if (d != DEC_VIEW)
            break;
        show = true;
    }
    sb_free(&view);

    syslog(LOG_AUTHPRIV | LOG_NOTICE, "review %s pkgbase=%s commit=%s max_severity=%s findings=%zu "
           "mode=%s", decision_name(d), c->pkgbase, c->commit, severity_name(rep->max), rep->n,
           c->full ? "full" : "simple");
    if (d == DEC_APPROVE && review_record(c->dir, c->commit) != 0)
        fprintf(stderr, "warning: could not record review state\n");
    return d;
}
