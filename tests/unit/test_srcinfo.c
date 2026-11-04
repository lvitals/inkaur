#include "../test_helper.h"
#include "srcinfo.h"

void run_srcinfo_tests(void);

void run_srcinfo_tests(void)
{
    const char *txt =
        "pkgbase = foo\n"
        "\tpkgver = 1.0\n"
        "\tdepends = glibc\n"
        "\tdepends_x86_64 = lib32-x\n"
        "\tdepends_aarch64 = armlib\n"
        "\tmakedepends = cmake\n"
        "\tsource = foo.tar.gz\n"
        "\toptdepends =\n"
        "\n"
        "pkgname = foo\n"
        "pkgname = foo-extra\n"
        "\tdepends = foo\n";
    SrcInfo si;
    ASSERT(srcinfo_parse(txt, &si) == 0, "parse ok");
    ASSERT_STR_EQ(srcinfo_first(&si, "pkgbase"), "foo", "pkgbase");

    StrList l;
    sl_init(&l);
    srcinfo_values(&si, "depends", "x86_64", &l);
    ASSERT(l.n == 3, "depends + x86_64 suffix, aarch64 excluded");
    ASSERT(sl_contains(&l, "lib32-x") && !sl_contains(&l, "armlib"), "arch filtering");
    sl_free(&l);

    srcinfo_pkgnames(&si, &l);
    ASSERT(l.n == 2, "split package names");
    sl_free(&l);
    srcinfo_free(&si);

    ASSERT(srcinfo_parse("pkgver = 1\npkgbase = x\n", &si) != 0, "data before pkgbase rejected");
    ASSERT(srcinfo_parse("pkgbase = x\ngarbage line\n", &si) != 0, "garbage rejected");
    ASSERT(srcinfo_parse("pkgname = x\n", &si) != 0, "missing pkgbase rejected");
    printf("srcinfo: ok\n");
}
