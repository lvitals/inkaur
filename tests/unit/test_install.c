#include "../test_helper.h"
#include "../../install.h"
#include <stdlib.h>
#include <string.h>

/* Forward declarations for static functions made visible by TESTING macro */
typedef struct {
        char **items;
        size_t count;
        size_t capacity;
} PkgList;

PkgList *pkglist_new(void);
void pkglist_free(PkgList *l);
void pkglist_add(PkgList *l, const char *name);
char *strip_version(const char *dep);

void test_strip_version() {
    char *s1 = strip_version("wlroots>=0.15");
    ASSERT_STR_EQ(s1, "wlroots", "strip_version failed for >=");
    free(s1);

    char *s2 = strip_version("libxkbcommon");
    ASSERT_STR_EQ(s2, "libxkbcommon", "strip_version failed for no version");
    free(s2);

    char *s3 = strip_version("wayland<2.0");
    ASSERT_STR_EQ(s3, "wayland", "strip_version failed for <");
    free(s3);
}

void test_pkglist() {
    PkgList *l = pkglist_new();
    ASSERT(l->count == 0, "new pkglist not empty");

    pkglist_add(l, "pkg1");
    ASSERT(l->count == 1, "pkglist count mismatch after add");
    ASSERT_STR_EQ(l->items[0], "pkg1", "pkglist item mismatch");

    pkglist_add(l, "pkg1");
    ASSERT(l->count == 1, "pkglist added duplicate");

    pkglist_add(l, "pkg2");
    ASSERT(l->count == 2, "pkglist count mismatch after add 2");
    
    pkglist_free(l);
}

void run_install_tests() {
    test_strip_version();
    test_pkglist();
    printf("Unit tests for Install passed!\n");
}
