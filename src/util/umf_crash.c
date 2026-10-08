/*
 * umf_crash.c — §CRASH: unhandled-exception filter, minidump, safe mode
 *
 * A fault inside an injected framework is indistinguishable from a host crash,
 * so UMF installs its own last-chance filter. On an unhandled exception it
 * writes a minidump and *arms safe mode*: a marker file that the next startup
 * can read (umf_safe_mode_active) to come up without hooks/mods/overlay and
 * break a crash loop. A clean shutdown calls umf_safe_mode_confirm_clean().
 *
 * The dump itself is MiniDumpWriteDump (dbghelp). All paths live under a
 * configurable directory so a sandboxed/elevated target can point them at a
 * writable location.
 */

#include "umf/umf.h"
#include <dbghelp.h>

static char g_crash_dir[MAX_PATH] = ".";
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = NULL;
static bool g_installed = false;

void umf_crash_set_dir(const char* dir) {
    if (dir && dir[0])
        snprintf(g_crash_dir, sizeof(g_crash_dir), "%s", dir);
    else
        snprintf(g_crash_dir, sizeof(g_crash_dir), ".");
}

static void marker_path(char* out, size_t outlen) {
    snprintf(out, outlen, "%s\\umf_safe_mode.flag", g_crash_dir);
}

/* ────────────────────────────────────────────────────────────────
 * Safe-mode marker
 * ──────────────────────────────────────────────────────────────── */

void umf_safe_mode_arm(void) {
    char path[MAX_PATH];
    marker_path(path, sizeof(path));
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(h, "1", 1, &wrote, NULL);
        CloseHandle(h);
    }
}

void umf_safe_mode_confirm_clean(void) {
    char path[MAX_PATH];
    marker_path(path, sizeof(path));
    DeleteFileA(path);
}

bool umf_safe_mode_active(void) {
    char path[MAX_PATH];
    marker_path(path, sizeof(path));
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/* ────────────────────────────────────────────────────────────────
 * Minidump
 * ──────────────────────────────────────────────────────────────── */

bool umf_write_minidump(void* exception_pointers, const char* path) {
    if (!path || !path[0]) return false;

    HANDLE hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_EXCEPTION_INFORMATION* pmei = NULL;
    if (exception_pointers) {
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = (PEXCEPTION_POINTERS)exception_pointers;
        mei.ClientPointers    = FALSE;
        pmei = &mei;
    }

    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                hFile, MiniDumpNormal, pmei, NULL, NULL);
    CloseHandle(hFile);
    return ok != FALSE;
}

/* ────────────────────────────────────────────────────────────────
 * Unhandled-exception filter
 * ──────────────────────────────────────────────────────────────── */

static LONG WINAPI umf_unhandled_filter(EXCEPTION_POINTERS* ep) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\umf-crash-%lu-%lu.dmp",
             g_crash_dir, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());

    umf_write_minidump(ep, path);
    umf_safe_mode_arm();
    UMF_FATAL("Unhandled exception 0x%08lX — dump written, safe mode armed",
              ep && ep->ExceptionRecord
                  ? (unsigned long)ep->ExceptionRecord->ExceptionCode : 0);

    /* Let the previously-registered filter (or the OS) finish the job. */
    if (g_prev_filter) return g_prev_filter(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

bool umf_crash_handler_install(void) {
    if (g_installed) return true;
    g_prev_filter = SetUnhandledExceptionFilter(umf_unhandled_filter);
    g_installed = true;
    return true;
}

void umf_crash_handler_uninstall(void) {
    if (!g_installed) return;
    SetUnhandledExceptionFilter(g_prev_filter);
    g_prev_filter = NULL;
    g_installed = false;
}
