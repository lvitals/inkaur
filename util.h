
#ifndef UTIL_H
#define UTIL_H

#include <stdio.h>
#include <stdbool.h>

#define AUR_PATH_MAX 4096

bool dir_is_empty(char *dir);
bool yesno_prompt(char *prompt, bool default_answer);
int run_command(char *const argv[]);
int run_command_at(char *const argv[], const char *path);
char *get_user_home(void);
bool dir_exists(char *path);
char *get_privilege_elevator(void);
int safe_setenv(const char *name, const char *value, int overwrite);

#endif  /* UTIL_H */
