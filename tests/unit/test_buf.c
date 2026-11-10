#include "../test_helper.h"
#include "buf.h"

void run_buf_tests(void);

void run_buf_tests(void)
{
    StrBuf b;
    sb_init(&b, 8);
    ASSERT(sb_puts(&b, "1234") == 0, "append within limit");
    ASSERT(sb_puts(&b, "5678") == 0, "append up to limit");
    ASSERT(sb_puts(&b, "9") != 0, "append beyond limit must fail");
    ASSERT_STR_EQ(b.data, "12345678", "content kept intact after refused append");
    sb_free(&b);

    StrList l;
    sl_init(&l);
    sl_split_lines(&l, "b\r\n\na\nc");
    ASSERT(l.n == 3, "split skips empty lines and strips CR");
    sl_sort(&l);
    ASSERT_STR_EQ(l.v[0], "a", "sorted");
    ASSERT(sl_bsearch(&l, "c") && !sl_bsearch(&l, "d"), "bsearch");
    ASSERT(!sl_push_unique(&l, "a"), "unique push rejects duplicate");
    sl_free(&l);
    printf("buf: ok\n");
}

#include "tty.h"
#include "review.h"
#include <stdio.h>

void run_tty_tests(void);

void run_tty_tests(void)
{
    /* security approvals must refuse non-interactive input ("yes | inkaur") */
    FILE *f = freopen("/dev/null", "r", stdin);
    ASSERT(f != NULL, "reopen stdin");
    ASSERT(!ask_approval("approve?"), "approval refused without a tty");
    ASSERT(ask_decision("pkg", "build?", SEV_INFO, true, "view") == DEC_ABORT,
           "package decision aborts without a tty");
    ASSERT(ask_yesno("q?", true, true), "noconfirm answers non-security prompts");
    printf("tty: ok\n");
}
