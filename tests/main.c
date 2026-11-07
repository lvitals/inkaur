#include "test_helper.h"

int tests_run = 0;

void run_buf_tests(void);
void run_validate_tests(void);
void run_json_tests(void);
void run_vercmp_tests(void);
void run_srcinfo_tests(void);
void run_audit_tests(void);
void run_audit_calibration_tests(void);
void run_tty_tests(void);
void run_cli_parse_tests(void);
void run_perf_tests(void);
void run_mirrors_tests(void);
void run_cli_tests(void);

int main(void)
{
    printf("inkaur test suite (ASan + UBSan)\n\n--- unit ---\n");
    run_buf_tests();
    run_validate_tests();
    run_json_tests();
    run_vercmp_tests();
    run_srcinfo_tests();
    run_audit_tests();
    run_audit_calibration_tests();
    run_tty_tests();
    run_cli_parse_tests();
    run_perf_tests();
    run_mirrors_tests();
    printf("\n--- functional ---\n");
    run_cli_tests();
    printf("\nall %d assertions passed\n", tests_run);
    return 0;
}
