/*
 * umf_init.c — Framework lifecycle + shared runtime state
 *
 * Owns the two process-global objects the rest of the runtime shares:
 *   - g_trampoline_pool : proximity trampoline allocator
 *   - g_mitigations     : detected process security mitigations
 *
 * umf_init() runs the mandatory pre-flight checks (Windows floor,
 * anti-cheat refusal), detects mitigations, and brings up the hook
 * subsystems. It does NOT initialise logging — the host/proxy owns the
 * log destination and must call umf_log_init() first.
 */

#include "umf/umf.h"

/* ── Shared runtime state (declared extern in umf.h) ── */
UmfTrampolinePool   g_trampoline_pool;
UmfMitigationStatus g_mitigations;

static bool g_umf_initialized = false;

bool umf_init(void) {
    if (g_umf_initialized) {
        UMF_WARN("umf_init() called twice — ignoring");
        return true;
    }

    UMF_INFO("UMF v%s initializing...", UMF_VERSION_STRING);

    /* 1. Windows version floor (§14) */
    if (!umf_check_minimum_version()) {
        UMF_ERROR("Windows version below minimum — aborting init");
        return false;
    }

    /* 2. Anti-cheat refusal (§13) — never inject into protected processes */
    if (umf_check_anticheat()) {
        UMF_ERROR("Anti-cheat detected — refusing to initialize");
        return false;
    }

    /* 3. Detect active mitigations (§7, §9) — drives strategy selection */
    umf_detect_all_mitigations(&g_mitigations);

    /* 4. Bring up hook subsystems */
    umf_trampoline_pool_init(&g_trampoline_pool);
    umf_hook_registry_init();

    g_umf_initialized = true;
    UMF_INFO("UMF initialized successfully");
    return true;
}

void umf_shutdown(void) {
    if (!g_umf_initialized) return;

    UMF_INFO("UMF shutting down...");
    umf_trampoline_pool_destroy(&g_trampoline_pool);
    g_umf_initialized = false;
    UMF_INFO("UMF shutdown complete");
}

/* Minimal DLL entry — the framework is initialised explicitly via
 * umf_init(), not from the loader lock. We only opt out of per-thread
 * attach/detach callbacks we do not use. */
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
