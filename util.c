#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pwd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "util.h"
#include "alloc.h"

bool dir_is_empty(char *dir) {
    int n = 0;
    struct dirent *d;
    DIR *dir_p = opendir(dir);
    if (dir_p == NULL) return true;
    while ((d = readdir(dir_p)) != NULL) {
        if (++n > 2) break;
    }
    closedir(dir_p);
    return n <= 2;
}

bool yesno_prompt(char *prompt, bool default_answer) {
    char buf[16];
    char *answer = default_answer ? "[Y/n]" : "[y/N]";
    printf("%s %s ", prompt, answer);
    if (fgets(buf, 16, stdin) == NULL) return default_answer;
    if (buf[0] == '\n') return default_answer;
    return buf[0] == 'y' || buf[0] == 'Y';
}

int run_command(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        perror("execvp");
        exit(1);
    } else if (pid < 0) {
        return -1;
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int run_command_at(char *const argv[], const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        if (chdir(path) != 0) {
            perror("chdir");
            exit(1);
        }
        execvp(argv[0], argv);
        perror("execvp");
        exit(1);
    } else if (pid < 0) {
        return -1;
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

char *get_user_home(void) {
    char *home = getenv("HOME");
    if (home) return safe_strdup(home);
    struct passwd *pw = getpwuid(getuid());
    return pw ? safe_strdup(pw->pw_dir) : NULL;
}

bool dir_exists(char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int safe_setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    size_t len = strlen(name) + strlen(value) + 2;
    char *buf = safe_malloc(len);
    snprintf(buf, len, "%s=%s", name, value);
    /* putenv(3) is part of XSI and POSIX, but widely available.
     * We use it here to avoid implicit declaration issues with setenv in strict C99. */
    extern int putenv(char *string);
    return putenv(buf);
}

char *get_privilege_elevator(void) {
    const char *candidates[] = {"/usr/bin/sudo", "/usr/bin/doas", "/usr/bin/pkexec"};
    for (size_t i = 0; i < 3; i++) {
        if (access(candidates[i], X_OK) == 0) {
            return (char *)candidates[i];
        }
    }
    return NULL;
}
