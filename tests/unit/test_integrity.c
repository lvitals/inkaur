#include "../test_helper.h"
#include "integrity.h"

#include <stdlib.h>
#include <unistd.h>

void run_integrity_tests(void);

static const char *qkk =
    "warning: aide: /etc/aide.conf (Permissions mismatch)\n"
    "ack-cpm: 154 total files, 0 altered files\n"
    "backup file: aide: /etc/aide.conf (Modification time mismatch)\n"
    "backup file: aide: /etc/aide.conf (SHA256 checksum mismatch)\n"
    "warning: aide: /usr/bin/aide (SHA256 checksum mismatch)\n"
    "warning: aide: /usr/bin/aide (Modification time mismatch)\n"
    "warning: aide: /usr/share/doc/aide (No such file or directory)\n"
    "warning: aide: /etc/aide.d/secret (failed to calculate SHA256 checksum)\n"
    "warning: aide: /usr/lib/x (Symlink path mismatch)\n"
    "warning: aide-extra: /usr/bin/other (SHA256 checksum mismatch)\n"
    "aide: 22 total files, 5 altered files\n";

static size_t count_msg(const Report *r, const char *msg, Severity s)
{
    size_t n = 0;
    for (size_t i = 0; i < r->n; i++)
        if (strstr(r->f[i].msg, msg) && r->f[i].sev == s)
            n++;
    return n;
}

void run_integrity_tests(void)
{
    Report r;
    bool checked;

    report_init(&r);
    integrity_parse_qkk(qkk, "ack-cpm", &r, &checked);
    ASSERT(checked && r.n == 0, "untouched package: checked, no findings");
    report_free(&r);

    report_init(&r);
    integrity_parse_qkk(qkk, "aide", &r, &checked);
    ASSERT(checked, "summary line seen");
    ASSERT(r.max == SEV_HIGH, "changed binary is a problem");
    ASSERT(count_msg(&r, "content changed", SEV_HIGH) == 1, "only aide's own binary, not aide-extra's");
    ASSERT(count_msg(&r, "permissions or owner changed", SEV_HIGH) == 1, "permissions change");
    ASSERT(count_msg(&r, "symlink target changed", SEV_HIGH) == 1, "symlink change");
    ASSERT(count_msg(&r, "file missing", SEV_WARN) == 1, "missing file");
    ASSERT(count_msg(&r, "not readable without root", SEV_INFO) == 1, "unreadable file");
    ASSERT(count_msg(&r, "configuration file edited", SEV_INFO) == 2, "edited config is only a note");
    for (size_t i = 0; i < r.n; i++)
        ASSERT(audit_explain(r.f[i].msg) != NULL, "integrity finding has a plain explanation");
    ASSERT(strcmp(r.f[0].file, "/etc/aide.conf") == 0, "path parsed");
    report_free(&r);

    report_init(&r);
    integrity_parse_qkk("error: package 'zzz' was not found\n", "zzz", &r, &checked);
    ASSERT(!checked && r.n == 0, "missing package: not checked");
    report_free(&r);

    /* records */
    InstallRecord rec;
    const char *good = "pkgname=foo\npkgbase=foo\nversion=1.2-1\n"
                       "commit=0123456789abcdef0123456789abcdef01234567\n"
                       "sha256=e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
                       "installed=1700000000\n";
    ASSERT(integrity_parse_record(good, &rec), "valid record parses");
    ASSERT(strcmp(rec.version, "1.2-1") == 0 && rec.installed == 1700000000L, "record fields");
    ASSERT(!integrity_parse_record("pkgname=../x\npkgbase=x\nversion=1-1\n", &rec),
           "invalid record refused");

    char dir[512];
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/inkaur-int-XXXXXX", t ? t : "/tmp");
    ASSERT(mkdtemp(dir) != NULL, "mkdtemp");
    setenv("XDG_DATA_HOME", dir, 1);
    ASSERT(integrity_parse_record(good, &rec), "reparse");
    ASSERT(integrity_record(&rec) == 0, "record written");
    InstallRecord back;
    ASSERT(integrity_load("foo", &back) && strcmp(back.sha256, rec.sha256) == 0, "record read back");
    ASSERT(!integrity_load("bar", &back), "no record for another package");
    ASSERT(!integrity_load("../foo", &back), "invalid name refused");
    snprintf(rec.version, sizeof rec.version, "bad version!");
    ASSERT(integrity_record(&rec) != 0, "invalid record not written");

    char p[700];
    snprintf(p, sizeof p, "%s/inkaur/installed/foo", dir);
    unlink(p);
    snprintf(p, sizeof p, "%s/inkaur/installed", dir);
    rmdir(p);
    snprintf(p, sizeof p, "%s/inkaur", dir);
    rmdir(p);
    rmdir(dir);
    unsetenv("XDG_DATA_HOME");
    printf("integrity: ok\n");
}
