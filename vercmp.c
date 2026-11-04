/*
 * Port of libalpm's version comparison (lib/libalpm/version.c, GPL-2.0+
 * algorithm originally from rpm's rpmvercmp). Reimplemented here so that
 * inkaur does not link against libalpm.
 */
#include "vercmp.h"
#include "alloc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int c_isdigit(char c) { return isdigit((unsigned char)c); }
static int c_isalpha(char c) { return isalpha((unsigned char)c); }
static int c_isalnum(char c) { return isalnum((unsigned char)c); }

static void parse_evr(char *evr, const char **ep, const char **vp,
                      const char **rp)
{
    char *s = evr;
    while (*s && c_isdigit(*s))
        s++;
    char *se = strrchr(s, '-');

    if (*s == ':') {
        *ep = evr;
        *s++ = '\0';
        *vp = s;
        if (**ep == '\0')
            *ep = "0";
    } else {
        *ep = "0";
        *vp = evr;
    }
    if (se) {
        *se++ = '\0';
        *rp = se;
    } else {
        *rp = NULL;
    }
}

static int rpmvercmp(const char *a, const char *b)
{
    if (strcmp(a, b) == 0)
        return 0;

    char *str1 = xstrdup(a);
    char *str2 = xstrdup(b);
    char *one = str1, *two = str2;
    char *ptr1 = one, *ptr2 = two;
    int ret = 0;

    while (*one && *two) {
        while (*one && !c_isalnum(*one))
            one++;
        while (*two && !c_isalnum(*two))
            two++;
        if (!*one || !*two)
            break;
        if ((one - ptr1) != (two - ptr2)) {
            ret = (one - ptr1) < (two - ptr2) ? -1 : 1;
            goto out;
        }
        ptr1 = one;
        ptr2 = two;

        int isnum;
        if (c_isdigit(*ptr1)) {
            while (*ptr1 && c_isdigit(*ptr1))
                ptr1++;
            while (*ptr2 && c_isdigit(*ptr2))
                ptr2++;
            isnum = 1;
        } else {
            while (*ptr1 && c_isalpha(*ptr1))
                ptr1++;
            while (*ptr2 && c_isalpha(*ptr2))
                ptr2++;
            isnum = 0;
        }

        char oldch1 = *ptr1;
        *ptr1 = '\0';
        char oldch2 = *ptr2;
        *ptr2 = '\0';

        if (one == ptr1) {
            ret = -1;
            goto out;
        }
        if (two == ptr2) {
            ret = isnum ? 1 : -1;
            goto out;
        }
        if (isnum) {
            while (*one == '0')
                one++;
            while (*two == '0')
                two++;
            size_t l1 = strlen(one), l2 = strlen(two);
            if (l1 > l2) {
                ret = 1;
                goto out;
            }
            if (l2 > l1) {
                ret = -1;
                goto out;
            }
        }
        int rc = strcmp(one, two);
        if (rc) {
            ret = rc < 1 ? -1 : 1;
            goto out;
        }
        *ptr1 = oldch1;
        one = ptr1;
        *ptr2 = oldch2;
        two = ptr2;
    }

    if (!*one && !*two) {
        ret = 0;
        goto out;
    }
    if ((!*one && !c_isalpha(*two)) || c_isalpha(*one))
        ret = -1;
    else
        ret = 1;

out:
    free(str1);
    free(str2);
    return ret;
}

int pkg_vercmp(const char *a, const char *b)
{
    if (!a && !b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;
    if (strcmp(a, b) == 0)
        return 0;

    char *full1 = xstrdup(a), *full2 = xstrdup(b);
    const char *e1, *v1, *r1, *e2, *v2, *r2;
    parse_evr(full1, &e1, &v1, &r1);
    parse_evr(full2, &e2, &v2, &r2);

    int ret = rpmvercmp(e1, e2);
    if (ret == 0) {
        ret = rpmvercmp(v1, v2);
        if (ret == 0 && r1 && r2)
            ret = rpmvercmp(r1, r2);
    }
    free(full1);
    free(full2);
    return ret;
}
