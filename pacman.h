#ifndef PACMAN_H
#define PACMAN_H

#include "buf.h"

#include <stdbool.h>

/*
 * Thin wrappers over /usr/bin/pacman (no libalpm linkage). Every target is
 * validated by the caller and passed after "--".
 */

/* Dependency string satisfied by an installed package (pacman -T). */
bool pacman_dep_satisfied(const char *dep);
/* Dependency string resolvable from the sync repositories (pacman -Sp). */
bool pacman_dep_in_repos(const char *dep);
/* Installed version of name into out (caller frees), or NULL. */
char *pacman_installed_version(const char *name);
/* Foreign packages ("pacman -Qm"): names[i] / versions[i]. */
int pacman_foreign(StrList *names, StrList *versions);
/* All installed package names, sorted. */
int pacman_installed_names(StrList *out);

#endif /* PACMAN_H */
