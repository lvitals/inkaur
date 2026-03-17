#ifndef TEST_HELPER_H
#define TEST_HELPER_H

#include <stdio.h>
#include <stdlib.h>

#define ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAILED: %s (%s:%d)\n", message, __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

#define ASSERT_STR_EQ(actual, expected, message) \
    do { \
        if (strcmp(actual, expected) != 0) { \
            fprintf(stderr, "FAILED: %s\n  Actual:   \"%s\"\n  Expected: \"%s\"\n  (%s:%d)\n", \
                    message, actual, expected, __FILE__, __LINE__); \
            exit(1); \
        } \
    } while (0)

#endif
