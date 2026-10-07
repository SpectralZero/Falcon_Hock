/*
 * test_ipc.c — §IPC named-pipe JSON-RPC server test.
 *
 * Acts as the Studio client. A background reader thread drains the pipe into
 * a small queue so the server's writes never block (the real client model),
 * while the main thread sends requests and waits for matching responses.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <process.h>

static HANDLE   g_pipe = INVALID_HANDLE_VALUE;
static HANDLE   g_reader = NULL;
static volatile LONG g_reader_stop = 0;

/* A tiny broadcast: every line the reader sees is also posted here. */
#define QCAP 64
static SRWLOCK  g_q_lock = SRWLOCK_INIT;
static char     g_q[QCAP][8192];
static int      g_q_head = 0, g_q_tail = 0;
static HANDLE   g_q_sem = NULL;

static void q_push(const char* line) {
    AcquireSRWLockExclusive(&g_q_lock);
    int next = (g_q_tail + 1) % QCAP;
    if (next != g_q_head) {                 /* drop on overflow */
        strncpy(g_q[g_q_tail], line, 8191);
        g_q[g_q_tail][8191] = '\0';
        g_q_tail = next;
    }
    ReleaseSRWLockExclusive(&g_q_lock);
    ReleaseSemaphore(g_q_sem, 1, NULL);
}

/* Block until a line that contains `needle` arrives (skips others). */
static bool q_wait_for(const char* needle, char* out, size_t outlen, DWORD ms) {
    ULONGLONG deadline = GetTickCount64() + ms;
    while (GetTickCount64() < deadline) {
        ULONGLONG now = GetTickCount64();
        DWORD wait = (DWORD)(deadline - now);
        if (WaitForSingleObject(g_q_sem, wait) != WAIT_OBJECT_0) break;
        AcquireSRWLockExclusive(&g_q_lock);
        if (g_q_head != g_q_tail) {
            strncpy(out, g_q[g_q_head], outlen - 1);
            out[outlen - 1] = '\0';
            g_q_head = (g_q_head + 1) % QCAP;
        } else {
            out[0] = '\0';
        }
        ReleaseSRWLockExclusive(&g_q_lock);
        if (strstr(out, needle)) return true;
    }
    if (out && outlen) out[0] = '\0';
    return false;
}

static unsigned __stdcall reader_main(void* arg) {
    (void)arg;
    char line[8192];
    size_t used = 0;
    while (InterlockedCompareExchange(&g_reader_stop, 0, 0) == 0) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, NULL, 0, NULL, &avail, NULL)) break;
        if (avail == 0) { Sleep(2); continue; }

        char chunk[1024];
        DWORD toread = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
        DWORD got = 0;
        if (!ReadFile(g_pipe, chunk, toread, &got, NULL) || got == 0) break;

        for (DWORD k = 0; k < got; k++) {
            char c = chunk[k];
            if (c == '\n') {
                line[used] = '\0';
                if (used) q_push(line);
                used = 0;
            } else if (used + 1 < sizeof(line)) {
                line[used++] = c;
            }
        }
    }
    return 0;
}

static bool send_line(const char* s) {
    DWORD w = 0;
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s\n", s);
    return WriteFile(g_pipe, buf, (DWORD)strlen(buf), &w, NULL) != 0;
}

static HANDLE connect_client(const char* name) {
    for (int i = 0; i < 100; i++) {
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
        if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeA(name, 100);
        Sleep(20);
    }
    return INVALID_HANDLE_VALUE;
}

void run_ipc_tests(void) {
    if (!umf_init()) { CHECK(0, "umf_init for IPC"); return; }
    CHECK(umf_ipc_is_running(), "IPC server reports running");

    const char* name = umf_ipc_pipe_name();
    CHECK(name && strstr(name, "umf-studio-") != NULL, "pipe name has studio prefix");

    g_q_sem = CreateSemaphore(NULL, 0, QCAP * 4, NULL);
    g_pipe = connect_client(name);
    CHECK(g_pipe != INVALID_HANDLE_VALUE, "client connects to the pipe");

    char resp[8192];
    if (g_pipe != INVALID_HANDLE_VALUE) {
        g_reader = (HANDLE)_beginthreadex(NULL, 0, reader_main, NULL, 0, NULL);
        CHECK(g_reader != NULL, "reader thread started");
        Sleep(50);   /* let the reader enter ReadFile before the first write */

        send_line("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
        CHECK(q_wait_for("\"pong\":true", resp, sizeof(resp), 3000) &&
              strstr(resp, "\"id\":1"),
              "ping returns pong with echoed id");

        send_line("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"getProcessInfo\"}");
        CHECK(q_wait_for("\"arch\":\"x64\"", resp, sizeof(resp), 2000),
              "getProcessInfo returns pid + arch");

        send_line("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"getMitigations\"}");
        CHECK(q_wait_for("\"hvci\"", resp, sizeof(resp), 2000),
              "getMitigations returns mitigation flags");

        send_line("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"listHooks\"}");
        CHECK(q_wait_for("\"result\":[", resp, sizeof(resp), 2000),
              "listHooks returns an array");

        send_line("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"evalLua\","
                  "\"code\":\"local x = 1 + 1\"}");
        CHECK(q_wait_for("\"ok\":true", resp, sizeof(resp), 2000),
              "evalLua runs a chunk");

        send_line("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"subscribe\"}");
        CHECK(q_wait_for("\"subscribed\":true", resp, sizeof(resp), 2000),
              "subscribe acknowledged");

        UMF_ERROR("ipc-subscribe-test");
        CHECK(q_wait_for("ipc-subscribe-test", resp, sizeof(resp), 2000) &&
              strstr(resp, "\"method\":\"log\""),
              "subscriber receives streamed log notification");

        send_line("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"bogus\"}");
        CHECK(q_wait_for("-32601", resp, sizeof(resp), 2000),
              "unknown method returns -32601");

        InterlockedExchange(&g_reader_stop, 1);
        CancelSynchronousIo(g_reader);
        WaitForSingleObject(g_reader, 2000);
        CloseHandle(g_reader);
        CloseHandle(g_pipe);
    }

    if (g_q_sem) CloseHandle(g_q_sem);
    umf_ipc_stop();
    CHECK(!umf_ipc_is_running(), "IPC server stops cleanly");

    umf_shutdown();
}
