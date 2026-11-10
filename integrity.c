#include "integrity.h"
#include "alloc.h"
#include "config.h"
#include "fsutil.h"
#include "pacman.h"
#include "rpc.h"
#include "sha256.h"
#include "tty.h"
#include "validate.h"
#include "vercmp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char g_home[PATH_BUF];

void integrity_set_home(const char *home)
{
    snprintf(g_home, sizeof g_home, "%s", home ? home : "");
}

/* Creates dir if missing (mode 0700); an existing one must be ours or root's. */
static int ensure_dir(const char *dir)
{
    struct stat st;
    if (lstat(dir, &st) == 0)
        return S_ISDIR(st.st_mode) && (st.st_uid == getuid() || st.st_uid == 0) ? 0 : -1;
    return errno == ENOENT && mkdir(dir, 0700) == 0 ? 0 : -1;
}

/* ~/.local/share/inkaur/installed (created when create is true). */
static bool records_dir(char *buf, size_t sz, bool create)
{
    char base[PATH_BUF];
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] == '/') {
        if (snprintf(base, sizeof base, "%s", xdg) >= (int)sizeof base)
            return false;
        if (create && ensure_dir(base) != 0)
            return false;
    } else {
        char local[PATH_BUF];
        if (g_home[0] != '/' || !path_join(local, sizeof local, g_home, ".local") ||
            !path_join(base, sizeof base, local, "share"))
            return false;
        if (create && (ensure_dir(local) != 0 || ensure_dir(base) != 0))
            return false;
    }
    char ink[PATH_BUF];
    if (!path_join(ink, sizeof ink, base, "inkaur") || !path_join(buf, sz, ink, "installed"))
        return false;
    if (create && (ensure_private_dir(ink) != 0 || ensure_private_dir(buf) != 0))
        return false;
    return true;
}

static bool is_hex64(const char *s)
{
    size_t n = strlen(s);
    if (n != SHA256_HEX_LEN)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return false;
    return true;
}

int integrity_record(const InstallRecord *rec)
{
    if (!valid_pkgname(rec->name) || !valid_pkgname(rec->base) || !valid_version(rec->version) ||
        !valid_commit(rec->commit) || !is_hex64(rec->sha256))
        return -1;
    char dir[PATH_BUF], path[PATH_BUF];
    if (!records_dir(dir, sizeof dir, true) || !path_join(path, sizeof path, dir, rec->name))
        return -1;
    StrBuf b;
    sb_init(&b, 0);
    sb_printf(&b, "pkgname=%s\npkgbase=%s\nversion=%s\ncommit=%s\nsha256=%s\ninstalled=%ld\n",
              rec->name, rec->base, rec->version, rec->commit, rec->sha256, rec->installed);
    int rc = write_file_atomic(path, b.data, b.len);
    sb_free(&b);
    return rc;
}

static void field(const char *line, const char *key, char *out, size_t sz)
{
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) == 0 && line[kl] == '=')
        snprintf(out, sz, "%s", line + kl + 1);
}

bool integrity_parse_record(const char *text, InstallRecord *rec)
{
    memset(rec, 0, sizeof *rec);
    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, text ? text : "");
    char when[32] = "";
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        field(l, "pkgname", rec->name, sizeof rec->name);
        field(l, "pkgbase", rec->base, sizeof rec->base);
        field(l, "version", rec->version, sizeof rec->version);
        field(l, "commit", rec->commit, sizeof rec->commit);
        field(l, "sha256", rec->sha256, sizeof rec->sha256);
        field(l, "installed", when, sizeof when);
    }
    sl_free(&lines);
    rec->installed = strtol(when, NULL, 10);
    return valid_pkgname(rec->name) && valid_pkgname(rec->base) && valid_version(rec->version) &&
           valid_commit(rec->commit) && is_hex64(rec->sha256);
}

bool integrity_load(const char *name, InstallRecord *rec)
{
    char dir[PATH_BUF], path[PATH_BUF];
    if (!valid_pkgname(name) || !records_dir(dir, sizeof dir, false) ||
        !path_join(path, sizeof path, dir, name))
        return false;
    StrBuf b;
    sb_init(&b, 4096);
    bool ok = read_file_limited(path, 4096, &b) == 0 && integrity_parse_record(b.data, rec) &&
              strcmp(rec->name, name) == 0;
    sb_free(&b);
    return ok;
}

/* ------------------------------------------------------------ pacman -Qkk */

static void qkk_reason(Report *r, const char *path, const char *reason, bool backup)
{
    if (backup) {
        report_add(r, SEV_INFO, path, 0, "configuration file edited since installation", reason);
        return;
    }
    if (strstr(reason, "failed to calculate") || strstr(reason, "Permission denied"))
        report_add(r, SEV_INFO, path, 0, "file not readable without root: not checked", reason);
    else if (strstr(reason, "checksum mismatch") || strstr(reason, "Size mismatch") ||
             strstr(reason, "File type mismatch"))
        report_add(r, SEV_HIGH, path, 0, "file content changed since installation", reason);
    else if (strstr(reason, "Permissions mismatch") || strstr(reason, "UID mismatch") ||
             strstr(reason, "GID mismatch"))
        report_add(r, SEV_HIGH, path, 0, "file permissions or owner changed since installation",
                   reason);
    else if (strstr(reason, "Symlink path mismatch"))
        report_add(r, SEV_HIGH, path, 0, "symlink target changed since installation", reason);
    else if (strstr(reason, "No such file") || strstr(reason, "missing"))
        report_add(r, SEV_WARN, path, 0, "file missing since installation", reason);
    else if (strstr(reason, "Modification time mismatch"))
        report_add(r, SEV_INFO, path, 0, "modification time changed since installation", reason);
    else
        report_add(r, SEV_WARN, path, 0, "file differs from the installed version", reason);
}

void integrity_parse_qkk(const char *text, const char *name, Report *r, bool *checked)
{
    *checked = false;
    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, text ? text : "");
    char prefix[300];
    snprintf(prefix, sizeof prefix, "%s: ", name);
    size_t pl = strlen(prefix);
    for (size_t i = 0; i < lines.n; i++) {
        const char *l = lines.v[i];
        bool backup = false;
        if (strncmp(l, "warning: ", 9) == 0) {
            l += 9;
        } else if (strncmp(l, "backup file: ", 13) == 0) {
            l += 13;
            backup = true;
        } else {
            if (strncmp(l, prefix, pl) == 0 && strstr(l + pl, " total files") != NULL)
                *checked = true;   /* "name: 154 total files, 0 altered files" */
            continue;
        }
        if (strncmp(l, prefix, pl) != 0)
            continue;
        l += pl;
        /* "path (reason)": the reason is the last parenthesized part */
        const char *open = strrchr(l, '(');
        size_t len = strlen(l);
        if (open == NULL || len == 0 || l[len - 1] != ')' || open == l) {
            report_add(r, SEV_WARN, l, 0, "file differs from the installed version", NULL);
            continue;
        }
        char *path = xstrndup(l, (size_t)(open - l - 1 > 0 ? open - l - 1 : 0));
        char *reason = xstrndup(open + 1, (size_t)(l + len - 1 - (open + 1)));
        qkk_reason(r, path, reason, backup);
        free(path);
        free(reason);
    }
    sl_free(&lines);
}

/* ------------------------------------------------------------ command */

static const char *status_label(Verdict v)
{
    switch (v) {
    case VERDICT_OK:      return "ok";
    case VERDICT_CAUTION: return "check";
    case VERDICT_DANGER:  return "PROBLEM";
    case VERDICT_BLOCK:   return "DANGER";
    }
    return "?";
}

static const char *status_color(Verdict v)
{
    return v >= VERDICT_DANGER ? C_RED : v == VERDICT_CAUTION ? C_YEL : C_GREEN;
}

static bool ends_with_debug(const char *s)
{
    size_t n = strlen(s);
    return n > 6 && strcmp(s + n - 6, "-debug") == 0;
}

int cmd_integrity(const StrList *targets)
{
    StrList all, allv;
    sl_init(&all);
    sl_init(&allv);
    if (pacman_foreign(&all, &allv) != 0) {
        fprintf(stderr, "error: could not list the installed AUR packages (pacman -Qm)\n");
        sl_free(&all);
        sl_free(&allv);
        return 4;
    }
    StrList names, versions;
    sl_init(&names);
    sl_init(&versions);
    int rc = 0;
    for (size_t i = 0; i < all.n; i++)
        if (targets->n == 0 || sl_contains(targets, all.v[i])) {
            sl_push(&names, all.v[i]);
            sl_push(&versions, allv.v[i]);
        }
    for (size_t i = 0; i < targets->n; i++)
        if (!sl_contains(&all, targets->v[i])) {
            fputs("error: not an installed AUR (foreign) package: ", stderr);
            safe_fputs(stderr, targets->v[i]);
            fputc('\n', stderr);
            rc = 4;
        }
    sl_free(&all);
    sl_free(&allv);
    if (rc != 0 || names.n == 0) {
        if (rc == 0)
            printf(" no AUR packages are installed\n");
        sl_free(&names);
        sl_free(&versions);
        return rc;
    }

    printf("%s:: Checking the integrity of %zu AUR package%s%s\n", C_BOLD, names.n,
           names.n == 1 ? "" : "s", C_END);
    printf("   comparing every installed file with what was installed (pacman -Qkk)...\n");
    fflush(stdout);
    StrBuf qkk;
    sb_init(&qkk, MAX_GIT_OUTPUT);
    bool files_ok = pacman_check_files(&names, &qkk) == 0;
    if (!files_ok)
        printf("   %swarning:%s the file check could not run; only the other checks follow\n",
               C_YEL, C_END);

    printf("   asking the AUR about each package...\n\n");
    fflush(stdout);
    StrList aur_names;
    sl_init(&aur_names);
    for (size_t i = 0; i < names.n; i++)
        if (!ends_with_debug(names.v[i]))
            sl_push(&aur_names, names.v[i]);
    RpcResult *batches = xcalloc(aur_names.n / RPC_BATCH + 1, sizeof(RpcResult));
    size_t nb = 0;
    bool aur_ok = true;
    for (size_t off = 0; off < aur_names.n; off += RPC_BATCH) {
        StrList batch;
        sl_init(&batch);
        for (size_t i = off; i < aur_names.n && i < off + RPC_BATCH; i++)
            sl_push(&batch, aur_names.v[i]);
        if (rpc_info(&batch, &batches[nb]) == 0)
            nb++;
        else
            aur_ok = false;
        sl_free(&batch);
    }
    sl_free(&aur_names);
    if (!aur_ok)
        printf("   %swarning:%s the AUR could not be reached; AUR checks were skipped\n\n",
               C_YEL, C_END);

    size_t count[4] = { 0, 0, 0, 0 }, no_record = 0, unchecked = 0;
    Severity worst = SEV_INFO;
    size_t width = 0;
    for (size_t i = 0; i < names.n; i++)
        if (strlen(names.v[i]) > width)
            width = strlen(names.v[i]);
    if (width > 40)
        width = 40;

    for (size_t i = 0; i < names.n; i++) {
        const char *name = names.v[i], *ver = versions.v[i];
        Report r;
        report_init(&r);
        bool checked = false;
        if (files_ok) {
            integrity_parse_qkk(qkk.data, name, &r, &checked);
            if (!checked)
                unchecked++;
        }

        InstallRecord rec;
        if (integrity_load(name, &rec)) {
            if (strcmp(rec.version, ver) != 0) {
                char where[600];
                snprintf(where, sizeof where, "installed %s, inkaur checked %s", ver, rec.version);
                report_add(&r, SEV_WARN, where, 0,
                           "installed version differs from the version inkaur checked", NULL);
            }
        } else {
            no_record++;
        }

        if (aur_ok && !ends_with_debug(name)) {
            const AurPkg *a = NULL;
            for (size_t b = 0; b < nb && a == NULL; b++)
                a = rpc_find(&batches[b], name);
            if (a == NULL) {
                report_add(&r, SEV_WARN, "AUR", 0, "not found in the AUR", NULL);
            } else {
                if (a->orphan)
                    report_add(&r, SEV_WARN, "AUR", 0, "ORPHANED package (no maintainer)", NULL);
                if (a->outofdate)
                    report_add(&r, SEV_INFO, "AUR", 0, "flagged OUT OF DATE", NULL);
                if (pkg_vercmp(a->version, ver) > 0) {
                    char where[600];
                    snprintf(where, sizeof where, "AUR has %s", a->version);
                    report_add(&r, SEV_INFO, where, 0, "update available in the AUR", NULL);
                }
            }
        }

        Verdict v = audit_verdict(&r);
        count[v]++;
        if (r.max > worst)
            worst = r.max;
        printf("   %s%-7s%s %-*s %s\n", status_color(v), status_label(v), C_END, (int)width, name,
               ver);
        if (r.n) {
            report_print_plain(&r, stdout, use_color());
            putchar('\n');
        }
        report_free(&r);
    }

    printf("\n%s:: Summary%s: %zu ok, %zu to check, %zu with problems, %zu dangerous\n", C_BOLD,
           C_END, count[VERDICT_OK], count[VERDICT_CAUTION], count[VERDICT_DANGER],
           count[VERDICT_BLOCK]);
    if (no_record)
        printf("   %zu package%s installed before inkaur kept install records, or by another\n"
               "   tool: the files were checked, but their origin cannot be confirmed.\n",
               no_record, no_record == 1 ? " was" : "s were");
    if (unchecked)
        printf("   %zu package%s could not be checked file by file.\n", unchecked,
               unchecked == 1 ? "" : "s");
    if (worst >= SEV_HIGH)
        printf("\n   %sA file that changed after installation can be a program updating itself,\n"
               "   your own change, or tampering.%s If you did not expect it, reinstall the\n"
               "   package to restore the original files (inkaur -S <package>) and look into\n"
               "   who changed it (journalctl -t inkaur shows what inkaur installed).\n",
               C_RED, C_END);
    else if (worst == SEV_WARN)
        printf("\n   Nothing looks tampered with; the items marked * deserve a look.\n");
    else
        printf("\n   %sEverything matches what was installed.%s\n", C_GREEN, C_END);

    for (size_t b = 0; b < nb; b++)
        rpc_result_free(&batches[b]);
    free(batches);
    sb_free(&qkk);
    sl_free(&names);
    sl_free(&versions);
    return (int)worst;
}
