#include "../../alloc.h"
#include "../test_helper.h"
#include <string.h>

void test_safe_malloc() {
    void *ptr = safe_malloc(10);
    ASSERT(ptr != NULL, "safe_malloc returned NULL");
    free(ptr);
}

void test_safe_calloc() {
    int *ptr = safe_calloc(5, sizeof(int));
    ASSERT(ptr != NULL, "safe_calloc returned NULL");
    for(int i=0; i<5; i++) ASSERT(ptr[i] == 0, "calloc didn't zero memory");
    free(ptr);
}

void test_safe_realloc() {
    void *ptr = safe_malloc(10);
    ptr = safe_realloc(ptr, 20);
    ASSERT(ptr != NULL, "safe_realloc returned NULL");
    free(ptr);
}

void test_safe_strdup() {
    char *orig = "inkaur";
    char *copy = safe_strdup(orig);
    ASSERT_STR_EQ(copy, orig, "safe_strdup failed");
    ASSERT(copy != orig, "safe_strdup didn't allocate new memory");
    free(copy);
    ASSERT(safe_strdup(NULL) == NULL, "safe_strdup(NULL) should be NULL");
}

void run_alloc_tests() {
    test_safe_malloc();
    test_safe_calloc();
    test_safe_realloc();
    test_safe_strdup();
    printf("Unit tests for alloc passed!\n");
}
