/*
 * umf_thread_freezer.c — Freeze/resume all threads in the process
 *
 * Used during hook installation and uninstallation to ensure
 * atomic code patching. The calling thread is excluded from freezing.
 *
 * WARNING: SuspendThread is inherently dangerous. See §1 of spec
 * for the rule: NO allocation, logging, or lock-taking while
 * threads are frozen. This file only provides the freeze/resume
 * mechanism — the caller is responsible for the freeze-window rules.
 */

#include "umf/umf.h"
#include <tlhelp32.h>

bool umf_freeze_all_threads(UmfThreadFreezer* freezer) {
    memset(freezer, 0, sizeof(*freezer));
    freezer->owner_thread_id = GetCurrentThreadId();

    DWORD pid = GetCurrentProcessId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    THREADENTRY32 te;
    te.dwSize = sizeof(te);

    if (!Thread32First(snap, &te)) {
        CloseHandle(snap);
        return false;
    }

    do {
        if (te.th32OwnerProcessID != pid) continue;
        if (te.th32ThreadID == freezer->owner_thread_id) continue;
        if (freezer->count >= UMF_MAX_THREADS) break;

        HANDLE hThread = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
            FALSE, te.th32ThreadID);
        if (!hThread) continue;

        DWORD result = SuspendThread(hThread);
        if (result == (DWORD)-1) {
            CloseHandle(hThread);
            continue;
        }

        freezer->thread_ids[freezer->count] = te.th32ThreadID;
        freezer->thread_handles[freezer->count] = hThread;
        freezer->count++;
    } while (Thread32Next(snap, &te));

    CloseHandle(snap);
    return true;
}

void umf_resume_all_threads(UmfThreadFreezer* freezer) {
    for (int i = 0; i < freezer->count; i++) {
        ResumeThread(freezer->thread_handles[i]);
        CloseHandle(freezer->thread_handles[i]);
        freezer->thread_handles[i] = NULL;
    }
    freezer->count = 0;
}

bool umf_any_thread_in_range(const UmfThreadFreezer* freezer,
                              uintptr_t range_start,
                              uintptr_t range_end) {
    for (int i = 0; i < freezer->count; i++) {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;

        if (!GetThreadContext(freezer->thread_handles[i], &ctx))
            continue;

        uintptr_t rip = (uintptr_t)ctx.Rip;
        if (rip >= range_start && rip < range_end)
            return true;
    }
    return false;
}
