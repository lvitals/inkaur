#ifndef INKAUR_CONFIG_H
#define INKAUR_CONFIG_H

/*
 * Compile-time configuration. Everything that touches the outside world
 * (executables, URLs, limits) lives here so it can be audited in one place.
 *
 * Executables are always invoked by ABSOLUTE path with a sanitized
 * environment: $PATH is never consulted, so a planted ~/.local/bin/makepkg
 * (or similar) cannot hijack a build.
 */

#define INKAUR_VERSION "0.3.0"

#define AUR_RPC_BASE "https://aur.archlinux.org/rpc/v5"
#define AUR_GIT_FMT  "https://aur.archlinux.org/%s.git"

#define BIN_GIT            "/usr/bin/git"
#define BIN_CURL           "/usr/bin/curl"
#define BIN_PACMAN         "/usr/bin/pacman"
#define BIN_PACMAN_CONF    "/usr/bin/pacman-conf"
#define BIN_MAKEPKG        "/usr/bin/makepkg"
#define BIN_MAKECHROOTPKG  "/usr/bin/makechrootpkg"
#define BIN_GPG            "/usr/bin/gpg"
#define BIN_SUDO           "/usr/bin/sudo"
#define BIN_DOAS           "/usr/bin/doas"
#define BIN_LESS           "/usr/bin/less"
#define BIN_BSDTAR         "/usr/bin/bsdtar"
#define BIN_INSTALL        "/usr/bin/install"

#define PACMAN_CONF_PATH   "/etc/pacman.conf"
#define MIRRORLIST_PATH    "/etc/pacman.d/mirrorlist"

/* PATH handed to every child process. */
#define SAFE_PATH "/usr/bin:/usr/sbin:/usr/bin/site_perl:/usr/bin/vendor_perl:/usr/bin/core_perl"

/* Limits (defense against hostile or broken servers / repositories). */
#define MAX_RPC_BYTES        (16u * 1024u * 1024u)
#define MAX_GIT_OUTPUT       (32u * 1024u * 1024u)
#define MAX_PKG_LISTING      (64u * 1024u * 1024u)
#define MAX_SCAN_FILE_BYTES  (2u * 1024u * 1024u)
#define MAX_REPO_FILES       2000u
#define MAX_REPO_DEPTH       16
#define MAX_NAME_LEN         255u
#define MAX_VERSION_LEN      255u
#define MAX_DEP_DEPTH        64
#define RPC_BATCH            100u
#define PATH_BUF             4096

#endif /* INKAUR_CONFIG_H */
