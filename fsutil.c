#include "fsutil.h"
#include "alloc.h"
#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

bool path_join(char *buf, size_t sz, const char *a, const char *b)
{
    int n = snprintf(buf, sz, "%s/%s", a, b);
    return n >= 0 && (size_t)n < sz;
}

bool is_dir_nofollow(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file_nofollow(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int ensure_private_dir(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "error: %s: %s\n", path, strerror(errno));
            return -1;
        }
        if (mkdir(path, 0700) != 0 && errno != EEXIST) {
            fprintf(stderr, "error: mkdir %s: %s\n", path, strerror(errno));
            return -1;
        }
        if (lstat(path, &st) != 0)
            return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "error: %s is not a directory (symlink or file); "
                "refusing to use it\n", path);
        return -1;
    }
    if (st.st_uid != getuid()) {
        fprintf(stderr, "error: %s is not owned by you; refusing to use it\n", path);
        return -1;
    }
    if ((st.st_mode & 077) && chmod(path, 0700) != 0) {
        fprintf(stderr, "error: chmod %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static int rm_cb(const char *p, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb;
    (void)ftw;
    int r = (flag == FTW_DP || flag == FTW_D) ? rmdir(p) : unlink(p);
    if (r != 0 && errno != ENOENT) {
        fprintf(stderr, "error: remove %s: %s\n", p, strerror(errno));
        return -1;
    }
    return 0;
}

int rmtree(const char *path)
{
    if (path == NULL || path[0] != '/' || strcmp(path, "/") == 0) {
        fprintf(stderr, "error: refusing to remove '%s'\n", path ? path : "");
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    return nftw(path, rm_cb, 32, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

int read_file_limited(const char *path, size_t max, StrBuf *out)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_size > 0 && (size_t)st.st_size > max)) {
        close(fd);
        return -1;
    }
    char chunk[8192];
    for (;;) {
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        if (out->len + (size_t)r > max || sb_append(out, chunk, (size_t)r) != 0) {
            close(fd);
            return -1;
        }
    }
    close(fd);
    return 0;
}

int write_file_atomic(const char *path, const char *data, size_t len)
{
    char *tmp = xasprintf("%s.tmpXXXXXX", path);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        free(tmp);
        return -1;
    }
    fchmod(fd, 0600);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            free(tmp);
            return -1;
        }
        off += (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0 || rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

int acquire_lock(const char *dir)
{
    char path[PATH_BUF];
    if (!path_join(path, sizeof path, dir, ".lock"))
        return -1;
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
