#include "../test_helper.h"
#include "config.h"
#include "net.h"
#include "validate.h"

void run_validate_tests(void);

void run_validate_tests(void)
{
    /* package names: the old code turned "../.." into rm -rf ~/.cache/inkaur/../.. */
    ASSERT(valid_pkgname("aide"), "simple name");
    ASSERT(valid_pkgname("lib32-foo_bar+2@x.y"), "all allowed chars");
    ASSERT(!valid_pkgname(".."), "dot-dot");
    ASSERT(!valid_pkgname("."), "dot");
    ASSERT(!valid_pkgname("../../etc"), "traversal");
    ASSERT(!valid_pkgname("a/b"), "slash");
    ASSERT(!valid_pkgname("-rf"), "option injection");
    ASSERT(!valid_pkgname(".hidden"), "leading dot");
    ASSERT(!valid_pkgname("foo&type=search"), "URL parameter injection");
    ASSERT(!valid_pkgname("foo bar"), "space");
    ASSERT(!valid_pkgname("foo\n"), "newline");
    ASSERT(!valid_pkgname(""), "empty");
    ASSERT(!valid_pkgname("foo$(id)"), "shell metachar");
    char longname[MAX_NAME_LEN + 2];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    ASSERT(!valid_pkgname(longname), "too long");

    char nm[MAX_NAME_LEN + 1];
    ASSERT(valid_depstring("glibc>=2.38-1", nm, sizeof nm), "versioned dep");
    ASSERT_STR_EQ(nm, "glibc", "dep name extracted");
    ASSERT(valid_depstring("python", nm, sizeof nm), "plain dep");
    ASSERT(valid_depstring("foo=1:2.0", nm, sizeof nm), "epoch dep");
    ASSERT(!valid_depstring("foo>=", nm, sizeof nm), "empty version");
    ASSERT(!valid_depstring("foo>=1;rm", nm, sizeof nm), "bad version char");
    ASSERT(!valid_depstring("-foo", nm, sizeof nm), "leading dash");
    ASSERT(!valid_depstring("foo<>1", nm, sizeof nm), "bad operator");

    ASSERT(valid_fingerprint("ABCDEF0123456789ABCDEF0123456789ABCDEF01"), "v4 fingerprint");
    ASSERT(!valid_fingerprint("ABCDEF0123456789"), "long key id rejected");
    ASSERT(!valid_fingerprint("DEADBEEF"), "short key id rejected");
    ASSERT(!valid_fingerprint("--import ABCDEF0123456789ABCDEF0123456789AB"), "option in key");

    ASSERT(valid_commit("0123456789abcdef0123456789abcdef01234567"), "sha1 commit");
    ASSERT(!valid_commit("HEAD"), "ref name is not a commit");

    ASSERT(valid_search_term("vim"), "search ok");
    ASSERT(!valid_search_term("a"), "search too short");
    ASSERT(!valid_search_term("--help"), "search option");

    StrBuf b;
    sb_init(&b, 0);
    const char evil[] = "ok\033[2J\r\xe2\x80\xae" "x\x9b";
    sanitize_append(&b, evil, sizeof evil - 1, false);
    ASSERT(strchr(b.data, '\033') == NULL, "ESC removed");
    ASSERT(strchr(b.data, '\r') == NULL, "CR removed");
    ASSERT(strstr(b.data, "<U+202E>") != NULL, "RLO made visible");
    ASSERT(strstr(b.data, "\\x9B") != NULL, "invalid UTF-8 made visible");
    sb_free(&b);

    sb_init(&b, 0);
    url_encode(&b, "a b&c[]/é");
    ASSERT_STR_EQ(b.data, "a%20b%26c%5B%5D%2F%C3%A9", "url encoding");
    sb_free(&b);
    printf("validate: ok\n");
}
