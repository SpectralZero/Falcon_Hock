/*
 * umf_ipc.c — §IPC: JSON-RPC 2.0 server over a named pipe
 *
 * The runtime is the server; UMF Studio (or any script) is the client. Each
 * message is one JSON object on a line. Supported methods:
 *     ping, getProcessInfo, getMitigations, listHooks, listMods, evalLua,
 *     subscribe (streams log notifications)
 *
 * A single background thread owns the pipe: it creates one instance, waits
 * for a client, reads newline-delimited requests, dispatches, and writes
 * newline-delimited responses. On disconnect it re-listens, so the client can
 * reconnect freely. All pipe writes are serialized by a lock so log
 * notifications and responses never interleave mid-line.
 */

#include "umf/umf.h"
#include <stdio.h>

#define UMF_IPC_BUF      8192
#define UMF_IPC_MAX_RESP 65536
#define UMF_IPC_PIPE_PREFIX L"\\\\.\\pipe\\umf-studio-"

static HANDLE    g_pipe      = INVALID_HANDLE_VALUE;
static HANDLE    g_thread    = NULL;
static DWORD     g_thread_id = 0;
static volatile LONG g_running = 0;
static SRWLOCK   g_write_lock = SRWLOCK_INIT;
static SRWLOCK   g_sub_lock   = SRWLOCK_INIT;
static bool      g_subscribed = false;
static wchar_t   g_pipe_name[128];

extern UmfTrampolinePool   g_trampoline_pool;
extern UmfMitigationStatus g_mitigations;

/* ── tiny JSON helpers ── */

/* Find "key" and parse its string value into out. Returns true if found. */
static bool json_get_string(const char* json, const char* key,
                            char* out, size_t outlen) {
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return false;
    p = strchr(p + strlen(pat), ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outlen) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p++;
            switch (c) {
                case 'n': out[i++] = '\n'; break;
                case 't': out[i++] = '\t'; break;
                case 'r': out[i++] = '\r'; break;
                default:  out[i++] = c;    break;
            }
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return true;
}

/* Extract the raw "id" token (number or string); default "null". */
static void json_get_id(const char* json, char* out, size_t outlen) {
    strncpy(out, "null", outlen - 1);
    out[outlen - 1] = '\0';
    const char* p = strstr(json, "\"id\"");
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    size_t i = 0;
    if (*p == '"') {                       /* string id */
        out[i++] = *p++;
        while (*p && *p != '"' && i + 1 < outlen) out[i++] = *p++;
        if (*p == '"') out[i++] = *p;
    } else {                               /* numeric/null id */
        while (*p && *p != ',' && *p != '}' && i + 1 < outlen) out[i++] = *p++;
    }
    out[i] = '\0';
}

static void json_escape(const char* in, char* out, size_t outlen) {
    size_t j = 0;
    for (const char* p = in; *p && j + 2 < outlen; p++) {
        char c = *p;
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else out[j++] = c;
    }
    out[j] = '\0';
}

/* ── serialized pipe write ── */
static bool pipe_write(const char* s) {
    AcquireSRWLockExclusive(&g_write_lock);
    bool ok = false;
    if (g_pipe != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = WriteFile(g_pipe, s, (DWORD)strlen(s), &written, NULL) != 0;
    }
    ReleaseSRWLockExclusive(&g_write_lock);
    return ok;
}

/* ── method dispatch ── */
static void send_result(const char* id, const char* result_json) {
    char buf[UMF_IPC_MAX_RESP];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":%s}\n", id, result_json);
    pipe_write(buf);
}

static void send_error(const char* id, int code, const char* msg) {
    char buf[1024];
    char esc[512];
    json_escape(msg, esc, sizeof(esc));
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,"
             "\"message\":\"%s\"}}\n", id, code, esc);
    pipe_write(buf);
}

static void handle_request(const char* line) {
    char id[64];
    json_get_id(line, id, sizeof(id));

    char method[64];
    if (!json_get_string(line, "method", method, sizeof(method))) {
        send_error(id, -32700, "parse error: no method");
        return;
    }

    if (strcmp(method, "ping") == 0) {
        send_result(id, "{\"pong\":true}");

    } else if (strcmp(method, "getProcessInfo") == 0) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char esc[MAX_PATH * 2];
        json_escape(path, esc, sizeof(esc));
        char res[MAX_PATH * 2 + 64];
        snprintf(res, sizeof(res),
                 "{\"pid\":%lu,\"path\":\"%s\",\"arch\":\"x64\"}",
                 GetCurrentProcessId(), esc);
        send_result(id, res);

    } else if (strcmp(method, "getMitigations") == 0) {
        UmfMitigationStatus m;
        umf_detect_all_mitigations(&m);
        char res[256];
        snprintf(res, sizeof(res),
                 "{\"acg\":%s,\"hvci\":%s,\"cfg\":%s,\"cet_ss\":%s,"
                 "\"cet_ibt\":%s}",
                 m.acg_enforced ? "true" : "false",
                 m.hvci_active ? "true" : "false",
                 m.cfg_enforced ? "true" : "false",
                 m.cet_shadow_stack ? "true" : "false",
                 m.cet_ibt ? "true" : "false");
        send_result(id, res);

    } else if (strcmp(method, "listHooks") == 0) {
        static UmfHookInfo hooks[256];
        int n = umf_hook_list(hooks, 256);
        char* res = (char*)malloc(UMF_IPC_MAX_RESP);
        if (!res) { send_error(id, -32603, "oom"); return; }
        size_t off = 0;
        off += snprintf(res + off, UMF_IPC_MAX_RESP - off, "[");
        for (int i = 0; i < n && off < UMF_IPC_MAX_RESP - 256; i++) {
            char en[UMF_MAX_NAME_LEN * 2];
            json_escape(hooks[i].name, en, sizeof(en));
            off += snprintf(res + off, UMF_IPC_MAX_RESP - off,
                "%s{\"name\":\"%s\",\"address\":\"%p\",\"strategy\":%d,"
                "\"chain\":%d,\"installed\":%s}",
                i ? "," : "", en, hooks[i].address, hooks[i].strategy,
                hooks[i].chain_len, hooks[i].installed ? "true" : "false");
        }
        snprintf(res + off, UMF_IPC_MAX_RESP - off, "]");
        send_result(id, res);
        free(res);

    } else if (strcmp(method, "listMods") == 0) {
        static UmfModInfo mods[128];
        int n = umf_mod_list(mods, 128);
        char* res = (char*)malloc(UMF_IPC_MAX_RESP);
        if (!res) { send_error(id, -32603, "oom"); return; }
        size_t off = 0;
        off += snprintf(res + off, UMF_IPC_MAX_RESP - off, "[");
        for (int i = 0; i < n && off < UMF_IPC_MAX_RESP - 256; i++) {
            char en[256], ev[80];
            json_escape(mods[i].name, en, sizeof(en));
            json_escape(mods[i].version, ev, sizeof(ev));
            off += snprintf(res + off, UMF_IPC_MAX_RESP - off,
                "%s{\"name\":\"%s\",\"version\":\"%s\",\"type\":%d,"
                "\"caps\":%u,\"priority\":%d,\"active\":%s,"
                "\"category\":\"%s\",\"audience\":\"%s\"}",
                i ? "," : "", en, ev, mods[i].type, mods[i].capabilities,
                mods[i].priority, mods[i].active ? "true" : "false",
                umf_mod_category_name((UmfModCategory)mods[i].category),
                umf_mod_audience_name((UmfModAudience)mods[i].audience));
        }
        snprintf(res + off, UMF_IPC_MAX_RESP - off, "]");
        send_result(id, res);
        free(res);

    } else if (strcmp(method, "evalLua") == 0) {
        char code[4096];
        if (!json_get_string(line, "code", code, sizeof(code))) {
            send_error(id, -32602, "missing 'code'");
            return;
        }
        struct lua_State* L = umf_lua_create_sandbox();
        if (!L) { send_error(id, -32603, "cannot create sandbox"); return; }
        umf_lua_setup_api(L, NULL);
        bool ok = umf_lua_run_string(L, code);
        umf_lua_destroy_sandbox(L);
        send_result(id, ok ? "{\"ok\":true}" : "{\"ok\":false}");

    } else if (strcmp(method, "subscribe") == 0) {
        AcquireSRWLockExclusive(&g_sub_lock);
        g_subscribed = true;
        ReleaseSRWLockExclusive(&g_sub_lock);
        send_result(id, "{\"subscribed\":true}");

    } else {
        send_error(id, -32601, "method not found");
    }
}

/* ── pipe server thread ── */
static DWORD WINAPI ipc_thread(LPVOID param) {
    (void)param;

    while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
        g_pipe = CreateNamedPipeW(
            g_pipe_name,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, UMF_IPC_BUF, UMF_IPC_BUF, 0, NULL);
        if (g_pipe == INVALID_HANDLE_VALUE) break;

        BOOL connected = ConnectNamedPipe(g_pipe, NULL)
                       || GetLastError() == ERROR_PIPE_CONNECTED;

        if (connected && InterlockedCompareExchange(&g_running, 1, 1) == 1) {
            /* Non-blocking: poll for available bytes and read only what is
             * there, so the server never blocks mid-request and stops promptly
             * when g_running clears. */
            char line[UMF_IPC_BUF];
            size_t used = 0;

            while (InterlockedCompareExchange(&g_running, 1, 1) == 1) {
                DWORD avail = 0;
                if (!PeekNamedPipe(g_pipe, NULL, 0, NULL, &avail, NULL))
                    break;                      /* client disconnected */
                if (avail == 0) { Sleep(2); continue; }

                char chunk[1024];
                DWORD toread = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
                DWORD got = 0;
                if (!ReadFile(g_pipe, chunk, toread, &got, NULL) || got == 0)
                    break;

                for (DWORD k = 0; k < got; k++) {
                    char c = chunk[k];
                    if (c == '\n') {
                        line[used] = '\0';
                        if (used > 0) handle_request(line);
                        used = 0;
                    } else if (used + 1 < sizeof(line)) {
                        line[used++] = c;
                    }
                }
            }
        }

        FlushFileBuffers(g_pipe);
        DisconnectNamedPipe(g_pipe);
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;

        AcquireSRWLockExclusive(&g_sub_lock);
        g_subscribed = false;
        ReleaseSRWLockExclusive(&g_sub_lock);
    }
    return 0;
}

/* ── public API ── */

bool umf_ipc_start(void) {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return true; /* already */

    swprintf(g_pipe_name, 128, L"%s%lu", UMF_IPC_PIPE_PREFIX, GetCurrentProcessId());

    g_thread = CreateThread(NULL, 0, ipc_thread, NULL, 0, &g_thread_id);
    if (!g_thread) {
        InterlockedExchange(&g_running, 0);
        UMF_ERROR("IPC: CreateThread failed: %lu", GetLastError());
        return false;
    }

    UMF_INFO("IPC server listening on %ls", g_pipe_name);
    return true;
}

void umf_ipc_stop(void) {
    if (InterlockedCompareExchange(&g_running, 0, 1) != 1) return;

    /* Unblock ConnectNamedPipe (if waiting) with a throwaway client, and abort
     * any in-flight ReadFile so the thread notices the stop flag. */
    HANDLE h = CreateFileW(g_pipe_name, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (g_thread_id) CancelSynchronousIo(g_thread);

    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    g_thread_id = 0;
    UMF_INFO("IPC server stopped");
}

bool umf_ipc_is_running(void) {
    return InterlockedCompareExchange(&g_running, 1, 1) == 1;
}

const char* umf_ipc_pipe_name(void) {
    static char name[160];
    WideCharToMultiByte(CP_UTF8, 0, g_pipe_name, -1, name, sizeof(name), NULL, NULL);
    return name;
}

/* Called from the logging path to stream a log line to subscribers. */
void umf_ipc_on_log(int level, const char* msg) {
    if (!umf_ipc_is_running()) return;
    if (InterlockedCompareExchange(&g_running, 1, 1) != 1) return;

    bool sub;
    AcquireSRWLockShared(&g_sub_lock);
    sub = g_subscribed;
    ReleaseSRWLockShared(&g_sub_lock);
    if (!sub) return;

    char esc[2048];
    json_escape(msg, esc, sizeof(esc));
    char line[2200];
    snprintf(line, sizeof(line),
             "{\"jsonrpc\":\"2.0\",\"method\":\"log\",\"params\":"
             "{\"level\":%d,\"msg\":\"%s\"}}\n", level, esc);
    pipe_write(line);
}
