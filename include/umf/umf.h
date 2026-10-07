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

/* Mod kind + capability flags (see §MOD) */
typedef enum {
    UMF_MOD_NATIVE = 0,   /* a DLL exporting umf_mod_init/umf_mod_shutdown */
    UMF_MOD_LUA,          /* a sandboxed Lua script                       */
} UmfModType;

typedef enum {
    UMF_CAP_HOOK       = 0x01,   /* may install hooks            */
    UMF_CAP_READ_MEM   = 0x02,   /* may read target memory       */
    UMF_CAP_WRITE_MEM  = 0x04,   /* may write target memory      */
    UMF_CAP_OVERLAY    = 0x08,   /* may draw an overlay          */
    UMF_CAP_FILE_IO    = 0x10,   /* may touch the filesystem     */
} UmfCapability;

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
    char       name[128];
    HMODULE    module_handle;
    bool       active;
    UmfModType type;
    uint32_t   capabilities;   /* bitmask of UmfCapability           */
    void*      lua_state;      /* struct lua_State* for Lua mods      */
    char       version[32];
    int        priority;
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

/* Enumerate installed hook targets (for the inspector / IPC). */
typedef struct {
    char  name[UMF_MAX_NAME_LEN];
    void* address;
    int   strategy;     /* UmfHookStrategy */
    int   chain_len;    /* hooks on this target */
    bool  installed;    /* trampoline present */
} UmfHookInfo;
UMF_API int            umf_hook_list(UmfHookInfo* out, int max);

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
 * §IAT — Import Address Table hooking (data-only; ACG-compatible)
 *
 * Overwrites a function pointer in a module's import table instead of
 * patching code. Works where inline hooks cannot: under ACG (Chromium
 * renderers), on tiny functions, and on code pages that must stay RX.
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    void**  iat_slot;   /* address of the IAT entry (holds the function ptr) */
    void*   original;   /* original resolved pointer (for "call original")   */
    bool    is_delay;   /* located in a delay-load IAT                       */
    HMODULE module;     /* importing module                                  */
} UmfIatLocation;

/* Locate the IAT slot in `module` importing `dll!func`. Matches by name via
 * the import-name table, then by the forwarder-resolved address (handles
 * kernel32→kernelbase forwarding, ApiSets, and bound imports). */
UMF_API bool umf_find_iat_entry(HMODULE module, const char* dll,
                                const char* func, UmfIatLocation* out);

/* Overwrite the IAT slot with `hook`. Pointer-sized aligned write is atomic
 * on x64, so no trampoline and no thread freeze are needed. `out_original`
 * (optional) receives the pointer to call for the original; `out_loc`
 * (optional) receives the location for a later umf_unhook_iat(). */
UMF_API bool umf_hook_iat(HMODULE module, const char* dll, const char* func,
                          void* hook, void** out_original,
                          UmfIatLocation* out_loc);

/* Restore an IAT slot previously hooked by umf_hook_iat. */
UMF_API bool umf_unhook_iat(const UmfIatLocation* loc);

/* Fallback for dynamically-resolved APIs (#4): inline-hook GetProcAddress so
 * that whenever any module resolves `target_resolved`, `hook` is returned
 * instead. Requires inline hooking to be available (non-ACG). */
UMF_API bool umf_install_getprocaddress_hook(void* target_resolved, void* hook);

/* ════════════════════════════════════════════════════════════════
 * §VTABLE — C++ virtual method hooking (data-only)
 *
 * Swaps a function pointer in a vtable. Class-wide (affects all instances
 * sharing the vtable). The hook receives the object as its first argument
 * (x64: in RCX) exactly as the virtual method would.
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    void** vtable;      /* the vtable array        */
    int    index;       /* slot index that was hooked */
    void*  original;    /* saved original pointer  */
} UmfVtableLocation;

UMF_API bool umf_hook_vtable(void** vtable, int index, void* hook,
                             void** out_original, UmfVtableLocation* out_loc);
UMF_API bool umf_hook_vtable_object(void* object, int index, void* hook,
                                    void** out_original, UmfVtableLocation* out_loc);
UMF_API bool umf_unhook_vtable(const UmfVtableLocation* loc);

/* ════════════════════════════════════════════════════════════════
 * §EAT — Export Address Table hooking (data-only)
 *
 * Rewrites an export's 32-bit RVA so future GetProcAddress-style
 * resolutions of that export return the hook. For self-resolving plugins
 * and exported DLLs that bypass the importer's IAT. When the hook lies
 * beyond the module's 4 GiB RVA window, an above-base jump stub is placed.
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    HMODULE module;
    DWORD*  eat_slot;      /* address of the DWORD RVA in the EAT */
    DWORD   original_rva;  /* saved RVA                           */
    void*   stub;          /* above-base jmp stub, or NULL        */
} UmfEatLocation;

UMF_API bool umf_hook_eat(HMODULE module, const char* func, void* hook,
                          void** out_original, UmfEatLocation* out_loc);
UMF_API bool umf_unhook_eat(const UmfEatLocation* loc);

/* ════════════════════════════════════════════════════════════════
 * §HWBP — Hardware-breakpoint hooking (no memory writes at all)
 *
 * Uses the x86-64 debug registers (Dr0–Dr3) + a vectored exception
 * handler. Nothing in the target is modified, so this is the only
 * strategy that works under HVCI+ACG when no IAT/EAT slot exists.
 * Limited to 4 simultaneous breakpoints and applied per-thread.
 *
 * Calling the original: a hooked function invokes the original by first
 * calling umf_hwbp_enter_original() and then calling the target through a
 * function pointer. The handler lets that one re-entry pass through using
 * the CPU Resume Flag, so no trampoline or code edit is required.
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    int   slot;        /* debug-register index 0..3 */
    void* target;      /* breakpoint address        */
} UmfHwbpLocation;

UMF_API bool umf_hook_hwbp(void* target, void* hook, UmfHwbpLocation* out_loc);
UMF_API bool umf_unhook_hwbp(const UmfHwbpLocation* loc);
UMF_API void umf_hwbp_enter_original(void);  /* arm a one-shot pass-through */

/* ════════════════════════════════════════════════════════════════
 * §XFG — eXtended Flow Guard detection + hash handling
 *
 * /guard:xfg modules store a type hash in the 8 bytes preceding each valid
 * indirect-call target and verify it at the call site. Detection uses the
 * real PE loader-config GuardFlags bit (IMAGE_GUARD_XFG_ENABLED), not a
 * heuristic. Inline-hooking an XFG target is only safe if the hook carries
 * a matching hash slot; when it cannot be guaranteed the engine refuses the
 * inline hook rather than risk a control-flow-protection fault.
 * ════════════════════════════════════════════════════════════════ */

UMF_API bool umf_module_has_xfg(HMODULE module);

/* Copy the 8-byte XFG hash preceding `target` onto the slot preceding `hook`.
 * Fails (and writes nothing) if that slot would cross a page boundary.
 * *out_did is set true only when a hash was actually written. */
UMF_API bool umf_xfg_copy_hash(void* target, void* hook, bool* out_did);

/* ════════════════════════════════════════════════════════════════
 * §AOB — Array-of-bytes (byte-pattern) scanning
 *
 * Finds code/data by byte signature instead of a fixed address, so mods
 * survive module rebuilds (the technique trainers and CE tables rely on).
 * Pattern syntax: whitespace-separated tokens of 1–2 nibbles with '?'/'x'
 * wildcards. Examples:
 *     "48 8B ?? 89 05"   full-byte wildcard
 *     "4? ?F"            nibble wildcards
 *     "E9 ?? ?? ?? ??"   near jmp
 * ════════════════════════════════════════════════════════════════ */

/* First match in `module_name` (NULL or "" = main executable). */
UMF_API void* umf_aob_scan(const char* pattern, const char* module_name);

/* First match within an explicit [start, start+size) region. */
UMF_API void* umf_aob_scan_range(const char* pattern,
                                 const void* start, size_t size);

/* Collect up to `max` matches from `module_name`. Returns the count. */
UMF_API int   umf_aob_scan_all(const char* pattern, const char* module_name,
                               void** out, int max);

/* ════════════════════════════════════════════════════════════════
 * §MEM — Capability-gated memory access + region enumeration
 *
 * The scanner, trainers, and CE-table importer build on this. Reads/writes
 * are fault-guarded (an unmapped address returns false, never crashes) and
 * gated by the active mod's capabilities: when an owner is set with
 * umf_mem_set_owner(), reads require UMF_CAP_READ_MEM and writes require
 * UMF_CAP_WRITE_MEM. A NULL owner (engine/tests) is trusted.
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    uintptr_t base;      /* region base address           */
    size_t    size;      /* region size in bytes          */
    uint32_t  protect;   /* PAGE_* protection             */
    uint32_t  state;     /* MEM_COMMIT / MEM_FREE / ...   */
    uint32_t  type;      /* MEM_IMAGE / MEM_MAPPED / ...  */
} UmfMemRegion;

/* Set the mod whose capabilities gate memory access (NULL = trusted). */
UMF_API void umf_mem_set_owner(UmfMod* mod);

/* Fault-guarded read/write. Return false on access violation or on a
 * capability denial. Write temporarily makes the page(s) writable. */
UMF_API bool umf_mem_read(const void* addr, void* out, size_t size);
UMF_API bool umf_mem_write(void* addr, const void* in, size_t size);

/* Enumerate committed regions into `out` (up to `max`). Returns the count. */
UMF_API int  umf_mem_enum_regions(UmfMemRegion* out, int max);

/* Find the region containing `addr` (committed or not). Returns false if the
 * address is not mapped at all. */
UMF_API bool umf_mem_query(const void* addr, UmfMemRegion* out);

/* ════════════════════════════════════════════════════════════════
 * §OVERLAY — In-target Dear ImGui DX11 overlay
 *
 * Draws a transparent ImGui UI over a Direct3D 11 application. Hooks the
 * swapchain's Present/ResizeBuffers (vtable) and the window procedure.
 * swapchain/hwnd are passed as opaque void* to keep d3d/dxgi out of this
 * header; pass IDXGISwapChain* and HWND.
 * ════════════════════════════════════════════════════════════════ */

typedef void (*UmfOverlayFrameFn)(void);

UMF_API bool umf_overlay_dx11_init(void* swapchain, void* hwnd);
UMF_API void umf_overlay_dx11_shutdown(void);
UMF_API void umf_overlay_set_frame_callback(UmfOverlayFrameFn fn);
UMF_API void umf_overlay_block_input(bool block);   /* 1 = ImGui captures input */
UMF_API bool umf_overlay_is_active(void);
UMF_API void umf_overlay_apply_modern_theme(void);

/* Minimal draw helpers so callers never touch ImGui directly (ImGui's context
 * is a per-module global; with a static DLL-linked ImGui, out-of-module ImGui
 * calls would see a null context). The frame callback runs inside the runtime,
 * so these are safe to call from it. */
UMF_API void umf_imgui_begin(const char* title);
UMF_API void umf_imgui_text(const char* text);
UMF_API void umf_imgui_end(void);

/* ════════════════════════════════════════════════════════════════
 * §MOD — Mod manifest + loader
 *
 * A mod is described by a mod.json manifest and loaded from its directory.
 * Native mods are DLLs exporting:
 *     bool umf_mod_init(UmfMod* self);      // return true on success
 *     void umf_mod_shutdown(UmfMod* self);  // optional
 * The mod calls the umf_* API directly (it imports umf_runtime). Declared
 * capabilities gate privileged operations (e.g. hook installation).
 * ════════════════════════════════════════════════════════════════ */

#define UMF_MOD_MAX_DEPS 16

typedef struct {
    char       name[128];
    char       version[32];
    UmfModType type;
    char       entry[260];                 /* DLL or .lua filename        */
    int        priority;
    char       deps[UMF_MOD_MAX_DEPS][128]; /* dependency mod names        */
    int        dep_count;
    uint32_t   capabilities;               /* parsed capability bitmask   */
} UmfModManifest;

typedef bool (*UmfModInitFn)(UmfMod* self);
typedef void (*UmfModShutdownFn)(UmfMod* self);

/* Manifest parsing */
UMF_API bool umf_parse_manifest_string(const char* json, UmfModManifest* out);
UMF_API bool umf_parse_manifest_file(const char* path, UmfModManifest* out);

/* Order `n` manifests so dependencies precede dependents (DFS post-order).
 * Writes n indices to out_order; returns false on a dependency cycle. */
UMF_API bool umf_mod_topo_sort(const UmfModManifest* mods, int n, int* out_order);

/* Loading */
UMF_API bool    umf_mod_load(const char* manifest_path);   /* one mod       */
UMF_API int     umf_mod_load_dir(const char* mods_dir);    /* scan + order  */
UMF_API void    umf_mod_unload_all(void);
UMF_API UmfMod* umf_mod_find(const char* name);
UMF_API int     umf_mod_count(void);

/* Enumerate loaded mods (for the manager / IPC). */
typedef struct {
    char     name[128];
    char     version[32];
    int      type;          /* UmfModType   */
    uint32_t capabilities;
    int      priority;
    bool     active;
} UmfModInfo;
UMF_API int     umf_mod_list(UmfModInfo* out, int max);

/* ════════════════════════════════════════════════════════════════
 * §IPC — JSON-RPC 2.0 server over a named pipe (UMF Studio transport)
 *
 * Listens on \\.\pipe\umf-studio-{pid} with newline-delimited JSON-RPC 2.0.
 * Methods: listHooks, listMods, getMitigations, getProcessInfo, evalLua,
 * subscribe (log events). The runtime is the server; Studio is the client.
 * ════════════════════════════════════════════════════════════════ */

UMF_API bool        umf_ipc_start(void);
UMF_API void        umf_ipc_stop(void);
UMF_API bool        umf_ipc_is_running(void);
UMF_API const char* umf_ipc_pipe_name(void);   /* \\.\pipe\umf-studio-{pid} */

/* Called from the logger to stream a line to subscribed IPC clients. Not
 * part of the public modding surface. */
UMF_API void        umf_ipc_on_log(int level, const char* msg);

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

/* Install the `umf` Lua module (umf.hook / umf.log / umf.call_original /
 * umf.resolve) bound to `mod` (for capability checks; NULL = trusted). */
UMF_API void umf_lua_setup_api(struct lua_State* L, UmfMod* mod);

/* Compile and run a Lua chunk. Returns true on success. */
UMF_API bool umf_lua_run_string(struct lua_State* L, const char* chunk);

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
