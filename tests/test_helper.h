#ifndef TEST_HELPER_H
#define TEST_HELPER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int tests_run;

#define ASSERT(cond, msg) \
    do { \
        tests_run++; \
        if (!(cond)) { \
            fprintf(stderr, "FAILED: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

#define ASSERT_STR_EQ(actual, expected, msg) \
    do { \
        tests_run++; \
        const char *a_ = (actual), *e_ = (expected); \
        if (a_ == NULL || strcmp(a_, e_) != 0) { \
            fprintf(stderr, "FAILED: %s\n  actual:   \"%s\"\n  expected: \"%s\"\n  (%s:%d)\n", \
                    msg, a_ ? a_ : "(null)", e_, __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

#endif
