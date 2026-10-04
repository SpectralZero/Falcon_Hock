/*
 * UMF — UniversalModFramework
 * Master header — include this one file from all UMF sources.
 *
 * This header defines all public types, constants, and function
 * declarations used across the framework. Implementation files
 * include this and nothing else from include/umf/.
 */

#ifndef UMF_H
#define UMF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────
 * Platform headers (always included, always lean)
 * ──────────────────────────────────────────────────────────────── */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>   /* malloc/calloc/realloc/free — MUST be declared, else
                       * implicit-int return truncates pointers on x64 */
#include <string.h>   /* memcpy/memset/strcmp/strchr/strncpy */

/* ────────────────────────────────────────────────────────────────
 * DLL export/import
 * ──────────────────────────────────────────────────────────────── */
#ifdef UMF_EXPORTS
#define UMF_API __declspec(dllexport)
#else
#define UMF_API __declspec(dllimport)
#endif

/* ────────────────────────────────────────────────────────────────
 * Version
 * ──────────────────────────────────────────────────────────────── */
#define UMF_VERSION_MAJOR   0
#define UMF_VERSION_MINOR   1
#define UMF_VERSION_PATCH   0
#define UMF_VERSION_STRING  "0.1.0"

/* ────────────────────────────────────────────────────────────────
 * Limits
 * ──────────────────────────────────────────────────────────────── */
#define UMF_MAX_BATCH            64
#define UMF_MAX_HOOK_TARGETS     1024   /* Will be replaced with dynamic linked list */
#define UMF_MAX_NAME_LEN         256
#define UMF_MAX_REENTRANCY_DEPTH 64
#define UMF_MAX_PROLOGUE_INSNS   16     /* Max instructions relocated into a trampoline */

/* ────────────────────────────────────────────────────────────────
 * Trampoline Pool Constants
 *
 * Tier-1 #3 fix: each slot occupies its own 4 KiB page. VirtualProtect
 * is page-granular, so packing many slots per page meant one slot's
 * RW→RX (finalize) or →NOACCESS (GC) silently changed the protection of
 * every sibling slot on that page. Giving each slot a full page makes
 * every protection transition affect exactly one trampoline.
 * ──────────────────────────────────────────────────────────────── */
#define UMF_TRAMPOLINE_SEARCH_RANGE  (1024ULL * 1024 * 1024)  /* ±1GB */
#define UMF_TRAMPOLINE_BLOCK_SIZE    (64 * 1024)               /* 64KB alloc granularity */
#define UMF_TRAMPOLINE_SLOT_SIZE     4096                      /* One page per slot      */
#define UMF_SLOTS_PER_BLOCK \
    (UMF_TRAMPOLINE_BLOCK_SIZE / UMF_TRAMPOLINE_SLOT_SIZE)     /* = 16 slots / block     */

/* ════════════════════════════════════════════════════════════════
 * §LOG — Logging
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    UMF_LOG_TRACE = 0,
    UMF_LOG_DEBUG = 1,
    UMF_LOG_INFO  = 2,
    UMF_LOG_WARN  = 3,
    UMF_LOG_ERROR = 4,
    UMF_LOG_FATAL = 5,
} UmfLogLevel;

UMF_API void  umf_log_init(const wchar_t* log_dir);
UMF_API void  umf_log_shutdown(void);
UMF_API void  umf_log_set_level(UmfLogLevel level);
UMF_API void  umf_log_write(UmfLogLevel level, const char* file, int line,
                             const char* fmt, ...);

#define UMF_LOG(level, fmt, ...) \
    umf_log_write(level, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define UMF_TRACE(fmt, ...) UMF_LOG(UMF_LOG_TRACE, fmt, ##__VA_ARGS__)
#define UMF_DEBUG(fmt, ...) UMF_LOG(UMF_LOG_DEBUG, fmt, ##__VA_ARGS__)
#define UMF_INFO(fmt, ...)  UMF_LOG(UMF_LOG_INFO,  fmt, ##__VA_ARGS__)
#define UMF_WARN(fmt, ...)  UMF_LOG(UMF_LOG_WARN,  fmt, ##__VA_ARGS__)
#define UMF_ERROR(fmt, ...) UMF_LOG(UMF_LOG_ERROR, fmt, ##__VA_ARGS__)
#define UMF_FATAL(fmt, ...) UMF_LOG(UMF_LOG_FATAL, fmt, ##__VA_ARGS__)

/* ════════════════════════════════════════════════════════════════
 * §PLATFORM — Platform queries
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    DWORD major;
    DWORD minor;
    DWORD build;
} UmfWindowsVersion;

UMF_API bool umf_get_windows_version(UmfWindowsVersion* out);
UMF_API bool umf_check_minimum_version(void);    /* Win10 1709+ (16299) */
UMF_API bool umf_check_anticheat(void);           /* Returns true if AC detected */

/* ════════════════════════════════════════════════════════════════
 * §MITIGATIONS — Security mitigation detection
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    bool acg_enforced;       /* Arbitrary Code Guard (ProhibitDynamicCode)  */
    bool acg_audit_only;     /* ACG in audit mode (logs, does not enforce)  */
    bool hvci_active;        /* Hypervisor Code Integrity                   */
    bool cfg_enforced;       /* Control Flow Guard                          */
    bool cet_shadow_stack;   /* CET Shadow Stack                            */
    bool cet_ibt;            /* CET Indirect Branch Tracking (ENDBR64)      */
    bool xfg_present;        /* eXtended Flow Guard in target binary        */
} UmfMitigationStatus;

UMF_API void umf_detect_all_mitigations(UmfMitigationStatus* out);
UMF_API bool umf_can_write_code_page(const UmfMitigationStatus* m);

/* Strategy viability mask based on active mitigations */
typedef enum {
    UMF_STRAT_INLINE      = 0x01,
    UMF_STRAT_GAP         = 0x02,
    UMF_STRAT_VTABLE      = 0x04,
    UMF_STRAT_IAT         = 0x08,
    UMF_STRAT_EAT         = 0x10,
    UMF_STRAT_HARDWARE_BP = 0x20,
} UmfHookStrategyFlag;

typedef uint32_t UmfHookStrategyMask;

UMF_API UmfHookStrategyMask umf_viable_strategies(const UmfMitigationStatus* m);

/* CET helpers */
UMF_API bool umf_is_endbr64(const uint8_t* code);
UMF_API bool umf_pe_has_cet_compat(HMODULE module);

/* ════════════════════════════════════════════════════════════════
 * §TRAMPOLINE — Trampoline pool (Bug I fix: linked-list blocks)
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    UMF_SLOT_FREE,
    UMF_SLOT_ACTIVE,
    UMF_SLOT_PENDING_FREE,
    UMF_SLOT_ZOMBIE,
    UMF_SLOT_QUARANTINED,
} UmfSlotState;

/* Forward declaration — block owns slots, slot points back to block */
typedef struct UmfTrampolineBlock UmfTrampolineBlock;

typedef struct {
    uint8_t*              code;             /* Pointer into block's mapped page */
    size_t                used_size;        /* Bytes of trampoline code written */
    UmfSlotState          state;
    uint64_t              last_used_tick;   /* For GC timing                   */
    int                   gc_retry_count;
    bool                  logged_quarantine;
    RUNTIME_FUNCTION*     rt_entry;         /* For RtlDeleteFunctionTable      */
    UmfTrampolineBlock*   owner_block;      /* Back-pointer (Bug I fix)        */
} UmfTrampolineSlot;

struct UmfTrampolineBlock {
    void*                   base_address;   /* VirtualAlloc'd, page-aligned    */
    UmfTrampolineSlot       slots[UMF_SLOTS_PER_BLOCK];
    size_t                  total_slots;
    struct UmfTrampolineBlock* next;        /* Linked list (Bug I fix)         */
};

typedef struct {
    UmfTrampolineBlock*  head;           /* Linked list head                  */
    size_t               block_count;
    SRWLOCK              lock;
} UmfTrampolinePool;

UMF_API void              umf_trampoline_pool_init(UmfTrampolinePool* pool);
UMF_API void              umf_trampoline_pool_destroy(UmfTrampolinePool* pool);
UMF_API UmfTrampolineSlot* umf_trampoline_pool_allocate_near(
                               UmfTrampolinePool* pool,
                               void* target, size_t size_hint);
UMF_API void              umf_trampoline_pool_release(
                               UmfTrampolinePool* pool,
                               UmfTrampolineSlot* slot);
UMF_API bool              umf_trampoline_finalize(
                               UmfTrampolineSlot* slot,
                               size_t used_size,
                               bool cfg_active);
UMF_API void              umf_trampoline_gc(UmfTrampolinePool* pool);

/* ════════════════════════════════════════════════════════════════
 * §BUILDER — Instruction relocator (Zydis-backed)
 *
 * Steals the first instructions of a target function, relocates any
 * position-dependent instructions (RIP-relative memory operands and
 * relative branches/calls), appends a jump back to the continuation,
 * and records a src→dst instruction-boundary map so threads caught
 * mid-prologue during installation can have their RIP relocated.
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t src_off[UMF_MAX_PROLOGUE_INSNS];  /* Offset of insn start in target */
    uint8_t dst_off[UMF_MAX_PROLOGUE_INSNS];  /* Matching offset in trampoline  */
    int     count;
} UmfRelocMap;

/* Returns the summed length of whole instructions covering at least
 * `min_bytes`, or 0 if decoding fails. */
UMF_API size_t umf_disassemble_prologue_length(const uint8_t* code,
                                                size_t min_bytes);

/* Builds the trampoline body into slot->code. `min_steal` is the number
 * of target bytes the inline patch will overwrite (5 or 14). On success
 * sets slot->used_size, writes the actual stolen byte count to *out_steal,
 * and (optionally) fills *out_map. Returns false if the prologue contains
 * an instruction that cannot be safely relocated or exceeds slot capacity. */
UMF_API bool   umf_build_trampoline_code(void* target,
                                         UmfTrampolineSlot* slot,
                                         size_t min_steal,
                                         size_t* out_steal,
                                         UmfRelocMap* out_map);

/* ════════════════════════════════════════════════════════════════
 * §UNWIND — Exception handling for trampolines (RtlAddFunctionTable)
 * ════════════════════════════════════════════════════════════════ */

UMF_API RUNTIME_FUNCTION* umf_register_unwind_info(void* trampoline,
                                                    size_t code_size);
UMF_API void              umf_unregister_unwind_info(RUNTIME_FUNCTION* rf);

/* ════════════════════════════════════════════════════════════════
 * §FREEZER — Thread freeze/resume
 * ════════════════════════════════════════════════════════════════ */

#define UMF_MAX_THREADS 4096

typedef struct {
    DWORD  thread_ids[UMF_MAX_THREADS];
    HANDLE thread_handles[UMF_MAX_THREADS];
    int    count;
    DWORD  owner_thread_id;   /* Thread that called freeze (excluded) */
} UmfThreadFreezer;

UMF_API bool umf_freeze_all_threads(UmfThreadFreezer* freezer);
UMF_API void umf_resume_all_threads(UmfThreadFreezer* freezer);
UMF_API bool umf_any_thread_in_range(const UmfThreadFreezer* freezer,
                                      uintptr_t range_start,
                                      uintptr_t range_end);

/* ════════════════════════════════════════════════════════════════
 * §HOOKS — Hook registry and batch system
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    UMF_HOOK_INLINE = 0,
    UMF_HOOK_IAT,
    UMF_HOOK_VTABLE,
    UMF_HOOK_GAP,
    UMF_HOOK_EAT,
    UMF_HOOK_HARDWARE_BP,
} UmfHookStrategy;

/* Forward declarations */
typedef struct UmfHookEntry  UmfHookEntry;
typedef struct UmfHookTarget UmfHookTarget;
typedef struct UmfMod        UmfMod;

struct UmfHookEntry {
    void*             hook_func;        /* User's hook function            */
    void*             original_func;    /* Trampoline or next hook in chain */
    int               priority;         /* Higher = earlier in chain       */
    UmfMod*           owner_mod;
    char              debug_name[128];
    bool              skip_original;
    bool              call_original_requested;
    void**            user_original_slot; /* Mod's "call original" pointer  */
    UmfHookEntry*     next;             /* Next in priority chain          */
};

struct UmfHookTarget {
    void*              resolved_address;
    char               canonical_name[UMF_MAX_NAME_LEN];
    UmfHookStrategy    active_strategy;
    UmfHookEntry*      chain_head;
    UmfTrampolineSlot* trampoline;
    RUNTIME_FUNCTION*  rt_entry;
    size_t             original_prologue_size;
    uint8_t*           original_bytes;
    bool               deferred_unhook;
    bool               module_unloaded;  /* owning module was unloaded (dead) */
    UmfRelocMap        reloc_map;        /* src→dst insn map for RIP fixups */
    SRWLOCK            chain_lock;
};

struct UmfMod {
    char    name[128];
    HMODULE module_handle;
    bool    active;
};

/* Registry */
UMF_API void           umf_hook_registry_init(void);
UMF_API UmfHookTarget* umf_hook_registry_find(void* resolved_addr);
UMF_API UmfHookTarget* umf_hook_registry_create(void* resolved_addr,
                                                  const char* dll,
                                                  const char* func);
UMF_API bool           umf_register_hook(const char* dll, const char* func,
                                          void* hook_func, int priority,
                                          UmfMod* mod);
/* Like umf_register_hook, but `original_out` (if non-NULL) is populated
 * with the pointer the hook must call to invoke the original/next hook.
 * The pointer is kept current as the chain is rebuilt. */
UMF_API bool           umf_register_hook_ex(const char* dll, const char* func,
                                            void* hook_func, int priority,
                                            UmfMod* mod, void** original_out);

/* Register a hook directly on a resolved code address (vtable slots,
 * computed addresses, non-exported functions). `name` is cosmetic. */
UMF_API bool           umf_register_hook_addr(void* target_addr,
                                              const char* name,
                                              void* hook_func, int priority,
                                              UmfMod* mod, void** original_out);

/* Re-wire each chain entry's original_func to the next hook (or the
 * trampoline for the last entry) and refresh mod "call original" slots. */
UMF_API void           umf_rebuild_target_chain(UmfHookTarget* target);

/* Pick the best hook strategy for `addr` given active mitigations. */
UMF_API UmfHookStrategy umf_select_strategy(void* addr,
                                            const UmfMitigationStatus* m);

/* Tear down every hook target whose code lives in [base, base+size).
 * Called by the DLL watchdog when a module is unloaded: releases the
 * trampoline, drops the chain, and repoints mod "call original" pointers
 * to a safe stub so a late call returns 0 instead of faulting. */
UMF_API void           umf_registry_on_module_unload(uintptr_t base,
                                                      uintptr_t size);

/* Batch */
UMF_API void umf_queue_hook_enable(UmfHookTarget* target);
UMF_API void umf_queue_hook_disable(UmfHookTarget* target);
UMF_API bool umf_apply_pending_batch(void);

/* ════════════════════════════════════════════════════════════════
 * §RESOLVE — Function resolution (with forwarding)
 * ════════════════════════════════════════════════════════════════ */

UMF_API void* umf_resolve_function(const char* dll, const char* func);

/* ════════════════════════════════════════════════════════════════
 * §WATCHDOG — DLL load/unload notifications (crash-safe hook teardown)
 * ════════════════════════════════════════════════════════════════ */

UMF_API bool umf_dll_watchdog_start(void);
UMF_API void umf_dll_watchdog_stop(void);

/* ════════════════════════════════════════════════════════════════
 * §LUA — Lua sandbox
 * ════════════════════════════════════════════════════════════════ */

/* Forward-declare lua_State to avoid forcing Lua headers on all consumers */
struct lua_State;

UMF_API struct lua_State* umf_lua_create_sandbox(void);
UMF_API void              umf_lua_destroy_sandbox(struct lua_State* L);
UMF_API int               umf_lua_sandboxed_pcall(struct lua_State* L,
                                                   int nargs, int nresults,
                                                   int errfunc);

/* ════════════════════════════════════════════════════════════════
 * §INIT — Framework lifecycle
 * ════════════════════════════════════════════════════════════════ */

UMF_API bool umf_init(void);
UMF_API void umf_shutdown(void);

/* Shared framework state (defined in umf_init.c). Internal to the
 * runtime module — populated by umf_init(). */
extern UmfTrampolinePool  g_trampoline_pool;
extern UmfMitigationStatus g_mitigations;

#ifdef __cplusplus
}
#endif

#endif /* UMF_H */
