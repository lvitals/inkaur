#ifndef PACMAN_H
#define PACMAN_H

#include "buf.h"

#include <stdbool.h>

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
