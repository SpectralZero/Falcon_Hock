/*
 * test_framework.h — minimal check/report harness for the UMF test exe.
 */
#ifndef UMF_TEST_FRAMEWORK_H
#define UMF_TEST_FRAMEWORK_H

#include <stdio.h>

extern int g_checks_run;
extern int g_checks_failed;

#define CHECK(cond, msg)                                               \
    do {                                                               \
        g_checks_run++;                                                \
        if (cond) {                                                    \
            printf("  [ ok ] %s\n", (msg));                            \
        } else {                                                       \
            g_checks_failed++;                                         \
            printf("  [FAIL] %s  (%s:%d)\n", (msg), __FILE__, __LINE__);\
        }                                                              \
    } while (0)

/* Suites */
void run_log_tests(void);
void run_mitigation_tests(void);
void run_trampoline_pool_tests(void);
void run_hook_inline_tests(void);
void run_dll_watchdog_tests(void);
void run_iat_tests(void);
void run_vtable_tests(void);
void run_eat_tests(void);

#endif /* UMF_TEST_FRAMEWORK_H */
