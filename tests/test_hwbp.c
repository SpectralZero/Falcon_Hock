/*
 * test_hwbp.c — §HWBP hardware-breakpoint hooking test.
 *
 * Installs an execute breakpoint on a function, verifies the vectored
 * handler redirects the call into the hook, that the hook can invoke the
 * original via the Resume-Flag pass-through, and that unhook restores
 * normal execution. No bytes of the target are modified.
 */
#include "umf/umf.h"
#include "test_framework.h"

typedef int (*mul_fn)(int, int);

static int    target_mul(int a, int b) { volatile int r = a * b; return r; }
static mul_fn g_target_mul = NULL;
static int    g_hwbp_hits  = 0;

static int hwbp_hook(int a, int b) {
    g_hwbp_hits++;
    umf_hwbp_enter_original();           /* arm one-shot pass-through */
    int base = g_target_mul(a, b);       /* re-enters target; VEH lets it run */
    return base + 1;
}

void run_hwbp_tests(void) {
    g_target_mul = target_mul;
    volatile mul_fn call = target_mul;
    void* taddr = (void*)call;

    CHECK(call(6, 7) == 42, "hwbp baseline (6 * 7)");

    UmfHwbpLocation loc;
    bool hooked = umf_hook_hwbp(taddr, (void*)&hwbp_hook, &loc);
    CHECK(hooked, "install hardware-breakpoint hook");

    if (hooked) {
        g_hwbp_hits = 0;
        int r = call(6, 7);
        CHECK(g_hwbp_hits == 1, "VEH redirected the call into the hook");
        CHECK(r == 43, "hook ran + original via RF pass-through (42 + 1)");

        CHECK(umf_unhook_hwbp(&loc), "remove hardware breakpoint");

        g_hwbp_hits = 0;
        int r2 = call(6, 7);
        CHECK(r2 == 42 && g_hwbp_hits == 0,
              "original restored, no hook after unhook");
    }
}
