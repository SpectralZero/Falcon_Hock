/*
 * umf_profiler.c — §PROFILER: per-hook call counts + timing
 *
 * A small, thread-safe sampling registry. It is deliberately independent of
 * the hook engine (which is preserved): a hook or mod interns a named slot and
 * either brackets its work with umf_prof_enter()/umf_prof_exit() or submits a
 * precomputed duration via umf_prof_record(). Each slot accumulates call count
 * and total/min/max nanoseconds; the mean is derived on request.
 *
 * Timing uses QueryPerformanceCounter converted to nanoseconds. A global
 * enable flag lets callers compile the instrumentation in but switch it off
 * with near-zero cost (enter/exit/record become no-ops).
 */

#include "umf/umf.h"

typedef struct {
    char     name[128];
    uint64_t calls;
    uint64_t total_ns;
    uint64_t min_ns;
    uint64_t max_ns;
    bool     used;
} UmfProfSlot;

static UmfProfSlot   g_slots[UMF_PROF_MAX_SLOTS];
static SRWLOCK       g_prof_lock = SRWLOCK_INIT;
static volatile LONG g_prof_enabled = 1;

static LARGE_INTEGER g_qpc_freq;
static bool          g_qpc_ready = false;

static void ensure_freq(void) {
    if (!g_qpc_ready) {
        QueryPerformanceFrequency(&g_qpc_freq);
        g_qpc_ready = true;
    }
}

void umf_prof_reset(void) {
    AcquireSRWLockExclusive(&g_prof_lock);
    memset(g_slots, 0, sizeof(g_slots));
    ReleaseSRWLockExclusive(&g_prof_lock);
}

void umf_prof_set_enabled(bool enabled) {
    InterlockedExchange(&g_prof_enabled, enabled ? 1 : 0);
}

bool umf_prof_is_enabled(void) {
    return InterlockedCompareExchange(&g_prof_enabled, 0, 0) != 0;
}

int umf_prof_slot(const char* name) {
    if (!name || !name[0]) return -1;

    AcquireSRWLockExclusive(&g_prof_lock);
    int found = -1;
    int free_idx = -1;
    for (int i = 0; i < UMF_PROF_MAX_SLOTS; i++) {
        if (g_slots[i].used) {
            if (strcmp(g_slots[i].name, name) == 0) { found = i; break; }
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (found < 0 && free_idx >= 0) {
        UmfProfSlot* s = &g_slots[free_idx];
        memset(s, 0, sizeof(*s));
        snprintf(s->name, sizeof(s->name), "%s", name);
        s->used = true;
        found = free_idx;
    }
    ReleaseSRWLockExclusive(&g_prof_lock);
    return found;
}

void umf_prof_record(int slot, uint64_t ns) {
    if (!umf_prof_is_enabled()) return;
    if (slot < 0 || slot >= UMF_PROF_MAX_SLOTS) return;

    AcquireSRWLockExclusive(&g_prof_lock);
    UmfProfSlot* s = &g_slots[slot];
    if (s->used) {
        if (s->calls == 0) {
            s->min_ns = ns;
            s->max_ns = ns;
        } else {
            if (ns < s->min_ns) s->min_ns = ns;
            if (ns > s->max_ns) s->max_ns = ns;
        }
        s->calls++;
        s->total_ns += ns;
    }
    ReleaseSRWLockExclusive(&g_prof_lock);
}

uint64_t umf_prof_enter(void) {
    ensure_freq();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)c.QuadPart;
}

void umf_prof_exit(int slot, uint64_t start_token) {
    if (!umf_prof_is_enabled()) return;
    ensure_freq();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);

    uint64_t now = (uint64_t)c.QuadPart;
    uint64_t delta = now > start_token ? now - start_token : 0;
    uint64_t ns = g_qpc_freq.QuadPart
                ? (uint64_t)((double)delta * 1.0e9 / (double)g_qpc_freq.QuadPart)
                : 0;
    umf_prof_record(slot, ns);
}

bool umf_prof_get(int slot, UmfProfStat* out) {
    if (!out || slot < 0 || slot >= UMF_PROF_MAX_SLOTS) return false;

    AcquireSRWLockShared(&g_prof_lock);
    bool ok = g_slots[slot].used;
    if (ok) {
        snprintf(out->name, sizeof(out->name), "%s", g_slots[slot].name);
        out->calls    = g_slots[slot].calls;
        out->total_ns = g_slots[slot].total_ns;
        out->min_ns   = g_slots[slot].min_ns;
        out->max_ns   = g_slots[slot].max_ns;
    }
    ReleaseSRWLockShared(&g_prof_lock);
    return ok;
}

int umf_prof_list(UmfProfStat* out, int max) {
    if (!out || max <= 0) return 0;

    AcquireSRWLockShared(&g_prof_lock);
    int n = 0;
    for (int i = 0; i < UMF_PROF_MAX_SLOTS && n < max; i++) {
        if (!g_slots[i].used) continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", g_slots[i].name);
        out[n].calls    = g_slots[i].calls;
        out[n].total_ns = g_slots[i].total_ns;
        out[n].min_ns   = g_slots[i].min_ns;
        out[n].max_ns   = g_slots[i].max_ns;
        n++;
    }
    ReleaseSRWLockShared(&g_prof_lock);
    return n;
}

uint64_t umf_prof_avg_ns(int slot) {
    UmfProfStat s;
    if (!umf_prof_get(slot, &s) || s.calls == 0) return 0;
    return s.total_ns / s.calls;
}
