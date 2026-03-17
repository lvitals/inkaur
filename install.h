#ifndef INSTALL_H
#define INSTALL_H

#include "json.h"

#include <stdbool.h>

int import_pgp_keys(char *path);
int install_non_aur_package(char *name);
int install_package(char *name, char *cache_path);
HashMap *get_installed_packages(void);
int update_packages(char *cache_path);
int clean_cache(char *cache_path);
int remove_packages(int n, char **pkgs);

#endif  /* INSTALL_H */
