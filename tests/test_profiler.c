/*
 * test_profiler.c — §PROFILER: interning, accumulation, timing, enable gate.
 *
 * Exact arithmetic is checked via umf_prof_record (deterministic durations);
 * the QueryPerformanceCounter path is checked for call counting and a non-zero
 * measured duration using a Debug-safe busy loop.
 */
#include "umf/umf.h"
#include "test_framework.h"

void run_profiler_tests(void) {
    umf_prof_reset();
    CHECK(umf_prof_is_enabled(), "profiler enabled by default");

    /* ── Interning ── */
    int a = umf_prof_slot("hook.A");
    int b = umf_prof_slot("hook.B");
    CHECK(a >= 0 && b >= 0, "slots allocated");
    CHECK(a != b, "distinct names get distinct slots");
    CHECK(umf_prof_slot("hook.A") == a, "same name interns to the same slot");
    CHECK(umf_prof_slot(NULL) == -1, "NULL name is rejected");

    /* ── Exact accumulation via record ── */
    umf_prof_record(a, 100);
    umf_prof_record(a, 300);
    umf_prof_record(a, 200);
    UmfProfStat s;
    CHECK(umf_prof_get(a, &s), "get returns a used slot");
    CHECK(s.calls == 3, "three calls recorded");
    CHECK(s.total_ns == 600, "total_ns summed");
    CHECK(s.min_ns == 100, "min_ns tracked");
    CHECK(s.max_ns == 300, "max_ns tracked");
    CHECK(umf_prof_avg_ns(a) == 200, "avg_ns computed");
    CHECK(strcmp(s.name, "hook.A") == 0, "slot keeps its name");

    /* ── Timing path ── */
    int t = umf_prof_slot("hook.timed");
    uint64_t tok = umf_prof_enter();
    volatile uint64_t sink = 0;
    for (int i = 0; i < 300000; i++) sink += (uint64_t)i;
    umf_prof_exit(t, tok);
    (void)sink;
    CHECK(umf_prof_get(t, &s), "timed slot populated");
    CHECK(s.calls == 1, "enter/exit recorded one call");
    CHECK(s.total_ns > 0, "measured a non-zero duration");

    /* ── Listing ── */
    static UmfProfStat all[UMF_PROF_MAX_SLOTS];
    int n = umf_prof_list(all, UMF_PROF_MAX_SLOTS);
    CHECK(n == 3, "list reports the three used slots");

    /* ── Enable gate ── */
    umf_prof_set_enabled(false);
    CHECK(!umf_prof_is_enabled(), "profiler can be disabled");
    umf_prof_record(a, 9999);
    umf_prof_get(a, &s);
    CHECK(s.calls == 3, "record is a no-op while disabled");
    uint64_t tok2 = umf_prof_enter();
    umf_prof_exit(t, tok2);
    umf_prof_get(t, &s);
    CHECK(s.calls == 1, "exit is a no-op while disabled");
    umf_prof_set_enabled(true);

    /* ── Robustness ── */
    CHECK(!umf_prof_get(-1, &s), "get rejects a negative slot");
    CHECK(!umf_prof_get(UMF_PROF_MAX_SLOTS, &s), "get rejects an out-of-range slot");
    CHECK(umf_prof_avg_ns(-1) == 0, "avg of an invalid slot is zero");

    /* ── Reset clears everything ── */
    umf_prof_reset();
    CHECK(umf_prof_list(all, UMF_PROF_MAX_SLOTS) == 0, "reset clears all slots");
    CHECK(!umf_prof_get(a, &s), "slot is unused after reset");
}
