/*
 * test_main.c — UMF unit/integration test entry point.
 */
#include "umf/umf.h"
#include "test_framework.h"

int g_checks_run = 0;
int g_checks_failed = 0;

int main(void) {
    umf_log_init(NULL);
    umf_log_set_level(UMF_LOG_WARN);   /* keep test output readable */

    printf("====================================\n");
    printf(" UMF test suite (v%s)\n", UMF_VERSION_STRING);
    printf("====================================\n");

    printf("\n[log]\n");             run_log_tests();
    printf("\n[mitigations]\n");     run_mitigation_tests();
    printf("\n[trampoline_pool]\n"); run_trampoline_pool_tests();
    printf("\n[hook_inline]\n");     run_hook_inline_tests();
    printf("\n[dll_watchdog]\n");    run_dll_watchdog_tests();
    printf("\n[iat]\n");             run_iat_tests();
    printf("\n[vtable]\n");          run_vtable_tests();
    printf("\n[eat]\n");             run_eat_tests();
    printf("\n[hwbp]\n");            run_hwbp_tests();
    printf("\n[mod_loader]\n");      run_mod_loader_tests();
    printf("\n[lua_hook]\n");        run_lua_hook_tests();

    printf("\n------------------------------------\n");
    printf(" %d checks, %d failed\n", g_checks_run, g_checks_failed);
    printf("------------------------------------\n");

    umf_log_shutdown();
    return g_checks_failed ? 1 : 0;
}
