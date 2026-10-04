/*
 * umf_dll_watchdog.c — §WATCHDOG: module load/unload notifications
 *
 * Tier-1 #1 fix. When a target application unloads a module we have hooked
 * (a plugin, an Electron native addon, a lazily-unloaded DLL), the module's
 * code pages are freed. Any trampoline that replays that module's prologue —
 * and every mod "call original" pointer into it — then references freed
 * memory, so the next call faults.
 *
 * ntdll's documented LdrRegisterDllNotification delivers a synchronous
 * callback on every load/unload. On UNLOAD we hand the module's address
 * range to the registry, which releases the affected trampolines and
 * repoints "call original" pointers to a safe stub.
 *
 * The callback runs under the loader lock, so it must stay allocation-light
 * and must never load/unload libraries, freeze threads, or block. The
 * registry teardown it calls honours those constraints.
 */

#include "umf/umf.h"
#include <winternl.h>   /* UNICODE_STRING */

#define UMF_LDR_DLL_NOTIFICATION_REASON_LOADED   1
#define UMF_LDR_DLL_NOTIFICATION_REASON_UNLOADED 2

/* Both the Loaded and Unloaded variants share this layout. */
typedef struct {
    ULONG                 Flags;
    const UNICODE_STRING* FullDllName;
    const UNICODE_STRING* BaseDllName;
    PVOID                 DllBase;
    ULONG                 SizeOfImage;
} UMF_LDR_DLL_NOTIFICATION_DATA;

typedef VOID (CALLBACK *PUMF_LDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason,
    const UMF_LDR_DLL_NOTIFICATION_DATA* NotificationData,
    PVOID Context);

typedef NTSTATUS (NTAPI *PFN_LdrRegisterDllNotification)(
    ULONG Flags,
    PUMF_LDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    PVOID Context,
    PVOID* Cookie);

typedef NTSTATUS (NTAPI *PFN_LdrUnregisterDllNotification)(PVOID Cookie);

static PVOID g_notification_cookie = NULL;
static PFN_LdrUnregisterDllNotification g_unregister = NULL;

static VOID CALLBACK umf_ldr_notification(
    ULONG reason,
    const UMF_LDR_DLL_NOTIFICATION_DATA* data,
    PVOID context) {
    (void)context;
    if (!data) return;

    if (reason == UMF_LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
        /* Loader-lock context: no logging, no allocation beyond the
         * registry's own light bookkeeping. */
        umf_registry_on_module_unload((uintptr_t)data->DllBase,
                                      (uintptr_t)data->SizeOfImage);
    }
}

bool umf_dll_watchdog_start(void) {
    if (g_notification_cookie) return true;   /* already running */

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        UMF_ERROR("DLL watchdog: ntdll not found");
        return false;
    }

    PFN_LdrRegisterDllNotification reg =
        (PFN_LdrRegisterDllNotification)GetProcAddress(
            ntdll, "LdrRegisterDllNotification");
    g_unregister = (PFN_LdrUnregisterDllNotification)GetProcAddress(
            ntdll, "LdrUnregisterDllNotification");

    if (!reg || !g_unregister) {
        UMF_WARN("DLL watchdog unavailable (LdrRegisterDllNotification missing)");
        return false;
    }

    NTSTATUS st = reg(0, umf_ldr_notification, NULL, &g_notification_cookie);
    if (st != 0) {
        UMF_ERROR("LdrRegisterDllNotification failed: 0x%08lX", (unsigned long)st);
        g_notification_cookie = NULL;
        return false;
    }

    UMF_INFO("DLL watchdog active — module unloads will tear down stale hooks");
    return true;
}

void umf_dll_watchdog_stop(void) {
    if (g_notification_cookie && g_unregister) {
        g_unregister(g_notification_cookie);
    }
    g_notification_cookie = NULL;
}
