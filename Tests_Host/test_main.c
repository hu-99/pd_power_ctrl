#include "test_framework.h"

int g_tests_run = 0;
int g_tests_failed = 0;

int run_pd_sink_policy_tests(void);
int run_power_pid_tests(void);
int run_prot_ovp_ocp_tests(void);
int run_extdev_driver_tests(void);
int run_fault_blackbox_tests(void);
int run_bc1_decode_tests(void);

int main(void) {
    run_pd_sink_policy_tests();
    run_power_pid_tests();
    run_prot_ovp_ocp_tests();
    run_extdev_driver_tests();
    run_fault_blackbox_tests();
    run_bc1_decode_tests();

    printf("----------------------------------------\n");
    printf("Total: %d, Passed: %d, Failed: %d\n",
           g_tests_run, g_tests_run - g_tests_failed, g_tests_failed);
    return (g_tests_failed == 0) ? 0 : 1;
}
