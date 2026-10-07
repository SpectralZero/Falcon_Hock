/*
 * test_scan.c — §SCAN value-scanner tests.
 *
 * Scans the whole process for a unique magic value placed in a large static
 * buffer, then narrows via next-scan as the value changes.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <stdint.h>

/* A large, uniquely-valued region to scan. Pad so the magic int is aligned. */
static uint8_t g_arena[64 * 1024];
static uint32_t* g_health = (uint32_t*)0;

void run_scan_tests(void) {
    umf_mem_set_owner(NULL);   /* trusted */

    /* Place a value unlikely to appear elsewhere in memory. */
    for (size_t i = 0; i < sizeof(g_arena); i += 4)
        ((uint32_t*)g_arena)[i / 4] = 0x5A5A0000u + (uint32_t)(i / 4);
    g_health = (uint32_t*)(g_arena + 0x1000);
    *g_health = 0x12345678u;

    /* ── First scan for the exact value ── */
    UmfScanSession* s = umf_scan_first(UMF_SCAN_I32, 0x12345678u);
    CHECK(s != NULL, "first scan begins");

    void* results[64];
    int n = umf_scan_results(s, results, 64);
    CHECK(n >= 1, "first scan finds at least one match");

    bool found = false;
    for (int i = 0; i < n; i++) if (results[i] == (void*)g_health) found = true;
    CHECK(found, "first scan found our health address");

    /* ── Narrow: change the value, scan for 'decreased' (from old value) ── */
    *g_health = 0x12345678u - 100;
    int kept = umf_scan_next(s, UMF_SCAN_DECREASED, 0x12345678u);
    CHECK(kept >= 1, "next scan keeps decreased value");

    n = umf_scan_results(s, results, 64);
    found = false;
    for (int i = 0; i < n; i++) if (results[i] == (void*)g_health) found = true;
    CHECK(found, "health address survives the decrease scan");

    /* ── Value-at: read it back through the session ── */
    int hit_index = -1;
    for (int i = 0; i < n; i++) if (results[i] == (void*)g_health) hit_index = i;
    double v = 0;
    CHECK(hit_index >= 0 && umf_scan_value_at(s, hit_index, &v),
          "scan_value_at reads the current value");
    CHECK((uint32_t)v == 0x12345678u - 100, "scanned value matches memory");

    /* ── Exact narrow to a single known value ── */
    int single = umf_scan_next(s, UMF_SCAN_EXACT, (double)(0x12345678u - 100));
    CHECK(single >= 1, "exact narrow keeps the current value");

    umf_scan_free(s);

    /* ── Float scan (sanity) ── */
    static volatile float g_float = 1234.5f;
    UmfScanSession* sf = umf_scan_first(UMF_SCAN_F32, 1234.5f);
    CHECK(sf != NULL, "float first scan begins");
    if (sf) {
        void* fr[256];
        int fn = umf_scan_results(sf, fr, 256);
        bool ff = false;
        for (int i = 0; i < fn; i++) if (fr[i] == (void*)&g_float) ff = true;
        CHECK(ff, "float scan finds the float address");
        umf_scan_free(sf);
    }
}
