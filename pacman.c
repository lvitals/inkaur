#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alpm.h>
#include "pacman.h"
#include "alloc.h"

alpm_handle_t *pacman_init_handle(void) {
    alpm_errno_t err;
    /* Default Arch DB path: /var/lib/pacman */
    alpm_handle_t *handle = alpm_initialize("/", "/var/lib/pacman", &err);
    if (handle == NULL) {
        fprintf(stderr, "Failed to initialize alpm: %s\n", alpm_strerror(err));
        return NULL;
    }

    /* Local DB is registered automatically by libalpm upon initialization */
    
    /* Register sync databases to find 'foreign' packages */
    alpm_register_syncdb(handle, "core", 0);
    alpm_register_syncdb(handle, "extra", 0);
    alpm_register_syncdb(handle, "community", 0);
    alpm_register_syncdb(handle, "multilib", 0);

    return handle;
}

void pacman_cleanup_handle(alpm_handle_t *handle) {
    if (handle) {
        alpm_release(handle);
    }
}

bool pacman_is_package_installed(alpm_handle_t *handle, const char *pkgname) {
    alpm_db_t *db_local = alpm_get_localdb(handle);
    if (!db_local) return false;

    alpm_pkg_t *pkg = alpm_db_get_pkg(db_local, pkgname);
    return pkg != NULL;
}

bool pacman_package_in_sync_db(alpm_handle_t *handle, const char *pkgname) {
    alpm_list_t *sync_dbs = alpm_get_syncdbs(handle);
    for (alpm_list_t *j = sync_dbs; j; j = alpm_list_next(j)) {
        alpm_db_t *db_sync = j->data;
        if (alpm_db_get_pkg(db_sync, pkgname)) {
            return true;
        }
    }
    return false;
}

ForeignPkg *pacman_get_foreign_packages(alpm_handle_t *handle, size_t *count) {
    alpm_db_t *db_local = alpm_get_localdb(handle);
    alpm_list_t *sync_dbs = alpm_get_syncdbs(handle);
    alpm_list_t *i;
    
    ForeignPkg *foreign_pkgs = NULL;
    *count = 0;

    for (i = alpm_db_get_pkgcache(db_local); i; i = alpm_list_next(i)) {
        alpm_pkg_t *pkg = i->data;
        const char *pkgname = alpm_pkg_get_name(pkg);
        const char *pkgver = alpm_pkg_get_version(pkg);
        bool found = false;

        for (alpm_list_t *j = sync_dbs; j; j = alpm_list_next(j)) {
            alpm_db_t *db_sync = j->data;
            if (alpm_db_get_pkg(db_sync, pkgname)) {
                found = true;
                break;
            }
        }

        if (!found) {
            foreign_pkgs = realloc(foreign_pkgs, (*count + 1) * sizeof(ForeignPkg));
            foreign_pkgs[*count].name = safe_strdup(pkgname);
            foreign_pkgs[*count].version = safe_strdup(pkgver);
            (*count)++;
        }
    }

    return foreign_pkgs;
}

void list_installed_aur_packages(const char *operation) {
    alpm_handle_t *handle = pacman_init_handle();
    if (!handle) return;

    alpm_db_t *db_local = alpm_get_localdb(handle);
    alpm_list_t *sync_dbs = alpm_get_syncdbs(handle);
    alpm_list_t *i;

    bool explicit_only = (strstr(operation, "e") != NULL);
    bool deps_only = (strstr(operation, "d") != NULL);
    bool orphans_only = (strstr(operation, "t") != NULL);
    bool quiet = (strstr(operation, "q") != NULL);

    for (i = alpm_db_get_pkgcache(db_local); i; i = alpm_list_next(i)) {
        alpm_pkg_t *pkg = i->data;
        const char *pkgname = alpm_pkg_get_name(pkg);
        
        /* Check if foreign */
        bool found_in_sync = false;
        for (alpm_list_t *j = sync_dbs; j; j = alpm_list_next(j)) {
            if (alpm_db_get_pkg(j->data, pkgname)) {
                found_in_sync = true;
                break;
            }
        }
        if (found_in_sync) continue;

        /* Filter by install reason */
        alpm_pkgreason_t reason = alpm_pkg_get_reason(pkg);
        if (explicit_only && reason != ALPM_PKG_REASON_EXPLICIT) continue;
        if (deps_only && reason != ALPM_PKG_REASON_DEPEND) continue;

        /* Filter by orphans */
        if (orphans_only) {
            alpm_list_t *requiredby = alpm_pkg_compute_requiredby(pkg);
            alpm_list_t *optionalfor = alpm_pkg_compute_optionalfor(pkg);
            bool is_orphan = (requiredby == NULL && optionalfor == NULL);
            alpm_list_free_inner(requiredby, free);
            alpm_list_free(requiredby);
            alpm_list_free_inner(optionalfor, free);
            alpm_list_free(optionalfor);
            if (!is_orphan) continue;
        }

        if (quiet) {
            printf("%s\n", pkgname);
        } else {
            printf("%s %s\n", pkgname, alpm_pkg_get_version(pkg));
        }
    }

    pacman_cleanup_handle(handle);
}
