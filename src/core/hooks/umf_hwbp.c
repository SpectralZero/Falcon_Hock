/*
 * umf_hwbp.c — §HWBP: hardware-breakpoint hooking via Dr0–Dr3 + VEH
 *
 * The debug registers raise a #DB (EXCEPTION_SINGLE_STEP) when execution
 * reaches a watched address. A vectored exception handler catches it and
 * redirects RIP to the hook. Nothing in the target is written, so this is
 * the last-resort strategy when code pages are immutable (HVCI) and no
 * import/export slot is available (ACG, dynamic resolution).
 *
 * Constraints:
 *   - At most 4 breakpoints (Dr0–Dr3), process-wide.
 *   - Debug registers are per-thread: we program every current thread.
 *     Threads created later are not covered (a full solution would hook
 *     thread creation); documented limitation for v1.
 *
 * Calling the original (no trampoline): the hook calls umf_hwbp_enter_original()
 * then invokes the target through a function pointer. The handler sees the
 * armed per-thread flag, clears it, and sets the Resume Flag so the single
 * re-entry at the breakpoint address executes without re-faulting.
 */

#include "umf/umf.h"
#include <tlhelp32.h>

#define UMF_MAX_HWBP 4
#define UMF_EFLAGS_RF 0x00010000u   /* Resume Flag */

typedef struct {
    void* target;
    void* hook;
    bool  active;
} UmfHwbpEntry;

static UmfHwbpEntry g_hwbp[UMF_MAX_HWBP];
static SRWLOCK      g_hwbp_lock = SRWLOCK_INIT;
static PVOID        g_veh = NULL;

/* Per-thread one-shot: when set, the next #DB at a watched address passes
 * through (the hook is invoking the original) instead of redirecting. */
static __declspec(thread) bool t_pass_through = false;

/* ── Debug-register programming ── */

static void apply_dr(CONTEXT* ctx, int slot, void* target, bool enable) {
    DWORD64* dr;
    switch (slot) {
        case 0: dr = &ctx->Dr0; break;
        case 1: dr = &ctx->Dr1; break;
        case 2: dr = &ctx->Dr2; break;
        case 3: dr = &ctx->Dr3; break;
        default: return;
    }
    if (enable) {
        *dr = (DWORD64)(uintptr_t)target;
        /* RW/LEN nibble for this slot = 0000 → execute, 1 byte */
        ctx->Dr7 &= ~(0xFULL << (16 + 4 * slot));
        ctx->Dr7 |=  (1ULL   << (2 * slot));          /* local enable */
    } else {
        *dr = 0;
        ctx->Dr7 &= ~(1ULL   << (2 * slot));
        ctx->Dr7 &= ~(0xFULL << (16 + 4 * slot));
    }
}

static void program_all_threads(int slot, void* target, bool enable) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    DWORD pid  = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();

    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;

            HANDLE h = OpenThread(
                THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE, te.th32ThreadID);
            if (!h) continue;

            bool is_self = (te.th32ThreadID == self);
            if (!is_self) SuspendThread(h);   /* can't suspend self */

            CONTEXT ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &ctx)) {
                apply_dr(&ctx, slot, target, enable);
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                SetThreadContext(h, &ctx);
            }

            if (!is_self) ResumeThread(h);
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

/* ── Vectored exception handler ── */

static LONG CALLBACK umf_hwbp_veh(PEXCEPTION_POINTERS ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;

    void* addr = ep->ExceptionRecord->ExceptionAddress;

    void* hook = NULL;
    AcquireSRWLockShared(&g_hwbp_lock);
    for (int i = 0; i < UMF_MAX_HWBP; i++) {
        if (g_hwbp[i].active && g_hwbp[i].target == addr) {
            hook = g_hwbp[i].hook;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hwbp_lock);

    if (!hook) return EXCEPTION_CONTINUE_SEARCH;

    if (t_pass_through) {
        /* The hook is calling the original: let this one instruction run. */
        t_pass_through = false;
        ep->ContextRecord->EFlags |= UMF_EFLAGS_RF;
        return EXCEPTION_CONTINUE_EXECUTION;   /* RIP unchanged = target */
    }

    /* Redirect this call into the hook. */
    ep->ContextRecord->Rip = (DWORD64)(uintptr_t)hook;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* ── Public API ── */

void umf_hwbp_enter_original(void) {
    t_pass_through = true;
}

bool umf_hook_hwbp(void* target, void* hook, UmfHwbpLocation* out_loc) {
    if (!target || !hook) return false;

    AcquireSRWLockExclusive(&g_hwbp_lock);

    int slot = -1;
    for (int i = 0; i < UMF_MAX_HWBP; i++) {
        if (g_hwbp[i].active && g_hwbp[i].target == target) {
            ReleaseSRWLockExclusive(&g_hwbp_lock);
            UMF_WARN("HW breakpoint already set on %p", target);
            return false;
        }
        if (slot < 0 && !g_hwbp[i].active) slot = i;
    }
    if (slot < 0) {
        ReleaseSRWLockExclusive(&g_hwbp_lock);
        UMF_ERROR("All %d hardware breakpoints are in use", UMF_MAX_HWBP);
        return false;
    }

    if (!g_veh) {
        g_veh = AddVectoredExceptionHandler(1, umf_hwbp_veh);
        if (!g_veh) {
            ReleaseSRWLockExclusive(&g_hwbp_lock);
            UMF_ERROR("AddVectoredExceptionHandler failed: %lu", GetLastError());
            return false;
        }
    }

    g_hwbp[slot].target = target;
    g_hwbp[slot].hook   = hook;
    g_hwbp[slot].active = true;

    ReleaseSRWLockExclusive(&g_hwbp_lock);

    program_all_threads(slot, target, true);

    if (out_loc) { out_loc->slot = slot; out_loc->target = target; }
    UMF_INFO("HW-bp hook: %p -> %p (Dr%d)", target, hook, slot);
    return true;
}

bool umf_unhook_hwbp(const UmfHwbpLocation* loc) {
    if (!loc || loc->slot < 0 || loc->slot >= UMF_MAX_HWBP) return false;

    program_all_threads(loc->slot, NULL, false);

    AcquireSRWLockExclusive(&g_hwbp_lock);
    g_hwbp[loc->slot].active = false;
    g_hwbp[loc->slot].target = NULL;
    g_hwbp[loc->slot].hook   = NULL;

    bool any = false;
    for (int i = 0; i < UMF_MAX_HWBP; i++) if (g_hwbp[i].active) any = true;
    if (!any && g_veh) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = NULL;
    }
    ReleaseSRWLockExclusive(&g_hwbp_lock);
    return true;
}
