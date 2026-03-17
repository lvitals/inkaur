#include "json.h"
#include "alloc.h"
#include "requests.h"
#include "output.h"
#include "install.h"
#include "search.h"
#include "util.h"
#include "rpc.h"
#include "pacman.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static char *user_cache_path;
#define VERSION "0.1.0"

void init(void)
{
        char *userhome = get_user_home();
        user_cache_path = safe_malloc(sizeof(char) * AUR_PATH_MAX);
        snprintf(user_cache_path, AUR_PATH_MAX, "%s/.cache/inkaur", userhome);

        if (!dir_exists(user_cache_path)) {
                /* remove just in case the path exists but is not a folder. */
                char *rm_argv[] = {"rm", "-rf", user_cache_path, NULL};
                run_command(rm_argv);
                printf("Cache dir does not exist. Creating one at %s.\n",
                                user_cache_path);
                char *mkdir_argv[] = {"mkdir", "-p", user_cache_path, NULL};
                run_command(mkdir_argv);
        }

        free(userhome);
}

void usage(void)
{
        printf("usage:  inkaur <operation> [...]\n");
        printf("operations:\n");
        printf("    inkaur {-h --help}\n");
        printf("    inkaur {-V --version}\n");
        printf("    inkaur {-Q --query}    [options] [package(s)]\n");
        printf("    inkaur {-R --remove}   [options] <package(s)>\n");
        printf("    inkaur {-S --sync}     [options] [package(s)]\n");
        printf("\nuse \"inkaur {-h --help}\" with an operation to see available options\n");
        exit(0);
}

void version(void)
{
        printf("inkaur %s\n", VERSION);
        printf("Copyright (C) 2026 Leandro V. Catarin\n");
        exit(0);
}

int main(int argc, char **argv)
{
        init();

        if (argc < 2) usage();

        char *operation = argv[1];
        char **parameters = &argv[2];
        int parameters_i = argc - 2;

        if (strcmp(operation, "-h") == 0 || strcmp(operation, "--help") == 0) {
                usage();
        }

        if (strcmp(operation, "-V") == 0 || strcmp(operation, "--version") == 0) {
                version();
        }

        if (strcmp(operation, "-Ss") == 0 || strcmp(operation, "--search") == 0) {
                if (parameters_i == 0) {
                        fprintf(stderr, "error: no targets specified\n");
                        return 1;
                }
                return search_aur(parameters_i, parameters);
        }

        if (strcmp(operation, "-Si") == 0 || strcmp(operation, "--info") == 0) {
                if (parameters_i == 0) {
                        fprintf(stderr, "error: no targets specified\n");
                        return 1;
                }
                for (int i = 0; i < parameters_i; i++) {
                    show_package_info(parameters[i]);
                    if (i < parameters_i - 1) printf("\n");
                }
                return 0;
        }

        if (strcmp(operation, "-Syu") == 0 || strcmp(operation, "--update") == 0) {
                return update_packages(user_cache_path);
        }

        if (strncmp(operation, "-R", 2) == 0 || strcmp(operation, "--remove") == 0) {
                if (parameters_i == 0) {
                        fprintf(stderr, "error: no targets specified\n");
                        return 1;
                }
                /* Pass the flag directly to pacman via run_command */
                char **cmd = safe_malloc(sizeof(char *) * (parameters_i + 4));
                char *elevator = get_privilege_elevator();
                int idx = 0;
                if (elevator) cmd[idx++] = elevator;
                cmd[idx++] = "pacman";
                cmd[idx++] = operation;
                for (int i = 0; i < parameters_i; i++) cmd[idx++] = parameters[i];
                cmd[idx] = NULL;
                int r = run_command(cmd);
                free(cmd);
                return r;
        }

        if (strncmp(operation, "-Q", 2) == 0 || strcmp(operation, "--query") == 0) {
                list_installed_aur_packages(operation);
                return 0;
        }

        if (strncmp(operation, "-Sc", 3) == 0 || strcmp(operation, "--clean") == 0) {
                if (strcmp(operation, "-Scc") == 0) {
                    /* Remove whole directory */
                    char *rm_argv[] = {"rm", "-rf", user_cache_path, NULL};
                    return run_command(rm_argv);
                }
                return clean_cache(user_cache_path);
        }

        if (strcmp(operation, "-S") == 0 || strcmp(operation, "--sync") == 0) {
                if (parameters_i == 0) {
                        fprintf(stderr, "error: no targets specified\n");
                        return 1;
                }
                alpm_handle_t *handle = pacman_init_handle();
                if (!handle) return 1;

                int sync_pkgs_i = 0;
                char **sync_pkgs = safe_malloc(sizeof(char *) * parameters_i);
                int aur_pkgs_i = 0;
                char **aur_pkgs = safe_malloc(sizeof(char *) * parameters_i);

                for (int i = 0; i < parameters_i; i++) {
                        if (pacman_package_in_sync_db(handle, parameters[i])) {
                                sync_pkgs[sync_pkgs_i++] = parameters[i];
                        } else {
                                aur_pkgs[aur_pkgs_i++] = parameters[i];
                        }
                }
                pacman_cleanup_handle(handle);

                if (sync_pkgs_i > 0) {
                        char **cmd = safe_malloc(sizeof(char *) * (sync_pkgs_i + 4));
                        char *elevator = get_privilege_elevator();
                        int idx = 0;
                        if (elevator) cmd[idx++] = elevator;
                        cmd[idx++] = "pacman";
                        cmd[idx++] = "-S";
                        for (int i = 0; i < sync_pkgs_i; i++) cmd[idx++] = sync_pkgs[i];
                        cmd[idx] = NULL;
                        int r = run_command(cmd);
                        free(cmd);
                        if (r != 0) {
                                free(sync_pkgs);
                                free(aur_pkgs);
                                return r;
                        }
                }

                for (int i = 0; i < aur_pkgs_i; i++) {
                        if (install_package(aur_pkgs[i], user_cache_path)) {
                                free(sync_pkgs);
                                free(aur_pkgs);
                                return 1;
                        }
                }

                free(sync_pkgs);
                free(aur_pkgs);
                return 0;
        }

        fprintf(stderr, "error: invalid operation '%s'\n", operation);
        return 1;
}
