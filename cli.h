#ifndef CLI_H
#define CLI_H

#include "buf.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Command line parsing with pacman's option set.
 *
 * Every pacman operation (-D -F -Q -R -S -T -U -V) and every option pacman
 * documents for it is accepted, with the same short letters, long names,
 * unique long-name prefixes, "--opt=value" / "--opt value" and stacked short
 * flags ("-Syu", "-b/path"). Options are re-emitted in canonical long form
 * ("--dbpath=/path") so values can never be read as options by pacman.
 *
 * inkaur-only additions: the -A/--audit and --speedtest operations and, for
 * -S/-Q, --aur, --repo, --chroot <dir>, --jobs, --fetch-jobs, --serial and
 * --parallel.
 */

typedef struct {
    char op;               /* D F Q R S T U V A, N (--speedtest), or 0 */
    bool help;
    StrList seen;          /* long name of every option given (repeats kept) */
    StrList common;        /* forwarded: options valid for every operation */
    StrList xact;          /* forwarded: transaction/upgrade modifiers */
    StrList opflags;       /* forwarded: operation-specific flags (-s, -i, -u...) */
    StrList ctx;           /* --config/--root/--dbpath/--sysroot/--arch, for queries */
    StrList targets;
    StrList ignore;        /* values of --ignore (comma lists split) */
    const char *config;    /* --config value, or NULL */
    const char *chroot;    /* --chroot value, or NULL */
    bool noconfirm, needed, asdeps, asexplicit, aur_only, repo_only;
    int color;             /* -1 auto, 0 never, 1 always */
    const char *jobs;      /* --jobs value (build jobs), or NULL */
    const char *fetch_jobs;/* --fetch-jobs value, or NULL */
    int parallel;          /* --parallel 1, --serial 0, unset -1 */
} Cli;

void cli_init(Cli *c);
void cli_free(Cli *c);

/* 0 on success; on error writes a message (without "error: ") into err. */
int cli_parse(Cli *c, int argc, char **argv, char *err, size_t errsz);

/* How many times the option with this long name was given. */
size_t cli_count(const Cli *c, const char *lng);
static inline bool cli_has(const Cli *c, const char *lng) { return cli_count(c, lng) > 0; }

/* Long name of an operation letter ("sync" for 'S'), or NULL. */
const char *cli_op_name(char op);

#endif /* CLI_H */
