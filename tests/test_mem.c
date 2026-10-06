/*
 * test_mem.c — §MEM memory layer tests (fault-guard + capability gating).
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <stdint.h>

void run_mem_tests(void) {
    /* ── Basic read/write round-trip ── */
    uint8_t buf[16];
    memset(buf, 0, sizeof(buf));
    uint32_t value = 0xA1B2C3D4;

    /* Start with the engine as owner (trusted). */
    umf_mem_set_owner(NULL);
    CHECK(umf_mem_write(buf, &value, sizeof(value)), "trusted write succeeds");

    uint32_t back = 0;
    CHECK(umf_mem_read(buf, &back, sizeof(back)), "trusted read succeeds");
    CHECK(back == value, "read returns what was written");

    /* ── Fault-guarding: bad addresses return false, never crash ── */
    uint32_t sink = 0;
    CHECK(!umf_mem_read((const void*)0x1, &sink, sizeof(sink)),
          "read of unmapped address returns false (no fault)");
    CHECK(!umf_mem_write((void*)0x1, &value, sizeof(value)),
          "write of unmapped address returns false (no fault)");

    /* ── Capability gating ── */
    UmfMod no_caps;
    memset(&no_caps, 0, sizeof(no_caps));
    strcpy_s(no_caps.name, sizeof(no_caps.name), "nocaps");
    umf_mem_set_owner(&no_caps);
    CHECK(!umf_mem_read(buf, &back, sizeof(back)),
          "read denied for mod without 'read_memory'");
    CHECK(!umf_mem_write(buf, &value, sizeof(value)),
          "write denied for mod without 'write_memory'");

    UmfMod read_only;
    memset(&read_only, 0, sizeof(read_only));
    strcpy_s(read_only.name, sizeof(read_only.name), "reader");
    read_only.capabilities = UMF_CAP_READ_MEM;
    umf_mem_set_owner(&read_only);
    CHECK(umf_mem_read(buf, &back, sizeof(back)), "read allowed with read cap");
    CHECK(!umf_mem_write(buf, &value, sizeof(value)),
          "write still denied for read-only mod");

    UmfMod rw;
    memset(&rw, 0, sizeof(rw));
    strcpy_s(rw.name, sizeof(rw.name), "rw");
    rw.capabilities = UMF_CAP_READ_MEM | UMF_CAP_WRITE_MEM;
    umf_mem_set_owner(&rw);
    uint32_t v2 = 0x11223344;
    CHECK(umf_mem_write(buf, &v2, sizeof(v2)), "write allowed with write cap");
    umf_mem_read(buf, &back, sizeof(back));
    CHECK(back == v2, "capped write took effect");

    umf_mem_set_owner(NULL);   /* restore trusted */

    /* ── Region enumeration + query ── */
    UmfMemRegion regions[8];
    int n = umf_mem_enum_regions(regions, 8);
    CHECK(n > 0, "enum regions returns committed regions");
    CHECK(n <= 8, "enum regions respects max");

    UmfMemRegion here;
    CHECK(umf_mem_query(buf, &here), "query finds the region for a stack buffer");
    CHECK(here.state == MEM_COMMIT, "queried region is committed");
    CHECK((uintptr_t)buf >= here.base &&
          (uintptr_t)buf < here.base + here.size, "buffer lies within queried region");

    UmfMemRegion nope;
    CHECK(umf_mem_query((const void*)0x1, &nope) == false ||
          nope.state != MEM_COMMIT,
          "query of address 0x1 is not a committed region");
}
