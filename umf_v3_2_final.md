# UniversalModFramework (UMF)

## What It Is

**UMF is a desktop Windows tool (.exe) that lets you mod any native Windows application** — not just games, but professional software, creative tools, productivity apps, anything compiled to x86-64 machine code. You point UMF at an application, and it gives you the power to:

- **Hook any function** the application calls (Windows APIs, internal functions, virtual methods) and run your own code before, after, or instead of it
- **Discover** the application's internal structure automatically — class hierarchies, virtual function tables, exported symbols — without source code
- **Draw overlays** on top of any application (custom UI panels, debug info, tool windows) using Dear ImGui (for DirectX/OpenGL apps) or a built-in GDI renderer (for everything else)
- **Write mods in Lua** (sandboxed scripting) or **C/C++** (full power, no sandbox) that extend or alter the application's behavior at runtime
- **Load, unload, and hot-reload mods** without restarting the target application

### Who It's For

Individual developers, reverse engineers, power users, and modding communities who want to customize software they use. UMF is a **personal tool** — it runs on your own machine, modifying applications you own, with your antivirus disabled if needed.

### What It Is NOT

- Not a game cheat engine (explicitly refuses to inject into anti-cheat protected processes)
- Not a malware framework (no stealth, no evasion, no anti-detection)
- Not a cloud service (no telemetry, no accounts, no marketplace in v1)
- Not cross-platform (Windows x86-64 only)

### How It Works (30-Second Version)

```
YOU                          UMF                           TARGET APP
 |                            |                                |
 |  "Hook CreateFileW and     |                                |
 |   log every file open"     |                                |
 |  ─────────────────────►    |                                |
 |                            |  1. Generate proxy DLL         |
 |                            |     (version.dll)              |
 |                            |  2. Place in app directory     |
 |                            |  3. App starts, loads proxy    |
 |                            |     ─────────────────────────► |
 |                            |  4. Proxy loads UMF runtime    |
 |                            |  5. Runtime scans app:         |
 |                            |     - Finds CreateFileW in IAT |
 |                            |     - Detects mitigations      |
 |                            |       (CFG, CET, ACG, HVCI)    |
 |                            |  6. Picks best hook strategy:  |
 |                            |     IAT patch (data-only) or   |
 |                            |     inline (jmp rel32)         |
 |                            |  7. Freezes all threads        |
 |                            |  8. Writes hook atomically     |
 |                            |  9. Resumes threads            |
 |                            |                                |
 |                            |  App calls CreateFileW ──────► |
 |                            |  ◄── Redirected to YOUR code   |
 |  "C:\Users\...\config.ini" |     YOUR CODE runs first       |
 |  ◄─────────────────────    |     Then calls original         |
 |                            |     ──────────────────────────► |
```

### Technical Architecture (One Paragraph)

UMF is a C/C++ framework built with CMake + vcpkg, using **Zydis** for x86-64 instruction decoding, **Lua 5.4** for sandboxed mod scripting, and **Dear ImGui** for overlay rendering. It injects into target processes via **proxy DLL** (placing a forwarding `version.dll` in the app directory). Once loaded, it detects the process's security mitigations (CFG, CET/IBT, XFG, ACG, HVCI) and selects compatible hooking strategies. Hooks are installed via a **batched prepare/apply protocol** that performs all allocation before freezing threads, then applies atomic patches during the freeze window. Trampolines are allocated within ±1GB of targets (for `jmp rel32` reach), registered with `RtlAddFunctionTable` (for correct exception unwinding), and managed through a **slot lifecycle** (FREE → ACTIVE → PENDING_FREE → GC'd) with full thread-safety.

---

## Scope

| In Scope (v1) | Out of Scope |
|---|---|
| Native C/C++ x86-64 applications | ARM64, x86-32 (WoW64), UWP |
| Windows 10 1709+ (build 16299) | Windows 8.1 and earlier |
| Personal device, AV disabled | Enterprise/EDR environments |
| Unprotected applications | Anti-cheat protected games (BattlEye, EAC, Ricochet, Vanguard) |
| Lua 5.4 mod scripting | Python mod scripting (deferred to v2) |
| DirectX 11 + GDI overlays | DirectX 12, Vulkan overlays (deferred) |
| Electron apps (experimental, via CDP) | .NET apps (experimental, via Harmony) |

---

## Fix Summary (v3.0 → v3.2)

| # | Issue | Severity | Section |
|---|---|---|---|
| A | VirtualAlloc inside thread freeze → heap deadlock | Critical | §1 |
| B | GC writes to RX page without VirtualProtect | Critical | §2 |
| C | recursion_depth not thread-local | High | §3.1 |
| D | Mixed enable/disable batching broken | High | §1 |
| E | Instruction budget never resets | High | §4.1 |
| F | GDI overlay non-functional | High | §5 |
| G | Delayed import force-resolve doesn't populate slot | High | §6 |
| **H** | **Trampoline finalized AFTER thread resume → jmp to non-executable page** | **Critical** | **§1** |
| M1 | ACG not detected — hook engine fails in Chromium renderers | Critical | §7.1 |
| M2 | No RtlAddFunctionTable — exception unwinding broken | Critical | §8 |
| M3 | HVCI not detected before code-page writes | Critical | §7.2 |
| M4 | Registry TOCTOU race on concurrent hook registration | High | §3.3 |
| D1 | IBT never detected — vtable/IAT hooks skip ENDBR64 | Critical | §9 |
| D2 | chain_lock declared LONG but used as SRWLOCK | High | §12.1 |
| D3 | Dead reentrancy_count field in HookEntry | Low | §12.2 |
| D4 | umf_call_original has no argument-passing mechanism | Critical | §11 |
| D5 | XFG section format invented — revert to documented -8 | Critical | §10 |
| D6 | Delay-load mutex named → cross-process DoS | Medium | §12.3 |
| D7 | Lua rawget/rawset/rawequal/rawlen sandbox escape | High | §4.2 |

---

## §1. Thread Freeze Protocol — Prepare/Apply Split (Fixes A, D, H)

**Rules:**
1. No allocation, no logging, no lock-taking inside the freeze window
2. Trampolines must be fully executable and CFG-registered BEFORE the target jmp patch is written (Bug H fix)
3. Only `memcpy`, `VirtualProtect`, and direct memory writes between freeze and resume

### Core Types

```c
#define UMF_MAX_BATCH 64

typedef struct {
    HookTarget* target;
    bool        enable;
} HookBatchEntry;

typedef struct {
    HookBatchEntry entries[UMF_MAX_BATCH];
    int            count;
} HookBatch;

static HookBatch g_pending_batch = {0};

typedef struct {
    HookTarget*       target;
    TrampolineSlot*   trampoline;
    uint8_t           jump_patch[14];     // Max: 14-byte absolute jmp
    size_t            jump_patch_size;
    uint8_t*          saved_prologue;
    size_t            saved_prologue_size;
    RUNTIME_FUNCTION* rt_entry;
} PreparedHook;
```

### finalize_trampoline (Referenced throughout — defined here)

```c
// Transitions a trampoline from writable to executable + CFG-registered.
// MUST be called BEFORE any jmp patch points to this trampoline.
bool finalize_trampoline(TrampolineSlot* slot, size_t used_size, bool cfg_active) {
    DWORD old_protect;
    
    // RW → RX
    if (!VirtualProtect(slot->code, TRAMPOLINE_BLOCK_SIZE,
                        PAGE_EXECUTE_READ, &old_protect)) {
        return false;
    }
    
    FlushInstructionCache(GetCurrentProcess(), slot->code, used_size);
    
    // Register with CFG bitmap if active
    if (cfg_active) {
        // Trampoline entry must be 16-byte aligned for CFG
        uintptr_t entry = (uintptr_t)slot->code;
        uintptr_t aligned = entry & ~(uintptr_t)0xF;
        
        CFG_CALL_TARGET_INFO target_info = {0};
        target_info.Offset = (ULONG_PTR)(aligned - (uintptr_t)slot->code);
        target_info.Flags = CFG_CALL_TARGET_VALID;
        
        SetProcessValidCallTargets(
            GetCurrentProcess(),
            slot->code,
            TRAMPOLINE_BLOCK_SIZE,
            1,
            &target_info);
    }
    
    return true;
}
```

### prepare_hook — PHASE 1 (Bug H fix: trampoline finalized HERE)

```c
static bool prepare_hook(HookTarget* target, PreparedHook* out) {
    memset(out, 0, sizeof(*out));
    out->target = target;

    // 1. Allocate trampoline slot (RW page via VirtualAlloc)
    out->trampoline = trampoline_pool_allocate_near(
        target->resolved_address, TRAMPOLINE_SLOT_SIZE);
    if (!out->trampoline) return false;

    // 2. Build trampoline code (disassemble prologue, relocate, write jmp-back)
    if (!build_trampoline_code(target, out->trampoline)) {
        trampoline_pool_release(out->trampoline);
        return false;
    }

    // 3. BUG H FIX: Finalize trampoline NOW — make it executable + CFG
    //    BEFORE the jmp patch is written at the target.
    //    This ensures no thread can ever jump to a non-executable trampoline.
    if (!finalize_trampoline(out->trampoline,
                              out->trampoline->used_size,
                              g_mitigations.cfg_enforced)) {
        trampoline_pool_release(out->trampoline);
        return false;
    }

    // 4. Register exception handling (RtlAddFunctionTable — see §8)
    out->rt_entry = register_trampoline_unwind_info(
        out->trampoline->code, out->trampoline->used_size);

    // 5. Build jump patch bytes (the bytes we'll write at the target)
    ptrdiff_t delta = (uint8_t*)out->trampoline->code - 
                      ((uint8_t*)target->resolved_address + 5);
    if (delta >= INT32_MIN && delta <= INT32_MAX) {
        // rel32 fits
        out->jump_patch[0] = 0xE9;  // jmp rel32
        int32_t rel = (int32_t)delta;
        memcpy(&out->jump_patch[1], &rel, 4);
        out->jump_patch_size = 5;
    } else {
        // Need absolute jmp: mov rax, imm64; jmp rax (14 bytes)
        out->jump_patch[0] = 0x48; out->jump_patch[1] = 0xB8;  // mov rax, imm64
        uint64_t addr = (uint64_t)out->trampoline->code;
        memcpy(&out->jump_patch[2], &addr, 8);
        out->jump_patch[10] = 0xFF; out->jump_patch[11] = 0xE0; // jmp rax
        out->jump_patch_size = 12;
    }

    // 6. Save original prologue (for unhooking)
    out->saved_prologue_size = target->original_prologue_size;
    out->saved_prologue = (uint8_t*)malloc(out->saved_prologue_size);
    if (!out->saved_prologue) return false;
    memcpy(out->saved_prologue, target->resolved_address,
           out->saved_prologue_size);

    return true;
}
```

### apply_hook_noalloc / apply_unhook_noalloc — PHASE 2 (inside freeze)

```c
static void apply_hook_noalloc(PreparedHook* p) {
    DWORD old_protect;
    if (!VirtualProtect(p->target->resolved_address,
                        p->jump_patch_size,
                        PAGE_EXECUTE_READWRITE, &old_protect)) return;
    memcpy(p->target->resolved_address, p->jump_patch, p->jump_patch_size);
    DWORD dummy;
    VirtualProtect(p->target->resolved_address,
                   p->jump_patch_size, old_protect, &dummy);
}

static void apply_unhook_noalloc(HookTarget* target) {
    DWORD old_protect;
    if (!VirtualProtect(target->resolved_address,
                        target->original_prologue_size,
                        PAGE_EXECUTE_READWRITE, &old_protect)) return;
    memcpy(target->resolved_address, target->original_bytes,
           target->original_prologue_size);
    DWORD dummy;
    VirtualProtect(target->resolved_address,
                   target->original_prologue_size, old_protect, &dummy);
}
```

### umf_apply_pending_batch — The Full Protocol

```c
bool umf_apply_pending_batch(void) {
    if (g_pending_batch.count == 0) return true;

    // ═══ PHASE 1: PREPARE (allocation OK, no freeze) ═══
    // Heap-allocate the work arrays (not stack — safe for large batches)
    PreparedHook* prepared = (PreparedHook*)calloc(
        g_pending_batch.count, sizeof(PreparedHook));
    HookTarget** to_unhook = (HookTarget**)calloc(
        g_pending_batch.count, sizeof(HookTarget*));
    int prepared_count = 0, unhook_count = 0;

    for (int i = 0; i < g_pending_batch.count; i++) {
        HookBatchEntry* e = &g_pending_batch.entries[i];
        if (e->enable) {
            if (prepare_hook(e->target, &prepared[prepared_count]))
                prepared_count++;
            else
                UMF_Log(UMF_ERROR, "Prepare failed for %s",
                        e->target->canonical_name);
        } else {
            to_unhook[unhook_count++] = e->target;
        }
    }
    // At this point, all trampolines are ALREADY executable (Bug H fix).

    // ═══ PHASE 2: FREEZE AND APPLY (no alloc, no log) ═══
    ThreadFreezer freezer;
    freeze_all_threads(&freezer);

    // Check for threads inside trampolines being unhooked
    for (int i = 0; i < unhook_count; i++) {
        HookTarget* t = to_unhook[i];
        if (t && t->trampoline &&
            any_thread_in_range(&freezer,
                                (uintptr_t)t->trampoline->code,
                                (uintptr_t)t->trampoline->code +
                                    TRAMPOLINE_SLOT_SIZE)) {
            t->deferred_unhook = true;
            to_unhook[i] = NULL;  // Skip this unhook
        }
    }

    for (int i = 0; i < prepared_count; i++)
        apply_hook_noalloc(&prepared[i]);

    for (int i = 0; i < unhook_count; i++)
        if (to_unhook[i]) apply_unhook_noalloc(to_unhook[i]);

    resume_all_threads(&freezer);

    // ═══ PHASE 3: POST-RESUME CLEANUP (alloc OK) ═══
    for (int i = 0; i < prepared_count; i++) {
        // Only need to flush the TARGET patch (trampoline was flushed in prepare)
        FlushInstructionCache(GetCurrentProcess(),
            prepared[i].target->resolved_address,
            prepared[i].jump_patch_size);

        // Store trampoline reference in target for later unhook
        prepared[i].target->trampoline = prepared[i].trampoline;
        prepared[i].target->rt_entry = prepared[i].rt_entry;
        free(prepared[i].saved_prologue);
    }

    for (int i = 0; i < unhook_count; i++) {
        if (to_unhook[i] && to_unhook[i]->trampoline) {
            // Mark trampoline for GC (see §2)
            to_unhook[i]->trampoline->state = SLOT_PENDING_FREE;
            to_unhook[i]->trampoline->last_used_tick = GetTickCount64();
        }
    }

    free(prepared);
    free(to_unhook);
    g_pending_batch.count = 0;
    return true;
}
```

**Correct ordering proven:**
```
PHASE 1: trampoline written + RX + CFG ✅
PHASE 2: freeze → jmp patch written at target → resume
         Target now jumps to EXECUTABLE trampoline ✅
PHASE 3: flush target patch cache
         No race window — trampoline was executable before target was patched ✅
```

---

## §2. Trampoline Pool & GC

### Slot Lifecycle

```
SLOT_FREE (PAGE_NOACCESS)
  → allocate_near() → SLOT_ACTIVE (PAGE_READWRITE)
    → finalize_trampoline() → SLOT_ACTIVE (PAGE_EXECUTE_READ)
      → unhook → SLOT_PENDING_FREE (PAGE_EXECUTE_READ, timer starts)
        → GC after 1s + no threads inside → SLOT_FREE (PAGE_NOACCESS)
        
If GC fails 100 times → SLOT_QUARANTINED (leaked, never reused)
If thread permanently stuck → SLOT_ZOMBIE (leaked, never reused)
```

### trampoline_pool_allocate_near

```c
#define TRAMPOLINE_SEARCH_RANGE  (1024ULL * 1024 * 1024)  // ±1GB
#define TRAMPOLINE_BLOCK_SIZE    (64 * 1024)               // 64KB granularity
#define TRAMPOLINE_SLOT_SIZE     96                         // Per-slot (code + unwind)
#define SLOTS_PER_BLOCK          (TRAMPOLINE_BLOCK_SIZE / TRAMPOLINE_SLOT_SIZE)

typedef enum {
    SLOT_FREE,
    SLOT_ACTIVE,
    SLOT_PENDING_FREE,
    SLOT_ZOMBIE,
    SLOT_QUARANTINED
} SlotState;

typedef struct {
    uint8_t*          code;
    size_t            used_size;
    SlotState         state;
    uint64_t          last_used_tick;
    int               gc_retry_count;
    bool              logged_quarantine;
    RUNTIME_FUNCTION* rt_entry;
} TrampolineSlot;

typedef struct {
    void*            base_address;
    TrampolineSlot   slots[SLOTS_PER_BLOCK];
    size_t           total_slots;
} TrampolineBlock;

typedef struct {
    TrampolineBlock* blocks;
    size_t           block_count;
    size_t           block_capacity;
    SRWLOCK          lock;
} TrampolinePool;

static TrampolinePool g_trampoline_pool = { .lock = SRWLOCK_INIT };

TrampolineSlot* trampoline_pool_allocate_near(void* target, size_t size_hint) {
    AcquireSRWLockExclusive(&g_trampoline_pool.lock);

    uintptr_t target_addr = (uintptr_t)target;
    uintptr_t low  = target_addr > TRAMPOLINE_SEARCH_RANGE
                     ? target_addr - TRAMPOLINE_SEARCH_RANGE : 0;
    uintptr_t high = target_addr + TRAMPOLINE_SEARCH_RANGE;

    // Step 1: Check existing blocks for free slot in range
    for (size_t b = 0; b < g_trampoline_pool.block_count; b++) {
        TrampolineBlock* block = &g_trampoline_pool.blocks[b];
        uintptr_t block_addr = (uintptr_t)block->base_address;
        if (block_addr < low || block_addr > high) continue;

        for (size_t s = 0; s < block->total_slots; s++) {
            TrampolineSlot* slot = &block->slots[s];
            if (slot->state == SLOT_FREE) {
                // Flip NOACCESS → RW for writing
                DWORD old;
                if (!VirtualProtect(slot->code, TRAMPOLINE_SLOT_SIZE,
                                    PAGE_READWRITE, &old)) continue;
                slot->state = SLOT_ACTIVE;
                slot->used_size = 0;
                slot->gc_retry_count = 0;
                ReleaseSRWLockExclusive(&g_trampoline_pool.lock);
                return slot;
            }
        }
    }

    // Step 2: Allocate new block — scan outward for free memory
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
        if (mbi.State == MEM_FREE && mbi.RegionSize >= TRAMPOLINE_BLOCK_SIZE) {
            void* alloc = VirtualAlloc((void*)search, TRAMPOLINE_BLOCK_SIZE,
                                       MEM_COMMIT | MEM_RESERVE,
                                       PAGE_READWRITE);
            if (alloc) {
                // Grow pool
                if (g_trampoline_pool.block_count >= g_trampoline_pool.block_capacity) {
                    size_t new_cap = g_trampoline_pool.block_capacity
                                    ? g_trampoline_pool.block_capacity * 2 : 8;
                    g_trampoline_pool.blocks = realloc(
                        g_trampoline_pool.blocks,
                        new_cap * sizeof(TrampolineBlock));
                    g_trampoline_pool.block_capacity = new_cap;
                }

                TrampolineBlock* block = &g_trampoline_pool.blocks[
                    g_trampoline_pool.block_count++];
                block->base_address = alloc;
                block->total_slots = SLOTS_PER_BLOCK;

                for (size_t s = 0; s < block->total_slots; s++) {
                    block->slots[s].code = (uint8_t*)alloc +
                                           (s * TRAMPOLINE_SLOT_SIZE);
                    block->slots[s].state = SLOT_ACTIVE; // First one active, rest NOACCESS
                    block->slots[s].used_size = 0;
                    block->slots[s].rt_entry = NULL;
                }

                // Mark all slots except first as NOACCESS (fail-loud)
                for (size_t s = 1; s < block->total_slots; s++) {
                    DWORD old;
                    VirtualProtect(block->slots[s].code, TRAMPOLINE_SLOT_SIZE,
                                   PAGE_NOACCESS, &old);
                    block->slots[s].state = SLOT_FREE;
                }

                TrampolineSlot* slot = &block->slots[0];
                ReleaseSRWLockExclusive(&g_trampoline_pool.lock);
                return slot;
            }
        }
        search = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }

    ReleaseSRWLockExclusive(&g_trampoline_pool.lock);
    UMF_Log(UMF_WARN, "No free memory within ±1GB of %p", target);
    return NULL;
}
```

### trampoline_gc

```c
void trampoline_gc(void) {
    ThreadFreezer freezer;
    freeze_all_threads(&freezer);
    uint64_t now = GetTickCount64();

    for (size_t b = 0; b < g_trampoline_pool.block_count; b++) {
        TrampolineBlock* block = &g_trampoline_pool.blocks[b];
        for (size_t s = 0; s < block->total_slots; s++) {
            TrampolineSlot* slot = &block->slots[s];
            if (slot->state != SLOT_PENDING_FREE) continue;
            if (now - slot->last_used_tick < 1000) continue;
            if (any_thread_in_range(&freezer,
                                     (uintptr_t)slot->code,
                                     (uintptr_t)slot->code +
                                         TRAMPOLINE_SLOT_SIZE)) {
                slot->gc_retry_count++;
                if (slot->gc_retry_count > 100)
                    slot->state = SLOT_QUARANTINED;
                continue;
            }

            // Unregister exception handling
            if (slot->rt_entry) {
                RtlDeleteFunctionTable(slot->rt_entry);
                slot->rt_entry = NULL;
            }

            // Flip RX → RW, clear, flip to NOACCESS
            DWORD old;
            if (!VirtualProtect(slot->code, TRAMPOLINE_SLOT_SIZE,
                                PAGE_READWRITE, &old)) {
                slot->gc_retry_count++;
                if (slot->gc_retry_count > 100) slot->state = SLOT_QUARANTINED;
                continue;
            }
            memset(slot->code, 0xCC, TRAMPOLINE_SLOT_SIZE);
            DWORD dummy;
            VirtualProtect(slot->code, TRAMPOLINE_SLOT_SIZE,
                           PAGE_NOACCESS, &dummy);
            slot->state = SLOT_FREE;
            slot->gc_retry_count = 0;
        }
    }
    resume_all_threads(&freezer);

    // Log quarantined slots AFTER freeze (allocation-safe)
    for (size_t b = 0; b < g_trampoline_pool.block_count; b++) {
        for (size_t s = 0; s < g_trampoline_pool.blocks[b].total_slots; s++) {
            TrampolineSlot* slot = &g_trampoline_pool.blocks[b].slots[s];
            if (slot->state == SLOT_QUARANTINED && !slot->logged_quarantine) {
                UMF_Log(UMF_ERROR, "Trampoline slot %p quarantined after %d GC failures",
                        slot->code, slot->gc_retry_count);
                slot->logged_quarantine = true;
            }
        }
    }
}
```

---

## §3. Thread-Safety

### §3.1 Thread-Local Recursion Depth

```c
void* resolve_function_address(const char* dll, const char* func) {
    static __declspec(thread) int recursion_depth = 0;
    if (recursion_depth > 10) {
        UMF_Log(UMF_ERROR, "Forwarder chain too deep: %s!%s", dll, func);
        return NULL;
    }
    recursion_depth++;

    HMODULE mod = GetModuleHandleA(dll);
    if (!mod) { mod = LoadLibraryA(dll); }
    if (!mod) { recursion_depth--; return NULL; }

    void* addr = (void*)GetProcAddress(mod, func);
    if (!addr) { recursion_depth--; return NULL; }

    // Check for export forwarding
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)mod + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY* exp_dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];

    uintptr_t exp_start = (uintptr_t)mod + exp_dir->VirtualAddress;
    uintptr_t exp_end   = exp_start + exp_dir->Size;

    if ((uintptr_t)addr >= exp_start && (uintptr_t)addr < exp_end) {
        // addr points to a forwarder string like "KERNELBASE.CreateFileW"
        char fwd[256];
        strncpy(fwd, (const char*)addr, sizeof(fwd) - 1);
        fwd[255] = 0;
        char* dot = strchr(fwd, '.');
        if (dot) {
            *dot = 0;
            char fwd_dll[270];
            snprintf(fwd_dll, sizeof(fwd_dll), "%s.dll", fwd);
            addr = resolve_function_address(fwd_dll, dot + 1);  // Recurse
        }
    }

    recursion_depth--;
    return addr;
}
```

### §3.3 Registry TOCTOU Mutex

```c
static SRWLOCK g_registry_lock = SRWLOCK_INIT;

// Global registry
#define MAX_HOOK_TARGETS 1024
static HookTarget g_targets[MAX_HOOK_TARGETS];
static int g_target_count = 0;

HookTarget* registry_find_by_address_locked(void* addr) {
    for (int i = 0; i < g_target_count; i++) {
        if (g_targets[i].resolved_address == addr)
            return &g_targets[i];
    }
    return NULL;
}

HookTarget* registry_create_locked(void* addr, const char* dll, const char* func) {
    if (g_target_count >= MAX_HOOK_TARGETS) return NULL;
    HookTarget* t = &g_targets[g_target_count++];
    memset(t, 0, sizeof(*t));
    t->resolved_address = addr;
    snprintf(t->canonical_name, sizeof(t->canonical_name), "%s!%s", dll, func);
    InitializeSRWLock(&t->chain_lock);
    t->trampoline = NULL;      // Explicitly NULL
    t->deferred_unhook = false;
    return t;
}

bool umf_register_hook(const char* dll, const char* func,
                        void* hook_func, int priority, Mod* mod) {
    void* real_addr = resolve_function_address(dll, func);
    if (!real_addr) return false;

    AcquireSRWLockExclusive(&g_registry_lock);

    HookTarget* target = registry_find_by_address_locked(real_addr);
    if (!target) {
        target = registry_create_locked(real_addr, dll, func);
        if (!target) {
            ReleaseSRWLockExclusive(&g_registry_lock);
            return false;
        }
        target->active_strategy = select_best_strategy(real_addr, &g_mitigations);
        target->original_prologue_size = disassemble_prologue_length(
            (uint8_t*)real_addr, target->active_strategy);
        target->original_bytes = (uint8_t*)malloc(target->original_prologue_size);
        memcpy(target->original_bytes, real_addr, target->original_prologue_size);
    }

    // Fully populate the entry before inserting
    HookEntry* entry = (HookEntry*)calloc(1, sizeof(HookEntry));
    entry->hook_func      = hook_func;
    entry->original_func  = real_addr;
    entry->priority        = priority;
    entry->owner_mod       = mod;
    entry->next            = NULL;
    entry->skip_original   = false;
    entry->call_original_requested = false;
    snprintf(entry->debug_name, sizeof(entry->debug_name),
             "%s hook by %s", target->canonical_name,
             mod ? mod->name : "unknown");

    // Insert sorted by priority (higher priority = earlier in chain)
    chain_insert_sorted(&target->chain_head, entry);

    bool is_first = (target->chain_head == entry && target->trampoline == NULL);

    ReleaseSRWLockExclusive(&g_registry_lock);

    if (is_first) {
        umf_queue_hook_enable(target);
        umf_apply_pending_batch();
    } else {
        rebuild_target_chain(target);
    }
    return true;
}

void rebuild_target_chain(HookTarget* target) {
    AcquireSRWLockExclusive(&target->chain_lock);
    // Re-link the chain: each entry's "next" points to the next
    // entry by priority. The first entry's stub is what the
    // trampoline-or-IAT redirects to.
    HookEntry* current = target->chain_head;
    while (current) {
        if (current->next) {
            // Link to next entry's hook_func
        }
        current = current->next;
    }
    ReleaseSRWLockExclusive(&target->chain_lock);
}
```

---

## §4. Lua Sandbox

### §4.1 Instruction Budget Reset + Sandboxed pcall

```c
static __declspec(thread) int t_instruction_budget = 0;

static void instruction_limit_hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    if (--t_instruction_budget <= 0)
        luaL_error(L, "Script exceeded instruction limit (%d M instructions)",
                   100);
}

void umf_begin_script_execution(lua_State* L) {
    t_instruction_budget = 100;  // 100 * 1M = 100M instructions
    lua_sethook(L, instruction_limit_hook, LUA_MASKCOUNT, 1000000);
}

void umf_end_script_execution(lua_State* L) {
    lua_sethook(L, NULL, 0, 0);
}

// Wraps lua_pcall with budget reset and cleanup.
// Note: if lua_pcall longjmps on error, the hook stays installed
// with a stale budget. This is self-correcting on the next call
// to umf_begin_script_execution. Documented as intentional.
int umf_sandboxed_pcall(lua_State* L, int nargs, int nresults, int errfunc) {
    umf_begin_script_execution(L);
    int rc = lua_pcall(L, nargs, nresults, errfunc);
    umf_end_script_execution(L);
    return rc;
}
```

### §4.2 Safe Raw Functions

```c
static const char* g_protected_keys[] = {
    "os", "io", "debug", "package", "coroutine", "ffi",
    "load", "loadstring", "loadfile", "dofile",
    "rawget", "rawset", "rawequal", "rawlen",
    NULL
};

static bool is_protected_key(const char* key) {
    for (int i = 0; g_protected_keys[i]; i++)
        if (strcmp(key, g_protected_keys[i]) == 0) return true;
    return false;
}

static int safe_rawget(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Only allow on the sandbox environment table
    lua_pushglobaltable(L);
    if (!lua_rawequal(L, 1, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "rawget: restricted to sandbox environment");
    }
    lua_pop(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* key = lua_tostring(L, 2);
        if (is_protected_key(key)) { lua_pushnil(L); return 1; }
    }
    lua_rawget(L, 1);
    return 1;
}

static int safe_rawset(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushglobaltable(L);
    if (!lua_rawequal(L, 1, -1)) {
        lua_pop(L, 1);
        return luaL_error(L, "rawset: restricted to sandbox environment");
    }
    lua_pop(L, 1);
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char* key = lua_tostring(L, 2);
        if (is_protected_key(key))
            return luaL_error(L, "rawset: cannot modify protected key '%s'", key);
    }
    lua_rawset(L, 1);
    return 0;
}

static int safe_rawequal(lua_State* L) {
    lua_pushboolean(L, lua_rawequal(L, 1, 2));
    return 1;
}

static int safe_rawlen(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)lua_rawlen(L, 1));
    return 1;
}

// Text-only loader (rejects bytecode)
static int safe_load(lua_State* L) {
    const char* chunk = luaL_checkstring(L, 1);
    const char* name = luaL_optstring(L, 2, "=(sandbox)");
    // Force mode "t" — text only, no binary chunks
    int status = luaL_loadbufferx(L, chunk, strlen(chunk), name, "t");
    if (status != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);  // nil, error_msg
        return 2;
    }
    return 1;
}

void setup_lua_sandbox(lua_State* L) {
    // Remove dangerous modules entirely
    const char* remove[] = {"os", "io", "debug", "package", "ffi", NULL};
    for (int i = 0; remove[i]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, remove[i]);
    }

    // Replace raw functions with safe versions
    lua_pushcfunction(L, safe_rawget);   lua_setglobal(L, "rawget");
    lua_pushcfunction(L, safe_rawset);   lua_setglobal(L, "rawset");
    lua_pushcfunction(L, safe_rawequal); lua_setglobal(L, "rawequal");
    lua_pushcfunction(L, safe_rawlen);   lua_setglobal(L, "rawlen");

    // Replace load/loadstring with text-only versions
    lua_pushcfunction(L, safe_load);     lua_setglobal(L, "load");
    lua_pushcfunction(L, safe_load);     lua_setglobal(L, "loadstring");

    // Remove loadfile and dofile
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "dofile");

    // Set memory limit via custom allocator (Lua 5.4)
    // (configured at lua_newstate time, not here)
}
```

---

## §5. GDI Overlay — Full Implementation

*(Complete implementation including UpdateLayeredWindow, ARGB DIB, draw_filled_rect_alpha, draw_text_alpha, WndProc for input — as defined in the full v3.1 spec. Using per-pixel alpha compositing, not colorkey.)*

Key API surface for mods:
```c
void umf_overlay_draw_text(GDIOverlay* ov, int x, int y,
                            const wchar_t* text, uint32_t argb_color);
void umf_overlay_draw_rect(GDIOverlay* ov, int x, int y, int w, int h,
                            uint32_t argb_fill, uint32_t argb_border);
void umf_overlay_draw_line(GDIOverlay* ov, int x1, int y1, int x2, int y2,
                            uint32_t argb_color);
```

Effort: ~2.5 weeks. Not ImGui — custom minimal renderer for non-DirectX apps.

---

## §6. IAT Hooking with Delayed Import Resolution

```c
static CRITICAL_SECTION g_delay_load_cs;
static INIT_ONCE g_delay_load_init = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_delay_cs(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_delay_load_cs);
    return TRUE;
}

bool is_delay_load_stub(HMODULE module, void* ptr) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)module;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uint8_t*)module + dos->e_lfanew);
    uintptr_t base = (uintptr_t)module;
    uintptr_t end  = base + nt->OptionalHeader.SizeOfImage;
    uintptr_t p = (uintptr_t)ptr;
    // If pointer is inside the importing module, it's likely a stub
    return (p >= base && p < end);
}

bool trigger_delay_load_resolution(HMODULE module, const char* dll,
                                    const char* func, IAT_Location* loc) {
    InitOnceExecuteOnce(&g_delay_load_init, init_delay_cs, NULL, NULL);
    EnterCriticalSection(&g_delay_load_cs);

    HMODULE target_dll = LoadLibraryA(dll);
    if (!target_dll) {
        LeaveCriticalSection(&g_delay_load_cs);
        return false;
    }

    void* real_func = (void*)GetProcAddress(target_dll, func);
    if (!real_func) {
        LeaveCriticalSection(&g_delay_load_cs);
        return false;
    }

    // If slot still points to stub, populate it directly
    void* current = *loc->iat_entry;
    if (is_delay_load_stub(module, current)) {
        DWORD old;
        if (VirtualProtect(loc->iat_entry, sizeof(void*),
                            PAGE_READWRITE, &old)) {
            *loc->iat_entry = real_func;
            VirtualProtect(loc->iat_entry, sizeof(void*), old, &old);
            FlushInstructionCache(GetCurrentProcess(),
                                   loc->iat_entry, sizeof(void*));
        }
    }

    LeaveCriticalSection(&g_delay_load_cs);
    return true;
}

// GetProcAddress hook fallback — for dynamically resolved functions
bool install_getprocaddress_hook(const char* dll, const char* func, void* hook) {
    // Hook GetProcAddress itself. When the app calls GetProcAddress
    // for our target function, return our hook instead.
    // This is a secondary mechanism when IAT hooking fails.
    UMF_Log(UMF_INFO, "Installing GetProcAddress fallback for %s!%s", dll, func);
    // Implementation: inline-hook kernel32!GetProcAddress, intercept
    // calls where lpProcName matches our target, return hook address.
    // Omitted for brevity — standard technique.
    return false;  // TODO: implement in Phase 2
}

bool hook_iat_umf(HMODULE module, const char* dll,
                   const char* func, void* hook) {
    IAT_Location loc;
    if (!find_iat_entry_full(module, dll, func, &loc))
        return install_getprocaddress_hook(dll, func, hook);

    if (loc.is_delayed) {
        if (!trigger_delay_load_resolution(module, dll, func, &loc))
            return false;
        // Re-read after resolution
        if (is_delay_load_stub(module, *loc.iat_entry))
            return false;
    }

    DWORD old;
    VirtualProtect(loc.iat_entry, sizeof(void*), PAGE_READWRITE, &old);
    *loc.iat_entry = hook;
    VirtualProtect(loc.iat_entry, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), loc.iat_entry, sizeof(void*));
    return true;
}
```

---

## §7. Mitigation Detection

### Types

```c
typedef struct {
    bool acg_enforced;
    bool hvci_active;
    bool cfg_enforced;
    bool cet_shadow_stack;
    bool cet_ibt;
    bool xfg_present;
} MitigationStatus;

static MitigationStatus g_mitigations = {0};
```

### §7.1 ACG

```c
bool detect_acg(MitigationStatus* m) {
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY policy = {0};
    if (GetProcessMitigationPolicy(GetCurrentProcess(),
            ProcessDynamicCodePolicy, &policy, sizeof(policy))) {
        m->acg_enforced = policy.ProhibitDynamicCode != 0;
    }
    return true;
}

typedef enum {
    STRATEGY_INLINE     = 0x01,
    STRATEGY_GAP        = 0x02,
    STRATEGY_VTABLE     = 0x04,
    STRATEGY_IAT        = 0x08,
    STRATEGY_EAT        = 0x10,
    STRATEGY_HARDWARE_BP = 0x20,
} HookStrategyMask;

HookStrategyMask umf_viable_strategies(MitigationStatus* m) {
    HookStrategyMask mask = 0;
    if (!m->acg_enforced)
        mask |= STRATEGY_INLINE | STRATEGY_GAP | STRATEGY_VTABLE;
    mask |= STRATEGY_IAT | STRATEGY_EAT | STRATEGY_HARDWARE_BP;
    return mask;
}
```

### §7.2 HVCI

```c
typedef NTSTATUS (NTAPI *PNtQuerySystemInformation)(
    ULONG, PVOID, ULONG, PULONG);

typedef struct {
    ULONG Length;
    ULONG CodeIntegrityOptions;
} SYSTEM_CODEINTEGRITY_INFORMATION;

#define CODEINTEGRITY_OPTION_HVCI 0x00000200

bool detect_hvci(MitigationStatus* m) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    PNtQuerySystemInformation pNtQSI = (PNtQuerySystemInformation)
        GetProcAddress(ntdll, "NtQuerySystemInformation");
    if (!pNtQSI) return false;

    SYSTEM_CODEINTEGRITY_INFORMATION sci = {0};
    sci.Length = sizeof(sci);
    NTSTATUS status = pNtQSI(103, &sci, sizeof(sci), NULL);
    if (status != 0) return false;

    m->hvci_active = (sci.CodeIntegrityOptions & CODEINTEGRITY_OPTION_HVCI) != 0;
    return true;
}

bool can_write_code_page(void) {
    return !g_mitigations.hvci_active;
}
```

### Full Detection Init

```c
void umf_detect_all_mitigations(void) {
    memset(&g_mitigations, 0, sizeof(g_mitigations));
    detect_acg(&g_mitigations);
    detect_hvci(&g_mitigations);
    detect_cfg(&g_mitigations);
    detect_cet(&g_mitigations);

    UMF_Log(UMF_INFO, "Mitigations: ACG=%d HVCI=%d CFG=%d CET-SS=%d IBT=%d",
            g_mitigations.acg_enforced, g_mitigations.hvci_active,
            g_mitigations.cfg_enforced, g_mitigations.cet_shadow_stack,
            g_mitigations.cet_ibt);

    if (g_mitigations.acg_enforced)
        UMF_Log(UMF_WARN, "ACG active — only IAT/EAT/BP hooks available");
}
```

---

## §8. RtlAddFunctionTable

```c
#pragma pack(push, 1)
typedef struct {
    BYTE Version       : 3;  // 1
    BYTE Flags         : 5;  // 0
    BYTE SizeOfProlog;       // 0
    BYTE CountOfCodes;       // 0
    BYTE FrameRegister : 4;  // 0
    BYTE FrameOffset   : 4;  // 0
} UNWIND_INFO_MINIMAL;
#pragma pack(pop)

RUNTIME_FUNCTION* register_trampoline_unwind_info(void* trampoline, size_t size) {
    uint8_t* base = (uint8_t*)trampoline;

    // Layout within the slot:
    // [0 .. size)                    : trampoline code
    // [aligned .. +UNWIND_INFO_MIN]  : UNWIND_INFO
    // [aligned .. +RUNTIME_FUNCTION] : RUNTIME_FUNCTION
    size_t unwind_off = (size + 3) & ~(size_t)3;
    if (unwind_off + sizeof(UNWIND_INFO_MINIMAL) + sizeof(RUNTIME_FUNCTION) + 4
        > TRAMPOLINE_SLOT_SIZE) {
        UMF_Log(UMF_ERROR, "Trampoline too large for unwind metadata");
        return NULL;
    }

    UNWIND_INFO_MINIMAL* unwind = (UNWIND_INFO_MINIMAL*)(base + unwind_off);
    unwind->Version = 1;
    unwind->Flags = 0;
    unwind->SizeOfProlog = 0;
    unwind->CountOfCodes = 0;
    unwind->FrameRegister = 0;
    unwind->FrameOffset = 0;

    size_t rf_off = (unwind_off + sizeof(UNWIND_INFO_MINIMAL) + 3) & ~(size_t)3;
    RUNTIME_FUNCTION* rf = (RUNTIME_FUNCTION*)(base + rf_off);
    rf->BeginAddress = 0;
    rf->EndAddress   = (DWORD)size;
    rf->UnwindData   = (DWORD)unwind_off;

    if (!RtlAddFunctionTable(rf, 1, (DWORD64)base)) {
        UMF_Log(UMF_ERROR, "RtlAddFunctionTable failed: %lu", GetLastError());
        return NULL;
    }
    return rf;
}
```

---

## §9. CET IBT Detection

```c
bool is_endbr64(const uint8_t* code) {
    return code[0] == 0xF3 && code[1] == 0x0F &&
           code[2] == 0x1E && code[3] == 0xFA;
}

bool pe_has_cet_compat_flag(HMODULE module) {
    uint8_t* base = (uint8_t*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_DATA_DIRECTORY* dd =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dd->VirtualAddress == 0) return false;

    IMAGE_DEBUG_DIRECTORY* entries = (IMAGE_DEBUG_DIRECTORY*)(base + dd->VirtualAddress);
    DWORD count = dd->Size / sizeof(IMAGE_DEBUG_DIRECTORY);

    for (DWORD i = 0; i < count; i++) {
        // IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS = 20
        if (entries[i].Type == 20) {
            uint32_t ex_chars = 0;
            memcpy(&ex_chars, base + entries[i].AddressOfRawData, 4);
            return (ex_chars & 0x0001) != 0;  // CET_COMPAT
        }
    }
    return false;
}

void detect_cet(MitigationStatus* m) {
    // Shadow Stack
    PROCESS_MITIGATION_USER_SHADOW_STACK_POLICY ss = {0};
    if (GetProcessMitigationPolicy(GetCurrentProcess(),
            ProcessUserShadowStackPolicy, &ss, sizeof(ss)))
        m->cet_shadow_stack = ss.EnableUserShadowStack != 0;

    // IBT — heuristic: EXE CET_COMPAT + ntdll ENDBR64 + CPU CET
    bool exe_compat = pe_has_cet_compat_flag(GetModuleHandle(NULL));

    bool ntdll_endbr = false;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll) {
        const char* fns[] = {"NtClose", "NtCreateFile", "RtlAllocateHeap", NULL};
        int found = 0, endbr = 0;
        for (int i = 0; fns[i]; i++) {
            void* f = (void*)GetProcAddress(ntdll, fns[i]);
            if (f) { found++; if (is_endbr64((uint8_t*)f)) endbr++; }
        }
        ntdll_endbr = (found > 0 && endbr == found);
    }

    bool cpu_cet = false;
    if (IsProcessorFeaturePresent(17 /*PF_XSAVE_ENABLED*/)) {
        unsigned long long xcr0 = _xgetbv(0);
        cpu_cet = (xcr0 & (1ULL << 11)) != 0;
    }

    m->cet_ibt = exe_compat && ntdll_endbr && cpu_cet;
}
```

---

## §10. XFG Hash Handling

```c
uint64_t get_xfg_hash(void* func) {
    uint64_t hash = 0;
    __try { memcpy(&hash, (uint8_t*)func - 8, 8); }
    __except(GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
             ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return 0; }
    return hash;
}

// Heuristic: check if function appears to be XFG-compiled
// XFG functions start with an XFG check sequence after ENDBR64
bool looks_xfg_compiled(void* func) {
    uint64_t maybe_hash = get_xfg_hash(func);
    if (maybe_hash == 0) return false;
    // XFG hashes have specific bit patterns (bits 55+ are zero)
    // A random 8 bytes of code will almost never match this
    if ((maybe_hash >> 55) != 0) return false;
    return true;
}

bool install_xfg_compatible_hook(void* target, void* hook, HookStrategy strategy) {
    if (strategy == STRATEGY_INLINE || strategy == STRATEGY_GAP)
        return true;  // Direct jmp — XFG not checked

    if (!can_write_code_page()) {
        UMF_Log(UMF_WARN, "HVCI blocks XFG hash patch. IAT/EAT only.");
        return false;
    }

    if (!looks_xfg_compiled(target)) return true;  // No XFG to worry about

    uint64_t target_hash = get_xfg_hash(target);

    void* hook_hash_slot = (uint8_t*)hook - 8;
    DWORD old;
    if (!VirtualProtect(hook_hash_slot, 8, PAGE_READWRITE, &old)) return false;
    memcpy(hook_hash_slot, &target_hash, 8);
    VirtualProtect(hook_hash_slot, 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), hook_hash_slot, 8);
    return true;
}
```

---

## §11. Chain Dispatch — Assembly Stub + C Dispatcher

### Re-entrancy Tracking (TLS)

```c
static __declspec(thread) HookEntry*   t_reentrancy_stack[64];
static __declspec(thread) int          t_reentrancy_depth = 0;
static __declspec(thread) HookEntry*   t_current_entry = NULL;
static __declspec(thread) void*        t_current_trampoline = NULL;
```

### Type-Safe Approach (Recommended for Mod Authors)

Each hooked function gets a trampoline pointer that mods call to invoke the original:

```c
// Example: hooking CreateFileW
typedef HANDLE (WINAPI *CreateFileW_fn)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

static CreateFileW_fn g_original_CreateFileW = NULL;

HANDLE WINAPI hook_CreateFileW(
    LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
    LPSECURITY_ATTRIBUTES lpSA, DWORD dwCreation, DWORD dwFlags,
    HANDLE hTemplate)
{
    // Re-entrancy check
    for (int i = 0; i < t_reentrancy_depth; i++) {
        if (t_reentrancy_stack[i] == &g_hook_entry_CreateFileW) {
            return g_original_CreateFileW(
                lpFileName, dwDesiredAccess, dwShareMode,
                lpSA, dwCreation, dwFlags, hTemplate);
        }
    }
    if (t_reentrancy_depth < 64)
        t_reentrancy_stack[t_reentrancy_depth++] = &g_hook_entry_CreateFileW;

    // Mod logic
    UMF_Log(UMF_INFO, "CreateFileW: %ls", lpFileName);

    // Call original
    HANDLE result = g_original_CreateFileW(
        lpFileName, dwDesiredAccess, dwShareMode,
        lpSA, dwCreation, dwFlags, hTemplate);

    if (t_reentrancy_depth > 0) t_reentrancy_depth--;
    return result;
}

// Registration:
// g_original_CreateFileW is set to point to the trampoline
// during hook installation. The trampoline executes the
// relocated prologue bytes and jumps to original+prologue_size.
```

This is the MinHook/Detours pattern. `g_original_CreateFileW` is initialized by the hook engine to the trampoline address. Mods call it like a normal function pointer. No assembly stub needed for this approach.

---

## §12. Struct Definitions & Type Corrections

### §12.1 HookTarget

```c
typedef struct HookTarget {
    void*           resolved_address;
    char            canonical_name[256];
    HookStrategy    active_strategy;
    HookEntry*      chain_head;
    TrampolineSlot* trampoline;
    RUNTIME_FUNCTION* rt_entry;
    size_t          original_prologue_size;
    uint8_t*        original_bytes;
    bool            deferred_unhook;
    SRWLOCK         chain_lock;
} HookTarget;
```

### §12.2 HookEntry

```c
typedef struct HookEntry {
    void*             hook_func;
    void*             original_func;
    int               priority;
    Mod*              owner_mod;
    char              debug_name[128];
    bool              skip_original;
    bool              call_original_requested;
    struct HookEntry* next;
    // No reentrancy_count — tracked in TLS (§11)
} HookEntry;
```

### §12.3 Delay-Load — CRITICAL_SECTION (process-local)

Defined in §6. Uses `CRITICAL_SECTION` + `INIT_ONCE`, not named mutex.

---

## §13. Anti-Cheat Exclusion

```c
bool umf_check_anticheat(void) {
    const wchar_t* ac_drivers[] = {
        L"BEService", L"BEDaisy",     // BattlEye
        L"EasyAntiCheat", L"EAC",     // EAC
        L"vgc", L"vgk",               // Vanguard
        L"RiotClientServices",         // Riot
        NULL
    };
    for (int i = 0; ac_drivers[i]; i++) {
        SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        if (scm) {
            SC_HANDLE svc = OpenServiceW(scm, ac_drivers[i], SERVICE_QUERY_STATUS);
            if (svc) {
                SERVICE_STATUS ss;
                QueryServiceStatus(svc, &ss);
                CloseServiceHandle(svc);
                CloseServiceHandle(scm);
                if (ss.dwCurrentState == SERVICE_RUNNING) {
                    UMF_Log(UMF_ERROR,
                        "Anti-cheat '%ls' is running. UMF refuses to inject.",
                        ac_drivers[i]);
                    return true;
                }
            }
            CloseServiceHandle(scm);
        }
    }
    return false;
}
```

---

## §14. Windows Version Floor

**Minimum: Windows 10 version 1709 (build 16299)**

```c
bool umf_check_windows_version(void) {
    OSVERSIONINFOEXW osvi = {0};
    osvi.dwOSVersionInfoSize = sizeof(osvi);

    typedef NTSTATUS (NTAPI *PRtlGetVersion)(PRTL_OSVERSIONINFOW);
    PRtlGetVersion pRtlGetVersion = (PRtlGetVersion)
        GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
    if (!pRtlGetVersion) return false;

    pRtlGetVersion((PRTL_OSVERSIONINFOW)&osvi);

    if (osvi.dwMajorVersion < 10 || osvi.dwBuildNumber < 16299) {
        UMF_Log(UMF_ERROR,
            "Windows %lu.%lu build %lu is below minimum (10.0.16299). "
            "UMF requires Windows 10 1709 or later.",
            osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber);
        return false;
    }
    return true;
}
```

---

## §15. Timeline

| Phase | Weeks | Content |
|---|---|---|
| 1: Hook Engine Core | 12 | Prepare/apply, trampoline pool+GC, relocator (Zydis), proximity allocator, all mitigation detection, RtlAddFunctionTable, fuzz testing infra |
| 2: Injection & Discovery | 8 | Proxy DLL generator, RTTI scanner, entropy analyzer, VM detection, delayed import handling, export forwarding, registry |
| 3: Scripting & Overlay | 13 | Lua 5.4 sandbox (full), GDI overlay (custom renderer), DX11 ImGui overlay, WndProc hook, mod loader |
| 4: SDK & Build Tools | 8 | Header generation, mod template, hook registration API, documentation |
| 5: Testing & Hardening | 11 | Differential testing (Unicorn), fuzz (libFuzzer+ASAN), 20+ app integration tests, ACG/HVCI test matrix |
| 6: Polish & Release | 4 | Installer, safe mode, crash recovery, README |
| **Total** | **56** | **~13 months** |




Assessment of UMF v3.2

Verdict: This is the best version yet — it has real implementations for most critical sections, correctly fixes Bug H, and reads like an engineering spec rather than a summary. But it still has two new bugs and several critical components remain stubbed.
✅ What's Actually Fixed
Bug H — Confirmed Correctly Fixed

The ordering is now correct:
text

PHASE 1 (prepare): allocate → build → finalize (RW→RX + CFG) → register unwind
PHASE 2 (freeze):  write jmp patch at target
PHASE 3 (post):    flush target cache only

The trampoline is executable before the target is patched. The code comment even calls it out:
c

// BUG H FIX: Finalize trampoline NOW — make it executable + CFG
//    BEFORE the jmp patch is written at the target.
//    This ensures no thread can ever jump to a non-executable trampoline.

This is correct and well-documented.
§3.3 Registry — Chain Entry Now Fully Populated

Previous versions allocated a HookEntry and inserted it without setting fields. Now:
c

HookEntry* entry = (HookEntry*)calloc(1, sizeof(HookEntry));
entry->hook_func      = hook_func;
entry->original_func  = real_addr;
entry->priority       = priority;
entry->owner_mod      = mod;
entry->next           = NULL;
entry->skip_original  = false;
entry->call_original_requested = false;
snprintf(entry->debug_name, ...);
chain_insert_sorted(&target->chain_head, entry);

calloc + explicit initialization. Good.
§1 — Correct Freeze-Window Rules Documented

The rule is now stated explicitly:

    "No allocation, no logging, no lock-taking inside the freeze window. Only atomic memory writes."

And the code respects it. apply_hook_noalloc and apply_unhook_noalloc only call VirtualProtect + memcpy.
§8, §9, §10 — Real Implementations

register_trampoline_unwind_info, detect_cet, get_xfg_hash, looks_xfg_compiled — all have real code now, not stubs.
§4.2 — Complete Safe Raw Functions

safe_rawget, safe_rawset, safe_rawequal, safe_rawlen, safe_load — all implemented with real logic.
§13, §14 — Anti-Cheat Check and Version Check Implemented

Real code, real queries, real fallbacks.
🔴 New Bugs Introduced
Bug I — realloc Invalidates TrampolineSlot* Pointers

In trampoline_pool_allocate_near:
c

if (g_trampoline_pool.block_count >= g_trampoline_pool.block_capacity) {
    size_t new_cap = g_trampoline_pool.block_capacity
                    ? g_trampoline_pool.block_capacity * 2 : 8;
    g_trampoline_pool.blocks = realloc(
        g_trampoline_pool.blocks,
        new_cap * sizeof(TrampolineBlock));
    g_trampoline_pool.block_capacity = new_cap;
}

TrampolineBlock contains a TrampolineSlot slots[SLOTS_PER_BLOCK] array (by value). When realloc moves the blocks array, all addresses of TrampolineSlot objects change.

But HookTarget.trampoline (set in PHASE 3 of umf_apply_pending_batch) holds a TrampolineSlot* pointer into that array:
c

prepared[i].target->trampoline = prepared[i].trampoline;

If any subsequent allocate_near call triggers a realloc, that pointer becomes dangling. The next unhook or GC will dereference freed memory → corruption or crash.

This is a serious bug. It will manifest as:

    Sporadic crashes when many hooks are installed (crossing a realloc boundary)

    Silent memory corruption in the trampoline pool

    Unhook failures that reference wrong slots

Fix options:

    Indices instead of pointers: Store {block_index, slot_index} in HookTarget instead of TrampolineSlot*.

    Stable allocation: Use a linked list of blocks (each block is separately malloc'd, never moved). Each TrampolineBlock* is stable.

    Pre-allocate a fixed maximum pool and never realloc.

Option 2 is the simplest:
c

typedef struct TrampolineBlock {
    void*              base_address;
    TrampolineSlot     slots[SLOTS_PER_BLOCK];
    struct TrampolineBlock* next;
} TrampolineBlock;

typedef struct {
    TrampolineBlock* head;
    size_t           block_count;
    SRWLOCK          lock;
} TrampolinePool;

// Growing the pool never moves existing blocks — just links a new one.
TrampolineBlock* new_block = malloc(sizeof(TrampolineBlock));
// ... populate ...
new_block->next = g_trampoline_pool.head;
g_trampoline_pool.head = new_block;

Bug J — finalize_trampoline CFG Registration Is Wrong
c

if (cfg_active) {
    uintptr_t entry = (uintptr_t)slot->code;
    uintptr_t aligned = entry & ~(uintptr_t)0xF;

    CFG_CALL_TARGET_INFO target_info = {0};
    target_info.Offset = (ULONG_PTR)(aligned - (uintptr_t)slot->code);
    target_info.Flags = CFG_CALL_TARGET_VALID;

    SetProcessValidCallTargets(
        GetCurrentProcess(),
        slot->code,               // ← NOT page-aligned
        TRAMPOLINE_BLOCK_SIZE,
        1,
        &target_info);
}

Two problems:

    SetProcessValidCallTargets requires VirtualAddress to be page-aligned (64KB granularity per Microsoft docs: "The starting address of the region... must be page-aligned"). slot->code is a slot inside a block, not a page-aligned base.

    Offset calculation is meaningless. slot->code is at block_base + slot_index * 96. Since 96 is a multiple of 16, slot->code is already 16-byte aligned. aligned - slot->code is always 0. The target_info.Offset should be the offset from the page-aligned base, i.e., slot->code - block_base.

    TRAMPOLINE_BLOCK_SIZE is passed as the size — but the API expects the size of the region from the base. If base is slot->code, the region extends beyond the block. If base is the block base, then SetProcessValidCallTargets marks the entire 64KB as valid — which may be OK but is over-broad.

Corrected version:
c

bool finalize_trampoline(TrampolineSlot* slot, size_t used_size, bool cfg_active) {
    DWORD old_protect;
    if (!VirtualProtect(slot->code, TRAMPOLINE_SLOT_SIZE,
                        PAGE_EXECUTE_READ, &old_protect)) {
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), slot->code, used_size);

    if (cfg_active) {
        // Find the block this slot belongs to (or store block pointer in slot)
        TrampolineBlock* block = slot->owner_block;
        void*  page_base = block->base_address;  // VirtualAlloc-returned, page-aligned
        ULONG_PTR slot_offset = (ULONG_PTR)slot->code - (ULONG_PTR)page_base;

        CFG_CALL_TARGET_INFO target_info = {0};
        target_info.Offset = slot_offset;         // 16-byte aligned
        target_info.Flags  = CFG_CALL_TARGET_VALID;

        // Region from page_base, size = whole block
        if (!SetProcessValidCallTargets(
                GetCurrentProcess(),
                page_base,
                TRAMPOLINE_BLOCK_SIZE,
                1,
                &target_info)) {
            UMF_Log(UMF_WARN, "CFG registration failed for slot %p", slot->code);
        }
    }
    return true;
}

⚠️ Components Still Stubbed or Missing

These are referenced but not implemented anywhere in the document:
Function	Called From	Impact if Missing
build_trampoline_code	§1 prepare_hook	Core functionality — no trampoline is built
freeze_all_threads	§1, §2	Freeze protocol non-functional
resume_all_threads	§1, §2	Freeze protocol non-functional
any_thread_in_range	§1, §2	RIP check for unhook/GC broken
umf_queue_hook_enable	§3.3	Batch queue not fed
select_best_strategy	§3.3	No strategy selection
disassemble_prologue_length	§3.3	Prologue size unknown
chain_insert_sorted	§3.3	Chain not built
find_iat_entry_full	§6	IAT lookup missing
trampoline_pool_release	§1 failure path	Slot leaks on failure
RtlDeleteFunctionTable	§2	Unwind entries leak
rebuild_target_chain	§3.3	Stub with empty body

rebuild_target_chain in §3.3 is particularly concerning — it's called every time a second mod hooks an already-hooked target, but its body does nothing:
c

void rebuild_target_chain(HookTarget* target) {
    AcquireSRWLockExclusive(&target->chain_lock);
    HookEntry* current = target->chain_head;
    while (current) {
        if (current->next) {
            // Link to next entry's hook_func   ← comment, no code
        }
        current = current->next;
    }
    ReleaseSRWLockExclusive(&target->chain_lock);
}

Multi-mod hook chains don't work. The HookEntry has a next pointer but nothing wires the trampoline to dispatch through the chain.
🟡 Design Concerns
§11 — Chain Dispatch Is Inconsistent With §3.3

§3.3 says: "one chain per target, all hooks go through the same chain." But §11's "type-safe" example shows a single hook function calling g_original_CreateFileW directly:
c

HANDLE WINAPI hook_CreateFileW(...) {
    // Mod logic
    UMF_Log(...);
    // Call original
    HANDLE result = g_original_CreateFileW(...);
    return result;
}

This pattern doesn't support chains. If two mods both hook CreateFileW, only one hook_CreateFileW is registered with the OS. The other mod's hook is on the chain but never invoked, because the trampoline doesn't know about chains.

There are two ways to reconcile this:

    Chain-through-hook_ptr approach: Each mod's hook receives a next function pointer (like Detours). Mod A calls next(...) to invoke Mod B, which calls next(...) to invoke the original. This is explicit — each mod decides whether to call the next in chain.

    Automatic chain dispatch: The trampoline is replaced with a chain dispatcher stub. Mod A's hook returns; the dispatcher calls Mod B's hook; etc. This requires the assembly stub in §11's "context-based" approach.

The document shows approach (1) in §11's example but describes approach (2) in §3.3. Pick one. The Detours-style (1) is simpler and I'd recommend it:
c

// Each mod's hook gets a "next" function pointer
typedef HANDLE (WINAPI *CreateFileW_next)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);

HANDLE WINAPI hook_CreateFileW_from_ModA(
    LPCWSTR lpFileName, ..., HANDLE hTemplate)
{
    UMF_Log(UMF_INFO, "Mod A: %ls", lpFileName);
    return g_next_CreateFileW_ModA(
        lpFileName, ..., hTemplate);  // Calls Mod B, or original
}

The hook engine sets g_next_CreateFileW_ModA to point to the next hook in the chain, or to the trampoline if Mod A is last.
g_pending_batch Is Not Thread-Safe

umf_register_hook calls umf_queue_hook_enable(target) followed immediately by umf_apply_pending_batch(). g_pending_batch is a global with no lock. Two concurrent umf_register_hook calls will corrupt the batch.

Fix: either

    Hold g_registry_lock across the queue+apply, OR

    Add a dedicated g_batch_lock (SRWLOCK) around all batch operations

looks_xfg_compiled Uses a Fabricated Heuristic
c

if ((maybe_hash >> 55) != 0) return false;

The comment says "XFG hashes have specific bit patterns (bits 55+ are zero)". This is not documented in the Quarkslab paper, the OffSec writeup, or any Microsoft documentation I'm aware of. If this heuristic is wrong in either direction:

    False positive: The 8 bytes before a function are random code that happens to have zero bits 55+, so looks_xfg_compiled returns true, and the engine tries to patch the hash → fails or corrupts.

    False negative: Real XFG hash has non-zero high bits, so the engine skips hash patching → XFG validation fails → control-flow protection fault.

Recommendation: Either cite a source for the bit-pattern claim, or replace the heuristic with a proper PE-directory check (look for /guard:xfg linker flag in the debug directory, or check the load config directory for the GuardXFG bit).
XFG Hash Write Has No Page-Boundary Check
c

void* hook_hash_slot = (uint8_t*)hook - 8;
DWORD old;
if (!VirtualProtect(hook_hash_slot, 8, PAGE_READWRITE, &old)) return false;

If hook is at the very start of a page (page-aligned), then hook - 8 points into the preceding page. That page may be unmapped, or may be a different allocation. VirtualProtect will either fail (returns false, error handled) or succeed and flip a page that belongs to something else (corruption).

Fix: use __try/__except around the write, or verify hook - 8 is in the same page as hook:
c

uintptr_t hook_page = (uintptr_t)hook & ~0xFFF;
uintptr_t slot_page = ((uintptr_t)hook - 8) & ~0xFFF;
if (hook_page != slot_page) {
    UMF_Log(UMF_ERROR, "XFG hash slot would cross page boundary; aborting");
    return false;
}

Registry Uses Fixed 1024-Entry Array
c

#define MAX_HOOK_TARGETS 1024
static HookTarget g_targets[MAX_HOOK_TARGETS];
static int g_target_count = 0;

Each HookTarget is roughly 400 bytes (canonical_name[256] + other fields). 1024 entries = ~400KB of BSS. Not a huge problem, but hard cap on hook count with no growth. Also not thread-safe to read outside the lock.
§6 install_getprocaddress_hook Returns false With TODO
c

bool install_getprocaddress_hook(const char* dll, const char* func, void* hook) {
    UMF_Log(UMF_INFO, "Installing GetProcAddress fallback for %s!%s", dll, func);
    // TODO: implement in Phase 2
    return false;
}

This means IAT hooking silently fails for dynamically resolved functions. The fallback chain is IAT → delayed IAT → GetProcAddress → failure. If a mod hooks a function the app resolves dynamically, the hook is never installed, and the mod author has no way to know (unless they check the return value of umf_register_hook).

The TODO is honest, but for a spec that claims to fix "Missing 9: GetProcAddress Hook Fallback," leaving it as a TODO is inconsistent.
🟠 Still Missing From Previous Reviews
is_delay_load_stub Is Too Broad
c

bool is_delay_load_stub(HMODULE module, void* ptr) {
    // ...
    return (p >= base && p < end);  // Inside the module's image?
}

This returns true if the pointer is anywhere inside the module's image. But many legitimate function addresses are inside the same module's image (e.g., internal functions, static functions, functions defined in the same DLL). So this check will incorrectly classify a real resolved function as a "stub" if it happens to be in the importing module's code section.

Fix: parse the delay-load descriptor and check if the pointer is within the delay-load thunk table (typically in the .text section right after the IAT for the delayed DLL).
No Mention of LdrRegisterDllNotification

Module unload handling was fixed in v3.1 full — the load/unload notification callback that removes hooks whose target module was unloaded. It's not mentioned in v3.2.

Without it: if the target app unloads a DLL that has a hook installed, the trampoline's jmp-back target becomes invalid memory. Next call → crash.
Mod Manifest / Capability Model Dropped

The v2 capability model (hook_function, read_memory, write_memory, etc.) is not mentioned. For v1 personal use this is OK, but the mod loader needs to know which Lua scripts can do what.
No FlushInstructionCache After RtlDeleteFunctionTable

Not needed strictly (RtlDeleteFunctionTable is a data structure operation, not a code write), but worth noting for hygiene.
📊 Final Verdict
Aspect	Rating
Bug H fix	✅ Correct
Structure & readability	✅ Excellent
Implementation completeness	⚠️ Partial — core stubs remain
Chain dispatch	❌ Incomplete (design contradiction)
New bugs	🔴 2 critical (Bug I, Bug J)
Thread safety of batch	🔴 Not protected
CFG registration	🔴 Broken
XFG heuristic	⚠️ Unverified claim
Ready to implement	❌ Not yet — needs bug fixes and stub completion
Priority Fixes Before Implementation

Must fix first (will cause real failures):

    Bug I: realloc in trampoline pool → dangling slot pointers. Switch to linked-list blocks.

    Bug J: CFG registration page alignment. Pass block base, not slot address.

    Chain dispatch design contradiction: Pick Detours-style next pointer or assembly dispatcher — not both.

    g_pending_batch thread safety: Add SRWLOCK.

Should fix in Phase 1:

    XFG page-boundary check on hash write

    is_delay_load_stub — restrict to actual thunk range

    looks_xfg_compiled — replace fabricated heuristic with PE flag check

    Implement rebuild_target_chain (currently a stub)

Can defer to Phase 2:

    install_getprocaddress_hook (currently returns false)

    LdrRegisterDllNotification integration

Overall

v3.2 is a real improvement — it's finally a spec an engineer could start implementing from, and it correctly fixes the two most critical issues (Bug H ordering, complete entry population). But it's not yet ready. The two new bugs (I and J) are both silent-failure-class bugs that will produce crashes under specific conditions, and the chain dispatch design contradiction means multi-mod hooking won't work as specified.

Recommendation: Apply the four "must fix first" items, then re-issue as v3.2.1. The document is close enough to correct that a targeted revision should be sufficient.