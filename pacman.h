#ifndef PACMAN_H
#define PACMAN_H

#include <alpm.h>
#include <stdbool.h>

typedef struct {
    char *name;
    char *version;
} ForeignPkg;

/* Initializes the ALPM handle. Needs to be called once. */
alpm_handle_t *pacman_init_handle(void);

/* Frees the ALPM handle. */
void pacman_cleanup_handle(alpm_handle_t *handle);

/* Checks if a package is installed locally. */
bool pacman_is_package_installed(alpm_handle_t *handle, const char *pkgname);

/* Lists all packages not found in sync databases (foreign packages). 
   Equivalent to 'pacman -Qm'. */
ForeignPkg *pacman_get_foreign_packages(alpm_handle_t *handle, size_t *count);
void list_installed_aur_packages(const char *operation);

#endif /* PACMAN_H */
