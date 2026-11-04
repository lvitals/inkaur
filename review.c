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
                      bool installed, bool can_view)
{
    if (!stdin_is_tty()) {
        fprintf(stderr, "error: a decision about %s requires an interactive terminal; aborting\n",
                pkgbase);
        return DEC_ABORT;
    }
    for (;;) {
        printf("\n%s:: %s%s %s — highest severity: %s%s%s\n", C_BOLD, pkgbase, C_END, what,
               max >= SEV_HIGH ? C_RED : (max == SEV_WARN ? C_YEL : C_GREEN),
               severity_name(max), C_END);
        printf("   [a] approve\n");
        printf("   [s] skip %s and continue with the other packages%s\n", pkgbase,
               installed ? " (installed version is kept)" : "");
        if (installed)
            printf("   [r] remove %s from the system and continue with the others\n", pkgbase);
        if (can_view)
            printf("   [v] show the review again\n");
        printf("   [x] abort everything\n");
        printf("%schoice:%s ", C_BLUE, C_END);

        char buf[64];
        if (!read_line(buf, sizeof buf))
            return DEC_ABORT;
        char c = buf[0];
        if (buf[0] != '\0' && buf[1] != '\0')
            c = '?';   /* only single-letter answers */

        switch (c) {
        case 'a': case 'A':
            if (max < SEV_HIGH)
                return DEC_APPROVE;
            printf("%s   %s has %s findings. Type the word yes to confirm approval "
                   "(anything else goes back):%s ", C_RED, pkgbase, severity_name(max), C_END);
            if (read_line(buf, sizeof buf) && strcmp(buf, "yes") == 0)
                return DEC_APPROVE;
            break;
        case 's': case 'S':
            return DEC_SKIP;
        case 'r': case 'R':
            if (installed)
                return DEC_REMOVE;
            break;
        case 'v': case 'V':
            if (can_view)
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
    if (have_prev && strcmp(prev, c->commit) != 0 && git_diff(c->dir, prev, c->commit, &diff) == 0) {
        sb_printf(&view, "==== %s: changes since your last approved review ====\n"
                         "==== %.12s -> %.12s ====\n\n", c->pkgbase, prev, c->commit);
        sanitize_append(&view, diff.data, diff.len, true);
    } else {
        build_full_view(c, &view);
    }
    sb_free(&diff);

    printf("\n%s:: Security audit of %s @ %.12s%s\n", C_BOLD, c->pkgbase, c->commit, C_END);
    report_print(rep, stdout, use_color());

    printf("\n%s::%s Opening review of %s (q to leave the pager)...\n", C_BLUE, C_END, c->pkgbase);
    if (stdin_is_tty() && isatty(STDOUT_FILENO)) {
        printf("   press ENTER to continue ");
        char tmp[8];
        read_line(tmp, sizeof tmp);
    }

    Decision d;
    for (;;) {
        show_paged(c->cache, view.data, view.len);
        if (rep->n) {
            printf("\n%s:: Audit findings for %s%s\n", C_BOLD, c->pkgbase, C_END);
            report_print(rep, stdout, use_color());
        }
        char what[128];
        snprintf(what, sizeof what, "@ %.12s: build this commit?", c->commit);
        d = ask_decision(c->pkgbase, what, rep->max, c->installed, true);
        if (d != DEC_VIEW)
            break;
    }
    sb_free(&view);

    syslog(LOG_AUTHPRIV | LOG_NOTICE, "review %s pkgbase=%s commit=%s max_severity=%s findings=%zu",
           decision_name(d), c->pkgbase, c->commit, severity_name(rep->max), rep->n);
    if (d == DEC_APPROVE && review_record(c->dir, c->commit) != 0)
        fprintf(stderr, "warning: could not record review state\n");
    return d;
}
