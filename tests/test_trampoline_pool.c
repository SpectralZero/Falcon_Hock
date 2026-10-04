/*
 * test_trampoline_pool.c — proximity allocation + slot lifecycle + unwind.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <stdint.h>

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
}
