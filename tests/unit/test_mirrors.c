#include "../test_helper.h"
#include "mirrors.h"

void run_mirrors_tests(void);

void run_mirrors_tests(void)
{
    /* ParallelDownloads: replace a commented line inside [options] only */
    const char *conf = "[options]\nArchitecture = auto\n#ParallelDownloads = 5\n\n"
                       "[core]\nInclude = /etc/pacman.d/mirrorlist\n";
    char *r = pacman_conf_with_parallel(conf, 8);
    ASSERT(strstr(r, "ParallelDownloads = 8\n") && !strstr(r, "#ParallelDownloads"),
           "commented ParallelDownloads replaced");
    ASSERT(strstr(r, "[core]\nInclude = /etc/pacman.d/mirrorlist\n"), "rest of file kept");
    free(r);

    const char *conf2 = "[options]\nColor\n[extra]\n# ParallelDownloads = 3 in a comment\n";
    r = pacman_conf_with_parallel(conf2, 2);
    ASSERT(strstr(r, "[options]\nColor\nParallelDownloads = 2\n[extra]\n"),
           "inserted at the end of [options] when missing");
    ASSERT(strstr(r, "# ParallelDownloads = 3 in a comment"), "other sections untouched");
    free(r);

    r = pacman_conf_with_parallel("[options]\nParallelDownloads = 4\n", 4);
    ASSERT_STR_EQ(r, "[options]\nParallelDownloads = 4\n", "unchanged when equal");
    free(r);

    ASSERT(recommended_parallel_downloads(50 * 1024) == 1 &&
           recommended_parallel_downloads(20.0 * 1024 * 1024) == 8, "recommended values");

    /* mirrorlist parsing */
    const char *ml = "## Brazil\nServer = https://a.example/archlinux/$repo/os/$arch\n"
                     "#Server = http://b.example/arch/$repo/os/$arch\n"
                     "  Server=https://c.example/$repo/os/$arch # trailing\n"
                     "Server = ftp://bad.example/$repo\nServer = https://a.example/archlinux/$repo/os/$arch\n";
    StrList s;
    sl_init(&s);
    mirrorlist_servers(ml, false, &s);
    ASSERT(s.n == 2 && strcmp(s.v[1], "https://c.example/$repo/os/$arch") == 0,
           "active servers, deduplicated, ftp ignored");
    sl_free(&s);
    sl_init(&s);
    mirrorlist_servers(ml, true, &s);
    ASSERT(s.n == 3, "commented servers with --all");

    /* rewrite: ranked first, old entries commented, repeatable */
    StrList ranked;
    sl_init(&ranked);
    sl_push(&ranked, "https://c.example/$repo/os/$arch");
    char *neu = mirrorlist_rewrite(ml, &ranked, "2026-11-07 12:00");
    StrList act;
    sl_init(&act);
    mirrorlist_servers(neu, false, &act);
    ASSERT(act.n == 1 && strcmp(act.v[0], ranked.v[0]) == 0, "only ranked servers active");
    ASSERT(strstr(neu, "## Brazil") && strstr(neu, "#Server = https://a.example"),
           "previous content kept as comments");
    char *again = mirrorlist_rewrite(neu, &ranked, "2026-11-08 12:00");
    ASSERT(strstr(again, "2026-11-07") == NULL, "previous inkaur block replaced, not stacked");
    sl_free(&act);
    sl_init(&act);
    mirrorlist_servers(again, false, &act);
    ASSERT(act.n == 1, "still one active server after a second ranking");
    free(again);
    free(neu);
    sl_free(&act);
    sl_free(&ranked);
    sl_free(&s);
    printf("mirrors: ok\n");
}
