#ifndef PROC_H
#define PROC_H

#include "buf.h"

#include <stdbool.h>
#include <stddef.h>
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
    bool stderr_to_capture;        /* with capture: stderr goes there too */
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

/* Wait until one of the n pids in set exits. Other children that exit
 * meanwhile are remembered for proc_wait_pid(), so independent background
 * work is never lost. Returns the pid (status in *rc), or -1. */
pid_t proc_wait_set(const pid_t *set, size_t n, int *rc);
/* Same, but gives up after timeout_ms and returns 0 (so callers can show
 * progress while children run). */
pid_t proc_wait_set_timeout(const pid_t *set, size_t n, int *rc, int timeout_ms);
/* Wait for one specific child; returns its status or -1. */
int proc_wait_pid(pid_t pid);
/* true if the child has already exited (it is then reaped and remembered). */
bool proc_has_exited(pid_t pid);

/*
 * Run fn(i, ctx, out) for i in 0..n-1 in forked worker processes, at most
 * `jobs` at a time. Each worker's out (keep it small, < 4 KiB) is returned
 * in results->v[i] ("" if the worker failed). Workers have no terminal.
 */
typedef void (*ProcJobFn)(size_t i, void *ctx, StrBuf *out);
void proc_fork_jobs(size_t n, size_t jobs, ProcJobFn fn, void *ctx, StrList *results);

/* Number of online CPUs (at least 1). */
long cpu_count(void);

/* true if path is a root-owned, non group/other-writable regular file. */
bool tool_is_trusted(const char *path);

/* Absolute path of sudo or doas (must be root-owned and setuid), or NULL. */
const char *privilege_elevator(void);

#endif /* PROC_H */
