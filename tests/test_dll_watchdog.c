/*
 * test_dll_watchdog.c — Tier-1 #1 integration test.
 *
 * Loads a plugin DLL, hooks an exported function in it, then unloads the
 * DLL. The LdrRegisterDllNotification watchdog must tear the hook down so
 * that (a) the registry marks the target dead and releases its trampoline,
 * and (b) a late "call original" is neutralized to 0 instead of faulting
 * on freed code.
 */
#include "umf/umf.h"
#include "test_framework.h"

typedef int (*add_fn)(int, int);

static add_fn g_orig_plugin = NULL;
static int    g_plugin_hits = 0;

static int plugin_hook(int a, int b) {
    g_plugin_hits++;
    int base = g_orig_plugin ? g_orig_plugin(a, b) : -1000;
    return base + 7;
}

void run_dll_watchdog_tests(void) {
    if (!umf_init()) { CHECK(0, "umf_init succeeded"); return; }

    UmfMitigationStatus m;
    umf_detect_all_mitigations(&m);
    bool inline_ok = (umf_viable_strategies(&m) & UMF_STRAT_INLINE) &&
                     umf_can_write_code_page(&m);
    if (!inline_ok) {
        printf("  [skip] inline hooking unavailable on this host (ACG/HVCI)\n");
        umf_shutdown();
        return;
    }

    HMODULE h = LoadLibraryA("umf_test_plugin.dll");
    CHECK(h != NULL, "load test plugin DLL");
    if (!h) { umf_shutdown(); return; }

    add_fn padd = (add_fn)(void*)GetProcAddress(h, "plugin_add");
    CHECK(padd != NULL, "resolve plugin_add export");
    if (!padd) { FreeLibrary(h); umf_shutdown(); return; }

    CHECK(padd(2, 3) == 5, "plugin baseline before hook");

    bool reg = umf_register_hook_addr((void*)padd, "plugin_add",
                                      (void*)&plugin_hook, 0, NULL,
                                      (void**)&g_orig_plugin);
    CHECK(reg, "hook the plugin export");
    CHECK(umf_apply_pending_batch(), "apply hook");

    g_plugin_hits = 0;
    CHECK(padd(2, 3) == 12, "hook active on plugin (5 + 7)");
    CHECK(g_plugin_hits == 1, "plugin hook fired");

    UmfHookTarget* t = umf_hook_registry_find((void*)padd);
    CHECK(t != NULL && t->trampoline != NULL, "target installed with trampoline");

    /* Unload the module. FreeLibrary fires the UNLOADED notification
     * synchronously (under the loader lock) before returning. */
    FreeLibrary(h);

    CHECK(t != NULL && t->module_unloaded, "watchdog flagged the unload");
    CHECK(t != NULL && t->trampoline == NULL, "watchdog released the trampoline");
    CHECK(g_orig_plugin != NULL, "'call original' pointer still non-NULL");
    /* The plugin's code is gone; calling the (now-neutralized) original must
     * not fault. Before the watchdog this jumped into freed memory. */
    CHECK(g_orig_plugin(2, 3) == 0, "late 'call original' neutralized to 0 (no crash)");

    umf_shutdown();
    CHECK(1, "umf_shutdown after watchdog test");
}
