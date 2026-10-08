/*
 * test_log.c — logging smoke tests + JSON formatting + size-based rotation.
 *
 * The JSON record formatter is pure, so it is asserted field-by-field. The
 * rotation + JSON-file paths run against a throwaway log in a temp directory;
 * the global logger is restored to its default (umf.log, WARN) at the end so
 * later suites are unaffected.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <string.h>

void run_log_tests(void) {
    /* ── Original smoke checks ── */
    umf_log_set_level(UMF_LOG_TRACE);
    UMF_TRACE("trace %d", 1);
    UMF_DEBUG("debug %d", 2);
    UMF_INFO("info %d", 3);
    UMF_WARN("warn %d", 4);
    UMF_ERROR("error %d", 5);
    CHECK(1, "all log levels emit without crashing");

    umf_log_set_level(UMF_LOG_ERROR);
    UMF_INFO("this info line is below threshold");
    CHECK(1, "level filter accepts below-threshold calls");

    /* ── JSON record formatting (deterministic) ── */
    char j[1024];
    int jn = umf_log_format_json(j, sizeof(j), UMF_LOG_WARN, "file.c", 42,
                                 1234, 12.5, "a\"b\tc");
    CHECK(jn > 0 && j[0] == '{', "json record is an object");
    CHECK(strstr(j, "\"level\":\"WARN\"") != NULL, "json level is trimmed");
    CHECK(strstr(j, "\"line\":42") != NULL, "json carries the line number");
    CHECK(strstr(j, "\"tid\":1234") != NULL, "json carries the thread id");
    CHECK(strstr(j, "\"t\":12.500") != NULL, "json carries the timestamp");
    CHECK(strstr(j, "\"file\":\"file.c\"") != NULL, "json carries the file");
    CHECK(strstr(j, "a\\\"b\\tc") != NULL, "json escapes quote and tab");

    /* ── Rotation + JSON file output in a temp directory ── */
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%sumf_log_test", tmp);
    wchar_t wdir[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, MAX_PATH);

    umf_log_init(wdir);                     /* reopen into the temp dir */
    umf_log_set_level(UMF_LOG_TRACE);
    umf_log_set_rotation(400, 3);
    for (int i = 0; i < 60; i++)
        UMF_INFO("rotation fill line %d padding-padding-padding", i);

    char base[MAX_PATH], b1[MAX_PATH], b2[MAX_PATH], b3[MAX_PATH];
    snprintf(base, sizeof(base), "%s\\umf.log", dir);
    snprintf(b1, sizeof(b1), "%s.1", base);
    snprintf(b2, sizeof(b2), "%s.2", base);
    snprintf(b3, sizeof(b3), "%s.3", base);
    CHECK(GetFileAttributesA(base) != INVALID_FILE_ATTRIBUTES, "active log exists");
    CHECK(GetFileAttributesA(b1) != INVALID_FILE_ATTRIBUTES, "rotated backup .1 exists");
    CHECK(GetFileAttributesA(b2) != INVALID_FILE_ATTRIBUTES, "rotated backup .2 exists");

    umf_log_set_json(true);
    UMF_WARN("json file line %d", 7);       /* WARN flushes to disk */

    bool found_json = false;
    HANDLE h = CreateFileA(base, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        char data[8192];
        DWORD rd = 0;
        if (ReadFile(h, data, sizeof(data) - 1, &rd, NULL)) {
            data[rd] = 0;
            if (strstr(data, "{\"t\":") != NULL) found_json = true;
        }
        CloseHandle(h);
    }
    CHECK(found_json, "json mode writes JSON lines to the file");

    /* ── Restore the default global logger + clean up ── */
    umf_log_set_json(false);
    umf_log_set_rotation(0, 0);
    umf_log_shutdown();
    DeleteFileA(base);
    DeleteFileA(b1);
    DeleteFileA(b2);
    DeleteFileA(b3);
    RemoveDirectoryA(dir);

    umf_log_init(NULL);
    umf_log_set_level(UMF_LOG_WARN);
}
