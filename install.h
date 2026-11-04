#ifndef INSTALL_H
#define INSTALL_H

#include "buf.h"

#include <stdbool.h>

typedef struct {
    bool noconfirm;      /* only for pacman/makepkg prompts; reviews always ask */
    bool needed;
    const char *chroot;  /* devtools chroot dir (contains root/), or NULL */
    const char *cache;
} Options;

int cmd_sync(const StrList *targets, const Options *o);
int cmd_upgrade(const Options *o, bool sync_repos);
int cmd_clean(const Options *o, bool all);
/* Returns the highest severity (0..3) or 4 on error. */
int cmd_audit_dir(const char *dir);

#endif /* INSTALL_H */
