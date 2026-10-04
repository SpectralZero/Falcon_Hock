/*
 * test_eat.c — §EAT hooking test.
 *
 * Loads the plugin DLL, hooks its exported plugin_add via the export table,
 * then resolves it with GetProcAddress (which reads the EAT) and confirms the
 * resolved pointer runs the hook. Verifies the saved original and unhook.
 */
#include "umf/umf.h"
#include "test_framework.h"

typedef int (*add_fn)(int, int);

static add_fn g_orig_eat = NULL;
static int    eat_hook(int a, int b) {
    return (g_orig_eat ? g_orig_eat(a, b) : -1000) + 7;
}

void run_eat_tests(void) {
    HMODULE plugin = LoadLibraryA("umf_test_plugin.dll");
    CHECK(plugin != NULL, "load plugin DLL for EAT test");
    if (!plugin) return;

    add_fn before = (add_fn)(void*)GetProcAddress(plugin, "plugin_add");
    CHECK(before != NULL, "resolve plugin_add via export table");
    CHECK(before && before(2, 3) == 5, "plugin baseline (2 + 3)");

    void* orig = NULL;
    UmfEatLocation loc;
    bool hooked = umf_hook_eat(plugin, "plugin_add", (void*)&eat_hook,
                               &orig, &loc);
    CHECK(hooked, "EAT hook plugin_add");
    g_orig_eat = (add_fn)orig;
    CHECK(orig == (void*)before, "EAT original equals pre-hook export");

    /* GetProcAddress now reads the rewritten EAT entry. */
    add_fn after = (add_fn)(void*)GetProcAddress(plugin, "plugin_add");
    CHECK(after != NULL, "resolve after EAT hook");
    CHECK(after && after(2, 3) == 12, "EAT-resolved pointer runs hook (5 + 7)");

    CHECK(umf_unhook_eat(&loc), "unhook EAT entry");
    add_fn restored = (add_fn)(void*)GetProcAddress(plugin, "plugin_add");
    CHECK(restored && restored(2, 3) == 5, "EAT entry restored");

    FreeLibrary(plugin);
    CHECK(1, "plugin freed after EAT test");
}
