# inkaur

<p align="center">
  <img src="mascot.svg" width="160" alt="inkaur mascot: an ink-drop ghost">
</p>

**inkaur** is a security-first AUR helper and a complete front-end for
pacman. Its goal is to make installing and updating software from the Arch
User Repository as safe as installing from the official repositories, without
changing the way you already work with pacman.

The AUR is open to everyone, so a package build script can do anything on
your machine. inkaur is built around one rule: **nothing from the AUR is
built until a human has reviewed the exact commit that will be built.** Every
AUR package, including every AUR dependency, is checked out at a fixed git
commit and run through a static security audit. You get a **plain-language
safety summary** (a verdict, what each warning means, how every download is
verified, and how established the package is in the AUR), so you do not need
to read shell scripts to make a good decision; the full source is one key
away. On updates, only what changed since your last approval is shown. After
the build, the package is copied to a private directory, its SHA-256 is
recorded, its contents and real install script are audited, and it is
installed only if the hash still matches. Every decision is logged.

It also aims to be a drop-in replacement for pacman. It accepts the same
operations and options, handles the AUR side itself, and passes everything
else to pacman unchanged. After every update or installation it prints a
summary of what changed: packages upgraded, installed, reinstalled,
downgraded and removed.

Updates are fast without giving up safety. AUR repositories are fetched in
parallel. AUR packages are built in parallel, always in dependency order.
Repository packages are downloaded in the background while you review the
AUR changes. The level of parallelism adapts to the machine and to the
measured connection speed, so it can speed up a fast system and stay gentle
on slow or metered links and modest computers. inkaur can also rank your
pacman mirrors by speed and freshness, and tune pacman's parallel downloads
to your connection.

inkaur is written in C99 and links only against the C library: no
third-party libraries, and no shell is ever used to run commands.

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
inkaur -A <dir>          # audit of a local PKGBUILD dir, explained (exit 0..3 = severity)
inkaur --integrity [pkg...]     # check the installed AUR packages (exit 0..3 = severity)
inkaur -S --review=full pkg     # open the code in the pager right away
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
* **Overlap.** `-Syu` runs `pacman -Sy`, then downloads the repository upgrade
  in the background (`sudo -n pacman -Suw`, log in
  `~/.cache/inkaur/.logs/repo-download.log`) while the AUR updates are looked
  up, fetched and reviewed. Right before the first AUR build, `pacman -Su`
  installs from the cache, so AUR packages are never built against a system
  that has not been upgraded yet. If that upgrade fails or you decline it, no
  AUR package is built. Any foreground root pacman call waits for the
  background one first (database lock). Queries (`-T`, `-Sp`, `-Q`, `-Si`)
  don't take the lock, so resolution keeps working meanwhile.
* **Build.** Up to *BuildJobs* AUR packages are built at once. A package starts
  only after every AUR package it depends on has been built, audited and
  installed. Repository dependencies of the builds are installed first in one
  `pacman -S --asdeps --needed`. Builds run without a terminal and write to
  `~/.cache/inkaur/.logs/<pkgbase>.log` (the last lines are shown on failure).
  While they run, a small block is refreshed every second with one line per
  build: what it is doing in plain words (downloading the source code,
  checking download fingerprints, compiling, running tests, packaging...),
  a percentage when the build tools print one, and the elapsed time:

  ```
     / 0 of 3 finished, 2 building now, 1 waiting to start
       libarchive-static  1m20s   downloading the source code 70% (libarchive-3.8.1.tar.xz)
       openmsx            1m20s   compiling 47%
  ```

  Without a terminal, the same lines are printed once a minute.
  Everything interactive stays one at a time: reviews, PGP key confirmation,
  HIGH-finding decisions and `pacman -U`. `--chroot` builds always run one at
  a time.

The defaults (`auto`) speed things up. They scale down on fewer than 4 CPUs,
less than 3 GiB of free RAM, or a slow connection:

| measured speed | fetch jobs | background repo download |
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
inkaur -Syu --repo-download=mirrors --mirrors 3   # split downloads across mirrors
inkaur --rankmirrors     # rank /etc/pacman.d/mirrorlist (--all: commented ones too)
```

`--speedtest` saves its result for 7 days, and `auto` uses it. Settings go in
`/etc/inkaur.conf` and then `~/.config/inkaur/inkaur.conf` (the latter
overrides; files must not be group/world-writable):

```ini
[options]
Parallel  = auto     # auto | yes | no
BuildJobs = auto     # auto | 1..64
FetchJobs = auto     # auto | 1..64
RepoDownload = auto  # auto | pacman (pacman -Suw) | mirrors (inkaur) | no
Mirrors   = auto     # mirrors per repository for RepoDownload = mirrors (1..8)
SpeedTest = auto     # auto (use the saved result) | always (measure before -Su) | never
Review    = simple   # simple (safety summary, code with [v]) | full (code opens right away)
```

### Integrity of installed AUR packages

`inkaur --integrity` checks every installed AUR (foreign) package, or only
the ones you name, and explains the result in plain words:

* **Files.** Every installed file is compared with the checksum, size,
  permissions and owner recorded when it was installed (`pacman -Qkk`). A
  changed program is a *problem*, a deleted file is *to check*, and an edited
  configuration file is only a note. Files only root can read are listed as
  not checked.
* **Origin.** Every package inkaur installs is recorded (version, reviewed
  AUR commit, SHA-256 of the package file) in
  `~/.local/share/inkaur/installed/`. If the installed version is not the one
  inkaur checked, it was installed some other way (makepkg, `pacman -U`,
  another helper).
* **AUR.** Packages deleted from the AUR (sometimes removed for being
  malicious, and never updated again), orphaned, flagged out of date, or with
  an update available.

```
   ok      downgrade                12.0.4-1
   PROBLEM networkmanager-snx       9dce681-1
   !  a file is different from the one the package installed
        (/usr/bin/nm-snx-discover, and 3 more places)

:: Summary: 39 ok, 1 to check, 2 with problems, 0 dangerous
```

The exit status is the highest severity (0..3), so it can run from a timer.

### Repository downloads and mirrors

* **ParallelDownloads.** `inkaur --speedtest` recommends a value for the
  measured throughput (< 128 KiB/s: 1, < 512 KiB/s: 2, < 2 MiB/s: 3,
  < 8 MiB/s: 5, faster: 8). Interactively, it offers to write it to
  `/etc/pacman.conf`: it shows the change, asks (default no) and keeps
  `pacman.conf.inkaur-bak`.
* **Mirror ranking.** `inkaur --rankmirrors` downloads `core.db` from every
  `Server` in the mirrorlist in parallel and reads each mirror's `lastsync`.
  Mirrors more than 6 hours behind the freshest are listed as out of date and
  never selected. After you confirm, the fastest up-to-date ones (`--count`,
  default 10) become active, and the previous entries stay as comments
  (backup `mirrorlist.inkaur-bak`).
* **Multi-mirror download** (`RepoDownload = mirrors`). The pending files
  (`pacman -Sup`) are split across the first *Mirrors* servers of each
  repository, with fallback to the others. They are downloaded as your user
  into `~/.cache/inkaur/.pkgdl`, checked for the exact size, and copied
  root-owned into pacman's cache with `install -o root`. pacman then verifies
  every signature from the sync database, exactly as for its own downloads, so
  a tampered or truncated file is rejected. Files that fail are simply left
  for pacman. This only helps if a mirror limits the speed per connection;
  otherwise `RepoDownload = pacman` is just as fast.

Files written as root (`pacman.conf`, `mirrorlist`, cached packages) are
always installed through `sudo`/`doas` `/usr/bin/install`, never through a
shell.

## Security model

### Easy to use: the safety summary

You do not need to read a PKGBUILD to use inkaur safely. Before anything is
built, each package gets a summary like this:

```
:: Safety check of foo-bin @ 3f2a9c1e0b7d
   Serious warning signs
   Do not install unless you know why the package needs this. If unsure,
   choose [s] skip and ask in the package's AUR comments or the forums.

   About it (AUR): 0 votes, popularity 0.00, first published 2026-11-08, last updated 2026-11-08
   Downloads: 1 file: 0 with a strong fingerprint, 0 weak, 1 NOT checked
   First install: the whole package is new to you

   What the automatic check found (! serious, * check, - note):
   !  a download has no fingerprint (SKIP), so it could be replaced on the server unnoticed
        (.SRCINFO)
   *  this package is very new and nobody has voted for it yet; recent AUR malware looked exactly like this
        (AUR)
   -  installs a ready-made program instead of building it; you trust whoever made that file
        (.SRCINFO)

:: foo-bin @ 3f2a9c1e0b7d: build and install this version?
   safety check: Serious warning signs (HIGH)
   [a] approve
   [s] skip foo-bin and continue with the other packages  <- recommended
   [v] review the build files: PKGBUILD and scripts (optional)
   [x] abort everything
```

* **Verdict.** *No warning signs found*, *Some things need your attention*,
  *Serious warning signs* or *DANGER: looks malicious or broken*, with advice
  in plain words and the recommended choice marked.
* **Every warning explained.** Each finding says what it means for you
  ("downloads a program from the internet and runs it at once"), grouped so
  the same warning is not repeated, with the file and line.
* **Trust signals from the AUR.** Votes, popularity, age, maintainer and
  out-of-date flag. A brand-new package with no votes is a warning: recent AUR
  malware campaigns used exactly such packages.
* **How downloads are verified.** How many files have a SHA-256 (or stronger)
  fingerprint, how many only a weak one, and how many are not checked at all.
* **Hard stops.** Approving HIGH findings needs the word `yes`; CRITICAL
  findings need the package name typed out.

`[v]` opens the full code (or, on updates, exactly what changed) in the
secure pager. To always open the code first, as before, set `Review = full`
in `inkaur.conf` or pass `--review=full`.

### What happens on `-S`

1. **Resolve.** AUR dependencies are read from each repo's `.SRCINFO` at the
   fetched commit. They are parsed as data, never executed.
2. **Review everything first.** Each package is checked out at an exact commit
   (`git reset --hard` + `git clean -ffdx`) and audited, and the safety
   summary is shown. The whole repository (on updates, only the diff since
   your last approved commit) is shown in a pager with `[v]`, or right away
   with `Review = full`.
3. **Decide, per package.** Every package gets an explicit menu with no
   default answer:
   ```
   [a] approve
   [s] skip pkg and continue with the other packages (installed version is kept)
   [r] remove pkg from the system and continue with the others
   [v] show the review again
   [x] abort everything
   ```
   Approving a package with HIGH findings also requires typing the
   fixed word `yes`; CRITICAL findings require typing the package name. Skipping or removing a package also skips the packages in
   the plan that need it, unless an installed older version still satisfies
   them. A failed build or a rejected built package works the same way. The
   run ends with a summary of what was installed, skipped, removed or failed.
   Decisions require a real terminal (`yes |` is refused), and `--noconfirm`
   only affects pacman/makepkg questions.
4. **Verify before building.** The worktree is reset again to the approved
   commit. PGP keys from `validpgpkeys` are never imported silently: each full
   fingerprint is shown, and you confirm it separately. Short key IDs are
   rejected. The `.SRCINFO` is compared with `makepkg --printsrcinfo`.
5. **Pin the built package by hash.** Each built file is copied (never
   through a symlink, never over an existing file) into a private
   `~/.cache/inkaur/.staged/<pkgbase>/` while its SHA-256 is computed. The
   copy is what is audited and installed, and the hash is shown and logged.
6. **Audit the built package** before `pacman -U`. HIGH findings bring up the
   same menu. It checks for setuid/setgid and world-writable files, `..`
   paths, files under `/home`, `/root`, `sudoers`, `ld.so.preload`, PAM,
   `profile.d`, pacman hooks, pre-enabled systemd units, cron and kernel
   modules. The real `.INSTALL` inside the package (which runs as root) is
   scanned with the same rules as the PKGBUILD, and `.PKGINFO` must name the
   expected package and pkgbase: a package file that would install itself as
   another package (say `sudo`) is CRITICAL.
7. **Verify the hash before installing.** Right before `pacman -U`, every
   staged file is hashed again. If anything changed since it was audited,
   nothing is installed and the mismatch is logged.
8. Every approval, rejection, key import, package hash and installation goes
   to syslog (`journalctl -t inkaur`).

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

* AUR metadata: brand-new packages with no votes, orphaned and out-of-date
  packages.
* Built package: file listing, the real `.INSTALL`, and `.PKGINFO` identity.

Every finding has a plain-language explanation, and the report becomes one
of four verdicts. The audit is a strong helper with hard stops, not a
guarantee: it cannot prove that a package is safe. When in doubt, skip the
package and ask in its AUR comments, or prefer well-established packages.

Limits of the hash pin: it guarantees that what is installed is byte for
byte what was audited, even if the build directory is changed afterwards. A
build already runs as your user, so it cannot protect against a malicious
PKGBUILD you approved; use `--chroot` to build in a clean, isolated
environment.

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

## Mascot

The mascot at the top is an original ink-drop ghost. The binary stores it as
a 16x16 pixel map in `mascot.c`, with no image file, and `--version` draws it
in color on a terminal. `mascot.svg` is generated from the same map with
`make mascot.svg`.

## License

MIT License.
