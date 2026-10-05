/*
 * test_lua_hook.c — Lua-mod hook bindings end-to-end.
 *
 * Creates the UMF Lua sandbox, installs the `umf` API, and runs a Lua script
 * that hooks a C function. The script both transforms the return value and
 * calls umf.call_original to reach the real function. Verifies the C target
 * is actually redirected through the Lua callback.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <lua.h>          /* host embeds Lua to expose the target address */

typedef uint64_t (*add2)(uint64_t, uint64_t);

static uint64_t __declspec(noinline) lua_target_add(uint64_t a, uint64_t b) {
    volatile uint64_t r = a + b;
    return r;
}

void run_lua_hook_tests(void) {
    if (!umf_init()) { CHECK(0, "umf_init for lua hook"); return; }

    UmfMitigationStatus m;
    umf_detect_all_mitigations(&m);
    bool inline_ok = (umf_viable_strategies(&m) & UMF_STRAT_INLINE) &&
                     umf_can_write_code_page(&m);
    if (!inline_ok) {
        printf("  [skip] Lua inline hook needs inline strategy (ACG/HVCI)\n");
        umf_shutdown();
        return;
    }

    lua_State* L = umf_lua_create_sandbox();
    CHECK(L != NULL, "create Lua sandbox");
    if (!L) { umf_shutdown(); return; }
    umf_lua_setup_api(L, NULL);

    /* Expose the C target to the script. */
    lua_pushlightuserdata(L, (void*)&lua_target_add);
    lua_setglobal(L, "TARGET");

    volatile add2 call = lua_target_add;
    void* taddr = (void*)call;
    CHECK(call(2, 3) == 5, "baseline before Lua hook");

    const char* script =
        "umf.log('installing lua hook')\n"
        "umf.hook(TARGET, function(a, b)\n"
        "    local orig = umf.call_original(a, b)\n"
        "    return orig + 100\n"
        "end, { priority = 5 })\n";
    CHECK(umf_lua_run_string(L, script), "run Lua hook script");

    int r = (int)call(2, 3);
    CHECK(r == 105, "C call redirected through Lua (orig 5 + 100)");

    /* Second call: confirm the hook persists and stays correct. */
    CHECK((int)call(10, 20) == 130, "Lua hook stable across calls (30 + 100)");

    /* Unhook via the registry and confirm restoration. */
    UmfHookTarget* t = umf_hook_registry_find(taddr);
    CHECK(t != NULL, "find Lua hook target");
    if (t) {
        umf_queue_hook_disable(t);
        umf_apply_pending_batch();
        CHECK((int)call(2, 3) == 5, "target restored after unhook");
    }

    umf_lua_destroy_sandbox(L);
    umf_shutdown();
    CHECK(1, "umf_shutdown after Lua hook tests");
}
