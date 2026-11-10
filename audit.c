#include "audit.h"
#include "alloc.h"
#include "buf.h"
#include "config.h"
#include "fsutil.h"
#include "git.h"
#include "proc.h"
#include "srcinfo.h"
#include "validate.h"

#include <dirent.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_FINDINGS_PER_FILE 60

/* ------------------------------------------------------------ report */

const char *severity_name(Severity s)
{
    switch (s) {
    case SEV_INFO: return "INFO";
    case SEV_WARN: return "WARN";
    case SEV_HIGH: return "HIGH";
    case SEV_CRIT: return "CRITICAL";
    }
    return "?";
}

void report_init(Report *r)
{
    memset(r, 0, sizeof *r);
    r->max = SEV_INFO;
}

void report_add(Report *r, Severity s, const char *file, long line,
                const char *msg, const char *excerpt)
{
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 32;
        r->f = xreallocarray(r->f, r->cap, sizeof(Finding));
    }
    Finding *f = &r->f[r->n++];
    f->sev = s;
    f->file = xstrdup(file ? file : "");
    f->line = line;
    f->msg = xstrdup(msg);
    f->excerpt = excerpt ? xstrndup(excerpt, 160) : NULL;
    if (s > r->max)
        r->max = s;
}

void report_free(Report *r)
{
    for (size_t i = 0; i < r->n; i++) {
        free(r->f[i].file);
        free(r->f[i].msg);
        free(r->f[i].excerpt);
    }
    free(r->f);
    report_init(r);
}

static const char *sev_color(Severity s)
{
    switch (s) {
    case SEV_CRIT: return "\033[1;97;41m";
    case SEV_HIGH: return "\033[1;91m";
    case SEV_WARN: return "\033[1;93m";
    default:       return "\033[1;94m";
    }
}

void report_print(const Report *r, FILE *fp, bool color)
{
    if (r->n == 0) {
        fputs("   no findings\n", fp);
        return;
    }
    for (int s = SEV_CRIT; s >= SEV_INFO; s--) {
        for (size_t i = 0; i < r->n; i++) {
            const Finding *f = &r->f[i];
            if ((int)f->sev != s)
                continue;
            if (color)
                fprintf(fp, "   %s%-8s\033[0m ", sev_color(f->sev), severity_name(f->sev));
            else
                fprintf(fp, "   %-8s ", severity_name(f->sev));
            safe_fputs(fp, f->file);
            if (f->line > 0)
                fprintf(fp, ":%ld", f->line);
            fputs(": ", fp);
            safe_fputs(fp, f->msg);
            fputc('\n', fp);
            if (f->excerpt) {
                fputs("              | ", fp);
                safe_fputs(fp, f->excerpt);
                fputc('\n', fp);
            }
        }
    }
}

/* ------------------------------------------------------------ plain language */

/*
 * Plain-language explanations, matched by substring of the finding message.
 * Order matters: the first match wins, so specific keys come first.
 */
static const struct {
    const char *key;
    const char *plain;
} explain[] = {
    /* these messages contain package names: match them before anything else */
    { "does not match the expected package", "the built package claims to be a different package than the one you approved" },
    { "no .PKGINFO", "the built package has no description; pacman would not accept it as built" },
    /* remote code / backdoors */
    { "download piped straight into a shell",
      "downloads a program from the internet and runs it at once; that code was never shown to you (typical of malware)" },
    { "base64 payload piped into a shell", "runs hidden, encoded code (typical of malware)" },
    { "/dev/tcp", "opens a network connection from the script (used by remote-control backdoors)" },
    { "netcat -e", "gives another computer control of a shell on yours (backdoor)" },
    { "interactive shell redirected", "gives another computer control of a shell on yours (backdoor)" },
    { "named pipe / socat", "uses a building block common in backdoors; rare in normal packages" },
    { "authorized_keys", "touches SSH login keys: could let someone log in to your computer" },
    { "references a critical system file", "touches files that control passwords, admin rights or every program" },
    { "recursive delete", "deletes your whole system or your personal folder" },
    /* obfuscation */
    { "base64 decoding", "decodes hidden text; sometimes harmless data, but often used to hide code" },
    { "hex/openssl decoding", "decodes hidden text; sometimes harmless data, but often used to hide code" },
    { "hex-escaped byte string", "text written as raw bytes, hard to read; can hide code" },
    { "eval executes", "builds code while running, so what runs may differ from what you read" },
    { "inline interpreter code", "runs a small program in another language (Python, Perl...) inside the script" },
    /* privileges / persistence */
    { "sudo inside", "asks for administrator rights during the build; packages never need this" },
    { "systemd units", "turns on a background service by itself; that should be your decision" },
    { "pre-enabled systemd unit", "a background service is switched on without asking you" },
    { "admin systemd directory", "puts a service in the folder reserved for the administrator's own settings" },
    { "world-writable", "lets any user on the computer change these files" },
    { "setuid/setgid file in repository", "the package repository contains a file with admin-rights bits set" },
    { "setuid", "a program runs with administrator rights for any user; only a few system tools need this" },
    { "setgid", "a program runs with extra group rights for any user; only a few system tools need this" },
    { "file capabilities", "gives a program special powers, or makes a file impossible to delete" },
    { "cron", "schedules something to run automatically, again and again" },
    { "shell startup file", "changes what runs every time you open a terminal or log in" },
    { "runs code at every login shell", "runs code every time anyone logs in" },
    { "global environment", "changes settings that every program on the system inherits" },
    { "autostart", "starts a program automatically when you log in" },
    { "dynamic loader injection", "forces extra code into other programs when they start" },
    { "library injection", "forces extra code into every program on the system" },
    { "kernel module loading", "loads code into the operating system core (full control of the machine)" },
    { "loads kernel modules at boot", "loads drivers at every boot; normal only for driver packages" },
    { "kernel module", "adds a driver to the operating system core; normal only for driver packages" },
    { "account manipulation", "creates or changes user accounts or passwords" },
    { "writes into a home directory", "writes into your personal folder; packages should only use system folders" },
    { "references a home directory", "mentions your personal folder; usually harmless (docs, defaults), check what it does there" },
    { "installs into a home directory", "puts files in a personal folder; real packages never do this" },
    { "installs into /root", "puts files in the administrator's personal folder; real packages never do this" },
    { "URL shortener", "uses a link shortener, paste site or webhook: places attackers use to hide or swap content" },
    { "raw IP", "connects to a bare number instead of a website name (common in malware)" },
    /* network during build */
    { "downloads during build", "downloads files while building; they are not checked against a fingerprint" },
    { "fetches code during build", "downloads extra code while building (git/pip/npm/go); common for some languages, but that code is not verified" },
    { "detaches a background process", "leaves a program running in the background after the build" },
    { "firewall", "changes firewall rules" },
    { "udev rule runs", "runs a program as administrator when a device is plugged in; normal for hardware tools" },
    { "udev rule", "reacts to devices being plugged in; normal for hardware tools" },
    /* hidden characters */
    { "carriage return", "contains invisible characters that can hide code from you on screen" },
    { "terminal control character", "contains invisible characters that can hide code from you on screen" },
    { "invisible/bidi Unicode", "contains invisible characters that make code look different from what really runs" },
    /* .SRCINFO / sources */
    { "entries but source", "the list of fingerprints does not match the list of downloads" },
    { "sources have no checksums", "downloads have no fingerprint, so nobody can tell if they were replaced" },
    { "remote source without checksum", "a download has no fingerprint (SKIP), so it could be replaced on the server unnoticed" },
    { "only weak checksums", "uses old fingerprints (MD5/SHA-1) that can be forged" },
    { "plain HTTP/FTP", "downloads without encryption; the network could tamper with it unless a fingerprint protects it" },
    { "git:// protocol", "downloads code over a connection that is not authenticated" },
    { "pinned to a tag", "uses a named version (tag) that its author could move later" },
    { "unpinned VCS source", "always builds the newest code from the developer, which nobody reviewed (normal for -git packages)" },
    { "prebuilt binary", "installs a ready-made program instead of building it; you trust whoever made that file" },
    { "no validpgpkeys", "the download is signed but nobody checks the signature" },
    { "not a full 40/64-hex fingerprint", "a signing key is given in a short form that can be faked" },
    { "invalid dependency string", "a dependency is written in an unexpected way" },
    { "install scriptlet referenced", "an install script is announced but missing" },
    { "install scriptlet", "has an install script that runs as administrator during installation; many packages have one" },
    { "malformed .SRCINFO", "the package description is broken or tampered with" },
    { "invalid pkg", "the package name is invalid (broken or tampered with)" },
    { "no pkgname", "the package description is broken" },
    { "missing or not a regular file", "the package is incomplete" },
    { "missing: not a valid AUR", "the package is incomplete" },
    /* repository content */
    { "symlink", "contains a link to another place on disk" },
    { "submodule", "pulls in code from another repository that was not shown to you" },
    { "binary image", "contains a picture (usually an icon)" },
    { "binary file in repository", "contains a compiled file nobody can read; AUR packages should hold only text" },
    { "hidden file", "contains a hidden file" },
    { "special file", "contains a special file (device, pipe or socket)" },
    { "large file not scanned", "a file too large to check automatically" },
    { "too many", "too much content to check automatically" },
    /* built package */
    { "sudoers", "changes who can become administrator" },
    { "shadow file", "replaces the password files" },
    { "passwd file", "replaces the user account files" },
    { "SSH configuration", "changes remote login settings" },
    { "PAM", "changes how logins and passwords are checked" },
    { "security limits", "changes system security limits and access rules" },
    { "pacman hook", "runs as administrator every time you install or update anything" },
    { "pacman configuration", "changes pacman's own settings" },
    { "/tmp", "installs into a temporary folder, which is unusual" },
    { "polkit", "changes which actions are allowed without a password" },
    { "systemd generator", "runs as administrator at every boot" },
    { "sysctl", "changes operating system settings" },
    { "/usr/local", "installs into /usr/local, which is reserved for your own manual installs" },
    { "'..' path", "tries to write files outside the normal places" },
    /* AUR trust signals */
    { "new package with", "this package is very new and nobody has voted for it yet; recent AUR malware looked exactly like this" },
    { "few votes", "few people use this package, so problems are less likely to have been noticed" },
    { "ORPHANED", "nobody maintains this package; anyone can adopt it and change it" },
    { "OUT OF DATE", "users reported that this package is outdated" },
};

const char *audit_explain(const char *msg)
{
    if (msg == NULL)
        return NULL;
    for (size_t i = 0; i < sizeof explain / sizeof explain[0]; i++)
        if (strstr(msg, explain[i].key) != NULL)
            return explain[i].plain;
    return NULL;
}

Verdict audit_verdict(const Report *r)
{
    switch (r->max) {
    case SEV_CRIT: return VERDICT_BLOCK;
    case SEV_HIGH: return VERDICT_DANGER;
    case SEV_WARN: return VERDICT_CAUTION;
    default:       return VERDICT_OK;
    }
}

const char *verdict_title(Verdict v)
{
    switch (v) {
    case VERDICT_OK:      return "No warning signs found";
    case VERDICT_CAUTION: return "Some things need your attention";
    case VERDICT_DANGER:  return "Serious warning signs";
    case VERDICT_BLOCK:   return "DANGER: looks malicious or broken";
    }
    return "?";
}

const char *verdict_advice(Verdict v)
{
    switch (v) {
    case VERDICT_OK:
        return "Nothing unusual was detected. Automatic checks cannot prove that a package\n"
               "   is safe, so prefer packages that many people use.";
    case VERDICT_CAUTION:
        return "These things are common in normal packages. If they fit what the program\n"
               "   is supposed to do, it is usually fine.";
    case VERDICT_DANGER:
        return "Do not install unless you know why the package needs this. If unsure,\n"
               "   choose [s] skip and ask in the package's AUR comments or the forums.";
    case VERDICT_BLOCK:
        return "Recommended: [s] skip. This pattern is typical of malicious packages.\n"
               "   Approving requires typing the package name.";
    }
    return "";
}

static const char *sev_mark(Severity s)
{
    switch (s) {
    case SEV_CRIT: return "!!";
    case SEV_HIGH: return "! ";
    case SEV_WARN: return "* ";
    default:       return "- ";
    }
}

void report_print_plain(const Report *r, FILE *fp, bool color)
{
    if (r->n == 0) {
        fputs("   nothing to report\n", fp);
        return;
    }
    bool *done = xcalloc(r->n, sizeof(bool));
    size_t shown = 0, hidden = 0;
    for (int s = SEV_CRIT; s >= SEV_INFO; s--) {
        for (size_t i = 0; i < r->n; i++) {
            const Finding *f = &r->f[i];
            if ((int)f->sev != s || done[i])
                continue;
            const char *plain = audit_explain(f->msg);
            /* group every finding with the same explanation (or message) */
            size_t count = 0;
            for (size_t j = i; j < r->n; j++) {
                const char *pj = audit_explain(r->f[j].msg);
                bool same = plain ? (pj == plain) : strcmp(r->f[j].msg, f->msg) == 0;
                if (!done[j] && (int)r->f[j].sev == s && same) {
                    done[j] = true;
                    count++;
                }
            }
            if (s == SEV_INFO && shown >= 12) {
                hidden++;
                continue;
            }
            shown++;
            if (color)
                fprintf(fp, "   %s%s\033[0m ", sev_color(f->sev), sev_mark(f->sev));
            else
                fprintf(fp, "   %s ", sev_mark(f->sev));
            safe_fputs(fp, plain ? plain : f->msg);
            fputs("\n        (", fp);
            safe_fputs(fp, f->file[0] ? f->file : "package");
            if (f->line > 0)
                fprintf(fp, " line %ld", f->line);
            if (count > 1)
                fprintf(fp, ", and %zu more place%s", count - 1, count > 2 ? "s" : "");
            fputs(")\n", fp);
        }
    }
    if (hidden)
        fprintf(fp, "   (%zu more minor notes; choose [v] for the technical details)\n", hidden);
    free(done);
}

void audit_aur_meta(Report *r, const AurMeta *m, long now)
{
    if (m == NULL || !m->known)
        return;
    long age_days = m->first_submitted > 0 && now > m->first_submitted
                        ? (now - m->first_submitted) / 86400 : -1;
    char msg[160];
    if (age_days >= 0 && age_days < 30 && m->votes <= 2) {
        snprintf(msg, sizeof msg, "new package with %ld vote%s, first submitted %ld day%s ago",
                 m->votes, m->votes == 1 ? "" : "s", age_days, age_days == 1 ? "" : "s");
        report_add(r, SEV_WARN, "AUR", 0, msg, NULL);
    } else if (m->votes < 5) {
        snprintf(msg, sizeof msg, "few votes (%ld): little community use", m->votes);
        report_add(r, SEV_INFO, "AUR", 0, msg, NULL);
    }
    if (m->orphan)
        report_add(r, SEV_WARN, "AUR", 0, "ORPHANED package (no maintainer)", NULL);
    if (m->outofdate)
        report_add(r, SEV_INFO, "AUR", 0, "flagged OUT OF DATE", NULL);
}

/* ------------------------------------------------------------ rules */

typedef struct {
    Severity sev;
    const char *re;
    const char *msg;
    int path_ref;   /* only names a path: informational inside data files */
    int redirect;   /* needs a real write/redirection: void on display-only lines */
} Rule;

static const Rule rules[] = {
    /* remote code execution / reverse shells */
    { SEV_CRIT, "(curl|wget|fetch|aria2c)[^|;&]*\\|[[:space:]]*(sudo[[:space:]]+)?(ba|z|da|k|fi)?sh([^[:alnum:]_.-]|$)",
      "download piped straight into a shell", 0, 0 },
    { SEV_CRIT, "base64[[:space:]]+(-d|--decode)[^|]*\\|[[:space:]]*(ba|z|da)?sh([^[:alnum:]_.-]|$)",
      "base64 payload piped into a shell", 0, 0 },
    { SEV_CRIT, "/dev/(tcp|udp)/", "bash network socket /dev/tcp (reverse shell pattern)", 0, 0 },
    { SEV_CRIT, "(^|[^[:alnum:]])(nc|ncat|netcat)[[:space:]]+.*-[[:alpha:]]*e[[:space:]]",
      "netcat -e (reverse shell)", 0, 0 },
    { SEV_CRIT, "(ba)?sh[[:space:]]+-i[[:space:]]*>&", "interactive shell redirected to a socket", 0, 0 },
    { SEV_CRIT, "authorized_keys", "touches SSH authorized_keys", 1, 0 },
    { SEV_CRIT, "/etc/(sudoers|shadow|gshadow|ld\\.so\\.preload)", "references a critical system file", 1, 0 },
    { SEV_CRIT, "rm[[:space:]]+-[[:alpha:]]*[rR][[:alpha:]]*[[:space:]]+(--no-preserve-root[[:space:]]+)?(/|~|\\$HOME|\\$\\{HOME\\})([[:space:]]|/?\\*|$)",
      "recursive delete of / or $HOME", 0, 0 },
    { SEV_HIGH, "(mkfifo|socat)[[:space:]]", "named pipe / socat (reverse shell building block)", 0, 0 },

    /* obfuscation */
    { SEV_HIGH, "base64[[:space:]]+(-d|--decode)", "base64 decoding (possible obfuscation)", 0, 0 },
    { SEV_HIGH, "xxd[[:space:]]+-r|openssl[[:space:]]+(enc|base64)[[:space:]].*-d", "hex/openssl decoding (possible obfuscation)", 0, 0 },
    { SEV_WARN, "(\\\\x[0-9a-fA-F]{2}){4,}", "hex-escaped byte string (possible obfuscation)", 0, 0 },
    { SEV_HIGH, "(^|[;&|[:space:]({])eval([[:space:]]|$)", "eval executes dynamically built code", 0, 0 },
    { SEV_HIGH, "(python[0-9.]*|perl|ruby|php|node)[[:space:]]+-[ce][[:space:]]",
      "inline interpreter code", 0, 0 },

    /* privilege / persistence */
    { SEV_HIGH, "(^|[^[:alnum:]_.-])sudo([[:space:]]|$)", "sudo inside a build/install script", 0, 0 },
    { SEV_HIGH, "systemctl[[:space:]]+(--[a-z-]+[[:space:]]+)*(enable|start|restart|mask|daemon-reload)",
      "enables/starts systemd units", 0, 0 },
    { SEV_HIGH, "chmod[[:space:]].*(777|666|[oa]\\+[rwx]*w)", "world-writable permissions", 0, 0 },
    { SEV_HIGH, "chmod[[:space:]](.*[[:space:]])?([ugoa]*\\+[rwxX]*s|[2467][0-7]{3})([[:space:],]|$)",
      "setuid/setgid bit", 0, 0 },
    { SEV_HIGH, "(^|[^[:alnum:]_.-])install[[:space:]](.*[[:space:]])?(-[[:alpha:]]*m[[:space:]]*|--mode=)[0-7]?[2467][0-7]{3}([[:space:]]|$)",
      "setuid/setgid bit", 0, 0 },
    { SEV_HIGH, "(^|[^[:alnum:]_])(setcap|chattr)[[:space:]]", "file capabilities / immutable attribute", 0, 0 },
    { SEV_HIGH, "crontab|/etc/cron|/var/spool/cron", "cron persistence", 1, 0 },
    { SEV_HIGH, "\\.bashrc|\\.bash_profile|\\.zshrc|\\.zprofile|/etc/profile\\.d|/etc/environment",
      "shell startup file (persistence)", 1, 0 },
    { SEV_HIGH, "xdg/autostart|\\.config/autostart", "desktop autostart (persistence)", 1, 0 },
    { SEV_HIGH, "LD_PRELOAD|LD_LIBRARY_PATH=", "dynamic loader injection", 1, 0 },
    { SEV_HIGH, "(^|[^[:alnum:]_.-])(insmod|modprobe|rmmod)([[:space:]]|$)", "kernel module loading", 0, 0 },
    { SEV_HIGH, "(^|[^[:alnum:]_])(useradd|usermod|groupadd|chpasswd)[[:space:]]", "account manipulation", 0, 0 },
    /* merely naming ~/.config (launchers, hints) is normal; writing there is not */
    { SEV_HIGH, "(>>?[[:space:]]*|(^|[^[:alnum:]_.-])(tee|cp|mv|install|ln|rsync)[[:space:]].*)[\"']?(/home/[[:alnum:]_]|/root/|\\$HOME|\\$\\{HOME\\}|~/)",
      "writes into a home directory", 0, 1 },
    { SEV_WARN, "(/home/[[:alnum:]_]|/root/|\\$HOME|\\$\\{HOME\\}|~/)", "references a home directory", 1, 0 },
    { SEV_HIGH, "https?://(bit\\.ly|tinyurl\\.com|goo\\.gl|t\\.co|is\\.gd|cutt\\.ly|rb\\.gy|pastebin\\.com|paste\\.ee|hastebin|transfer\\.sh|0x0\\.st|[a-z0-9-]+\\.ngrok|discord(app)?\\.com/api/webhooks|webhook\\.site)",
      "URL shortener / paste site / webhook", 0, 0 },
    { SEV_HIGH, "(//|@)([0-9]{1,3}\\.){3}[0-9]{1,3}([:/]|$)", "URL pointing to a raw IP address", 0, 0 },

    /* network during build (bypasses source=() + checksums) */
    { SEV_WARN, "(^|[^[:alnum:]_-])(curl|wget|aria2c)[[:space:]]", "downloads during build (not covered by checksums)", 0, 0 },
    { SEV_WARN, "git[[:space:]]+clone|pip[0-9]*[[:space:]]+install|npm[[:space:]]+install|go[[:space:]]+get",
      "fetches code during build outside source=()", 0, 0 },
    { SEV_WARN, "(^|[^[:alnum:]_])(nohup|disown|setsid)([[:space:]]|$)", "detaches a background process", 0, 0 },
    { SEV_WARN, "(^|[^[:alnum:]_])(iptables|nft|ufw)[[:space:]]", "firewall changes", 0, 0 },
    { SEV_WARN, "(^|[,[:space:]])RUN(\\{[a-z]+\\})?\\+?=[[:space:]]*\"", "udev rule runs a program as root on device events", 0, 0 },
};

#define NRULES (sizeof rules / sizeof rules[0])

static regex_t rx[NRULES];
static bool rx_ok[NRULES];
static bool rx_ready;

static void rules_compile(void)
{
    if (rx_ready)
        return;
    for (size_t i = 0; i < NRULES; i++) {
        rx_ok[i] = regcomp(&rx[i], rules[i].re, REG_EXTENDED | REG_NOSUB) == 0;
        if (!rx_ok[i])
            fprintf(stderr, "internal error: bad audit rule %zu\n", i);
    }
    rx_ready = true;
}

typedef enum { K_PKGBUILD, K_INSTALL, K_SCRIPT, K_PATCH, K_SRCINFO, K_DATA } Kind;

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static Kind classify(const char *name, const char *text, size_t len)
{
    const char *b = base_name(name);
    if (strcmp(b, "PKGBUILD") == 0) return K_PKGBUILD;
    if (strcmp(b, ".SRCINFO") == 0) return K_SRCINFO;
    if (ends_with(b, ".install") || strcmp(b, ".INSTALL") == 0) return K_INSTALL;
    if (ends_with(b, ".patch") || ends_with(b, ".diff")) return K_PATCH;
    if (ends_with(b, ".sh") || ends_with(b, ".bash") || ends_with(b, ".zsh") ||
        ends_with(b, ".py") || ends_with(b, ".pl") || ends_with(b, ".rb") ||
        ends_with(b, ".js") || ends_with(b, ".hook") || ends_with(b, ".service") ||
        ends_with(b, ".desktop"))
        return K_SCRIPT;
    if (len >= 2 && text[0] == '#' && text[1] == '!')
        return K_SCRIPT;
    return K_DATA;
}

/* Hidden-character checks: terminal controls, stray CR, bidi/zero-width. */
static void check_line_chars(Report *r, const char *name, long ln,
                             const char *line, size_t n, int *budget)
{
    const unsigned char *s = (const unsigned char *)line;
    for (size_t i = 0; i < n && *budget > 0;) {
        unsigned char c = s[i];
        if ((c < 0x20 && c != '\t' && !(c == '\r' && i == n - 1)) || c == 0x7f) {
            report_add(r, SEV_HIGH, name, ln,
                       c == '\r' ? "carriage return inside a line (can hide code on screen)"
                                 : "terminal control character (can hide code on screen)",
                       NULL);
            (*budget)--;
            return;
        }
        if (c >= 0x80) {
            uint32_t cp;
            size_t len = utf8_decode(s + i, n - i, &cp);
            if (len == 0) {
                i++;
                continue;
            }
            if (is_deceptive_codepoint(cp) || (cp >= 0x80 && cp <= 0x9f)) {
                char msg[96];
                snprintf(msg, sizeof msg,
                         "invisible/bidi Unicode U+%04X (Trojan Source)", (unsigned)cp);
                report_add(r, SEV_HIGH, name, ln, msg, NULL);
                (*budget)--;
                return;
            }
            i += len;
            continue;
        }
        i++;
    }
}

static const char *skip_blank(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* Shell metacharacters outside quotes (or substitutions inside "..."). An
 * unterminated quote counts as code: the string may continue elsewhere. */
static bool has_shell_meta(const char *s)
{
    for (; *s; s++) {
        if (*s == '\\') {
            if (*++s == '\0')
                return true;
        } else if (*s == '\'') {
            s = strchr(s + 1, '\'');
            if (!s)
                return true;
        } else if (*s == '"') {
            for (s++; *s != '"'; s++) {
                if (*s == '\0' || *s == '`' || (s[0] == '$' && s[1] == '('))
                    return true;
                if (*s == '\\' && s[1] != '\0')
                    s++;
            }
        } else if (strchr("|>;&`", *s) || (s[0] == '$' && s[1] == '(')) {
            return true;
        }
    }
    return false;
}

/* Line that only displays text: echo/printf/msg without any pipe,
 * redirection, command substitution or command chaining. */
static bool display_only(const char *line)
{
    const char *s = skip_blank(line);
    if (strncmp(s, "echo ", 5) != 0 && strncmp(s, "printf ", 7) != 0 &&
        strncmp(s, "msg ", 4) != 0 && strncmp(s, "msg2 ", 5) != 0 &&
        strncmp(s, "warning ", 8) != 0)
        return false;
    return !has_shell_meta(s);
}

/* Detects "<<WORD" / "<<-WORD" / "<<'WORD'". Returns true and fills delim. */
static bool heredoc_start(const char *line, char *delim, size_t dsz, bool *is_text)
{
    const char *p = line;
    while ((p = strstr(p, "<<")) != NULL) {
        if (p[2] == '<') {          /* <<< here-string */
            p += 3;
            continue;
        }
        const char *q = p + 2;
        if (*q == '-')
            q++;
        q = skip_blank(q);
        if (*q == '\'' || *q == '"')
            q++;
        size_t n = 0;
        while ((q[n] >= 'A' && q[n] <= 'Z') || (q[n] >= 'a' && q[n] <= 'z') ||
               (q[n] >= '0' && q[n] <= '9') || q[n] == '_')
            n++;
        if (n == 0 || n >= dsz) {
            p += 2;
            continue;
        }
        memcpy(delim, q, n);
        delim[n] = '\0';
        /* plain "cat <<EOF" prints text; anything redirected/piped is code */
        const char *s = skip_blank(line);
        *is_text = strncmp(s, "cat ", 4) == 0 && strpbrk(line, ">|;&`") == NULL &&
                   strstr(line, "$(") == NULL;
        return true;
    }
    return false;
}

void audit_scan_text(Report *r, const char *name, const char *text, size_t len)
{
    rules_compile();
    Kind kind = classify(name, text, len);
    bool shell = kind == K_PKGBUILD || kind == K_INSTALL || kind == K_SCRIPT;
    int budget = MAX_FINDINGS_PER_FILE;
    long ln = 0;
    const char *p = text, *end = text + len;
    bool in_doc = false, doc_text = false;
    char delim[64] = "";

    while (p < end && budget > 0) {
        const char *e = memchr(p, '\n', (size_t)(end - p));
        size_t n = e ? (size_t)(e - p) : (size_t)(end - p);
        ln++;
        check_line_chars(r, name, ln, p, n, &budget);

        const char *body = p;
        size_t bn = n;
        bool scan = kind != K_SRCINFO;
        if (kind == K_PATCH) {
            /* only lines the patch adds can introduce code */
            scan = n > 0 && p[0] == '+' && !(n >= 3 && strncmp(p, "+++", 3) == 0);
            body = p + 1;
            bn = n ? n - 1 : 0;
        }
        if (scan && bn > 0) {
            char *line = xstrndup(body, bn > 4096 ? 4096 : bn);
            Severity cap = SEV_CRIT;
            bool ends_doc = false;
            /* "# ..." comment (shell, udev, conf): not executed */
            bool text_line = false;
            bool comment = !in_doc && kind != K_PATCH && *skip_blank(line) == '#';
            if (kind == K_PATCH)
                cap = SEV_WARN;
            if (shell && in_doc) {
                const char *t = skip_blank(line);
                size_t tl = strlen(t);
                while (tl && (t[tl - 1] == ' ' || t[tl - 1] == '\t' || t[tl - 1] == '\r'))
                    tl--;
                if (tl == strlen(delim) && strncmp(t, delim, tl) == 0) {
                    ends_doc = true;
                    cap = SEV_INFO;
                } else if (doc_text) {
                    cap = SEV_INFO;   /* message shown to the user, not executed */
                }
            } else if (shell) {
                bool is_text = false;
                if (heredoc_start(line, delim, sizeof delim, &is_text)) {
                    in_doc = true;
                    doc_text = is_text;
                }
                if (display_only(line)) {
                    cap = SEV_WARN;
                    text_line = true;
                }
            }
            for (size_t i = 0; i < NRULES && budget > 0; i++) {
                if (!rx_ok[i] || regexec(&rx[i], line, 0, NULL, 0) != 0)
                    continue;
                if (rules[i].redirect && text_line)
                    continue;   /* the ">" is inside quoted text */
                Severity s = rules[i].sev;
                if (comment) {
                    if (s < SEV_CRIT)
                        continue;
                    s = SEV_WARN;   /* still worth a look: may be uncommented later */
                }
                if (kind == K_DATA && rules[i].path_ref)
                    s = SEV_INFO;
                if (s > cap)
                    s = cap;
                if (s == SEV_INFO && cap == SEV_INFO && !(kind == K_DATA))
                    continue;   /* text inside a displayed heredoc: not code */
                report_add(r, s, name, ln, rules[i].msg, line);
                budget--;
            }
            if (ends_doc)
                in_doc = false;
            free(line);
        }
        if (!e)
            break;
        p = e + 1;
    }
    if (budget <= 0)
        report_add(r, SEV_WARN, name, 0, "too many findings, output truncated", NULL);
}

/* ------------------------------------------------------------ .SRCINFO */

static const char *const strong_algos[] = { "sha256sums", "sha384sums", "sha512sums", "b2sums", NULL };
static const char *const weak_algos[] = { "md5sums", "sha1sums", "sha224sums", "cksums", NULL };

static bool is_vcs(const char *url)
{
    return strncmp(url, "git+", 4) == 0 || strncmp(url, "svn+", 4) == 0 ||
           strncmp(url, "hg+", 3) == 0 || strncmp(url, "bzr+", 4) == 0 ||
           strncmp(url, "fossil+", 7) == 0 || strncmp(url, "git://", 6) == 0;
}

static bool is_sig(const char *url)
{
    const char *q = strchr(url, '#');
    size_t n = q ? (size_t)(q - url) : strlen(url);
    char *u = xstrndup(url, n);
    bool r = ends_with(u, ".sig") || ends_with(u, ".asc") || ends_with(u, ".sign");
    free(u);
    return r;
}

static void scan_url_rules(Report *r, const char *url)
{
    rules_compile();
    for (size_t i = 0; i < NRULES; i++) {
        if (!rx_ok[i] || rules[i].sev < SEV_HIGH)
            continue;
        if (strstr(rules[i].msg, "URL") && regexec(&rx[i], url, 0, NULL, 0) == 0)
            report_add(r, rules[i].sev, ".SRCINFO", 0, rules[i].msg, url);
    }
}

static void check_sources(Report *r, const SrcInfo *si, const char *suffix)
{
    char key[64];
    snprintf(key, sizeof key, "source%s", suffix);
    StrList src;
    sl_init(&src);
    srcinfo_values_exact(si, key, &src);
    if (src.n == 0) {
        sl_free(&src);
        return;
    }

    StrList sums[8];
    const char *names[8];
    bool strong[8];
    size_t na = 0;
    for (int w = 0; w < 2; w++) {
        const char *const *list = w ? weak_algos : strong_algos;
        for (size_t i = 0; list[i]; i++) {
            snprintf(key, sizeof key, "%s%s", list[i], suffix);
            sl_init(&sums[na]);
            srcinfo_values_exact(si, key, &sums[na]);
            if (sums[na].n == 0) {
                sl_free(&sums[na]);
                continue;
            }
            names[na] = list[i];
            strong[na] = !w;
            if (sums[na].n != src.n) {
                char msg[160];
                snprintf(msg, sizeof msg, "%s%s has %zu entries but source%s has %zu",
                         list[i], suffix, sums[na].n, suffix, src.n);
                report_add(r, SEV_HIGH, ".SRCINFO", 0, msg, NULL);
            }
            na++;
        }
    }
    if (na == 0)
        report_add(r, SEV_HIGH, ".SRCINFO", 0, "sources have no checksums at all", NULL);

    bool weak_only_reported = false;
    for (size_t i = 0; i < src.n; i++) {
        const char *s = src.v[i];
        const char *sep = strstr(s, "::");
        const char *url = sep ? sep + 2 : s;
        bool remote = strstr(url, "://") != NULL;
        bool vcs = is_vcs(url);

        bool have_strong = false, have_any = false;
        for (size_t a = 0; a < na; a++) {
            if (i >= sums[a].n || strcmp(sums[a].v[i], "SKIP") == 0)
                continue;
            have_any = true;
            if (strong[a])
                have_strong = true;
        }
        (void)names;

        if (remote && !vcs && !is_sig(url) && !have_any)
            report_add(r, SEV_HIGH, ".SRCINFO", 0,
                       "remote source without checksum (SKIP): content is not pinned", url);
        if (have_any && !have_strong && !weak_only_reported) {
            report_add(r, SEV_WARN, ".SRCINFO", 0,
                       "only weak checksums (md5/sha1/cksum) used", url);
            weak_only_reported = true;
        }
        if (strncmp(url, "http://", 7) == 0 || strncmp(url, "ftp://", 6) == 0 ||
            strstr(url, "+http://") != NULL) {
            report_add(r, have_strong ? SEV_WARN : SEV_HIGH, ".SRCINFO", 0,
                       "source fetched over plain HTTP/FTP (no TLS)", url);
        }
        if (strncmp(url, "git://", 6) == 0)
            report_add(r, SEV_HIGH, ".SRCINFO", 0,
                       "git:// protocol is unauthenticated", url);
        if (vcs) {
            if (strstr(url, "#commit=") != NULL)
                ;
            else if (strstr(url, "#tag=") != NULL)
                report_add(r, SEV_INFO, ".SRCINFO", 0,
                           "VCS source pinned to a tag (tags can be moved)", url);
            else
                report_add(r, SEV_WARN, ".SRCINFO", 0,
                           "unpinned VCS source: code can change after your review", url);
        }
        if (remote)
            scan_url_rules(r, url);
    }

    for (size_t a = 0; a < na; a++)
        sl_free(&sums[a]);
    sl_free(&src);
}

void audit_srcinfo_text(Report *r, const char *text)
{
    SrcInfo si;
    if (srcinfo_parse(text, &si) != 0) {
        report_add(r, SEV_CRIT, ".SRCINFO", 0, "malformed .SRCINFO", NULL);
        return;
    }

    const char *base = srcinfo_first(&si, "pkgbase");
    if (!valid_pkgname(base))
        report_add(r, SEV_CRIT, ".SRCINFO", 0, "invalid pkgbase", base);

    StrList names;
    sl_init(&names);
    srcinfo_pkgnames(&si, &names);
    if (names.n == 0)
        report_add(r, SEV_CRIT, ".SRCINFO", 0, "no pkgname", NULL);
    for (size_t i = 0; i < names.n; i++) {
        if (!valid_pkgname(names.v[i]))
            report_add(r, SEV_CRIT, ".SRCINFO", 0, "invalid pkgname", names.v[i]);
        else if (ends_with(names.v[i], "-bin"))
            report_add(r, SEV_INFO, ".SRCINFO", 0,
                       "prebuilt binary package: upstream binary is installed, not built from source",
                       names.v[i]);
    }
    sl_free(&names);

    /* every source/sourceXX suffix */
    StrList suffixes;
    sl_init(&suffixes);
    for (size_t i = 0; i < si.n; i++) {
        const char *k = si.e[i].key;
        if (strncmp(k, "source", 6) == 0 && (k[6] == '\0' || k[6] == '_'))
            sl_push_unique(&suffixes, k + 6);
    }
    for (size_t i = 0; i < suffixes.n; i++)
        check_sources(r, &si, suffixes.v[i]);
    sl_free(&suffixes);

    /* signatures */
    bool has_sig = false;
    for (size_t i = 0; i < si.n; i++) {
        if (strncmp(si.e[i].key, "source", 6) == 0 &&
            (is_sig(si.e[i].val) || strstr(si.e[i].val, "?signed") != NULL))
            has_sig = true;
    }
    StrList keys;
    sl_init(&keys);
    srcinfo_values_exact(&si, "validpgpkeys", &keys);
    if (has_sig && keys.n == 0)
        report_add(r, SEV_WARN, ".SRCINFO", 0,
                   "signed sources but no validpgpkeys: signature will not be verified", NULL);
    for (size_t i = 0; i < keys.n; i++)
        if (!valid_fingerprint(keys.v[i]))
            report_add(r, SEV_HIGH, ".SRCINFO", 0,
                       "validpgpkeys entry is not a full 40/64-hex fingerprint", keys.v[i]);
    sl_free(&keys);

    /* dependency strings */
    static const char *const depkeys[] = { "depends", "makedepends", "checkdepends", NULL };
    for (size_t d = 0; depkeys[d]; d++) {
        StrList deps;
        sl_init(&deps);
        srcinfo_values(&si, depkeys[d], NULL, &deps);
        char nm[MAX_NAME_LEN + 1];
        for (size_t i = 0; i < deps.n; i++)
            if (!valid_depstring(deps.v[i], nm, sizeof nm))
                report_add(r, SEV_HIGH, ".SRCINFO", 0, "invalid dependency string", deps.v[i]);
        sl_free(&deps);
    }

    StrList inst;
    sl_init(&inst);
    srcinfo_values_exact(&si, "install", &inst);
    for (size_t i = 0; i < inst.n; i++)
        report_add(r, SEV_WARN, ".SRCINFO", 0,
                   "install scriptlet runs as ROOT during pacman -U; read it carefully",
                   inst.v[i]);
    sl_free(&inst);

    srcinfo_free(&si);
}

void audit_source_stats(const char *srcinfo_text, SourceStats *st)
{
    memset(st, 0, sizeof *st);
    SrcInfo si;
    if (srcinfo_text == NULL || srcinfo_parse(srcinfo_text, &si) != 0)
        return;
    StrList suffixes;
    sl_init(&suffixes);
    for (size_t i = 0; i < si.n; i++) {
        const char *k = si.e[i].key;
        if (strncmp(k, "source", 6) == 0 && (k[6] == '\0' || k[6] == '_'))
            sl_push_unique(&suffixes, k + 6);
    }
    char key[64];
    for (size_t x = 0; x < suffixes.n; x++) {
        const char *suffix = suffixes.v[x];
        StrList src;
        sl_init(&src);
        snprintf(key, sizeof key, "source%s", suffix);
        srcinfo_values_exact(&si, key, &src);
        StrList sums[8];
        bool strong[8];
        size_t na = 0;
        for (int w = 0; w < 2; w++) {
            const char *const *list = w ? weak_algos : strong_algos;
            for (size_t i = 0; list[i] && na < 8; i++) {
                snprintf(key, sizeof key, "%s%s", list[i], suffix);
                sl_init(&sums[na]);
                srcinfo_values_exact(&si, key, &sums[na]);
                strong[na] = !w;
                na++;
            }
        }
        for (size_t i = 0; i < src.n; i++) {
            const char *sep = strstr(src.v[i], "::");
            const char *url = sep ? sep + 2 : src.v[i];
            st->total++;
            if (is_vcs(url)) {
                st->vcs++;
                if (strstr(url, "#commit=") != NULL)
                    st->vcs_pinned++;
                continue;
            }
            if (is_sig(url)) {
                st->signatures++;
                continue;
            }
            if (strstr(url, "://") == NULL)
                continue;   /* local file from the reviewed repository */
            st->remote++;
            bool have_strong = false, have_weak = false;
            for (size_t a = 0; a < na; a++) {
                if (i >= sums[a].n || strcmp(sums[a].v[i], "SKIP") == 0)
                    continue;
                if (strong[a])
                    have_strong = true;
                else
                    have_weak = true;
            }
            if (have_strong)
                st->strong++;
            else if (have_weak)
                st->weak++;
            else
                st->unchecked++;
        }
        for (size_t a = 0; a < na; a++)
            sl_free(&sums[a]);
        sl_free(&src);
    }
    sl_free(&suffixes);
    srcinfo_free(&si);
}

/* ------------------------------------------------------------ repository */

typedef struct {
    Report *r;
    const char *root;
    size_t rootlen;
    bool is_git;
    size_t files;
    bool too_many;
} Walk;

static bool allowed_hidden(const char *b)
{
    return strcmp(b, ".SRCINFO") == 0 || strcmp(b, ".gitignore") == 0 ||
           strcmp(b, ".nvchecker.toml") == 0;
}

static void scan_file(Report *r, const char *path, const char *rel, const struct stat *st)
{
    if ((size_t)st->st_size > MAX_SCAN_FILE_BYTES) {
        report_add(r, SEV_WARN, rel, 0, "large file not scanned; inspect manually", NULL);
        return;
    }
    StrBuf b;
    sb_init(&b, MAX_SCAN_FILE_BYTES);
    if (read_file_limited(path, MAX_SCAN_FILE_BYTES, &b) != 0) {
        report_add(r, SEV_WARN, rel, 0, "could not read file", NULL);
        sb_free(&b);
        return;
    }
    size_t probe = b.len < 8000 ? b.len : 8000;
    if (memchr(b.data, '\0', probe) != NULL) {
        bool image = ends_with(rel, ".png") || ends_with(rel, ".ico") ||
                     ends_with(rel, ".jpg") || ends_with(rel, ".jpeg") ||
                     ends_with(rel, ".gif") || ends_with(rel, ".webp");
        report_add(r, image ? SEV_WARN : SEV_HIGH, rel, 0,
                   image ? "binary image in repository (not scanned)"
                         : "binary file in repository (not scanned; AUR repos should hold text only)",
                   NULL);
        sb_free(&b);
        return;
    }
    if (strcmp(rel, ".SRCINFO") == 0)
        audit_srcinfo_text(r, b.data);
    audit_scan_text(r, rel, b.data, b.len);
    sb_free(&b);
}

static void walk_dir(Walk *w, const char *path, int depth)
{
    if (depth > MAX_REPO_DEPTH) {
        report_add(w->r, SEV_HIGH, path + w->rootlen, 0, "directory nesting too deep", NULL);
        return;
    }
    DIR *d = opendir(path);
    if (d == NULL)
        return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && !w->too_many) {
        const char *n = de->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
            continue;
        if (depth == 0 && strcmp(n, ".git") == 0)
            continue;
        if (!w->is_git && depth == 0 &&
            (strcmp(n, "src") == 0 || strcmp(n, "pkg") == 0 || strstr(n, ".pkg.tar") != NULL ||
             strstr(n, ".src.tar") != NULL || strstr(n, ".tar.") != NULL ||
             ends_with(n, ".tgz") || ends_with(n, ".zip") || ends_with(n, ".asc") ||
             ends_with(n, ".sig")))
            continue;

        char child[PATH_BUF];
        if (!path_join(child, sizeof child, path, n))
            continue;
        const char *rel = child + w->rootlen + 1;
        struct stat st;
        if (lstat(child, &st) != 0)
            continue;

        if (++w->files > MAX_REPO_FILES) {
            report_add(w->r, SEV_HIGH, "", 0, "too many files in repository; audit stopped", NULL);
            w->too_many = true;
            break;
        }
        if (n[0] == '.' && !allowed_hidden(n))
            report_add(w->r, SEV_WARN, rel, 0, "hidden file", NULL);

        if (S_ISLNK(st.st_mode)) {
            report_add(w->r, SEV_HIGH, rel, 0, "symlink (may point outside the build tree)", NULL);
        } else if (S_ISDIR(st.st_mode)) {
            walk_dir(w, child, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            if (st.st_mode & (S_ISUID | S_ISGID))
                report_add(w->r, SEV_HIGH, rel, 0, "setuid/setgid file in repository", NULL);
            scan_file(w->r, child, rel, &st);
        } else {
            report_add(w->r, SEV_HIGH, rel, 0, "special file (fifo/device/socket)", NULL);
        }
    }
    closedir(d);
}

/* git mode: audit exactly the tracked files (what the reviewed commit holds). */
static void audit_git_tree(Report *r, const char *dir)
{
    StrBuf out;
    sb_init(&out, MAX_GIT_OUTPUT);
    const char *a[] = { "ls-tree", "-r", "-z", "--full-tree", "HEAD", NULL };
    if (git_run(dir, a, &out) != 0) {
        report_add(r, SEV_HIGH, "", 0, "could not list git tree", NULL);
        sb_free(&out);
        return;
    }
    size_t count = 0;
    const char *p = out.data, *end = out.data + out.len;
    while (p < end) {
        size_t n = strnlen(p, (size_t)(end - p));
        const char *tab = memchr(p, '\t', n);
        if (tab) {
            const char *path = tab + 1;
            if (++count > MAX_REPO_FILES) {
                report_add(r, SEV_HIGH, "", 0, "too many files in repository; audit stopped", NULL);
                break;
            }
            const char *b = base_name(path);
            if (b[0] == '.' && !allowed_hidden(b))
                report_add(r, SEV_WARN, path, 0, "hidden file", NULL);
            if (strncmp(path, "src/", 4) == 0 || strncmp(path, "pkg/", 4) == 0)
                report_add(r, SEV_WARN, path, 0, "tracked file inside makepkg build directory", NULL);

            if (strncmp(p, "120000", 6) == 0) {
                report_add(r, SEV_HIGH, path, 0,
                           "symlink committed to the repository (checked out as plain file)", NULL);
            } else if (strncmp(p, "160000", 6) == 0) {
                report_add(r, SEV_HIGH, path, 0, "git submodule (external code not reviewed)", NULL);
            } else {
                char full[PATH_BUF];
                struct stat st;
                if (!path_join(full, sizeof full, dir, path) || lstat(full, &st) != 0) {
                    report_add(r, SEV_WARN, path, 0, "tracked file missing from worktree", NULL);
                } else if (!S_ISREG(st.st_mode)) {
                    report_add(r, SEV_HIGH, path, 0, "not a regular file in worktree", NULL);
                } else {
                    if (st.st_mode & (S_ISUID | S_ISGID))
                        report_add(r, SEV_HIGH, path, 0, "setuid/setgid file in repository", NULL);
                    scan_file(r, full, path, &st);
                }
            }
        }
        p += n + 1;
    }
    sb_free(&out);
}

int audit_repo(const char *dir, bool is_git, Report *r)
{
    char path[PATH_BUF];
    if (!path_join(path, sizeof path, dir, "PKGBUILD") || !is_file_nofollow(path))
        report_add(r, SEV_CRIT, "PKGBUILD", 0, "missing or not a regular file", NULL);
    if (!path_join(path, sizeof path, dir, ".SRCINFO") || !is_file_nofollow(path))
        report_add(r, SEV_CRIT, ".SRCINFO", 0,
                   "missing: not a valid AUR package (or empty repository)", NULL);

    if (is_git) {
        audit_git_tree(r, dir);
    } else {
        Walk w;
        memset(&w, 0, sizeof w);
        w.r = r;
        w.root = dir;
        w.rootlen = strlen(dir);
        w.is_git = false;
        walk_dir(&w, dir, 0);
    }

    /* install= files must exist inside the repo */
    StrBuf sb;
    sb_init(&sb, MAX_SCAN_FILE_BYTES);
    if (path_join(path, sizeof path, dir, ".SRCINFO") &&
        read_file_limited(path, MAX_SCAN_FILE_BYTES, &sb) == 0) {
        SrcInfo si;
        if (srcinfo_parse(sb.data, &si) == 0) {
            StrList inst;
            sl_init(&inst);
            srcinfo_values_exact(&si, "install", &inst);
            for (size_t i = 0; i < inst.n; i++) {
                if (strchr(inst.v[i], '/') || !path_join(path, sizeof path, dir, inst.v[i]) ||
                    !is_file_nofollow(path))
                    report_add(r, SEV_HIGH, inst.v[i], 0,
                               "install scriptlet referenced but not present in repository", NULL);
            }
            sl_free(&inst);
            srcinfo_free(&si);
        }
    }
    sb_free(&sb);
    return 0;
}

/* ------------------------------------------------------------ package */

typedef struct {
    const char *prefix;
    Severity sev;
    const char *msg;
} PathRule;

static const PathRule path_rules[] = {
    { "home/",                       SEV_CRIT, "installs into a home directory" },
    { "root/",                       SEV_CRIT, "installs into /root" },
    { "etc/sudoers",                 SEV_CRIT, "installs sudoers configuration" },
    { "etc/ld.so.preload",           SEV_CRIT, "installs /etc/ld.so.preload (library injection)" },
    { "etc/shadow",                  SEV_CRIT, "overwrites shadow file" },
    { "etc/passwd",                  SEV_CRIT, "overwrites passwd file" },
    { "etc/ssh/",                    SEV_HIGH, "changes SSH configuration" },
    { "etc/pam.d/",                  SEV_HIGH, "changes PAM authentication" },
    { "usr/lib/security/",           SEV_HIGH, "installs a PAM module" },
    { "etc/security/",               SEV_HIGH, "changes security limits/access" },
    { "etc/profile.d/",              SEV_HIGH, "runs code at every login shell" },
    { "etc/environment",             SEV_HIGH, "changes global environment" },
    { "etc/systemd/system/",         SEV_HIGH, "writes to the admin systemd directory" },
    { "usr/share/libalpm/hooks/",    SEV_HIGH, "pacman hook (runs as root on every transaction)" },
    { "etc/pacman.d/hooks/",         SEV_HIGH, "pacman hook (runs as root on every transaction)" },
    { "etc/pacman",                  SEV_HIGH, "changes pacman configuration" },
    { "etc/cron",                    SEV_HIGH, "cron job" },
    { "var/spool/cron",              SEV_HIGH, "cron job" },
    { "tmp/",                        SEV_HIGH, "installs into /tmp" },
    { "var/tmp/",                    SEV_HIGH, "installs into /var/tmp" },
    { "etc/xdg/autostart/",          SEV_WARN, "desktop autostart entry" },
    { "usr/lib/modules/",            SEV_WARN, "kernel module" },
    { "etc/modules-load.d/",         SEV_WARN, "loads kernel modules at boot" },
    { "usr/lib/modules-load.d/",     SEV_WARN, "loads kernel modules at boot" },
    { "etc/udev/rules.d/",           SEV_WARN, "udev rule" },
    { "usr/lib/udev/rules.d/",       SEV_WARN, "udev rule" },
    { "etc/polkit-1/rules.d/",       SEV_WARN, "polkit rule (privilege policy)" },
    { "usr/share/polkit-1/rules.d/", SEV_WARN, "polkit rule (privilege policy)" },
    { "usr/lib/systemd/system-generators/", SEV_WARN, "systemd generator (runs as root at boot)" },
    { "etc/sysctl.d/",               SEV_WARN, "kernel sysctl change" },
    { "usr/lib/sysctl.d/",           SEV_WARN, "kernel sysctl change" },
    { "usr/local/",                  SEV_WARN, "packages files in /usr/local" },
    { NULL, SEV_INFO, NULL }
};

static const char *skip_fields(const char *line, int count)
{
    const char *p = line;
    for (int i = 0; i < count; i++) {
        while (*p == ' ')
            p++;
        while (*p && *p != ' ')
            p++;
    }
    while (*p == ' ')
        p++;
    return p;
}

int audit_package_file(const char *pkgfile, Report *r)
{
    StrBuf out;
    sb_init(&out, MAX_PKG_LISTING);
    const char *argv[] = { BIN_BSDTAR, "-tvf", pkgfile, NULL };
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.capture = &out;
    o.stdin_null = true;
    if (proc_run(argv, &o) != 0) {
        report_add(r, SEV_HIGH, base_name(pkgfile), 0, "could not list package contents", NULL);
        sb_free(&out);
        return -1;
    }

    const char *pname = base_name(pkgfile);
    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, out.data);
    sb_free(&out);

    int budget = 200;
    for (size_t i = 0; i < lines.n && budget > 0; i++) {
        const char *ln = lines.v[i];
        if (strlen(ln) < 10)
            continue;
        char mode[11];
        memcpy(mode, ln, 10);
        mode[10] = '\0';
        const char *path = skip_fields(ln, 8);
        char *p = xstrdup(path);
        char *arrow = strstr(p, " -> ");
        if (arrow)
            *arrow = '\0';
        char *hl = strstr(p, " link to ");
        if (hl)
            *hl = '\0';
        if (strncmp(p, "./", 2) == 0)
            memmove(p, p + 2, strlen(p + 2) + 1);

        char where[PATH_BUF];
        snprintf(where, sizeof where, "%s:/%s", pname, p);

        if (p[0] == '/' || strstr(p, "../") || strcmp(p, "..") == 0 || ends_with(p, "/..")) {
            report_add(r, SEV_CRIT, where, 0, "absolute or '..' path inside package", NULL);
            budget--;
        }
        if (strcmp(p, ".INSTALL") == 0) {
            report_add(r, SEV_WARN, pname, 0, "package has an install scriptlet (runs as root)", NULL);
            budget--;
        }
        if (mode[3] == 's' || mode[3] == 'S') {
            report_add(r, SEV_HIGH, where, 0, "setuid file", mode);
            budget--;
        }
        if (mode[6] == 's' || mode[6] == 'S') {
            report_add(r, SEV_HIGH, where, 0, "setgid file", mode);
            budget--;
        }
        if (mode[8] == 'w' && mode[0] != 'l' && !(mode[0] == 'd' && (mode[9] == 't' || mode[9] == 'T'))) {
            report_add(r, SEV_HIGH, where, 0, "world-writable", mode);
            budget--;
        }
        if (strstr(p, ".wants/") != NULL) {
            report_add(r, SEV_HIGH, where, 0, "pre-enabled systemd unit", NULL);
            budget--;
        }
        for (size_t k = 0; path_rules[k].prefix; k++) {
            if (strncmp(p, path_rules[k].prefix, strlen(path_rules[k].prefix)) == 0) {
                /* directories themselves are only noise */
                if (mode[0] != 'd' || path_rules[k].sev == SEV_CRIT) {
                    report_add(r, path_rules[k].sev, where, 0, path_rules[k].msg, NULL);
                    budget--;
                }
                break;
            }
        }
        free(p);
    }
    if (budget <= 0)
        report_add(r, SEV_WARN, pname, 0, "too many findings, output truncated", NULL);
    sl_free(&lines);
    return 0;
}

/* "key = value" lines of a .PKGINFO */
static const char *pkginfo_value(const StrList *lines, const char *key, char *buf, size_t sz)
{
    size_t kl = strlen(key);
    for (size_t i = 0; i < lines->n; i++) {
        const char *l = lines->v[i];
        if (strncmp(l, key, kl) == 0 && strncmp(l + kl, " = ", 3) == 0) {
            snprintf(buf, sz, "%s", l + kl + 3);
            return buf;
        }
    }
    return NULL;
}

void audit_pkginfo_text(Report *r, const char *where, const char *text,
                        const char *expect_name, const char *expect_base)
{
    StrList lines;
    sl_init(&lines);
    sl_split_lines(&lines, text ? text : "");
    char name[MAX_NAME_LEN + 1], base[MAX_NAME_LEN + 1];
    const char *n = pkginfo_value(&lines, "pkgname", name, sizeof name);
    const char *b = pkginfo_value(&lines, "pkgbase", base, sizeof base);
    if (n == NULL) {
        report_add(r, SEV_CRIT, where, 0, "no .PKGINFO pkgname in the built package", NULL);
    } else if (expect_name && strcmp(n, expect_name) != 0) {
        char msg[MAX_NAME_LEN + 96];
        snprintf(msg, sizeof msg, "pkgname inside does not match the expected package %s",
                 expect_name);
        report_add(r, SEV_CRIT, where, 0, msg, n);
    }
    if (b && expect_base && strcmp(b, expect_base) != 0) {
        char msg[MAX_NAME_LEN + 96];
        snprintf(msg, sizeof msg, "pkgbase inside does not match the expected package %s",
                 expect_base);
        report_add(r, SEV_CRIT, where, 0, msg, b);
    }
    sl_free(&lines);
}

/* bsdtar -xOqf pkgfile member: one member's content (bounded). */
static int extract_member(const char *pkgfile, const char *member, StrBuf *out)
{
    const char *argv[] = { BIN_BSDTAR, "-xOqf", pkgfile, member, NULL };
    ProcOpts o;
    memset(&o, 0, sizeof o);
    o.capture = out;
    o.stdin_null = true;
    o.stderr_null = true;
    return proc_run(argv, &o) == 0 ? 0 : -1;
}

int audit_built_package(const char *pkgfile, const char *expect_name,
                        const char *expect_base, Report *r)
{
    int rc = audit_package_file(pkgfile, r);
    const char *pname = base_name(pkgfile);
    char where[PATH_BUF];

    StrBuf info;
    sb_init(&info, MAX_SCAN_FILE_BYTES);
    snprintf(where, sizeof where, "%s:/.PKGINFO", pname);
    if (extract_member(pkgfile, ".PKGINFO", &info) != 0)
        report_add(r, SEV_CRIT, where, 0, "no .PKGINFO in the built package", NULL);
    else
        audit_pkginfo_text(r, where, info.data, expect_name, expect_base);
    sb_free(&info);

    /* the scriptlet that really runs as root (it may differ from *.install
     * in the repository if PKGBUILD generated or edited it) */
    StrBuf inst;
    sb_init(&inst, MAX_SCAN_FILE_BYTES);
    snprintf(where, sizeof where, "%s:/.INSTALL", pname);
    if (extract_member(pkgfile, ".INSTALL", &inst) == 0 && inst.len > 0)
        audit_scan_text(r, where, inst.data, inst.len);
    sb_free(&inst);
    return rc;
}
