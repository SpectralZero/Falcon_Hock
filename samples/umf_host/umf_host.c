/*
 * umf_host.c — minimal self-host for exercising Hexforge Studio against REAL data.
 *
 * Loads the Hexforge runtime in-process, which starts the JSON-RPC IPC server
 * on \\.\pipe\umf-studio-<pid>, installs one genuine inline hook so the Studio's
 * Hooks panel is populated, then streams heartbeat log lines once a second so
 * the live Logs view and evalLua have something real to talk to.
 *
 * This is a developer harness — no game or injection required. Build the
 * `umf_host` target, run it, note the printed PID, and attach Studio to it.
 */

#include "umf/umf.h"
#include <stdio.h>

typedef int (*add_fn)(int, int);

/* The function we hook (volatile pointer => a real indirect call to the patch). */
static int __declspec(noinline) demo_add(int a, int b) {
    volatile int r = a + b;
    return r;
}

static add_fn g_orig_add = NULL;
static int __declspec(noinline) hook_add(int a, int b) {
    int base = g_orig_add ? g_orig_add(a, b) : (a + b);
    return base + 100; /* observable effect: 2+3 => 105 */
}

int main(void) {
    umf_log_init(L"logs");
    umf_log_set_level(UMF_LOG_INFO);

    if (!umf_init()) {
        printf("umf_init failed (anti-cheat present or Windows too old?)\n");
        return 1;
    }

    printf("=================================================\n");
    printf("  Hexforge runtime host\n");
    printf("  PID : %lu\n", (unsigned long)GetCurrentProcessId());
    printf("  Pipe: %s\n", umf_ipc_pipe_name());
    printf("=================================================\n");
    printf("  Attach Hexforge Studio to this PID. Ctrl+C to exit.\n\n");
    fflush(stdout);

    UmfMitigationStatus m;
    umf_detect_all_mitigations(&m);
    bool inline_ok = (umf_viable_strategies(&m) & UMF_STRAT_INLINE) &&
                     umf_can_write_code_page(&m);

    volatile add_fn call = demo_add;
    if (inline_ok) {
        if (umf_register_hook_addr((void*)call, "umf_host!demo_add",
                                   (void*)&hook_add, 0, NULL,
                                   (void**)&g_orig_add) &&
            umf_apply_pending_batch()) {
            UMF_INFO("installed demo inline hook on umf_host!demo_add");
        } else {
            UMF_WARN("failed to install demo hook");
        }
    } else {
        UMF_WARN("inline hooking unavailable on this host; running without a demo hook");
    }

    int i = 0;
    for (;;) {
        int v = call(2, 3); /* triggers the hook when installed */
        UMF_INFO("heartbeat %d  demo_add(2,3) = %d", i++, v);
        Sleep(1000);
    }
}
