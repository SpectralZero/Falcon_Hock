/*
 * umf_hook_registry.c — Global hook target registry + function resolution
 *
 * Registry is protected by SRWLOCK for TOCTOU safety (§3.3 fix).
 * resolve_function follows export forwarding chains (§3.1 fix).
 */

#include "umf/umf.h"
#include <stdio.h>

/* ── Global registry ── */
static UmfHookTarget g_targets[UMF_MAX_HOOK_TARGETS];
static int           g_target_count = 0;
static SRWLOCK       g_registry_lock = SRWLOCK_INIT;

void umf_hook_registry_init(void) {
    memset(g_targets, 0, sizeof(g_targets));
    g_target_count = 0;
    InitializeSRWLock(&g_registry_lock);
}

/* ── Find by resolved address (must hold lock) ── */
UmfHookTarget* umf_hook_registry_find(void* resolved_addr) {
    for (int i = 0; i < g_target_count; i++) {
        if (g_targets[i].resolved_address == resolved_addr)
            return &g_targets[i];
    }
    return NULL;
}

/* ── Create new target entry (must hold lock) ── */
UmfHookTarget* umf_hook_registry_create(void* resolved_addr,
                                         const char* dll,
                                         const char* func) {
    if (g_target_count >= UMF_MAX_HOOK_TARGETS) {
        UMF_ERROR("Hook target limit reached (%d)", UMF_MAX_HOOK_TARGETS);
        return NULL;
    }

    UmfHookTarget* t = &g_targets[g_target_count++];
    memset(t, 0, sizeof(*t));
    t->resolved_address = resolved_addr;
    snprintf(t->canonical_name, sizeof(t->canonical_name), "%s!%s", dll, func);
    InitializeSRWLock(&t->chain_lock);
    /* trampoline, rt_entry, chain_head, original_bytes all NULL from memset */
    return t;
}

/* ── Function resolution with export forwarding ── */

void* umf_resolve_function(const char* dll, const char* func) {
    static __declspec(thread) int recursion_depth = 0;

    if (recursion_depth > 10) {
        UMF_ERROR("Forwarder chain too deep: %s!%s (depth > 10)", dll, func);
        return NULL;
    }

    recursion_depth++;

    HMODULE mod = GetModuleHandleA(dll);
    if (!mod) mod = LoadLibraryA(dll);
    if (!mod) {
        UMF_ERROR("Cannot load module '%s'", dll);
        recursion_depth--;
        return NULL;
    }

    void* addr = (void*)GetProcAddress(mod, func);
    if (!addr) {
        UMF_ERROR("Cannot find '%s' in '%s'", func, dll);
        recursion_depth--;
        return NULL;
    }

    /* ── Check for export forwarding ── */
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)mod + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY* exp_dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    if (exp_dir->VirtualAddress != 0 && exp_dir->Size != 0) {
        uintptr_t exp_start = (uintptr_t)mod + exp_dir->VirtualAddress;
        uintptr_t exp_end   = exp_start + exp_dir->Size;

        if ((uintptr_t)addr >= exp_start && (uintptr_t)addr < exp_end) {
            /* addr points to a forwarder string like "KERNELBASE.CreateFileW" */
            char fwd[256];
            strncpy_s(fwd, sizeof(fwd), (const char*)addr, _TRUNCATE);

            char* dot = strchr(fwd, '.');
            if (dot) {
                *dot = '\0';
                char fwd_dll[270];
                snprintf(fwd_dll, sizeof(fwd_dll), "%s.dll", fwd);

                UMF_DEBUG("Export forwarding: %s!%s -> %s!%s",
                          dll, func, fwd_dll, dot + 1);

                addr = umf_resolve_function(fwd_dll, dot + 1);
            }
        }
    }

    recursion_depth--;
    return addr;
}

/* ── Register a hook (public API) ── */

/* Shared mitigation state (defined in umf_init.c) */
extern UmfMitigationStatus g_mitigations;

/* Helper: insert into sorted chain (by priority, descending) */
static void chain_insert_sorted(UmfHookEntry** head, UmfHookEntry* entry) {
    if (!*head || entry->priority > (*head)->priority) {
        entry->next = *head;
        *head = entry;
        return;
    }
    UmfHookEntry* cur = *head;
    while (cur->next && cur->next->priority >= entry->priority)
        cur = cur->next;
    entry->next = cur->next;
    cur->next = entry;
}

UmfHookStrategy umf_select_strategy(void* addr, const UmfMitigationStatus* m) {
    (void)addr;  /* Future: inspect prologue (ENDBR64/XFG) to refine choice */
    UmfHookStrategyMask mask = umf_viable_strategies(m);
    if (mask & UMF_STRAT_INLINE) return UMF_HOOK_INLINE;
    if (mask & UMF_STRAT_IAT)    return UMF_HOOK_IAT;
    if (mask & UMF_STRAT_EAT)    return UMF_HOOK_EAT;
    return UMF_HOOK_HARDWARE_BP;
}

/* Re-wire the dispatch chain: each hook's original_func points to the next
 * hook in priority order, and the final hook's original_func points to the
 * trampoline (the "call original" entry). Mod "call original" slots are
 * refreshed so g_original_X pointers stay valid as the chain changes. */
void umf_rebuild_target_chain(UmfHookTarget* target) {
    AcquireSRWLockExclusive(&target->chain_lock);

    void* original = target->trampoline ? (void*)target->trampoline->code : NULL;

    for (UmfHookEntry* cur = target->chain_head; cur; cur = cur->next) {
        cur->original_func = cur->next ? cur->next->hook_func : original;
        if (cur->user_original_slot)
            *cur->user_original_slot = cur->original_func;
    }

    ReleaseSRWLockExclusive(&target->chain_lock);
}

bool umf_register_hook_ex(const char* dll, const char* func,
                           void* hook_func, int priority, UmfMod* mod,
                           void** original_out) {
    if (!hook_func) return false;

    void* real_addr = umf_resolve_function(dll, func);
    if (!real_addr) return false;

    return umf_register_hook_addr(real_addr, func, hook_func, priority,
                                  mod, original_out);
}

bool umf_register_hook_addr(void* real_addr, const char* name,
                             void* hook_func, int priority, UmfMod* mod,
                             void** original_out) {
    if (!real_addr || !hook_func) return false;

    UMF_INFO("Registering hook @ %p '%s' (priority %d, mod '%s')",
             real_addr, name ? name : "<anon>", priority,
             mod ? mod->name : "<anonymous>");

    AcquireSRWLockExclusive(&g_registry_lock);

    UmfHookTarget* target = umf_hook_registry_find(real_addr);
    if (!target) {
        target = umf_hook_registry_create(real_addr,
                                          name ? name : "addr", "");
        if (!target) {
            ReleaseSRWLockExclusive(&g_registry_lock);
            return false;
        }
        target->active_strategy = umf_select_strategy(real_addr, &g_mitigations);
    }

    /* Fully populate entry before insertion */
    UmfHookEntry* entry = (UmfHookEntry*)calloc(1, sizeof(UmfHookEntry));
    if (!entry) {
        ReleaseSRWLockExclusive(&g_registry_lock);
        return false;
    }
    entry->hook_func              = hook_func;
    entry->original_func          = NULL;     /* set by rebuild after install */
    entry->priority               = priority;
    entry->owner_mod              = mod;
    entry->next                   = NULL;
    entry->skip_original          = false;
    entry->call_original_requested = false;
    entry->user_original_slot     = original_out;
    snprintf(entry->debug_name, sizeof(entry->debug_name),
             "%s hook by %s", target->canonical_name,
             mod ? mod->name : "anonymous");

    AcquireSRWLockExclusive(&target->chain_lock);
    chain_insert_sorted(&target->chain_head, entry);
    ReleaseSRWLockExclusive(&target->chain_lock);

    ReleaseSRWLockExclusive(&g_registry_lock);

    /* Queue (re-)installation. The batch patches the target to the current
     * highest-priority hook and rebuilds the chain. The caller drives timing
     * via umf_apply_pending_batch(). */
    umf_queue_hook_enable(target);
    return true;
}

bool umf_register_hook(const char* dll, const char* func,
                        void* hook_func, int priority, UmfMod* mod) {
    return umf_register_hook_ex(dll, func, hook_func, priority, mod, NULL);
}

