/*
 * umf_platform.c — Windows version check + anti-cheat detection
 *
 * Uses RtlGetVersion (not GetVersionEx, which lies on Win8.1+).
 * Anti-cheat detection checks for known kernel-mode AC services.
 */

#include "umf/umf.h"
#include <stdio.h>

/* ── ntdll types (avoid full ntdll.h dependency) ── */
typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);

bool umf_get_windows_version(UmfWindowsVersion* out) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return false;

    PFN_RtlGetVersion pRtlGetVersion =
        (PFN_RtlGetVersion)GetProcAddress(ntdll, "RtlGetVersion");
    if (!pRtlGetVersion) return false;

    RTL_OSVERSIONINFOW osvi;
    memset(&osvi, 0, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);

    NTSTATUS status = pRtlGetVersion(&osvi);
    if (status != 0) return false;

    out->major = osvi.dwMajorVersion;
    out->minor = osvi.dwMinorVersion;
    out->build = osvi.dwBuildNumber;
    return true;
}

bool umf_check_minimum_version(void) {
    UmfWindowsVersion ver;
    if (!umf_get_windows_version(&ver)) {
        UMF_WARN("Cannot determine Windows version — proceeding anyway");
        return true;  /* Optimistic — let it try */
    }

    UMF_INFO("Windows %lu.%lu build %lu detected", ver.major, ver.minor, ver.build);

    if (ver.major < 10 || ver.build < 16299) {
        UMF_ERROR("Windows 10 1709 (build 16299) or later required. "
                  "Current: %lu.%lu.%lu", ver.major, ver.minor, ver.build);
        return false;
    }
    return true;
}

bool umf_check_anticheat(void) {
    /* Known anti-cheat kernel services */
    static const wchar_t* ac_services[] = {
        L"BEService",              /* BattlEye user-mode service       */
        L"BEDaisy",                /* BattlEye kernel driver           */
        L"EasyAntiCheat",          /* EAC service                      */
        L"EasyAntiCheatSys",       /* EAC kernel driver                */
        L"vgc",                    /* Riot Vanguard service             */
        L"vgk",                    /* Riot Vanguard kernel driver       */
        L"atc_helper",             /* Activision Ricochet helper        */
        NULL,
    };

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        /* Cannot open SCM — might be running with limited privileges.
         * This is OK for personal use; skip the check. */
        UMF_WARN("Cannot open Service Control Manager — skipping AC check");
        return false;
    }

    bool found = false;

    for (int i = 0; ac_services[i]; i++) {
        SC_HANDLE svc = OpenServiceW(scm, ac_services[i], SERVICE_QUERY_STATUS);
        if (!svc) continue;

        SERVICE_STATUS ss;
        if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState == SERVICE_RUNNING) {
            /* Convert wchar_t to char for logging */
            char name_a[128];
            WideCharToMultiByte(CP_UTF8, 0, ac_services[i], -1,
                                name_a, sizeof(name_a), NULL, NULL);
            UMF_ERROR("Anti-cheat service '%s' is RUNNING. "
                      "UMF refuses to inject into protected processes.", name_a);
            found = true;
        }
        CloseServiceHandle(svc);
        if (found) break;
    }

    CloseServiceHandle(scm);
    return found;
}
