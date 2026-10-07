/*
 * umf_launcher.c — Elevated launcher front-end (§LAUNCH)
 *
 * A minimal WIN32 front-end that demonstrates the launcher-side helpers: it
 * reports elevation, enables SeDebugPrivilege so it can reach elevated
 * targets, and offers the Defender-exclusion flow. The AV step prints the
 * exact command and requires explicit confirmation (a MessageBox here) before
 * changing anything — it never silently edits security settings.
 *
 * The manifest requests elevation (requireAdministrator) via the build.
 */

#include "umf/umf.h"
#include <stdio.h>

static void show(const char* text, const char* caption, UINT flags) {
    MessageBoxA(NULL, text, caption, flags);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show_cmd) {
    (void)hInst; (void)hPrev; (void)cmd; (void)show_cmd;

    umf_log_init(NULL);

    /* 1. Elevation state */
    bool elevated = umf_is_elevated();
    char msg[1024];
    snprintf(msg, sizeof(msg),
             "UMF Launcher\n\nElevated: %s\n",
             elevated ? "yes" : "NO (some targets will be unreachable)");

    /* 2. SeDebugPrivilege (only meaningful when elevated) */
    if (elevated) {
        bool dbg = umf_enable_debug_privilege();
        size_t n = strlen(msg);
        snprintf(msg + n, sizeof(msg) - n,
                 "SeDebugPrivilege: %s\n", dbg ? "enabled" : "unavailable");
    }

    /* 3. AV exclusion — consented, never automatic */
    char cmdline[1200];
    umf_av_exclusion_command(NULL, cmdline, sizeof(cmdline));  /* template */
    size_t n = strlen(msg);
    snprintf(msg + n, sizeof(msg) - n,
             "\nAntivirus note:\nUMF injects code into processes, which "
             "Defender may flag. Adding an exclusion is optional and requires "
             "your explicit approval.");

    int choice = MessageBoxA(NULL, msg, "UMF Launcher",
                             MB_OKCANCEL | MB_ICONINFORMATION);
    if (choice == IDOK) {
        /* The user must confirm a second time before we touch Defender. */
        int confirm = MessageBoxA(NULL,
            "Add your UMF folder to Windows Defender exclusions?",
            "Confirm AV exclusion", MB_YESNO | MB_ICONWARNING);
        if (confirm == IDYES) {
            char exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, MAX_PATH);
            /* Use the containing folder. */
            char* slash = strrchr(exe, '\\');
            if (slash) *slash = '\0';
            bool done = umf_request_av_exclusion(exe, true);
            show(done ? "Exclusion added." : "Exclusion not added.",
                 "UMF Launcher", MB_OK | MB_ICONINFORMATION);
        }
    }

    umf_log_shutdown();
    return 0;
}
