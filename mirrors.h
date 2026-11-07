#ifndef MIRRORS_H
#define MIRRORS_H

#include "buf.h"

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/*
 * Repository side helpers. Nothing here replaces pacman's verification:
 * packages downloaded by inkaur are copied root-owned into pacman's cache and
 * pacman then checks their signatures exactly as for its own downloads.
 */

/* Copy files as root with mode 0644 (sudo/doas install -o root -g root).
 * dest_is_dir: install -t dest -- srcs...; else one src to file dest, with a
 * ".inkaur-bak" backup of the old file when backup is true. */
int root_install(const StrList *srcs, const char *dest, bool dest_is_dir, bool backup);

/* ParallelDownloads that fits a measured throughput. */
long recommended_parallel_downloads(double bytes_per_sec);

/* Rewrites ParallelDownloads in pacman.conf (shows the change, asks first,
 * keeps a backup). Returns 0 if written, 1 if declined/unchanged, -1 on error. */
int pacman_conf_set_parallel(const char *cache, const char *conf_path, long n);

/* Text transformation used by pacman_conf_set_parallel (exposed for tests). */
char *pacman_conf_with_parallel(const char *text, long n);

/* Server lines of a mirrorlist ($repo/$arch kept); commented ones too if asked. */
void mirrorlist_servers(const char *text, bool include_commented, StrList *out);
/* New mirrorlist: ranked servers first, previous content kept commented. */
char *mirrorlist_rewrite(const char *old_text, const StrList *ranked, const char *date);

/* inkaur --rankmirrors */
int cmd_rankmirrors(const char *cache, bool all, int count, int jobs, bool noconfirm);

/*
 * Background download of the pending repository upgrades, split across the
 * first `mirrors` servers of each repository, `jobs` files at a time.
 * print_args: pacman arguments that select the upgrade (--sync --sysupgrade
 * ... plus the user's options); --print/--print-format are added here.
 */
typedef struct {
    pid_t pid;
    size_t nfiles;
    double bytes;
    char staging[4096];
    char cachedir[4096];
    StrList files;       /* file names, in staging and later in the cache */
    StrList sizes;
} RepoDownload;

int repo_download_start(RepoDownload *d, const char *cache, const StrList *print_args,
                        const char *cachedir_override, int mirrors, int jobs);
/* Waits for the download and copies the complete files into the cache. */
int repo_download_finish(RepoDownload *d);

#endif /* MIRRORS_H */
