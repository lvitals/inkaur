#include "../test_helper.h"
#include "cli.h"
#include "pacman.h"

#include <unistd.h>

void run_cli_parse_tests(void);

static int parse(Cli *c, char **argv, char *err, size_t sz)
{
    int argc = 0;
    while (argv[argc])
        argc++;
    cli_init(c);
    return cli_parse(c, argc, argv, err, sz);
}

static const char *joined(const StrList *l)
{
    static char buf[1024];
    buf[0] = '\0';
    for (size_t i = 0; i < l->n; i++) {
        if (i)
            strncat(buf, " ", sizeof buf - strlen(buf) - 1);
        strncat(buf, l->v[i], sizeof buf - strlen(buf) - 1);
    }
    return buf;
}

static void test_summary(void)
{
    StrList before, after, touched;
    sl_init(&before);
    sl_init(&after);
    sl_init(&touched);
    sl_push(&before, "harfbuzz 14.0.0-1");
    sl_push(&before, "old 1.0-1");
    sl_push(&before, "same 2.0-1");
    sl_push(&before, "vtk 9.8.0-1");
    sl_push(&after, "harfbuzz 15.1.0-1");
    sl_push(&after, "new 0.1-1");
    sl_push(&after, "same 2.0-1");
    sl_push(&after, "vtk 9.7.2-1");
    sl_push(&touched, "same");

    char path[] = "/tmp/inkaur-summary-XXXXXX";
    int fd = mkstemp(path);
    ASSERT(fd >= 0, "mkstemp");
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(fd, STDOUT_FILENO);
    size_t n = pacman_print_changes(&before, &after, &touched);
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);

    static char out[4096];
    FILE *fp = fopen(path, "r");
    size_t len = fread(out, 1, sizeof out - 1, fp);
    out[len] = '\0';
    fclose(fp);
    close(fd);
    unlink(path);

    ASSERT(n == 5, "five changes");
    ASSERT(strstr(out, "Upgraded (1)") && strstr(out, "14.0.0-1 -> 15.1.0-1"), "upgrade listed");
    ASSERT(strstr(out, "Downgraded (1)") && strstr(out, "9.8.0-1 -> 9.7.2-1"), "downgrade listed");
    ASSERT(strstr(out, "Installed (1)") && strstr(out, "new"), "install listed");
    ASSERT(strstr(out, "Removed (1)") && strstr(out, "old"), "removal listed");
    ASSERT(strstr(out, "Reinstalled (1)") && strstr(out, "same"), "touched same version = reinstall");
    ASSERT(strstr(out, "total: 1 upgraded, 1 installed, 1 reinstalled, 1 downgraded, 1 removed"),
           "totals");
    sl_free(&before);
    sl_free(&after);
    sl_free(&touched);
}

void run_cli_parse_tests(void)
{
    Cli c;
    char err[256];

    char *a1[] = { "inkaur", "-Syu", "--noconfirm", "--ignore=foo,bar", "--overwrite", "/x/*", NULL };
    ASSERT(parse(&c, a1, err, sizeof err) == 0, "-Syu parses");
    ASSERT(c.op == 'S' && cli_count(&c, "sysupgrade") == 1 && cli_count(&c, "refresh") == 1, "-Syu flags");
    ASSERT(c.noconfirm, "--noconfirm");
    ASSERT(c.ignore.n == 2 && sl_contains(&c.ignore, "bar"), "--ignore list split");
    ASSERT_STR_EQ(joined(&c.xact), "--ignore=foo,bar --overwrite=/x/*", "xact canonical");
    ASSERT_STR_EQ(joined(&c.common), "--noconfirm", "common canonical");
    cli_free(&c);

    char *a2[] = { "inkaur", "-Syyuu", NULL };
    ASSERT(parse(&c, a2, err, sizeof err) == 0 && cli_count(&c, "refresh") == 2 &&
           cli_count(&c, "sysupgrade") == 2, "repeated letters counted");
    cli_free(&c);

    /* same letter, different meaning per operation */
    char *a3[] = { "inkaur", "-Qdt", NULL };
    ASSERT(parse(&c, a3, err, sizeof err) == 0 && cli_has(&c, "deps") && cli_has(&c, "unrequired"),
           "-Qd = deps");
    cli_free(&c);
    char *a4[] = { "inkaur", "-Rdd", "x", NULL };
    ASSERT(parse(&c, a4, err, sizeof err) == 0 && cli_count(&c, "nodeps") == 2, "-Rd = nodeps");
    cli_free(&c);
    char *a5[] = { "inkaur", "-Rns", "x", NULL };
    ASSERT(parse(&c, a5, err, sizeof err) == 0 && cli_has(&c, "recursive") && cli_has(&c, "nosave"),
           "-Rns");
    ASSERT_STR_EQ(joined(&c.opflags), "--nosave --recursive", "-Rns forwarded long");
    cli_free(&c);

    /* short options with values, stacked or separate; operation after options */
    char *a6[] = { "inkaur", "-b/tmp/db", "-r", "/mnt", "-Q", "--config=/etc/p.conf", NULL };
    ASSERT(parse(&c, a6, err, sizeof err) == 0 && c.op == 'Q', "op found after options");
    ASSERT_STR_EQ(joined(&c.ctx), "--dbpath=/tmp/db --root=/mnt --config=/etc/p.conf",
                  "context options");
    ASSERT(c.config && strcmp(c.config, "/etc/p.conf") == 0, "--config value");
    cli_free(&c);

    /* long names: unique prefixes, long operations */
    char *a7[] = { "inkaur", "--sync", "--sysu", "--refr", "--need", NULL };
    ASSERT(parse(&c, a7, err, sizeof err) == 0 && cli_has(&c, "sysupgrade") && c.needed,
           "unique prefixes accepted");
    cli_free(&c);
    char *a8[] = { "inkaur", "-S", "--no", NULL };
    ASSERT(parse(&c, a8, err, sizeof err) != 0 && strstr(err, "ambiguous"), "ambiguous prefix");
    cli_free(&c);

    /* rejections */
    char *b1[] = { "inkaur", "-Qx", NULL };
    ASSERT(parse(&c, b1, err, sizeof err) != 0, "-Qx invalid");
    cli_free(&c);
    char *b2[] = { "inkaur", "-SQ", NULL };
    ASSERT(parse(&c, b2, err, sizeof err) != 0 && strstr(err, "one operation"), "two operations");
    cli_free(&c);
    char *b3[] = { "inkaur", "-Q", "--ignore", "x", NULL };
    ASSERT(parse(&c, b3, err, sizeof err) != 0, "--ignore invalid for -Q");
    cli_free(&c);
    char *b4[] = { "inkaur", "-S", "--needed=1", NULL };
    ASSERT(parse(&c, b4, err, sizeof err) != 0, "flag with value refused");
    cli_free(&c);
    char *b5[] = { "inkaur", "-S", "--aur", "--repo", "x", NULL };
    ASSERT(parse(&c, b5, err, sizeof err) != 0, "--aur with --repo refused");
    cli_free(&c);
    char *b6[] = { "inkaur", "-S", "--overwrite", NULL };
    ASSERT(parse(&c, b6, err, sizeof err) != 0 && strstr(err, "requires an argument"),
           "missing value");
    cli_free(&c);

    /* targets after -- even if they look like options */
    char *c1[] = { "inkaur", "-R", "--", "-rf", NULL };
    ASSERT(parse(&c, c1, err, sizeof err) == 0 && c.targets.n == 1 &&
           strcmp(c.targets.v[0], "-rf") == 0, "-- ends options");
    cli_free(&c);

    test_summary();
    printf("cli parser: ok\n");
}
