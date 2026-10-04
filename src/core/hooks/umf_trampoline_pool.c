/*
 * umf_trampoline_pool.c — Proximity-aware trampoline memory allocator
 *
 * Design:
 *   - Blocks are linked-list (Bug I fix: never realloc'd, pointers stable)
 *   - Each block is 64KB, VirtualAlloc'd near the target (±1GB)
 *   - Slots are 96 bytes each (~682 per block)
 *   - Slot lifecycle: FREE → ACTIVE → PENDING_FREE → GC'd → FREE
 *   - GC runs periodically, checks thread RIPs, flips pages
 *
 * Thread safety: pool-level SRWLOCK for allocation/free.
 * The freeze window must NOT call allocate (would deadlock on heap).
 */

#include "umf/umf.h"

/* ════════════════════════════════════════════════════════════════
 * Init / Destroy
 * ════════════════════════════════════════════════════════════════ */

void umf_trampoline_pool_init(UmfTrampolinePool* pool) {
    memset(pool, 0, sizeof(*pool));
    InitializeSRWLock(&pool->lock);
}

void umf_trampoline_pool_destroy(UmfTrampolinePool* pool) {
    AcquireSRWLockExclusive(&pool->lock);

    UmfTrampolineBlock* block = pool->head;
    while (block) {
        UmfTrampolineBlock* next = block->next;

        /* Unregister any remaining unwind entries */
        for (size_t s = 0; s < block->total_slots; s++) {
            if (block->slots[s].rt_entry) {
                RtlDeleteFunctionTable(block->slots[s].rt_entry);
                block->slots[s].rt_entry = NULL;
            }
        }

        /* Free the VirtualAlloc'd region */
        if (block->base_address)
            VirtualFree(block->base_address, 0, MEM_RELEASE);

        /* Free the block struct itself */
        free(block);
        block = next;
    }

    pool->head = NULL;
    pool->block_count = 0;
    ReleaseSRWLockExclusive(&pool->lock);
}

/* ════════════════════════════════════════════════════════════════
 * Allocate Near — find a free slot within ±1GB of target
 * ════════════════════════════════════════════════════════════════ */

UmfTrampolineSlot* umf_trampoline_pool_allocate_near(
    UmfTrampolinePool* pool, void* target, size_t size_hint)
{
    (void)size_hint;  /* All slots are fixed-size */

    AcquireSRWLockExclusive(&pool->lock);

    uintptr_t tgt = (uintptr_t)target;
    uintptr_t low  = tgt > UMF_TRAMPOLINE_SEARCH_RANGE
                     ? tgt - UMF_TRAMPOLINE_SEARCH_RANGE : 0;
    uintptr_t high = tgt + UMF_TRAMPOLINE_SEARCH_RANGE;

    /* ── Step 1: Search existing blocks for a free slot in range ── */
    for (UmfTrampolineBlock* blk = pool->head; blk; blk = blk->next) {
        uintptr_t blk_addr = (uintptr_t)blk->base_address;
        if (blk_addr < low || blk_addr > high) continue;

        for (size_t s = 0; s < blk->total_slots; s++) {
            UmfTrampolineSlot* slot = &blk->slots[s];
            if (slot->state != UMF_SLOT_FREE) continue;

            /* Flip NOACCESS → RW for writing */
            DWORD old;
            if (!VirtualProtect(slot->code, UMF_TRAMPOLINE_SLOT_SIZE,
                                PAGE_READWRITE, &old)) continue;

            slot->state = UMF_SLOT_ACTIVE;
            slot->used_size = 0;
            slot->gc_retry_count = 0;
            slot->logged_quarantine = false;
            slot->rt_entry = NULL;

            ReleaseSRWLockExclusive(&pool->lock);
            return slot;
        }
    }

    /* ── Step 2: No free slot — allocate a new block near target ── */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t granularity = si.dwAllocationGranularity;
    uintptr_t search = (low + granularity - 1) & ~(granularity - 1);

    while (search < high) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)search, &mbi, sizeof(mbi)) == 0) {
            search += granularity;
            continue;
        }

        if (mbi.State == MEM_FREE &&
            mbi.RegionSize >= UMF_TRAMPOLINE_BLOCK_SIZE) {

            void* alloc = VirtualAlloc(
                (void*)search,
                UMF_TRAMPOLINE_BLOCK_SIZE,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE);

            if (alloc) {
                /* Create block struct (heap-allocated, never moved) */
                UmfTrampolineBlock* blk =
                    (UmfTrampolineBlock*)calloc(1, sizeof(UmfTrampolineBlock));
                if (!blk) {
                    VirtualFree(alloc, 0, MEM_RELEASE);
                    ReleaseSRWLockExclusive(&pool->lock);
                    return NULL;
                }

                blk->base_address = alloc;
                blk->total_slots  = UMF_SLOTS_PER_BLOCK;

                /* Initialize all slots */
                for (size_t s = 0; s < blk->total_slots; s++) {
                    blk->slots[s].code = (uint8_t*)alloc +
                                         (s * UMF_TRAMPOLINE_SLOT_SIZE);
                    blk->slots[s].state = UMF_SLOT_FREE;
                    blk->slots[s].owner_block = blk;
                    blk->slots[s].rt_entry = NULL;
                }

                /* First slot is active (for the caller) */
                blk->slots[0].state = UMF_SLOT_ACTIVE;
                /* Page is already RW from VirtualAlloc */

                /* Mark remaining slots as NOACCESS for fail-loud */
                for (size_t s = 1; s < blk->total_slots; s++) {
                    /* Only flip if the slot is on a different page boundary
                     * — VirtualProtect operates on full pages. Since all
                     * slots share the same 64KB region and it was allocated
                     * as RW, we skip individual slot protection for now.
                     * The real protection transition happens in finalize. */
                }

                /* Link into pool (prepend to head — O(1)) */
                blk->next = pool->head;
                pool->head = blk;
                pool->block_count++;

                UmfTrampolineSlot* slot = &blk->slots[0];
                ReleaseSRWLockExclusive(&pool->lock);
                return slot;
            }
        }

        search = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }

    ReleaseSRWLockExclusive(&pool->lock);
    return NULL;
}

/* ════════════════════════════════════════════════════════════════
 * Release — mark slot for GC (does NOT free immediately)
 * ════════════════════════════════════════════════════════════════ */

void umf_trampoline_pool_release(UmfTrampolinePool* pool,
                                  UmfTrampolineSlot* slot) {
    (void)pool;  /* Pool lock not needed for state flip */
    slot->state = UMF_SLOT_PENDING_FREE;
    slot->last_used_tick = GetTickCount64();
}

/* ════════════════════════════════════════════════════════════════
 * Finalize — transition slot from RW to RX + register CFG
 *
 * Bug J fix: Uses block->base_address (page-aligned from VirtualAlloc)
 * as the base for SetProcessValidCallTargets, NOT slot->code.
 *
 * SetProcessValidCallTargets lives behind an API set whose import
 * library is not universally available, and the API is absent on older
 * systems — so it is resolved dynamically and treated as best-effort.
 * ════════════════════════════════════════════════════════════════ */

typedef BOOL (WINAPI *PFN_SetProcessValidCallTargets)(
    HANDLE, PVOID, SIZE_T, ULONG, PCFG_CALL_TARGET_INFO);

static PFN_SetProcessValidCallTargets resolve_spvct(void) {
    static PFN_SetProcessValidCallTargets fn = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        if (k32) {
            fn = (PFN_SetProcessValidCallTargets)
                GetProcAddress(k32, "SetProcessValidCallTargets");
        }
    }
    return fn;
}

bool umf_trampoline_finalize(UmfTrampolineSlot* slot, size_t used_size,
                              bool cfg_active) {
    /* RW → RX */
    DWORD old_protect;
    if (!VirtualProtect(slot->code, UMF_TRAMPOLINE_SLOT_SIZE,
                        PAGE_EXECUTE_READ, &old_protect)) {
        return false;
    }

    FlushInstructionCache(GetCurrentProcess(), slot->code, used_size);
    slot->used_size = used_size;

    /* Register with CFG (Bug J fix: page-aligned base) */
    if (cfg_active && slot->owner_block) {
        PFN_SetProcessValidCallTargets spvct = resolve_spvct();
        if (spvct) {
            void* page_base = slot->owner_block->base_address;
            ULONG_PTR slot_offset =
                (ULONG_PTR)slot->code - (ULONG_PTR)page_base;

            CFG_CALL_TARGET_INFO target_info;
            memset(&target_info, 0, sizeof(target_info));
            target_info.Offset = slot_offset;
            target_info.Flags  = CFG_CALL_TARGET_VALID;

            if (!spvct(GetCurrentProcess(), page_base,
                       UMF_TRAMPOLINE_BLOCK_SIZE, 1, &target_info)) {
                /* Non-fatal: hook still works if the target doesn't use CFG */
            }
        }
    }

    return true;
}

/* ════════════════════════════════════════════════════════════════
 * GC — Reclaim PENDING_FREE slots after timeout + RIP check
 *
 * MUST be called from OUTSIDE the freeze window.
 * This function freezes threads internally.
 * ════════════════════════════════════════════════════════════════ */

void umf_trampoline_gc(UmfTrampolinePool* pool) {
    UmfThreadFreezer freezer;
    umf_freeze_all_threads(&freezer);

    uint64_t now = GetTickCount64();

    AcquireSRWLockExclusive(&pool->lock);

    for (UmfTrampolineBlock* blk = pool->head; blk; blk = blk->next) {
        for (size_t s = 0; s < blk->total_slots; s++) {
            UmfTrampolineSlot* slot = &blk->slots[s];
            if (slot->state != UMF_SLOT_PENDING_FREE) continue;
            if (now - slot->last_used_tick < 1000) continue;

            if (umf_any_thread_in_range(&freezer,
                                         (uintptr_t)slot->code,
                                         (uintptr_t)slot->code +
                                             UMF_TRAMPOLINE_SLOT_SIZE)) {
                slot->gc_retry_count++;
                if (slot->gc_retry_count > 100)
                    slot->state = UMF_SLOT_QUARANTINED;
                continue;
            }

            /* Unregister exception handling */
            if (slot->rt_entry) {
                RtlDeleteFunctionTable(slot->rt_entry);
                slot->rt_entry = NULL;
            }

            /* RX → RW → wipe → NOACCESS */
            DWORD old;
            if (!VirtualProtect(slot->code, UMF_TRAMPOLINE_SLOT_SIZE,
                                PAGE_READWRITE, &old)) {
                slot->gc_retry_count++;
                if (slot->gc_retry_count > 100)
                    slot->state = UMF_SLOT_QUARANTINED;
                continue;
            }

            memset(slot->code, 0xCC, UMF_TRAMPOLINE_SLOT_SIZE);

            DWORD dummy;
            VirtualProtect(slot->code, UMF_TRAMPOLINE_SLOT_SIZE,
                           PAGE_NOACCESS, &dummy);

            slot->state = UMF_SLOT_FREE;
            slot->gc_retry_count = 0;
        }
    }

    ReleaseSRWLockExclusive(&pool->lock);
    umf_resume_all_threads(&freezer);
}
