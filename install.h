#ifndef INSTALL_H
#define INSTALL_H

#include "buf.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool noconfirm;        /* only for pacman/makepkg prompts; reviews always ask */
    bool needed;
    bool asdeps, asexplicit;
    bool aur_only, repo_only;
    size_t upgrade_level;  /* number of -u (-uu allows downgrades in the repos) */
    size_t refresh_level;  /* number of -y */
    const char *chroot;    /* devtools chroot dir (contains root/), or NULL */
    const char *cache;
    const char *config;    /* --config for pacman-conf, or NULL */
    const StrList *common; /* pacman options valid for every operation */
    const StrList *xact;   /* pacman transaction/upgrade modifiers */
    const StrList *ignore; /* --ignore values */
    StrList *touched;      /* names this run (re)installed, for the change summary */
} Options;

int cmd_sync(const StrList *targets, const Options *o);
int cmd_upgrade(const Options *o);
int cmd_clean(const Options *o, bool all);
/* -Qu for foreign packages: prints AUR updates; returns how many were found,
 * or -1 on error. */
int cmd_aur_updates(const Options *o, bool quiet);
/* Returns the highest severity (0..3) or 4 on error. */
int cmd_audit_dir(const char *dir);

#endif /* INSTALL_H */
