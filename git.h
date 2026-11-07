#ifndef GIT_H
#define GIT_H

#include "buf.h"

#include <sys/types.h>

/*
 * git invoked with a fully controlled configuration:
 *  - system and global config ignored (GIT_CONFIG_NOSYSTEM, GIT_CONFIG_GLOBAL=/dev/null)
 *  - only https transport allowed, hooks disabled, fsmonitor disabled
 *  - symlinks checked out as plain files (core.symlinks=false)
 *  - objects fsck'd on transfer, no textconv/external diff, no pager
 */
int git_run(const char *cwd, const char *const args[], StrBuf *capture);

int git_clone(const char *url, const char *dest);
/* Background clone (dir NULL) or fetch of dir; output discarded. Returns pid or -1. */
pid_t git_spawn_clone(const char *url, const char *dest);
pid_t git_spawn_fetch(const char *dir);
int git_fetch(const char *dir);
/* Commit id of rev into out (41/65 bytes buffer). */
int git_rev(const char *dir, const char *rev, char *out, size_t outsz);
/* Force worktree to exactly `commit` and delete every untracked file. */
int git_checkout_exact(const char *dir, const char *commit);
/* Contents of path at commit (blob). */
int git_show_file(const char *dir, const char *commit, const char *path, StrBuf *out);
int git_diff(const char *dir, const char *from, const char *to, StrBuf *out);
int git_origin_url(const char *dir, StrBuf *out);

#endif /* GIT_H */
