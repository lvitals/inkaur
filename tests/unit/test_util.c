#include "../../util.h"
#include "../test_helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

void test_dir_exists() {
    ASSERT(dir_exists("/tmp"), "/tmp should exist");
    ASSERT(!dir_exists("/nonexistent_directory_12345"), "nonexistent dir should not exist");
}

void test_run_command() {
    char *argv[] = {"true", NULL};
    ASSERT(run_command(argv) == 0, "run_command(true) should return 0");
    
    char *argv_false[] = {"false", NULL};
    ASSERT(run_command(argv_false) != 0, "run_command(false) should return non-zero");
}

void test_run_command_at() {
    char *argv[] = {"ls", "/tmp", NULL};
    // Redirect stdout to /dev/null for cleaner test output
    int stdout_fd = dup(STDOUT_FILENO);
    freopen("/dev/null", "w", stdout);
    ASSERT(run_command_at(argv, "/tmp") == 0, "run_command_at(ls, /tmp) should return 0");
    fflush(stdout);
    dup2(stdout_fd, STDOUT_FILENO);
    close(stdout_fd);
}

void test_dir_is_empty() {
    system("rm -rf /tmp/empty_dir && mkdir -p /tmp/empty_dir");
    ASSERT(dir_is_empty("/tmp/empty_dir"), "dir should be empty");
    system("touch /tmp/empty_dir/file");
    ASSERT(!dir_is_empty("/tmp/empty_dir"), "dir should not be empty");
}

void test_get_user_home() {
    char *home = get_user_home();
    ASSERT(home != NULL, "get_user_home returned NULL");
    ASSERT(home[0] == '/', "home path should start with /");
    free(home);
}

void run_util_tests() {
    test_dir_exists();
    test_run_command();
    test_run_command_at();
    test_dir_is_empty();
    test_get_user_home();
    printf("Unit tests for util passed!\n");
}
