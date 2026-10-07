/*
 * umf_launch.c — §LAUNCH: launcher privilege + AV helpers
 *
 * The runtime is in-process and needs no admin. These helpers serve the
 * launcher/injector side. The AV-exclusion helper is deliberately gated on an
 * explicit `user_confirmed` flag and never runs a command unless asked — a
 * modding tool must not silently change the user's security settings.
 */

#include "umf/umf.h"
#include <stdio.h>

bool umf_is_elevated(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;

    TOKEN_ELEVATION elevation;
    DWORD size = sizeof(elevation);
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation),
                            &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
}

bool umf_enable_debug_privilege(void) {
    HANDLE token = NULL;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        UMF_WARN("SeDebugPrivilege: cannot open process token (%lu)",
                 GetLastError());
        return false;
    }

    LUID luid;
    if (!LookupPrivilegeValueA(NULL, SE_DEBUG_NAME, &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    bool ok = false;
    if (AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        /* AdjustTokenPrivileges returns TRUE even when nothing changed; the
         * real result is GetLastError() == ERROR_SUCCESS. */
        ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(token);

    if (ok) UMF_INFO("SeDebugPrivilege enabled");
    else    UMF_WARN("SeDebugPrivilege not available (not elevated?)");
    return ok;
}

void umf_av_exclusion_command(const char* path, char* out, size_t outlen) {
    if (!out || outlen == 0) return;
    out[0] = '\0';
    if (!path) return;
    snprintf(out, outlen,
             "powershell -Command \"Add-MpPreference -ExclusionPath '%s'\"",
             path);
}

bool umf_request_av_exclusion(const char* path, bool user_confirmed) {
    if (!path || !user_confirmed) {
        UMF_WARN("AV exclusion not applied (explicit user confirmation required)");
        return false;
    }
    if (strstr(path, "'") || strstr(path, "\"")) {
        UMF_ERROR("AV exclusion: refusing path with quote characters");
        return false;
    }

    char cmd[1200];
    umf_av_exclusion_command(path, cmd, sizeof(cmd));
    UMF_INFO("Applying AV exclusion: %s", cmd);

    int rc = system(cmd);
    if (rc == 0) { UMF_INFO("Defender exclusion added for %s", path); return true; }
    UMF_WARN("Defender exclusion command returned %d", rc);
    return false;
}
