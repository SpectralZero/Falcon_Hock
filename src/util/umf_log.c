/*
 * umf_log.c — Thread-safe file + debug-output logger
 *
 * Design decisions:
 * - Uses SRWLOCK (not CRITICAL_SECTION) for minimal overhead on reads
 * - Writes to both a log file and OutputDebugStringA (visible in debugger)
 * - Timestamps use QueryPerformanceCounter for sub-ms precision
 * - Log function is safe to call from any thread at any time
 * - MUST NOT be called inside the freeze window (allocates via vsnprintf)
 */

#include "umf/umf.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/* ── State ── */
static HANDLE   g_log_file   = INVALID_HANDLE_VALUE;
static SRWLOCK  g_log_lock   = SRWLOCK_INIT;
static UmfLogLevel g_log_level = UMF_LOG_INFO;

static LARGE_INTEGER g_log_start_time;
static LARGE_INTEGER g_log_freq;

static const char* level_str(UmfLogLevel level) {
    switch (level) {
        case UMF_LOG_TRACE: return "TRACE";
        case UMF_LOG_DEBUG: return "DEBUG";
        case UMF_LOG_INFO:  return "INFO ";
        case UMF_LOG_WARN:  return "WARN ";
        case UMF_LOG_ERROR: return "ERROR";
        case UMF_LOG_FATAL: return "FATAL";
        default:            return "?????";
    }
}

void umf_log_init(const wchar_t* log_dir) {
    QueryPerformanceFrequency(&g_log_freq);
    QueryPerformanceCounter(&g_log_start_time);

    /* Build log file path: <log_dir>\umf.log */
    wchar_t path[MAX_PATH];
    if (log_dir) {
        CreateDirectoryW(log_dir, NULL);
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\umf.log", log_dir);
    } else {
        _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"umf.log");
    }

    g_log_file = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,           /* Allow reading while we write */
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (g_log_file != INVALID_HANDLE_VALUE) {
        /* Write UTF-8 BOM for log viewers */
        DWORD written;
        const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
        WriteFile(g_log_file, bom, 3, &written, NULL);
    }

    UMF_INFO("UMF v%s — Logger initialized", UMF_VERSION_STRING);
}

void umf_log_shutdown(void) {
    UMF_INFO("Logger shutting down");
    if (g_log_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log_file);
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
}

void umf_log_set_level(UmfLogLevel level) {
    InterlockedExchange((volatile LONG*)&g_log_level, (LONG)level);
}

void umf_log_write(UmfLogLevel level, const char* file, int line,
                    const char* fmt, ...) {
    if ((int)level < (int)g_log_level) return;

    /* Elapsed time since init (ms) */
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed_ms = (double)(now.QuadPart - g_log_start_time.QuadPart)
                        / (double)g_log_freq.QuadPart * 1000.0;

    /* Format user message */
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    /* Extract filename from full path */
    const char* basename = file;
    for (const char* p = file; *p; p++) {
        if (*p == '\\' || *p == '/') basename = p + 1;
    }

    /* Build final line */
    char line_buf[4096];
    int len = snprintf(line_buf, sizeof(line_buf),
                       "[%10.3f] [%s] [%s:%d] [TID:%lu] %s\r\n",
                       elapsed_ms, level_str(level),
                       basename, line,
                       GetCurrentThreadId(), msg);
    if (len < 0) len = 0;
    if (len >= (int)sizeof(line_buf)) len = (int)sizeof(line_buf) - 1;

    /* Output to debugger (always) */
    OutputDebugStringA(line_buf);

    /* Output to file (if open) */
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_log_file, line_buf, (DWORD)len, &written, NULL);
        /* Flush on WARN+ for crash safety */
        if (level >= UMF_LOG_WARN)
            FlushFileBuffers(g_log_file);
    }
    ReleaseSRWLockExclusive(&g_log_lock);

    /* Fatal = abort */
    if (level == UMF_LOG_FATAL) {
        if (IsDebuggerPresent()) __debugbreak();
        ExitProcess(1);
    }
}
