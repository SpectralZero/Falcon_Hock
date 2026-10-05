/*
 * native_mod.c — a minimal native UMF mod used by the loader test.
 *
 * On init it installs an IAT hook on GetCurrentProcessId in the host EXE
 * (returning a sentinel); on shutdown it removes the hook. It links against
 * umf_runtime and calls the umf_* API directly, exactly as a real mod would.
 */
#include "umf/umf.h"

#define NATIVE_MOD_SENTINEL 0x4D4F4421u   /* 'MOD!' */

static UmfIatLocation       g_loc;
static DWORD (WINAPI* g_orig)(void) = NULL;

static DWORD WINAPI mod_hook(void) { return NATIVE_MOD_SENTINEL; }

__declspec(dllexport) bool umf_mod_init(UmfMod* self) {
    (void)self;
    HMODULE host = GetModuleHandleW(NULL);
    return umf_hook_iat(host, "kernel32.dll", "GetCurrentProcessId",
                        (void*)&mod_hook, (void**)&g_orig, &g_loc);
}

__declspec(dllexport) void umf_mod_shutdown(UmfMod* self) {
    (void)self;
    umf_unhook_iat(&g_loc);
}
