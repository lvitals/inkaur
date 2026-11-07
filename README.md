# inkaur

A small AUR helper written in C99 with a `pacman`-like interface. It is built
around one rule: **nothing from the AUR is built until a human has reviewed the
exact commit that will be built.**

## Dependencies

Build: `make` and `gcc` or `clang`. Nothing else: no libcurl, no libalpm, no
third-party code. The binary links only against libc.

Runtime: the tools every Arch system already has for AUR work (`pacman`,
`makepkg`, `git`, `curl`, `gpg`, `bsdtar`, `less`, `sudo` or `doas`). They are
run by absolute path, never through a shell. `devtools` is optional and only
needed for `--chroot`.

## Installation

```bash
make            # as your user, never as root
make check      # unit tests under ASan/UBSan + functional tests
sudo make install
```

`make install` never compiles anything, so no build step runs as root.
`make hardening-report` shows PIE, RELRO/BIND_NOW, NX stack, FORTIFY and the stack canary.

## Usage

inkaur accepts **every pacman operation and option**, with the same letters,
long names (and unique prefixes such as `--sysu`), `--opt=value` / `--opt value`
forms and stacked flags (`-Syu`, `-Rns`, `-b/path`). It does the AUR side itself
and hands everything else to pacman unchanged, so `inkaur` can replace `pacman`
on the command line. Options are re-emitted in canonical long form
(`--dbpath=/path`) after validation, and targets always follow `--`.

```bash
inkaur -Syu              # pacman -Syu, then AUR upgrades (honors --ignore and IgnorePkg)
inkaur -Su --aur         # AUR upgrades only        (-Su --repo: repositories only)
inkaur -S pkg            # install (repo targets go to pacman, the rest to the AUR)
inkaur -S extra/pkg aur/other   # force the repository or the AUR
inkaur -S --chroot ~/chroot pkg # build in a clean devtools chroot
inkaur -Ss words...      # search the repositories and the AUR (-Ssq: names only)
inkaur -Si pkg...        # info from the repositories or the AUR
inkaur -Qu               # pending updates, repositories and AUR
inkaur -Q --aur          # foreign packages (same as -Qm; --repo = -Qn)
inkaur -Sc / -Scc        # pacman cache cleanup + AUR build files / all clones
inkaur -A <dir>          # static audit of a local PKGBUILD dir (exit 0..3 = severity)
inkaur -Qi / -Ql / -Qo / -R / -U / -D / -F / -T ...   # passed to pacman as is
```

Transaction options are forwarded to every pacman call they apply to,
including the `pacman -U` of built AUR packages: `--asdeps`, `--asexplicit`,
`--needed`, `--overwrite`, `--ignore`, `--nodeps`, `--assume-installed`,
`--noscriptlet`, `--dbonly`, `--noprogressbar`, and the global ones
(`--config`, `--root`, `--dbpath`, `--sysroot`, `--arch`, `--cachedir`,
`--hookdir`, `--gpgdir`, `--logfile`, `--color`, `--debug`, `--verbose`,
`--noconfirm`, `--confirm`, ...). `sudo`/`doas` is used only for the pacman
calls that need root.

Differences from pacman, on purpose: `-Sy` without `-u` is refused (partial
upgrade), and `--noconfirm` never skips an inkaur security review.

### Summary of changes

After every transaction (`-S`, `-Su`, `-Syu`, `-R`, `-U`) inkaur compares the
installed packages before and after and prints what changed:

```
:: Summary of changes
Upgraded (2)
   harfbuzz  14.0.0-1 -> 15.1.0-1
   nss       3.132-1 -> 3.133-1
Installed (1)
   yay-bin   13.1.0-1
   total: 2 upgraded, 1 installed, 0 reinstalled, 0 downgraded, 0 removed
```

### Parallel updates

Like dnf, inkaur runs independent work at the same time, and the dependency
order still holds:

* **Fetch.** AUR git repositories are cloned/fetched several at a time: the
  targets first, then each wave of AUR dependencies.
* **Overlap.** During `-Syu`, the AUR updates are looked up and their
  repositories fetched in the background while `pacman -Syu` downloads and
  waits at its prompt. Nothing from that background step is trusted: the main
  process checks everything again afterwards.
* **Build.** Up to *BuildJobs* AUR packages are built at once. A package starts
  only after every AUR package it depends on has been built, audited and
  installed. Repository dependencies of the builds are installed first in one
  `pacman -S --asdeps --needed`. Builds run without a terminal and write to
  `~/.cache/inkaur/.logs/<pkgbase>.log` (the last lines are shown on failure).
  Everything interactive stays one at a time: reviews, PGP key confirmation,
  HIGH-finding decisions and `pacman -U`. `--chroot` builds always run one at
  a time.

The defaults (`auto`) speed things up. They scale down on fewer than 4 CPUs,
less than 3 GiB of free RAM, or a slow connection:

| measured speed | fetch jobs | prefetch during -Syu |
|---|---|---|
| not measured | 4 | yes |
| < 128 KiB/s | 1 | no |
| < 1 MiB/s | 2 | no |
| < 4 MiB/s | 4 | yes |
| faster | 8 | yes |

Build jobs: 1 below 4 CPUs, otherwise max(2, CPUs/4), capped by free RAM/3 GiB
and at 4.

```bash
inkaur --speedtest       # AUR latency, mirror throughput, CPUs, RAM, pacman
                         # ParallelDownloads, and the values inkaur will use
inkaur -Syu --serial     # one thing at a time (slow/metered link, slow computer)
inkaur -Syu --jobs 4 --fetch-jobs 8
```

`--speedtest` saves its result for 7 days, and `auto` uses it. Settings go in
`/etc/inkaur.conf` and then `~/.config/inkaur/inkaur.conf` (the latter
overrides; files must not be group/world-writable):

```ini
[options]
Parallel  = auto     # auto | yes | no
BuildJobs = auto     # auto | 1..64
FetchJobs = auto     # auto | 1..64
Prefetch  = auto     # auto | yes | no
SpeedTest = auto     # auto (use the saved result) | always (measure before -Su) | never
```

Repository downloads stay with pacman: set `ParallelDownloads` in
`/etc/pacman.conf`. `--speedtest` suggests a value.

## Security model

### What happens on `-S`

1. **Resolve.** AUR dependencies are read from each repo's `.SRCINFO` at the
   fetched commit. They are parsed as data, never executed.
2. **Review everything first.** Each package is checked out at an exact commit
   (`git reset --hard` + `git clean -ffdx`) and audited. The whole repository
   is shown in a pager. On updates, only the diff since your last approved
   commit is shown. If any package is rejected, nothing is built.
3. **Decide, per package.** Every package gets an explicit menu with no
   default answer:
   ```
   [a] approve
   [s] skip pkg and continue with the other packages (installed version is kept)
   [r] remove pkg from the system and continue with the others
   [v] show the review again
   [x] abort everything
   ```
   Approving a package with HIGH/CRITICAL findings also requires typing the
   fixed word `yes`. Skipping or removing a package also skips the packages in
   the plan that need it, unless an installed older version still satisfies
   them. A failed build or a rejected built package works the same way. The
   run ends with a summary of what was installed, skipped, removed or failed.
   Decisions require a real terminal (`yes |` is refused), and `--noconfirm`
   only affects pacman/makepkg questions.
4. **Verify before building.** The worktree is reset again to the approved
   commit. PGP keys from `validpgpkeys` are never imported silently: each full
   fingerprint is shown, and you confirm it separately. Short key IDs are
   rejected. The `.SRCINFO` is compared with `makepkg --printsrcinfo`.
5. **Audit the built package** before `pacman -U`. HIGH findings bring up the same menu. It checks for setuid/setgid
   and world-writable files, `..` paths, files under `/home`, `/root`,
   `sudoers`, `ld.so.preload`, PAM, `profile.d`, pacman hooks, pre-enabled
   systemd units, cron, kernel modules, and `.INSTALL` scriptlets.
6. Every approval, rejection, key import and installation goes to syslog
   (`journalctl -t inkaur`).

The approved commit is stored in `.git/inkaur-reviewed`. That file sits
outside the worktree, so repository content cannot forge it.

### Static audit (`audit.c`)

* Repository: only tracked files are scanned. It reports symlinks,
  submodules, binaries, hidden/special/setuid files, control characters,
  stray CR and Trojan Source bidi/zero-width Unicode.
* Code patterns: download piped into a shell, base64/hex decoding, reverse
  shells (`/dev/tcp`, `nc -e`), `eval`, `sudo`, `systemctl enable`,
  setuid/world-writable `chmod`, `setcap`, cron/shell-rc/autostart
  persistence, `LD_PRELOAD`, kernel modules, account changes, home directory
  access, URL shorteners/paste sites/webhooks, raw IP URLs, and network access
  during the build.
* Context-aware severities: patches count only added lines (capped at WARN),
  text in a displayed `cat <<EOF` heredoc is not code, and config/data files
  that merely mention a path are INFO. Execution patterns stay at full
  severity in any file.
* `.SRCINFO`: remote sources without checksums, checksum count mismatches,
  weak-only checksums, plain HTTP/FTP, `git://`, unpinned VCS sources, signed
  sources without `validpgpkeys`, invalid names and dependency strings, and
  `install=` scriptlets.

The audit is a reading aid with hard stops, not a verdict. Read `PKGBUILD` and
`*.install` in full.

### Process and input hardening

* Refuses to run as root or setuid. `sudo`/`doas` is called only for `pacman`.
* Every name, version, dependency, fingerprint and commit is checked against
  strict character sets before it reaches a URL, path or argv, and is passed
  after `--`. Inputs like `../..`, `-rf` and `foo&type=search` are rejected.
* Child processes run via `fork`/`execve` with an allow-listed environment and
  a fixed `PATH`. `LD_*`, `LESSOPEN`, `BASH_ENV`, `GIT_*` and similar
  variables are dropped. Every executable must be root-owned and not
  group/world-writable. Inherited file descriptors are closed.
* git runs with system/global config ignored, https only, hooks/fsmonitor/
  textconv/external diff disabled, `core.symlinks=false` and fsck on transfer.
* curl runs with `~/.curlrc` ignored, https only (redirects too), TLS ≥ 1.2,
  globbing off, and bounded size and time.
* The JSON parser is strict and bounded (depth, sizes, duplicate keys, `\u0000`,
  surrogates) and fuzzed in the test suite. All accessors are type-checked
  and NULL-safe.
* Every server or repository string is sanitized before reaching the
  terminal: escape sequences, CR, C1 controls and bidi overrides are made
  visible.
* The cache (`~/.cache/inkaur`, mode 0700) is checked for owner and symlinks
  and is locked against concurrent runs. Deletion never follows symlinks or
  crosses mount points. The pager runs with `LESSSECURE=1`.

## License

MIT License.
