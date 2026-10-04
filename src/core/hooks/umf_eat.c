/*
 * umf_eat.c — §EAT: Export Address Table hooking
 *
 * Rewrites an export's 32-bit RVA in the module's export directory so that
 * subsequent GetProcAddress-style lookups of that export return our hook.
 * This catches callers that resolve the export dynamically rather than
 * through their own IAT (self-resolving plugins, LoadLibrary+GetProcAddress
 * patterns), and complements IAT hooking.
 *
 * x64 wrinkle: EAT entries are 32-bit RVAs relative to the module base, so
 * the target must sit in [base, base+4GiB). When the hook is outside that
 * window we place a tiny jmp stub above the module base (within 2 GiB) and
 * point the RVA at the stub.
 */

#include "umf/umf.h"

static bool pe_nt_headers(HMODULE module, uint8_t** out_base,
                          IMAGE_NT_HEADERS** out_nt) {
    uint8_t* base = (uint8_t*)module;
    if (!base) return false;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *out_base = base;
    *out_nt   = nt;
    return true;
}

/* Allocate a 14-byte absolute-jmp stub within [base, base+2GiB) so its RVA is
 * a positive 32-bit value. Returns an executable stub, or NULL. */
static void* alloc_stub_above(uintptr_t base, void* hook) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran  = si.dwAllocationGranularity;
    uintptr_t limit = base + 0x7FF00000ULL;        /* ~2 GiB above base */
    uintptr_t addr  = (base + gran - 1) & ~(gran - 1);

    while (addr < limit) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) break;

        if (mbi.State == MEM_FREE && mbi.RegionSize >= gran) {
            void* mem = VirtualAlloc((void*)addr, 64,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (mem) {
                uint8_t* s = (uint8_t*)mem;
                s[0] = 0xFF; s[1] = 0x25;           /* jmp qword [rip+0] */
                uint32_t zero = 0; memcpy(s + 2, &zero, 4);
                uint64_t h = (uint64_t)(uintptr_t)hook; memcpy(s + 6, &h, 8);
                DWORD old;
                VirtualProtect(mem, 64, PAGE_EXECUTE_READ, &old);
                FlushInstructionCache(GetCurrentProcess(), mem, 14);
                return mem;
            }
        }

        uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        addr = (next + gran - 1) & ~(gran - 1);
    }
    return NULL;
}

bool umf_hook_eat(HMODULE module, const char* func, void* hook,
                  void** out_original, UmfEatLocation* out_loc) {
    if (!module || !func || !hook) return false;

    uint8_t* base;
    IMAGE_NT_HEADERS* nt;
    if (!pe_nt_headers(module, &base, &nt)) return false;

    IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir->VirtualAddress || !dir->Size) return false;

    IMAGE_EXPORT_DIRECTORY* exp =
        (IMAGE_EXPORT_DIRECTORY*)(base + dir->VirtualAddress);
    DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);
    DWORD* names = (DWORD*)(base + exp->AddressOfNames);
    WORD*  ords  = (WORD*)(base + exp->AddressOfNameOrdinals);

    DWORD ordinal = (DWORD)-1;
    for (DWORD i = 0; i < exp->NumberOfNames; i++) {
        const char* nm = (const char*)(base + names[i]);
        if (strcmp(nm, func) == 0) { ordinal = ords[i]; break; }
    }
    if (ordinal == (DWORD)-1) {
        UMF_WARN("EAT: export '%s' not found in %p", func, (void*)module);
        return false;
    }

    DWORD* eat_slot     = &funcs[ordinal];
    DWORD  original_rva = *eat_slot;
    void*  original     = base + original_rva;

    /* Choose an RVA that reaches the hook, placing a stub if necessary. */
    void*     stub      = NULL;
    uintptr_t ubase     = (uintptr_t)base;
    uintptr_t hook_addr = (uintptr_t)hook;
    DWORD     new_rva;

    if (hook_addr >= ubase && (hook_addr - ubase) <= 0xFFFFFFFFULL) {
        new_rva = (DWORD)(hook_addr - ubase);
    } else {
        stub = alloc_stub_above(ubase, hook);
        if (!stub) {
            UMF_ERROR("EAT: cannot place stub within 2GiB of %p", (void*)base);
            return false;
        }
        new_rva = (DWORD)((uintptr_t)stub - ubase);
    }

    /* An RVA inside the export directory would be read as a forwarder. */
    if (new_rva >= dir->VirtualAddress &&
        new_rva <  dir->VirtualAddress + dir->Size) {
        UMF_ERROR("EAT: computed RVA collides with the export directory");
        if (stub) VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }

    DWORD old;
    if (!VirtualProtect(eat_slot, sizeof(DWORD), PAGE_READWRITE, &old)) {
        UMF_ERROR("VirtualProtect EAT slot %p failed: %lu",
                  (void*)eat_slot, GetLastError());
        if (stub) VirtualFree(stub, 0, MEM_RELEASE);
        return false;
    }
    *eat_slot = new_rva;
    VirtualProtect(eat_slot, sizeof(DWORD), old, &old);

    if (out_original) *out_original = original;
    if (out_loc) {
        out_loc->module       = module;
        out_loc->eat_slot     = eat_slot;
        out_loc->original_rva = original_rva;
        out_loc->stub         = stub;
    }

    UMF_INFO("EAT hook: %s in %p (rva %08lX -> %08lX%s)",
             func, (void*)module, original_rva, new_rva,
             stub ? " via stub" : "");
    return true;
}

bool umf_unhook_eat(const UmfEatLocation* loc) {
    if (!loc || !loc->eat_slot) return false;

    DWORD old;
    if (!VirtualProtect(loc->eat_slot, sizeof(DWORD), PAGE_READWRITE, &old))
        return false;
    *loc->eat_slot = loc->original_rva;
    VirtualProtect(loc->eat_slot, sizeof(DWORD), old, &old);

    if (loc->stub) VirtualFree(loc->stub, 0, MEM_RELEASE);
    return true;
}
