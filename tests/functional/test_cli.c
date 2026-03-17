#include "../test_helper.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

extern FILE *popen(const char *command, const char *type);
extern int pclose(FILE *stream);

void test_version_output(void) {
    FILE *fp = popen("./inkaur -V", "r");
    ASSERT(fp != NULL, "failed to run inkaur -V");
    
    char buf[1024];
    bool found_version = false;
    bool found_author = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "inkaur 3.0")) {
            found_version = true;
        }
        if (strstr(buf, "Leandro V. Catarin")) {
            found_author = true;
        }
    }
    
    ASSERT(found_version, "inkaur version string not found in output");
    ASSERT(found_author, "Author Leandro V. Catarin not found in output");
    pclose(fp);
}

void test_help_output(void) {
    FILE *fp = popen("./inkaur -h", "r");
    ASSERT(fp != NULL, "failed to run inkaur -h");
    
    char buf[1024];
    bool found = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "usage:  inkaur")) {
            found = true;
            break;
        }
    }
    
    ASSERT(found, "inkaur help usage not found in output");
    pclose(fp);
}

void run_cli_tests(void) {
    test_version_output();
    test_help_output();
    printf("Functional tests for CLI passed!\n");
}
