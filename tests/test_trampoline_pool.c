/*
 * test_trampoline_pool.c — proximity allocation + slot lifecycle + unwind.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <stdint.h>

/* Write a position-independent 'mov eax, imm32; ret' stub (6 bytes). */
static void write_ret_imm(uint8_t* code, int imm) {
    code[0] = 0xB8;                 /* mov eax, imm32 */
    uint32_t v = (uint32_t)imm;
    memcpy(code + 1, &v, 4);
    code[5] = 0xC3;                 /* ret */
}

void run_trampoline_pool_tests(void) {
    UmfTrampolinePool pool;
    umf_trampoline_pool_init(&pool);

    void* target = (void*)&run_trampoline_pool_tests;

    UmfTrampolineSlot* s = umf_trampoline_pool_allocate_near(
        &pool, target, UMF_TRAMPOLINE_SLOT_SIZE);
    CHECK(s != NULL, "allocate_near returns a slot");

    if (s) {
        CHECK(s->state == UMF_SLOT_ACTIVE, "new slot is ACTIVE");
        CHECK(s->owner_block != NULL, "slot has owner block back-pointer");

        intptr_t delta = (intptr_t)((uint8_t*)s->code - (uint8_t*)target);
        if (delta < 0) delta = -delta;
        CHECK((uintptr_t)delta <= UMF_TRAMPOLINE_SEARCH_RANGE,
              "slot allocated within +/-1GB of target");

        /* Write a trivial 'ret' and register unwind + finalize. */
        s->code[0] = 0xC3;
        RUNTIME_FUNCTION* rf = umf_register_unwind_info(s->code, 1);
        CHECK(rf != NULL, "unwind info registered");
        s->rt_entry = rf;

        bool fin = umf_trampoline_finalize(s, 1, false);
        CHECK(fin, "finalize flips RW -> RX");

        umf_trampoline_pool_release(&pool, s);
        CHECK(s->state == UMF_SLOT_PENDING_FREE, "release marks PENDING_FREE");
    }

    /* Second allocation should reuse the same block (linked-list stable). */
    UmfTrampolineSlot* s2 = umf_trampoline_pool_allocate_near(
        &pool, target, UMF_TRAMPOLINE_SLOT_SIZE);
    CHECK(s2 != NULL, "second allocation succeeds");
    CHECK(pool.block_count >= 1, "pool tracks at least one block");

    umf_trampoline_pool_destroy(&pool);
    CHECK(pool.head == NULL, "destroy clears the block list");

    /* ── Tier-1 #3 regression: per-page slot isolation ──
     * Finalizing (RW→RX) and GC-ing (→NOACCESS) one slot must not disturb a
     * sibling slot. Before the one-slot-per-page fix, both lived on the same
     * 4KB page and these transitions silently corrupted each other. */
    UmfTrampolinePool iso;
    umf_trampoline_pool_init(&iso);
    void* anchor = (void*)&write_ret_imm;

    UmfTrampolineSlot* a = umf_trampoline_pool_allocate_near(
        &iso, anchor, UMF_TRAMPOLINE_SLOT_SIZE);
    UmfTrampolineSlot* b = umf_trampoline_pool_allocate_near(
        &iso, anchor, UMF_TRAMPOLINE_SLOT_SIZE);
    CHECK(a != NULL && b != NULL, "two sibling slots allocated");

    if (a && b) {
        uintptr_t pa = (uintptr_t)a->code & ~(uintptr_t)0xFFF;
        uintptr_t pb = (uintptr_t)b->code & ~(uintptr_t)0xFFF;
        CHECK(pa != pb, "sibling slots live on distinct pages");

        write_ret_imm(a->code, 0xAA);
        write_ret_imm(b->code, 0xBB);
        CHECK(umf_trampoline_finalize(a, 6, false), "finalize slot A (RW->RX)");
        CHECK(umf_trampoline_finalize(b, 6, false), "finalize slot B (RW->RX)");

        int (*fa)(void) = (int (*)(void))a->code;
        int (*fb)(void) = (int (*)(void))b->code;
        CHECK(fa() == 0xAA, "slot A executes after finalize");
        CHECK(fb() == 0xBB, "slot B executes (A's finalize didn't flip B's page)");

        /* Force-GC slot A; slot B must survive A's NOACCESS wipe. */
        umf_trampoline_pool_release(&iso, a);
        a->last_used_tick -= 5000;   /* age past the 1s GC window */
        umf_trampoline_gc(&iso);
        CHECK(a->state == UMF_SLOT_FREE, "slot A reclaimed by GC");
        CHECK(fb() == 0xBB, "slot B still executes after A GC (page isolation)");
    }

    umf_trampoline_pool_destroy(&iso);
    CHECK(iso.head == NULL, "isolation pool destroyed");
}
