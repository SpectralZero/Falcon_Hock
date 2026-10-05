/*
 * umf_hook_batch.c — Batched hook enable/disable with prepare/apply split (§1)
 *
 *   PHASE 1 (prepare): allocate trampoline, relocate prologue (Zydis),
 *                       register unwind info, finalize (RW→RX+CFG),
 *                       build the inline jump patch bytes.
 *   PHASE 2 (freeze):  relocate any thread caught mid-prologue, then write
 *                       the inline jump patches / restore bytes for unhooks.
 *   PHASE 3 (post):    flush caches, publish trampolines, wire the chain.
 *
 * Dispatch model (Detours-style, §11): the target entry is patched to jump
 * to the highest-priority hook. Each hook calls its original_func, which is
 * wired to the next hook or — for the last hook — to the trampoline, which
 * replays the stolen prologue and jumps back into the original body.
 *
 * Ordering guarantees:
 *   - Bug H: the trampoline is finalized (executable + CFG) BEFORE the target
 *     is patched, so no thread can ever jump into a non-executable trampoline.
 *   - Unwind metadata is written while the slot is still RW (before finalize).
 *   - No allocation, logging, or lock-taking occurs inside the freeze window.
 */

#include "umf/umf.h"

/* ── Global batch queue ── */
typedef struct {
    UmfHookTarget* target;
    bool           enable;
} UmfBatchEntry;

static UmfBatchEntry g_batch[UMF_MAX_BATCH];
static int           g_batch_count = 0;
static SRWLOCK       g_batch_lock = SRWLOCK_INIT;

void umf_queue_hook_enable(UmfHookTarget* target) {
    AcquireSRWLockExclusive(&g_batch_lock);
    if (g_batch_count < UMF_MAX_BATCH) {
        g_batch[g_batch_count].target = target;
        g_batch[g_batch_count].enable = true;
        g_batch_count++;
    }
    ReleaseSRWLockExclusive(&g_batch_lock);
}

void umf_queue_hook_disable(UmfHookTarget* target) {
    AcquireSRWLockExclusive(&g_batch_lock);
    if (g_batch_count < UMF_MAX_BATCH) {
        g_batch[g_batch_count].target = target;
        g_batch[g_batch_count].enable = false;
        g_batch_count++;
    }
    ReleaseSRWLockExclusive(&g_batch_lock);
}

/* ── Prepared hook (ready for atomic application) ── */
typedef struct {
    UmfHookTarget*     target;
    UmfTrampolineSlot* trampoline;
    uint8_t            jump_patch[14];
    size_t             jump_patch_size;
    uint8_t*           saved_prologue;      /* NULL unless first install */
    size_t             saved_prologue_size;
    RUNTIME_FUNCTION*  rt_entry;            /* non-NULL unless first install */
    bool               new_trampoline;      /* did we allocate it this batch? */
    bool               apply;               /* cleared if a thread blocks it  */
} PreparedHook;

/* ── Shared runtime state (defined in umf_init.c) ── */
extern UmfTrampolinePool   g_trampoline_pool;
extern UmfMitigationStatus g_mitigations;

/* ── Build a jmp rel32 (5B) or absolute jmp (14B) from→to ── */
static void build_jump_patch(uint8_t* out, size_t* out_size,
                              void* from, void* to) {
    ptrdiff_t delta = (uint8_t*)to - ((uint8_t*)from + 5);
    if (delta >= INT32_MIN && delta <= INT32_MAX) {
        out[0] = 0xE9;                 /* jmp rel32 */
        int32_t rel = (int32_t)delta;
        memcpy(&out[1], &rel, 4);
        *out_size = 5;
    } else {
        out[0] = 0xFF; out[1] = 0x25;  /* jmp qword [rip+0] */
        uint32_t zero = 0;
        memcpy(&out[2], &zero, 4);
        uint64_t addr = (uint64_t)(uintptr_t)to;
        memcpy(&out[6], &addr, 8);
        *out_size = 14;
    }
}

/* ── PHASE 2 helpers (no allocation, no logging, no locks) ── */

static void apply_hook_noalloc(PreparedHook* p) {
    DWORD old;
    if (!VirtualProtect(p->target->resolved_address,
                        p->jump_patch_size,
                        PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(p->target->resolved_address, p->jump_patch, p->jump_patch_size);
    DWORD dummy;
    VirtualProtect(p->target->resolved_address,
                   p->jump_patch_size, old, &dummy);
}

static void apply_unhook_noalloc(UmfHookTarget* target) {
    if (!target->original_bytes || target->original_prologue_size == 0) return;
    DWORD old;
    if (!VirtualProtect(target->resolved_address,
                        target->original_prologue_size,
                        PAGE_EXECUTE_READWRITE, &old)) return;
    memcpy(target->resolved_address, target->original_bytes,
           target->original_prologue_size);
    DWORD dummy;
    VirtualProtect(target->resolved_address,
                   target->original_prologue_size, old, &dummy);
}

/* If a frozen thread sits inside the bytes we are about to overwrite,
 * relocate its RIP to the equivalent point in the trampoline. Only
 * Get/SetThreadContext — safe inside the freeze window. */
static void relocate_threads_in_prologue(UmfThreadFreezer* fz, PreparedHook* p) {
    uintptr_t t0 = (uintptr_t)p->target->resolved_address;
    uintptr_t t1 = t0 + p->jump_patch_size;
    const UmfRelocMap* map = &p->target->reloc_map;

    for (int i = 0; i < fz->count; i++) {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(fz->thread_handles[i], &ctx)) continue;

        uintptr_t rip = (uintptr_t)ctx.Rip;
        if (rip <= t0 || rip >= t1) continue;   /* ==t0 is fine (hits our jmp) */

        uintptr_t off = rip - t0;
        for (int k = 0; k < map->count; k++) {
            if (map->src_off[k] == off) {
                ctx.Rip = (DWORD64)(uintptr_t)(p->trampoline->code +
                                               map->dst_off[k]);
                SetThreadContext(fz->thread_handles[i], &ctx);
                break;
            }
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 * PHASE 1 — prepare one hook (allocation + relocation OK, no freeze)
 * ════════════════════════════════════════════════════════════════ */

static bool prepare_hook(UmfHookTarget* target, PreparedHook* out) {
    memset(out, 0, sizeof(*out));
    out->target = target;
    out->apply  = true;

    /* Resolve the dispatch entry (highest-priority hook) under the chain lock. */
    AcquireSRWLockShared(&target->chain_lock);
    void* dispatch = target->chain_head ? target->chain_head->hook_func : NULL;
    ReleaseSRWLockShared(&target->chain_lock);
    if (!dispatch) {
        UMF_ERROR("No hooks on chain for %s", target->canonical_name);
        return false;
    }

    /* Build the inline jump patch (target → dispatch); size drives steal. */
    build_jump_patch(out->jump_patch, &out->jump_patch_size,
                     target->resolved_address, dispatch);

    if (target->trampoline == NULL) {
        /* ── First install: strategy must permit inline patching ── */
        if (!(umf_viable_strategies(&g_mitigations) & UMF_STRAT_INLINE)) {
            UMF_WARN("Inline hooks unavailable (ACG) for %s — IAT/EAT "
                     "fallback not yet implemented", target->canonical_name);
            return false;
        }
        if (!umf_can_write_code_page(&g_mitigations)) {
            UMF_WARN("HVCI active — cannot write code page for %s",
                     target->canonical_name);
            return false;
        }

        /* XFG: a /guard:xfg target's indirect calls verify a type hash at
         * the callee's -8 slot. We cannot guarantee our hook carries it, so
         * refuse the inline hook rather than fault (use IAT/EAT/vtable). */
        HMODULE owner = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)target->resolved_address, &owner) &&
            owner && umf_module_has_xfg(owner)) {
            UMF_WARN("Target %s is in an XFG module — refusing inline hook; "
                     "use IAT/EAT/vtable/hwbp", target->canonical_name);
            return false;
        }

        /* 1. Allocate a trampoline slot. */
        out->trampoline = umf_trampoline_pool_allocate_near(
            &g_trampoline_pool, target->resolved_address,
            UMF_TRAMPOLINE_SLOT_SIZE);
        if (!out->trampoline) {
            UMF_ERROR("Cannot allocate trampoline for %s",
                      target->canonical_name);
            return false;
        }

        /* 2. Relocate the prologue + emit jmp-back; capture the reloc map. */
        size_t steal = 0;
        if (!umf_build_trampoline_code(target->resolved_address,
                                        out->trampoline,
                                        out->jump_patch_size,
                                        &steal, &target->reloc_map)) {
            umf_trampoline_pool_release(&g_trampoline_pool, out->trampoline);
            out->trampoline = NULL;
            return false;
        }

        /* 3. Register unwind metadata while the slot is still writable. */
        out->rt_entry = umf_register_unwind_info(out->trampoline->code,
                                                 out->trampoline->used_size);
        out->trampoline->rt_entry = out->rt_entry;

        /* 4. BUG H: finalize (RW→RX + CFG) BEFORE the target is patched. */
        if (!umf_trampoline_finalize(out->trampoline,
                                      out->trampoline->used_size,
                                      g_mitigations.cfg_enforced)) {
            umf_unregister_unwind_info(out->rt_entry);
            out->trampoline->rt_entry = NULL;
            umf_trampoline_pool_release(&g_trampoline_pool, out->trampoline);
            out->trampoline = NULL;
            return false;
        }

        /* 5. Snapshot original bytes for later unhook. */
        out->saved_prologue_size = steal;
        out->saved_prologue = (uint8_t*)malloc(steal);
        if (!out->saved_prologue) {
            umf_unregister_unwind_info(out->rt_entry);
            out->trampoline->rt_entry = NULL;
            umf_trampoline_pool_release(&g_trampoline_pool, out->trampoline);
            out->trampoline = NULL;
            return false;
        }
        memcpy(out->saved_prologue, target->resolved_address, steal);
        out->new_trampoline = true;
    } else {
        /* ── Re-install: reuse trampoline, only re-point the target. ── */
        out->trampoline = target->trampoline;
        out->new_trampoline = false;
        if (out->jump_patch_size > target->original_prologue_size) {
            UMF_ERROR("Dispatch moved out of rel32 range for %s; "
                      "re-point needs %zu bytes, have %zu",
                      target->canonical_name, out->jump_patch_size,
                      target->original_prologue_size);
            return false;
        }
    }

    return true;
}

/* ════════════════════════════════════════════════════════════════
 * umf_apply_pending_batch — full prepare / freeze / apply / cleanup
 * ════════════════════════════════════════════════════════════════ */

bool umf_apply_pending_batch(void) {
    AcquireSRWLockExclusive(&g_batch_lock);
    int count = g_batch_count;
    if (count == 0) {
        ReleaseSRWLockExclusive(&g_batch_lock);
        return true;
    }
    UmfBatchEntry local_batch[UMF_MAX_BATCH];
    memcpy(local_batch, g_batch, count * sizeof(UmfBatchEntry));
    g_batch_count = 0;
    ReleaseSRWLockExclusive(&g_batch_lock);

    /* ═══ PHASE 1: PREPARE ═══ */
    PreparedHook*   prepared  = (PreparedHook*)calloc(count, sizeof(PreparedHook));
    UmfHookTarget** to_unhook = (UmfHookTarget**)calloc(count, sizeof(UmfHookTarget*));
    if (!prepared || !to_unhook) {
        free(prepared); free(to_unhook);
        UMF_ERROR("Batch allocation failed");
        return false;
    }
    int prepared_count = 0, unhook_count = 0;

    for (int i = 0; i < count; i++) {
        if (local_batch[i].enable) {
            if (prepare_hook(local_batch[i].target, &prepared[prepared_count]))
                prepared_count++;
            else
                UMF_WARN("Prepare failed for %s",
                         local_batch[i].target->canonical_name);
        } else {
            to_unhook[unhook_count++] = local_batch[i].target;
        }
    }

    if (prepared_count == 0 && unhook_count == 0) {
        free(prepared); free(to_unhook);
        return true;
    }

    /* ═══ PHASE 2: FREEZE AND APPLY (no alloc, no log) ═══ */
    UmfThreadFreezer freezer;
    umf_freeze_all_threads(&freezer);

    /* Defer unhooks whose trampoline currently has a thread executing in it. */
    for (int i = 0; i < unhook_count; i++) {
        UmfHookTarget* t = to_unhook[i];
        if (t && t->trampoline &&
            umf_any_thread_in_range(&freezer,
                                     (uintptr_t)t->trampoline->code,
                                     (uintptr_t)t->trampoline->code +
                                         UMF_TRAMPOLINE_SLOT_SIZE)) {
            t->deferred_unhook = true;
            to_unhook[i] = NULL;
        }
    }

    /* Rescue threads sitting inside bytes we are about to overwrite. */
    for (int i = 0; i < prepared_count; i++)
        if (prepared[i].apply)
            relocate_threads_in_prologue(&freezer, &prepared[i]);

    for (int i = 0; i < prepared_count; i++)
        if (prepared[i].apply)
            apply_hook_noalloc(&prepared[i]);

    for (int i = 0; i < unhook_count; i++)
        if (to_unhook[i]) apply_unhook_noalloc(to_unhook[i]);

    umf_resume_all_threads(&freezer);

    /* ═══ PHASE 3: POST-RESUME CLEANUP (alloc OK) ═══ */
    for (int i = 0; i < prepared_count; i++) {
        PreparedHook* p = &prepared[i];
        if (!p->apply) {
            /* Could not patch safely — roll back a freshly built trampoline. */
            if (p->new_trampoline && p->trampoline) {
                umf_unregister_unwind_info(p->rt_entry);
                p->trampoline->rt_entry = NULL;
                umf_trampoline_pool_release(&g_trampoline_pool, p->trampoline);
            }
            free(p->saved_prologue);
            continue;
        }

        FlushInstructionCache(GetCurrentProcess(),
            p->target->resolved_address, p->jump_patch_size);

        if (p->new_trampoline) {
            p->target->trampoline = p->trampoline;
            p->target->rt_entry   = p->rt_entry;
            if (!p->target->original_bytes) {
                p->target->original_bytes        = p->saved_prologue;
                p->target->original_prologue_size = p->saved_prologue_size;
            } else {
                free(p->saved_prologue);
            }
        }

        /* Wire the dispatch chain now that the trampoline is published. */
        umf_rebuild_target_chain(p->target);
    }

    for (int i = 0; i < unhook_count; i++) {
        UmfHookTarget* t = to_unhook[i];
        if (t && t->trampoline) {
            umf_trampoline_pool_release(&g_trampoline_pool, t->trampoline);
            t->trampoline = NULL;
            t->rt_entry   = NULL;
        }
    }

    free(prepared);
    free(to_unhook);

    UMF_INFO("Batch applied: %d prepared, %d unhooked", prepared_count, unhook_count);
    return true;
}
