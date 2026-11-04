#ifndef FSUTIL_H
#define FSUTIL_H

#include "buf.h"

#include <stdbool.h>
#include <stddef.h>

/* Create (mode 0700) or verify a directory: must not be a symlink, must be
 * owned by the current user; group/other permissions are stripped. */
int ensure_private_dir(const char *path);

/* Recursively delete path without following symlinks or crossing mount
 * points. Refuses "/", "" and relative paths. Missing path is success. */
int rmtree(const char *path);

/* Read a regular file (O_NOFOLLOW) into out; fails if larger than max. */
int read_file_limited(const char *path, size_t max, StrBuf *out);

/* Write data to path atomically (temp file + rename), mode 0600. */
int write_file_atomic(const char *path, const char *data, size_t len);

/* Take an exclusive advisory lock on <dir>/.lock; returns fd or -1. */
int acquire_lock(const char *dir);

/* snprintf into buf, returns false on truncation. */
bool path_join(char *buf, size_t sz, const char *a, const char *b);

bool is_dir_nofollow(const char *path);
bool is_file_nofollow(const char *path);

#endif /* FSUTIL_H */
