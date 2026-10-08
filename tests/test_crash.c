/*
 * test_crash.c — §CRASH: safe-mode marker, minidump writer, filter lifecycle.
 *
 * Everything is tested without actually crashing: the unhandled filter is
 * installed/removed, the marker file round-trips, and a context-free minidump
 * is written and checked for the MDMP signature. All files live in a temp dir.
 */
#include "umf/umf.h"
#include "test_framework.h"

void run_crash_tests(void) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%sumf_crash_test", tmp);
    CreateDirectoryA(dir, NULL);
    umf_crash_set_dir(dir);

    char marker[MAX_PATH];
    snprintf(marker, sizeof(marker), "%s\\umf_safe_mode.flag", dir);

    /* ── Safe-mode marker round-trip ── */
    umf_safe_mode_confirm_clean();
    CHECK(!umf_safe_mode_active(), "safe mode starts clear");

    umf_safe_mode_arm();
    CHECK(umf_safe_mode_active(), "arm sets safe mode");
    CHECK(GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES,
          "arm writes the marker file");

    umf_safe_mode_confirm_clean();
    CHECK(!umf_safe_mode_active(), "confirm_clean clears safe mode");
    CHECK(GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES,
          "confirm_clean removes the marker file");

    /* ── Minidump (no exception context) ── */
    char dump[MAX_PATH];
    snprintf(dump, sizeof(dump), "%s\\umf_test.dmp", dir);
    DeleteFileA(dump);

    CHECK(umf_write_minidump(NULL, dump), "write_minidump succeeds");

    DWORD sz = 0;
    char  sig[4] = {0};
    HANDLE h = CreateFileA(dump, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE, "dump file was created");
    if (h != INVALID_HANDLE_VALUE) {
        DWORD rd = 0;
        sz = GetFileSize(h, NULL);
        ReadFile(h, sig, 4, &rd, NULL);
        CloseHandle(h);
    }
    CHECK(sz > 0, "dump file is non-empty");
    CHECK(sig[0] == 'M' && sig[1] == 'D' && sig[2] == 'M' && sig[3] == 'P',
          "dump carries the MDMP signature");

    CHECK(!umf_write_minidump(NULL, NULL), "write_minidump rejects a NULL path");

    /* ── Filter lifecycle (install without triggering a real crash) ── */
    CHECK(umf_crash_handler_install(), "crash handler installs");
    CHECK(umf_crash_handler_install(), "install is idempotent");
    umf_crash_handler_uninstall();
    CHECK(!umf_safe_mode_active(), "no crash fired, safe mode stayed clear");

    umf_crash_set_dir(NULL);     /* must tolerate NULL (defaults to ".") */
    CHECK(1, "set_dir tolerates NULL");

    /* ── Cleanup ── */
    umf_crash_set_dir(dir);
    umf_safe_mode_confirm_clean();
    DeleteFileA(dump);
    RemoveDirectoryA(dir);
}
