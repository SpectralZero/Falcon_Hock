/*
 * test_aob.c — §AOB pattern-scan tests.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <stdint.h>

/* A distinctive marker in the module's data section for module-wide scans. */
static const uint8_t kMarker[8] = {
    0xDE, 0xAD, 0xC0, 0xDE, 0xFE, 0xED, 0xFA, 0xCE
};

void run_aob_tests(void) {
    /* Keep the marker from being optimized away. */
    volatile uint8_t sink = kMarker[0] ^ kMarker[7];
    (void)sink;

    /* ── Range scans ── */
    static const uint8_t blob[16] = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0xAA, 0xBB, 0xCC,
        0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0x00
    };

    void* exact = umf_aob_scan_range("33 44 55", blob, sizeof(blob));
    CHECK(exact == (void*)(blob + 2), "exact match at first occurrence");

    void* mid = umf_aob_scan_range("33 ?? 55", blob, sizeof(blob));
    CHECK(mid == (void*)(blob + 2), "full-byte wildcard matches");

    void* nib = umf_aob_scan_range("4? 5?", blob, sizeof(blob));
    CHECK(nib == (void*)(blob + 3), "nibble wildcards match");

    /* Single-nibble token '9' = low nibble 0x09: first byte with low nibble
     * 9 is 0x99 at blob+14. */
    void* low = umf_aob_scan_range("9", blob, sizeof(blob));
    CHECK(low == (void*)(blob + 14), "single-nibble token matches low nibble");

    void* none = umf_aob_scan_range("99 88 77", blob, sizeof(blob));
    CHECK(none == NULL, "no-match returns NULL");

    void* at_end = umf_aob_scan_range("99 00", blob, sizeof(blob));
    CHECK(at_end == (void*)(blob + 14), "match at region end");

    /* 0x55 appears at blob+4 with following 0xAA; ensure ordering. */
    void* second = umf_aob_scan_range("33 44 55 66", blob, sizeof(blob));
    CHECK(second == (void*)(blob + 8), "match skips earlier partial and finds later");

    /* ── Module scans ── */
    void* found = umf_aob_scan("DE AD C0 DE FE ED FA CE", NULL);
    CHECK(found == (void*)kMarker, "module scan finds the marker in .rdata");

    void* found_wild = umf_aob_scan("DE AD C0 ?? FE ED", NULL);
    CHECK(found_wild == (void*)kMarker, "module scan with wildcard finds marker");

    void* missing = umf_aob_scan("DE AD C0 DE 00 00 00 00", NULL);
    CHECK(missing == NULL, "module scan of absent pattern returns NULL");

    /* scan_all: marker bytes 0xDE 0xAD are unique enough to count >= 1. */
    void* hits[16];
    int n = umf_aob_scan_all("DE AD C0 DE FE ED FA CE", NULL, hits, 16);
    CHECK(n >= 1, "scan_all returns at least one hit");
    CHECK(n >= 1 && hits[0] == (void*)kMarker, "scan_all first hit is the marker");

    /* max cap honored */
    int capped = umf_aob_scan_all("00", NULL, hits, 3);
    CHECK(capped <= 3, "scan_all respects the max cap");
}
