/*
 * test_hook_inline.c — end-to-end inline hook integration test.
 *
 * Exercises the full engine: prologue relocation (Zydis), unwind
 * registration, thread freeze + atomic patch, Detours-style chain
 * dispatch, calling the original through the trampoline, multi-hook
 * priority chaining, and clean unhook/restore.
 *
 * The test hooks functions in its own module by address. Targets are
 * reached through a volatile function pointer so the compiler must emit
 * a real indirect call to the (patched) entry point.
 */
#include "umf/umf.h"
#include "test_framework.h"

typedef int (*add_fn)(int, int);

/* ── Hook target ── */
static int __declspec(noinline) target_add(int a, int b) {
    volatile int r = a + b;   /* volatile: defeat constant folding */
    return r;
}

/* ── Primary hook (priority 0) ── */
static add_fn g_orig_add  = NULL;
static int    g_hook_hits = 0;
static int __declspec(noinline) hook_add(int a, int b) {
    g_hook_hits++;
    int base = g_orig_add ? g_orig_add(a, b) : -1000;
    return base + 100;
}

/* ── Secondary hook (priority 10, runs first) ── */
static add_fn g_orig_add2  = NULL;
static int    g_hook2_hits = 0;
static int __declspec(noinline) hook_add2(int a, int b) {
    g_hook2_hits++;
    int base = g_orig_add2 ? g_orig_add2(a, b) : -1000;
    return base * 2;
}

void run_hook_inline_tests(void) {
    if (!umf_init()) {
        CHECK(0, "umf_init succeeded");
        return;
    }
    CHECK(1, "umf_init succeeded");

    /* Skip gracefully where the platform forbids inline patching. */
    UmfMitigationStatus m;
    umf_detect_all_mitigations(&m);
    bool inline_ok = (umf_viable_strategies(&m) & UMF_STRAT_INLINE) &&
                     umf_can_write_code_page(&m);
    if (!inline_ok) {
        printf("  [skip] inline hooking unavailable on this host (ACG/HVCI)\n");
        umf_shutdown();
        return;
    }

    volatile add_fn call = target_add;
    void* target_addr = (void*)call;

    CHECK(call(2, 3) == 5, "baseline: target returns a+b");

    /* ── Install the primary hook ── */
    bool reg = umf_register_hook_addr(target_addr, "target_add",
                                      (void*)&hook_add, 0, NULL,
                                      (void**)&g_orig_add);
    CHECK(reg, "register inline hook by address");
    CHECK(umf_apply_pending_batch(), "apply pending batch");
    CHECK(g_orig_add != NULL, "engine populated 'call original' pointer");

    g_hook_hits = 0;
    int r = call(2, 3);
    CHECK(g_hook_hits == 1, "hook body ran exactly once");
    CHECK(r == 105, "hook returned original(5) + 100");

    if (g_orig_add) {
        CHECK(g_orig_add(10, 20) == 30, "trampoline replays original (10+20)");
    }

    /* ── Add a higher-priority hook: chain must become hook2 -> hook -> orig ── */
    bool reg2 = umf_register_hook_addr(target_addr, "target_add",
                                       (void*)&hook_add2, 10, NULL,
                                       (void**)&g_orig_add2);
    CHECK(reg2, "register second (higher-priority) hook");
    CHECK(umf_apply_pending_batch(), "apply chain update");

    g_hook_hits = g_hook2_hits = 0;
    int rc = call(2, 3);
    CHECK(g_hook2_hits == 1, "priority-10 hook ran first");
    CHECK(g_hook_hits == 1,  "priority-0 hook ran next");
    CHECK(rc == 210, "chained result = (5 + 100) * 2");

    /* ── Unhook: original behaviour must return ── */
    UmfHookTarget* t = umf_hook_registry_find(target_addr);
    CHECK(t != NULL, "registry finds target");
    if (t) {
        umf_queue_hook_disable(t);
        umf_apply_pending_batch();

        g_hook_hits = g_hook2_hits = 0;
        int r2 = call(2, 3);
        CHECK(g_hook_hits == 0 && g_hook2_hits == 0, "no hook fires after unhook");
        CHECK(r2 == 5, "original behaviour restored");
    }

    umf_shutdown();
    CHECK(1, "umf_shutdown without crash");
}
