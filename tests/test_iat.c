/*
 * test_iat.c — §IAT integration tests.
 *
 * Part A: hook an imported API (GetCurrentProcessId) through this module's
 *         own IAT, verify calls are redirected, the saved original works,
 *         and unhook restores the slot — all data-only, no code patching.
 *
 * Part B: GetProcAddress fallback — register a redirect for a target and
 *         confirm GetProcAddress hands back the hook while unrelated lookups
 *         still resolve normally.
 */
#include "umf/umf.h"
#include "test_framework.h"

typedef DWORD (WINAPI *gpid_fn)(void);

static gpid_fn g_orig_gpid = NULL;
static DWORD WINAPI hook_gpid(void) { return 0x00C0FFEE; }

/* Stand-in hook for the GetProcAddress fallback (identity-compared only). */
static DWORD WINAPI dummy_tid_hook(void) { return 0xBEEF; }

void run_iat_tests(void) {
    /* ── Part A: IAT hooking (no engine init needed — pure data edit) ── */
    HMODULE self = GetModuleHandleW(NULL);
    DWORD real_pid = GetCurrentProcessId();

    void* orig = NULL;
    UmfIatLocation loc;
    bool hooked = umf_hook_iat(self, "kernel32.dll", "GetCurrentProcessId",
                               (void*)&hook_gpid, &orig, &loc);
    CHECK(hooked, "IAT hook GetCurrentProcessId");

    if (hooked) {
        CHECK(orig != NULL, "original pointer returned");
        g_orig_gpid = (gpid_fn)orig;

        CHECK(GetCurrentProcessId() == 0x00C0FFEE,
              "call routed through IAT hook");
        CHECK(g_orig_gpid() == real_pid,
              "original reachable via saved IAT pointer");

        CHECK(umf_unhook_iat(&loc), "unhook IAT slot");
        CHECK(GetCurrentProcessId() == real_pid, "IAT slot restored");
    }

    /* ── Part B: GetProcAddress fallback (needs inline hooking) ── */
    if (!umf_init()) { CHECK(0, "umf_init for GPA fallback"); return; }

    UmfMitigationStatus m;
    umf_detect_all_mitigations(&m);
    bool inline_ok = (umf_viable_strategies(&m) & UMF_STRAT_INLINE) &&
                     umf_can_write_code_page(&m);
    if (!inline_ok) {
        printf("  [skip] GetProcAddress fallback needs inline hooking (ACG/HVCI)\n");
        umf_shutdown();
        return;
    }

    void* target     = umf_resolve_function("kernel32.dll", "GetCurrentThreadId");
    void* real_gpid  = umf_resolve_function("kernel32.dll", "GetCurrentProcessId");
    CHECK(target != NULL, "resolve GetCurrentThreadId");

    bool inst = umf_install_getprocaddress_hook(target, (void*)&dummy_tid_hook);
    CHECK(inst, "install GetProcAddress fallback");

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    FARPROC redirected = GetProcAddress(k32, "GetCurrentThreadId");
    CHECK((void*)redirected == (void*)&dummy_tid_hook,
          "GetProcAddress returns the redirected hook");

    FARPROC passthrough = GetProcAddress(k32, "GetCurrentProcessId");
    CHECK((void*)passthrough == real_gpid,
          "unrelated GetProcAddress lookup still resolves normally");

    /* Restore GetProcAddress before shutdown (inline hook on a live module). */
    void* gpa = umf_resolve_function("kernel32.dll", "GetProcAddress");
    UmfHookTarget* t = umf_hook_registry_find(gpa);
    CHECK(t != NULL, "find GetProcAddress hook target");
    if (t) {
        umf_queue_hook_disable(t);
        umf_apply_pending_batch();
        FARPROC after = GetProcAddress(k32, "GetCurrentThreadId");
        CHECK((void*)after == target,
              "GetProcAddress restored after unhook (no redirect)");
    }

    umf_shutdown();
    CHECK(1, "umf_shutdown after IAT tests");
}
