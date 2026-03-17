#include <stdio.h>

/* Declarations of test runners from other files */
void run_json_tests(void);
void run_util_tests(void);
void run_alloc_tests(void);
void run_rpc_tests(void);
void run_install_tests(void);
void run_cli_tests(void);

int main(void) {
    printf("Starting inkAUR Test Suite...\n\n");

    printf("--- Unit Tests ---\n");
    run_json_tests();
    run_util_tests();
    run_alloc_tests();
    run_rpc_tests();
    run_install_tests();

    printf("\n--- Functional Tests ---\n");
    run_cli_tests();

    printf("\nAll tests passed successfully!\n");
    return 0;
}
