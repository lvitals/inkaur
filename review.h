#ifndef REVIEW_H
#define REVIEW_H

#include "audit.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Mandatory human decision about an AUR repository at an exact commit.
 * The worktree must already be checked out at `commit` (git_checkout_exact).
 * A plain-language safety summary always comes first: a verdict with advice,
 * AUR trust signals, how every download is verified, and what the audit
 * found, explained for people who cannot read shell scripts.
 *  - simple mode (default): the code is shown on request ([v])
 *  - full mode: the code is opened in the pager right away
 * The code view:
 *  - first time: every file is shown (line-numbered, control characters and
 *    invisible Unicode made visible) in a pager running in LESSSECURE mode
 *  - later updates: only the diff from the last approved commit is shown
 * The approved commit is recorded in .git/inkaur-reviewed (outside the
 * worktree, so the repository content can never forge it).
 */
typedef struct {
    const char *dir;
    const char *pkgbase;
    const char *commit;
    const char *cache;      /* where the temporary review file is written */
    bool installed;         /* offer "remove from the system" */
    bool full;              /* open the code in the pager without asking */
    const AurMeta *meta;    /* AUR trust signals, or NULL */
} ReviewCtx;

typedef enum {
    DEC_APPROVE,   /* build / install it */
    DEC_SKIP,      /* leave it out, continue with the other packages */
    DEC_REMOVE,    /* uninstall it from the system, continue with the others */
    DEC_VIEW,      /* show the review again */
    DEC_ABORT      /* stop everything */
} Decision;

/*
 * Explicit per-package menu. There is no default answer: Enter re-asks.
 * Approving with HIGH findings additionally requires typing the fixed word
 * "yes", with CRITICAL findings the package name itself. Without an
 * interactive terminal the answer is ABORT. view_label (NULL: no [v]) is the
 * text of the [v] entry.
 */
Decision ask_decision(const char *pkgbase, const char *what, Severity max,
                      bool installed, const char *view_label);

Decision review_package(const ReviewCtx *ctx, const Report *rep);

/* Last approved commit, or false if none. */
bool review_last(const char *dir, char *out, size_t outsz);
int review_record(const char *dir, const char *commit);

/* Show a text through the secure pager (or stdout when not a tty). */
void show_paged(const char *cache, const char *text, size_t len);

#endif /* REVIEW_H */
