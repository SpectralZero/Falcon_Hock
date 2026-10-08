/*
 * test_main.c — UMF unit/integration test entry point.
 */
#include "umf/umf.h"
#include "test_framework.h"

int g_checks_run = 0;
int g_checks_failed = 0;

/* Suite selection: set UMF_TEST_ONLY=<name> to run one suite (debugging). */
static int want(const char* name) {
    const char* only = getenv("UMF_TEST_ONLY");
    return !only || _stricmp(only, name) == 0;
}

int main(void) {
    umf_log_init(NULL);
    umf_log_set_level(UMF_LOG_WARN);   /* keep test output readable */

    /* Unbuffered stdout so a crash doesn't hide the last line. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("====================================\n");
    printf(" UMF test suite (v%s)\n", UMF_VERSION_STRING);
    printf("====================================\n");

    if (want("log"))             { printf("\n[log]\n");             run_log_tests(); }
    if (want("mitigations"))     { printf("\n[mitigations]\n");     run_mitigation_tests(); }
    if (want("trampoline_pool")) { printf("\n[trampoline_pool]\n"); run_trampoline_pool_tests(); }
    if (want("hook_inline"))     { printf("\n[hook_inline]\n");     run_hook_inline_tests(); }
    if (want("dll_watchdog"))    { printf("\n[dll_watchdog]\n");    run_dll_watchdog_tests(); }
    if (want("iat"))             { printf("\n[iat]\n");             run_iat_tests(); }
    if (want("vtable"))          { printf("\n[vtable]\n");          run_vtable_tests(); }
    if (want("eat"))             { printf("\n[eat]\n");             run_eat_tests(); }
    if (want("hwbp"))            { printf("\n[hwbp]\n");            run_hwbp_tests(); }
    if (want("mod_loader"))      { printf("\n[mod_loader]\n");      run_mod_loader_tests(); }
    if (want("lua_hook"))        { printf("\n[lua_hook]\n");        run_lua_hook_tests(); }
    if (want("xfg"))             { printf("\n[xfg]\n");             run_xfg_tests(); }
    if (want("overlay_dx11"))    { printf("\n[overlay_dx11]\n");    run_overlay_dx11_tests(); }
    if (want("aob"))             { printf("\n[aob]\n");             run_aob_tests(); }
    if (want("mem"))             { printf("\n[mem]\n");             run_mem_tests(); }
    if (want("ipc"))             { printf("\n[ipc]\n");             run_ipc_tests(); }
    if (want("scan"))            { printf("\n[scan]\n");           run_scan_tests(); }
    if (want("ct"))              { printf("\n[ct]\n");             run_ct_tests(); }
    if (want("launch"))          { printf("\n[launch]\n");         run_launch_tests(); }
    if (want("manifest"))        { printf("\n[manifest]\n");       run_manifest_tests(); }
    if (want("discovery"))       { printf("\n[discovery]\n");      run_discovery_tests(); }
    if (want("proxy"))           { printf("\n[proxy]\n");          run_proxy_tests(); }
    if (want("gdi_overlay"))     { printf("\n[gdi_overlay]\n");    run_gdi_overlay_tests(); }
    if (want("crash"))           { printf("\n[crash]\n");          run_crash_tests(); }
    if (want("lua_reload"))      { printf("\n[lua_reload]\n");     run_lua_reload_tests(); }
    if (want("profiler"))        { printf("\n[profiler]\n");       run_profiler_tests(); }

    printf("\n------------------------------------\n");
    printf(" %d checks, %d failed\n", g_checks_run, g_checks_failed);
    printf("------------------------------------\n");

    umf_log_shutdown();
    return g_checks_failed ? 1 : 0;
}
