#ifndef PACMAN_H
#define PACMAN_H

#include "buf.h"

#include <stdbool.h>
#include <sys/types.h>

/*
 * Thin wrappers over /usr/bin/pacman (no libalpm linkage). Every target is
 * validated by the caller and passed after "--".
 */

/* Options (--config, --root, --dbpath, --sysroot, --arch) added to every
 * pacman call, including inkaur's own queries. The list must outlive use. */
void pacman_set_context(const StrList *ctx);

/* Dependency string satisfied by an installed package (pacman -T). */
bool pacman_dep_satisfied(const char *dep);
/* Dependency string resolvable from the sync repositories (pacman -Sp). */
bool pacman_dep_in_repos(const char *dep);
/* Package name present in a sync database (pacman -Si). */
bool pacman_in_sync_db(const char *name);
/* Installed version of name into out (caller frees), or NULL. */
char *pacman_installed_version(const char *name);
/* Foreign packages ("pacman -Qm"): names[i] / versions[i]. */
int pacman_foreign(StrList *names, StrList *versions);
/* All installed package names, sorted. */
int pacman_installed_names(StrList *out);
/* Values of a pacman.conf directive (pacman-conf <key>), e.g. IgnorePkg. */
int pacman_conf_list(const char *config, const char *key, StrList *out);

/*
 * Visible pacman run: [sudo|doas] pacman <context> <args...> -- <targets...>.
 * Returns pacman's exit status, or -1 if it could not be started.
 */
int pacman_run(bool as_root, const StrList *args, const StrList *targets);
/* true once pacman_run() has started pacman as root. */
bool pacman_ran_as_root(void);

/* Quiet non-root pacman run capturing stdout: pacman <context> <args...>. */
int pacman_capture(const StrList *args, StrBuf *out);

/* --config given on the command line (used by pacman-conf calls), or NULL. */
void pacman_set_config_path(const char *path);
/* Expanded Server URLs of a repository, in pacman's order. */
int pacman_conf_repo_servers(const char *repo, StrList *out);

/*
 * A root pacman running in the background (e.g. pacman -Suw while the AUR
 * is reviewed) holds the database lock. pacman_run(as_root) waits for it
 * first, so foreground transactions never collide with it.
 */
void pacman_bg_set(pid_t pid, const char *log);
/* Root pacman without a terminal: sudo -n / doas -n (never prompts; fails if
 * the credentials are not cached). Output to log_fd. Returns pid or -1. */
pid_t pacman_spawn_root(const StrList *args, int log_fd);
/* sudo -v, so a following background pacman_spawn_root() can run. */
void pacman_refresh_credentials(void);
/* Waits for the background pacman (if any); returns its exit status, 0 if none. */
int pacman_bg_wait(void);

/* Snapshot of the installed packages: sorted "name version" lines. */
int pacman_snapshot(StrList *out);

/*
 * Prints what changed between two snapshots: upgraded, downgraded,
 * installed, removed and (for names in touched) reinstalled packages.
 * Returns the number of changes.
 */
size_t pacman_print_changes(const StrList *before, const StrList *after,
                            const StrList *touched);

#endif /* PACMAN_H */
