#include "install.h"
#include "requests.h"
#include "output.h"
#include "alloc.h"
#include "util.h"
#include "pacman.h"
#include "rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <ctype.h>
#include <alpm.h>

#ifdef TESTING
#define STATIC
#else
#define STATIC static
#endif

typedef struct {
        char **items;
        size_t count;
        size_t capacity;
} PkgList;

STATIC PkgList *pkglist_new(void) {
        PkgList *l = safe_malloc(sizeof(PkgList));
        l->count = 0;
        l->capacity = 16;
        l->items = safe_malloc(sizeof(char *) * l->capacity);
        return l;
}

STATIC void pkglist_free(PkgList *l) {
        if (!l) return;
        for (size_t i = 0; i < l->count; i++) {
                free(l->items[i]);
        }
        free(l->items);
        free(l);
}

STATIC void pkglist_add(PkgList *l, const char *name) {
        for (size_t i = 0; i < l->count; i++) {
                if (strcmp(l->items[i], name) == 0) return;
        }
        if (l->count >= l->capacity) {
                l->capacity *= 2;
                l->items = realloc(l->items, sizeof(char *) * l->capacity);
        }
        l->items[l->count++] = safe_strdup(name);
}

STATIC char *strip_version(const char *dep) {
        char *s = safe_strdup(dep);
        char *p = s;
        while (*p) {
                if (*p == '>' || *p == '<' || *p == '=') {
                        *p = '\0';
                        break;
                }
                p++;
        }
        return s;
}

STATIC int resolve_deps_recursive(const char *pkgname, PkgList *aur_queue, PkgList *pacman_queue, HashMap *visited, alpm_handle_t *handle, bool top_level) {
        char *clean_name = strip_version(pkgname);

        if (hashmap_index(visited, clean_name)) {
                free(clean_name);
                return 0;
        }

        HMItem *item = new_item(clean_name, (void*)1, free, NULL);
        hashmap_set(visited, item);

        if (!top_level && pacman_is_package_installed(handle, clean_name)) {
                return 0;
        }

        if (pacman_package_in_sync_db(handle, clean_name)) {
                pkglist_add(pacman_queue, clean_name);
                return 0;
        }

        /* Check AUR */
        char url[AUR_PATH_MAX];
        snprintf(url, AUR_PATH_MAX, "https://aur.archlinux.org/rpc/?v=5&type=info&arg=%s", clean_name);
        struct rpc_data *rpc = make_rpc_request(url);

        if (!rpc || rpc->resultcount == 0) {
                fprintf(stderr, "error: package %s not found in repos or AUR\n", clean_name);
                free_rpc_data(rpc);
                return 1;
        }

        struct json *pkg_j = json_get_array_item(rpc->results, 0);
        struct package *pkg = parse_package_json(pkg_j);

        /* Recursively resolve dependencies */
        for (size_t i = 0; i < pkg->depends_count; i++) {
                if (resolve_deps_recursive(pkg->depends[i], aur_queue, pacman_queue, visited, handle, false)) {
                        free_package_data(pkg);
                        free_rpc_data(rpc);
                        return 1;
                }
        }

        for (size_t i = 0; i < pkg->makedepends_count; i++) {
                if (resolve_deps_recursive(pkg->makedepends[i], aur_queue, pacman_queue, visited, handle, false)) {
                        free_package_data(pkg);
                        free_rpc_data(rpc);
                        return 1;
                }
        }

        pkglist_add(aur_queue, clean_name);

        free_package_data(pkg);
        free_rpc_data(rpc);
        return 0;
}

int import_pgp_keys(char *path)
{
        char *full_path = safe_malloc(sizeof(char) * AUR_PATH_MAX);
        snprintf(full_path, AUR_PATH_MAX, "%s/.SRCINFO", path);

        FILE *fp = fopen(full_path, "rb");
        if (!fp) {
                free(full_path);
                return 1;
        }

        char buf[1024];
        while ((fgets(buf, 1024, fp)) != NULL) {
                size_t len = strlen(buf);
                size_t i;
                for (i = 0; i < len; i++) {
                        if (!isspace(buf[i]))
                                break;
                }

                char *searchterm = "validpgpkeys";
                if (strncmp(searchterm, buf + i, strlen(searchterm)) != 0)
                        continue;

                char *stripped = buf + i;
                for (i = 0; i < strlen(stripped); i++) {
                        char c = stripped[i];
                        if (c == '=') {
                                i++;
                                break;
                        }
                }

                char *key = stripped + i;
                while (isspace(*key)) key++;
                char *end = key + strlen(key) - 1;
                while (end > key && isspace(*end)) {
                        *end = '\0';
                        end--;
                }

                char *gpg_argv[] = {"gpg", "--recv-keys", key, NULL};
                if (run_command(gpg_argv) != 0) {
                        fprintf(stderr, "failed to import key: %s\n", key);
                        return 1;
                }
        }

        free(full_path);
        fclose(fp);

        return 0;
}

static int install_aur_single(char *name, char *cache_path) {
        int rc = 0;
        struct rpc_data *api_json_result = NULL;
        struct json *pkg_json = NULL;
        struct package *pkg_info = NULL;
        char *dest_path = NULL;

        size_t api_info_str_len = strlen(name) + 128;
        char *api_info_str = safe_malloc(sizeof(char) * api_info_str_len);
        snprintf(api_info_str, api_info_str_len,
                        "https://aur.archlinux.org/rpc/?v=5&type=info&arg=%s",
                        name
        );

        api_json_result = make_rpc_request(api_info_str);
        free(api_info_str);

        if (api_json_result->resultcount != 1) {
                fprintf(stderr, "error installing package %s.\n", name);
                rc = 1;
                goto end;
        }

        pkg_json = json_get_array_item(api_json_result->results, 0);
        pkg_info = parse_package_json(pkg_json);

        if (pkg_info->outofdate) {
                bool shouldcontinue = yesno_prompt(
                                "Package is out of date. Continue?", false);
                if (!shouldcontinue)
                        goto end;
        }

        dest_path = safe_malloc(sizeof(char) * AUR_PATH_MAX);
        snprintf(dest_path, AUR_PATH_MAX, "%s/%s", cache_path, name);

        printf("installing to directory %s\n", dest_path);

        int should_clone = 1;
        if (!dir_is_empty(dest_path)) {
                char prompt[1024];
                snprintf(prompt, 1024, "Existing package data found for %s. Rebuild?", name);
                should_clone = yesno_prompt(prompt, false);

                if (should_clone) {
                        char *rm_argv[] = {"rm", "-rf", dest_path, NULL};
                        run_command(rm_argv);
                }
        }

        if (should_clone) {
                char repo_url[AUR_PATH_MAX];
                snprintf(repo_url, AUR_PATH_MAX, "https://aur.archlinux.org/%s.git", pkg_info->name);
                char *git_argv[] = {"git", "clone", repo_url, dest_path, NULL};

                int git_clone_r = run_command(git_argv);

                if (git_clone_r) {
                        fprintf(stderr, "Failed to clone git repository.\n");
                        rc = 1;
                        goto end;
                }
        }

        if (import_pgp_keys(dest_path)) {
                fprintf(stderr, "failed to import PGP keys.\n");
                rc = 1;
                goto end;
        }

        char *elevator = get_privilege_elevator();
        if (elevator) {
                safe_setenv("PACMAN_AUTH", elevator, 1);
        }

        char *makepkg_argv[] = {"makepkg", "--noconfirm", "-si", NULL};
        int makepkg_r = run_command_at(makepkg_argv, dest_path);

        if (makepkg_r) {
                fprintf(stderr, "makepkg failed.\n");
                rc = 1;
        }

end:
        free(dest_path);
        free_rpc_data(api_json_result);
        free_package_data(pkg_info);

        return rc;
}

int install_package(char *name, char *cache_path)
{
        int rc = 0;
        PkgList *aur_queue = pkglist_new();
        PkgList *pacman_queue = pkglist_new();
        HashMap *visited = new_hashmap(16);
        alpm_handle_t *handle = pacman_init_handle();

        if (!handle) {
                rc = 1;
                goto end;
        }

        printf(":: Resolving dependencies...\n");
        if (resolve_deps_recursive(name, aur_queue, pacman_queue, visited, handle, true) != 0) {
                rc = 1;
                goto end;
        }

        if (pacman_queue->count > 0) {
                printf(":: The following packages will be installed from repositories:\n");
                for (size_t i = 0; i < pacman_queue->count; i++) {
                        printf("   %s\n", pacman_queue->items[i]);
                }
                
                char *elevator = get_privilege_elevator();
                char **argv = safe_malloc(sizeof(char *) * (pacman_queue->count + 4));
                int idx = 0;
                if (elevator) argv[idx++] = elevator;
                argv[idx++] = "pacman";
                argv[idx++] = "-S";
                argv[idx++] = "--needed";
                for (size_t i = 0; i < pacman_queue->count; i++) {
                        argv[idx++] = pacman_queue->items[i];
                }
                argv[idx++] = NULL;

                if (run_command(argv) != 0) {
                        fprintf(stderr, "error: failed to install dependencies from repositories\n");
                        free(argv);
                        rc = 1;
                        goto end;
                }
                free(argv);
        }

        if (aur_queue->count > 0) {
                printf(":: The following packages will be installed from AUR:\n");
                for (size_t i = 0; i < aur_queue->count; i++) {
                        printf("   %s\n", aur_queue->items[i]);
                }

                for (size_t i = 0; i < aur_queue->count; i++) {
                        if (install_aur_single(aur_queue->items[i], cache_path) != 0) {
                                rc = 1;
                                break;
                        }
                }
        } else {
                printf(" %s is already installed.\n", name);
        }

end:
        pkglist_free(aur_queue);
        pkglist_free(pacman_queue);
        free_hashmap(visited);
        pacman_cleanup_handle(handle);

        return rc;
}

/* returns a hashmap with the package names and values */
HashMap *get_installed_packages(void)
{
        HashMap *installed_packages = new_hashmap(16);
        if (installed_packages == NULL)
                return NULL;

        alpm_handle_t *handle = pacman_init_handle();
        if (!handle) {
                free_hashmap(installed_packages);
                return NULL;
        }

        size_t count = 0;
        ForeignPkg *foreign_pkgs = pacman_get_foreign_packages(handle, &count);

        for (size_t i = 0; i < count; i++) {
                HMItem *item = new_item(foreign_pkgs[i].name, foreign_pkgs[i].version, free, free);
                if (hashmap_set(installed_packages, item) != 0) {
                        fprintf(stderr, "failed to set hashmap item.\n");
                        free(foreign_pkgs[i].name);
                        free(foreign_pkgs[i].version);
                        continue;
                }
        }

        free(foreign_pkgs);
        pacman_cleanup_handle(handle);

        if (installed_packages->stored == 0) {
                free_hashmap(installed_packages);
                return NULL;
        }

        return installed_packages;
}

int update_packages(char *cache_path)
{
        char *elevator = get_privilege_elevator();
        char *pacman_argv[6];
        int idx = 0;
        if (elevator) pacman_argv[idx++] = elevator;
        pacman_argv[idx++] = "pacman";
        pacman_argv[idx++] = "-Syu";
        pacman_argv[idx++] = NULL;

        printf(":: Synchronizing package databases...\n");
        int r = run_command(pacman_argv);
        if (r != 0) {
                return r;
        }

        int repeat = 0;  /* multiple updates needed */

        HashMap *installed_packages = get_installed_packages();
        if (installed_packages == NULL)
                return 1;

        /* no installed AUR packages */
        if (installed_packages->stored == 0)
                return 0;

        /*
         * build one long api request - this is tedious, but works better for
         * not DDoS'ing the AUR.
         */
        size_t fmt_capacity = 4443;
        char *fmt = safe_calloc(1, sizeof(char) * fmt_capacity);
        snprintf(fmt, fmt_capacity, "https://aur.archlinux.org/rpc/?v=5&type=info");

        /* keep track of the longest package for nicer formatting below */
        size_t largest_package_name_length = 0;

        for (size_t i = 0; i < installed_packages->can_store; i++) {
                HMItem *item = installed_packages->items[i];
                if (item == NULL)
                        continue;

                char *key = item->key;
                size_t klen = strlen(key);

                if (klen > largest_package_name_length)
                        largest_package_name_length = klen;

                char *delim = "&arg[]=";
                size_t added = strlen(delim) + klen;
                size_t current_len = strlen(fmt);

                if (added + current_len >= fmt_capacity) {
                        repeat = 1;
                        break;
                }

                strncat(fmt, delim, fmt_capacity - current_len - 1);
                strncat(fmt, key, fmt_capacity - strlen(fmt) - 1);
        }

        struct rpc_data *data = make_rpc_request(fmt);
        if (data == NULL) {
                free(fmt);
                free_hashmap(installed_packages);
                return 1;
        }


        size_t update_queue_i = 0;
        char *update_queue[1024];

        printf("The following have updates:\n");
        /* parse the results */
        struct json *results = data->results;
        for (size_t i = 0; i < data->resultcount; i++) {
                struct json *pkg_j = json_get_array_item(results, i);
                struct package *pkg = parse_package_json(pkg_j);

                char *name = pkg->name;

                char *new_version = pkg->version;
                char *installed_version = hashmap_index(installed_packages,
                                name);

                /* add to list of old packages */
                if (installed_version && alpm_pkg_vercmp(new_version, installed_version) > 0) {
                        update_queue[update_queue_i++] = safe_strdup(name);

                        char name_fmt[1024];
                        snprintf(name_fmt, 1024, "%s%s%%-%zus%s: ",
                                        BLUE, BOLD,
                                        largest_package_name_length + 3, ENDC);

                        printf(name_fmt, name);

                        print_diff(installed_version, new_version);
                        printf("\n");

                        if (update_queue_i + 1 > 1024) {
                                repeat = 1;
                                free_package_data(pkg);
                                break;
                        }
                }

                free_package_data(pkg);
        }

        if (update_queue_i == 0) {
                printf("no updates. exiting.\n");
                goto end;
        }

        bool should_update = yesno_prompt("update?", true);
        if (!should_update) {
                repeat = 0;
                goto end;
        }

        for (size_t i = 0; i < update_queue_i; i++) {
                char *name = update_queue[i];
                char full_path[AUR_PATH_MAX];
                snprintf(full_path, AUR_PATH_MAX, "%s/%s", cache_path, name);
                char *rm_argv[] = {"rm", "-rf", full_path, NULL};
                run_command(rm_argv);

                install_package(name, cache_path);

                free(update_queue[i]);
        }

end:
        free(fmt);
        free_rpc_data(data);
        free_hashmap(installed_packages);

        if (repeat) {
                printf("Multiple updates needed. Re-running.\n");
                return update_packages(cache_path);
        }

        return 0;
}

int clean_cache(char *cache_path)
{
        char *rm_argv[] = {"rm", "-rfv", cache_path, NULL};
        return run_command(rm_argv);
}

int remove_packages(int n, char **pkgs)
{
        char **argv = safe_malloc(sizeof(char *) * (n + 3));
        argv[0] = "pacman";
        argv[1] = "-R";
        for (int i = 0; i < n; i++) {
                argv[i + 2] = pkgs[i];
        }
        argv[n + 2] = NULL;
        int rc = run_command(argv);
        free(argv);
        return rc;
}
