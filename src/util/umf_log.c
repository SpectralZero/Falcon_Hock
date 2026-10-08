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

/* Rotation + format (defaults preserve the original behaviour) */
static wchar_t  g_log_path[MAX_PATH] = L"umf.log";
static size_t   g_bytes_written = 0;
static size_t   g_max_bytes     = 0;    /* 0 = rotation disabled */
static int      g_max_backups   = 0;
static bool     g_json          = false;

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

/* Trimmed level name (no padding) for JSON. */
static const char* level_str_trim(UmfLogLevel level) {
    switch (level) {
        case UMF_LOG_TRACE: return "TRACE";
        case UMF_LOG_DEBUG: return "DEBUG";
        case UMF_LOG_INFO:  return "INFO";
        case UMF_LOG_WARN:  return "WARN";
        case UMF_LOG_ERROR: return "ERROR";
        case UMF_LOG_FATAL: return "FATAL";
        default:            return "UNKNOWN";
    }
}

/* Minimal JSON string escaping (", \, and control chars). */
static void json_escape(const char* in, char* out, size_t outlen) {
    size_t o = 0;
    if (outlen == 0) return;
    for (const char* p = in ? in : ""; *p && o + 2 < outlen; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '\"': out[o++] = '\\'; out[o++] = '\"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    if (o + 6 < outlen) {
                        static const char hex[] = "0123456789abcdef";
                        out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0';
                        out[o++] = '0';  out[o++] = hex[(c >> 4) & 0xF];
                        out[o++] = hex[c & 0xF];
                    }
                } else {
                    out[o++] = (char)c;
                }
        }
    }
    out[o < outlen ? o : outlen - 1] = 0;
}

int umf_log_format_json(char* out, size_t outlen,
                        int level, const char* file, int line,
                        unsigned long tid, double elapsed_ms,
                        const char* msg) {
    char efile[256], emsg[2048];
    json_escape(file, efile, sizeof(efile));
    json_escape(msg,  emsg, sizeof(emsg));
    return snprintf(out, outlen,
        "{\"t\":%.3f,\"level\":\"%s\",\"file\":\"%s\",\"line\":%d,"
        "\"tid\":%lu,\"msg\":\"%s\"}",
        elapsed_ms, level_str_trim((UmfLogLevel)level), efile, line, tid, emsg);
}

void umf_log_set_rotation(size_t max_bytes, int max_backups) {
    AcquireSRWLockExclusive(&g_log_lock);
    g_max_bytes   = max_bytes;
    g_max_backups = max_backups < 0 ? 0 : max_backups;
    ReleaseSRWLockExclusive(&g_log_lock);
}

void umf_log_set_json(bool enabled) {
    InterlockedExchange((volatile LONG*)&g_json, (LONG)enabled);
}

/* Caller must hold g_log_lock. Shift backups and reopen a fresh base log. */
static void rotate_locked(void) {
    if (g_log_file != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(g_log_file);
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }

    wchar_t from[MAX_PATH + 16], to[MAX_PATH + 16];
    _snwprintf_s(to, _countof(to), _TRUNCATE, L"%s.%d", g_log_path, g_max_backups);
    DeleteFileW(to);
    for (int k = g_max_backups - 1; k >= 1; k--) {
        _snwprintf_s(from, _countof(from), _TRUNCATE, L"%s.%d", g_log_path, k);
        _snwprintf_s(to,   _countof(to),   _TRUNCATE, L"%s.%d", g_log_path, k + 1);
        MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING);
    }
    if (g_max_backups >= 1) {
        _snwprintf_s(to, _countof(to), _TRUNCATE, L"%s.1", g_log_path);
        MoveFileExW(g_log_path, to, MOVEFILE_REPLACE_EXISTING);
    } else {
        DeleteFileW(g_log_path);
    }

    g_log_file = CreateFileW(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    g_bytes_written = 0;
    if (g_log_file != INVALID_HANDLE_VALUE && !g_json) {
        DWORD written;
        const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
        WriteFile(g_log_file, bom, 3, &written, NULL);
        g_bytes_written = 3;
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
    _snwprintf_s(g_log_path, _countof(g_log_path), _TRUNCATE, L"%s", path);

    g_log_file = CreateFileW(
        path,
        GENERIC_WRITE,
        FILE_SHARE_READ,           /* Allow reading while we write */
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    g_bytes_written = 0;
    if (g_log_file != INVALID_HANDLE_VALUE) {
        /* Write UTF-8 BOM for log viewers */
        DWORD written;
        const uint8_t bom[] = {0xEF, 0xBB, 0xBF};
        WriteFile(g_log_file, bom, 3, &written, NULL);
        g_bytes_written = 3;
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

    /* Build final line (JSON Lines or human-readable text) */
    char line_buf[4096];
    int len;
    if (g_json) {
        len = umf_log_format_json(line_buf, sizeof(line_buf) - 2, (int)level,
                                  basename, line, GetCurrentThreadId(),
                                  elapsed_ms, msg);
        if (len < 0) len = 0;
        if (len > (int)sizeof(line_buf) - 2) len = (int)sizeof(line_buf) - 2;
        line_buf[len++] = '\n';
        line_buf[len]   = 0;
    } else {
        len = snprintf(line_buf, sizeof(line_buf),
                       "[%10.3f] [%s] [%s:%d] [TID:%lu] %s\r\n",
                       elapsed_ms, level_str(level),
                       basename, line,
                       GetCurrentThreadId(), msg);
        if (len < 0) len = 0;
        if (len >= (int)sizeof(line_buf)) len = (int)sizeof(line_buf) - 1;
    }

    /* Output to debugger (always) */
    OutputDebugStringA(line_buf);

    /* Output to file (if open) */
    AcquireSRWLockExclusive(&g_log_lock);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        if (g_max_bytes > 0 && g_bytes_written + (size_t)len > g_max_bytes)
            rotate_locked();
        if (g_log_file != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(g_log_file, line_buf, (DWORD)len, &written, NULL);
            g_bytes_written += (size_t)len;
            /* Flush on WARN+ for crash safety */
            if (level >= UMF_LOG_WARN)
                FlushFileBuffers(g_log_file);
        }
    }
    ReleaseSRWLockExclusive(&g_log_lock);

    /* Stream to IPC subscribers (Studio) if any. Done OUTSIDE the log lock so
     * a blocking pipe write can never deadlock against file logging. */
    umf_ipc_on_log((int)level, msg);

    /* Fatal = abort */
    if (level == UMF_LOG_FATAL) {
        if (IsDebuggerPresent()) __debugbreak();
        ExitProcess(1);
    }
}
