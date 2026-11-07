#include "proc.h"
#include "alloc.h"
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static const char *g_home = "/nonexistent";
static const char *g_user = "nobody";

void proc_set_identity(const char *home, const char *user)
{
    g_home = home;
    g_user = user;
}

/* Variables copied from the parent environment when present. */
static const char *const env_allow[] = {
    "TERM", "LANG", "LANGUAGE", "LC_ALL", "LC_CTYPE", "LC_MESSAGES",
    "LC_COLLATE", "LC_TIME", "LC_NUMERIC", "TZ", "COLUMNS", "LINES",
    "NO_COLOR", "TMPDIR",
    /* makepkg knobs that only redirect output locations / identity */
    "MAKEFLAGS", "PACKAGER", "PKGDEST", "SRCDEST", "SRCPKGDEST", "LOGDEST",
    "BUILDDIR", "GPGKEY", "GNUPGHOME",
    /* networking through a proxy */
    "http_proxy", "https_proxy", "no_proxy", "HTTP_PROXY", "HTTPS_PROXY",
    "NO_PROXY", "ALL_PROXY", "all_proxy",
    /* sudo/pinentry need a display or a tty, never a shell hook */
    "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR",
    "GPG_TTY",
    NULL
};

static char **build_env(const char *const *extra)
{
    size_t cap = 64, n = 0;
    char **env = xcalloc(cap, sizeof(char *));

    env[n++] = xasprintf("PATH=%s", SAFE_PATH);
    env[n++] = xasprintf("HOME=%s", g_home);
    env[n++] = xasprintf("USER=%s", g_user);
    env[n++] = xasprintf("LOGNAME=%s", g_user);

    for (size_t i = 0; env_allow[i]; i++) {
        const char *v = getenv(env_allow[i]);
        if (v == NULL)
            continue;
        if (n + 2 >= cap) {
            cap *= 2;
            env = xreallocarray(env, cap, sizeof(char *));
        }
        env[n++] = xasprintf("%s=%s", env_allow[i], v);
    }
    for (size_t i = 0; extra && extra[i]; i++) {
        if (n + 2 >= cap) {
            cap *= 2;
            env = xreallocarray(env, cap, sizeof(char *));
        }
        env[n++] = xstrdup(extra[i]);
    }
    env[n] = NULL;
    return env;
}

static void free_env(char **env)
{
    for (size_t i = 0; env[i]; i++)
        free(env[i]);
    free(env);
}

bool tool_is_trusted(const char *path)
{
    struct stat st;
    if (path == NULL || path[0] != '/')
        return false;
    if (stat(path, &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode) || st.st_uid != 0)
        return false;
    if (st.st_mode & (S_IWGRP | S_IWOTH))
        return false;
    return (st.st_mode & S_IXUSR) != 0;
}

const char *privilege_elevator(void)
{
    static const char *const cand[] = { BIN_SUDO, BIN_DOAS, NULL };
    for (size_t i = 0; cand[i]; i++) {
        struct stat st;
        if (tool_is_trusted(cand[i]) && stat(cand[i], &st) == 0 &&
            (st.st_mode & S_ISUID))
            return cand[i];
    }
    return NULL;
}

static void child_close_fds(void)
{
    long maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 65536)
        maxfd = 65536;
    for (int fd = 3; fd < (int)maxfd; fd++)
        close(fd);
}

static int redirect_null(int target, int flags)
{
    int fd = open("/dev/null", flags);
    if (fd < 0)
        return -1;
    if (dup2(fd, target) < 0)
        return -1;
    if (fd != target)
        close(fd);
    return 0;
}

int proc_run(const char *const argv[], const ProcOpts *opts)
{
    static const ProcOpts defaults;
    if (opts == NULL)
        opts = &defaults;

    if (argv == NULL || argv[0] == NULL || !tool_is_trusted(argv[0])) {
        fprintf(stderr, "error: refusing to run untrusted or missing executable '%s'\n",
                (argv && argv[0]) ? argv[0] : "(null)");
        return -1;
    }

    int pipefd[2] = { -1, -1 };
    if (opts->capture && pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    char **env = build_env(opts->extra_env);

    struct sigaction ign, old_int, old_quit;
    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGINT, &ign, &old_int);
    sigaction(SIGQUIT, &ign, &old_quit);

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        sigaction(SIGINT, &old_int, NULL);
        sigaction(SIGQUIT, &old_quit, NULL);
        free_env(env);
        if (opts->capture) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        return -1;
    }

    if (pid == 0) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(SIGINT, &dfl, NULL);
        sigaction(SIGQUIT, &dfl, NULL);
        sigaction(SIGPIPE, &dfl, NULL);

        if (opts->capture) {
            close(pipefd[0]);
            if (dup2(pipefd[1], STDOUT_FILENO) < 0)
                _exit(126);
            close(pipefd[1]);
        } else if (opts->stdout_null && redirect_null(STDOUT_FILENO, O_WRONLY) != 0) {
            _exit(126);
        }
        if (opts->stdin_null && redirect_null(STDIN_FILENO, O_RDONLY) != 0)
            _exit(126);
        if (opts->stderr_null && redirect_null(STDERR_FILENO, O_WRONLY) != 0)
            _exit(126);
        if (opts->cwd && chdir(opts->cwd) != 0) {
            perror("chdir");
            _exit(126);
        }
        child_close_fds();
        execve(argv[0], (char *const *)argv, env);
        fprintf(stderr, "error: exec %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    free_env(env);

    bool overflow = false;
    if (opts->capture) {
        close(pipefd[1]);
        char chunk[8192];
        for (;;) {
            ssize_t r = read(pipefd[0], chunk, sizeof chunk);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (r == 0)
                break;
            if (!overflow && sb_append(opts->capture, chunk, (size_t)r) != 0) {
                overflow = true;
                kill(pid, SIGTERM);
            }
        }
        close(pipefd[0]);
    }

    int status = 0;
    pid_t w;
    do {
        w = waitpid(pid, &status, 0);
    } while (w < 0 && errno == EINTR);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGQUIT, &old_quit, NULL);

    if (w < 0)
        return -1;
    if (overflow) {
        fprintf(stderr, "error: output of %s exceeded %zu bytes, aborted\n",
                argv[0], opts->capture->max);
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

static int decode_status(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

pid_t proc_spawn(const char *const argv[], const ProcOpts *opts, int log_fd)
{
    static const ProcOpts defaults;
    if (opts == NULL)
        opts = &defaults;
    if (argv == NULL || argv[0] == NULL || !tool_is_trusted(argv[0])) {
        fprintf(stderr, "error: refusing to run untrusted or missing executable '%s'\n",
                (argv && argv[0]) ? argv[0] : "(null)");
        return -1;
    }
    char **env = build_env(opts->extra_env);
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        free_env(env);
        return -1;
    }
    if (pid == 0) {
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        sigaction(SIGINT, &dfl, NULL);
        sigaction(SIGQUIT, &dfl, NULL);
        sigaction(SIGPIPE, &dfl, NULL);
        if (redirect_null(STDIN_FILENO, O_RDONLY) != 0)
            _exit(126);
        if (log_fd >= 0) {
            if (dup2(log_fd, STDOUT_FILENO) < 0 || dup2(log_fd, STDERR_FILENO) < 0)
                _exit(126);
        } else if (redirect_null(STDOUT_FILENO, O_WRONLY) != 0 ||
                   redirect_null(STDERR_FILENO, O_WRONLY) != 0) {
            _exit(126);
        }
        if (opts->cwd && chdir(opts->cwd) != 0)
            _exit(126);
        child_close_fds();
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    free_env(env);
    return pid;
}

pid_t proc_wait_any(int *rc)
{
    int status = 0;
    pid_t w;
    do {
        w = waitpid(-1, &status, 0);
    } while (w < 0 && errno == EINTR);
    if (w > 0 && rc)
        *rc = decode_status(status);
    return w;
}

long cpu_count(void)
{
    long n = -1;
#ifdef _SC_NPROCESSORS_ONLN
    n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    return n > 0 ? n : 1;
}
