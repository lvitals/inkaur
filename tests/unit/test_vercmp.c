#include "../test_helper.h"
#include "vercmp.h"

void run_vercmp_tests(void);

static int sgn(int x) { return (x > 0) - (x < 0); }

void run_vercmp_tests(void)
{
    /* expected values verified against pacman's vercmp(8) */
    static const struct { const char *a, *b; int r; } t[] = {
        { "1.0", "1.0", 0 },
        { "1.0", "1.1", -1 },
        { "1.1", "1.0", 1 },
        { "1.0-1", "1.0-2", -1 },
        { "1.0-2", "1.0", 0 },
        { "1:1.0", "2.0", 1 },
        { "0:1.0", "1.0", 0 },
        { "1.0a", "1.0", -1 },
        { "1.0alpha", "1.0b", -1 },
        { "1.0rc1", "1.0", -1 },
        { "1.0", "1.0.a", -1 },
        { "1.0.a", "1.0.1", -1 },
        { "1.0.1", "1.0.10", -1 },
        { "1.01", "1.1", 0 },
        { "2.0", "10.0", -1 },
        { "1.0+git5", "1.0", 1 },
        { "r123.abc", "r124.abc", -1 },
        { "1_0", "1.0", 0 },
        { "1..0", "1.0", 1 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        char msg[96];
        snprintf(msg, sizeof msg, "vercmp %s %s", t[i].a, t[i].b);
        ASSERT(sgn(pkg_vercmp(t[i].a, t[i].b)) == t[i].r, msg);
        ASSERT(sgn(pkg_vercmp(t[i].b, t[i].a)) == -t[i].r || t[i].r == 0, msg);
    }
    printf("vercmp: ok\n");
}
