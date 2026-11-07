#ifndef PROC_H
#define PROC_H

#include "buf.h"

#include <stdbool.h>
#include <sys/types.h>

/*
 * Child process execution without a shell.
 *
 *  - argv[0] must be an absolute path; it is checked to be a regular file
 *    owned by root and not writable by group/others before every exec.
 *  - The child gets an allow-listed environment (PATH is fixed, LD_*,
 *    LESSOPEN, BASH_ENV, GIT_*, etc. are dropped).
 *  - All file descriptors above 2 are closed in the child.
 *  - While waiting, the parent ignores SIGINT/SIGQUIT (like system(3)), so
 *    Ctrl-C reaches the child and the parent sees the failure.
 */
typedef struct {
    const char *cwd;               /* chdir() here before exec */
    StrBuf *capture;               /* capture stdout (bounded by capture->max) */
    bool stdin_null;
    bool stdout_null;
    bool stderr_null;
    const char *const *extra_env;  /* NULL-terminated "KEY=VALUE" list */
} ProcOpts;

/* Identity used to build the child environment (from getpwuid, not $HOME). */
void proc_set_identity(const char *home, const char *user);

/* Returns the exit status, 128+signal if killed, -1 if it could not run. */
int proc_run(const char *const argv[], const ProcOpts *opts);

/*
 * Start a child without waiting for it (same checks and environment as
 * proc_run). stdin is /dev/null; stdout and stderr go to log_fd, or to
 * /dev/null when log_fd < 0. opts->capture is not supported here.
 * Returns the pid, or -1.
 */
pid_t proc_spawn(const char *const argv[], const ProcOpts *opts, int log_fd);

/* Wait for any child started with proc_spawn. Returns its pid and stores
 * the exit status (128+signal if killed) in *rc; -1 when none are left. */
pid_t proc_wait_any(int *rc);

/* Number of online CPUs (at least 1). */
long cpu_count(void);

/* true if path is a root-owned, non group/other-writable regular file. */
bool tool_is_trusted(const char *path);

/* Absolute path of sudo or doas (must be root-owned and setuid), or NULL. */
const char *privilege_elevator(void);

#endif /* PROC_H */
