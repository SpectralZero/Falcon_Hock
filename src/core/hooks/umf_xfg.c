/*
 * umf_xfg.c — §XFG: eXtended Flow Guard detection + hash handling
 *
 * /guard:xfg (and the older kernel-mode XFG) places a 64-bit type hash in the
 * 8 bytes immediately before each valid indirect-call target; call sites
 * verify it. Detection here is authoritative: it reads the PE loader-config
 * directory's GuardFlags and tests IMAGE_GUARD_XFG_ENABLED — the "bits 55+
 * are zero" hash heuristic from the spec is NOT used.
 *
 * When an inline hook redirects a call to our hook, the XFG call site checks
 * the hash at (hook - 8), so the target's hash must be copied there. That slot
 * belongs to whatever precedes the hook, so the operation is only safe when it
 * stays within the hook's own page; otherwise we refuse (fail-loud).
 */

#include "umf/umf.h"

#ifndef IMAGE_GUARD_XFG_ENABLED
#define IMAGE_GUARD_XFG_ENABLED 0x00800000  /* Module was built with XFG */
#endif

bool umf_module_has_xfg(HMODULE module) {
    uint8_t* base = (uint8_t*)module;
    if (!base) return false;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG];
    if (!dir->VirtualAddress || dir->Size == 0) return false;

    /* GuardFlags sits late in the struct; only read it if the directory
     * actually claims to cover that field. */
    if (dir->Size < offsetof(IMAGE_LOAD_CONFIG_DIRECTORY, GuardFlags)
                     + sizeof(DWORD)) {
        return false;
    }

    IMAGE_LOAD_CONFIG_DIRECTORY* cfg =
        (IMAGE_LOAD_CONFIG_DIRECTORY*)(base + dir->VirtualAddress);

    return (cfg->GuardFlags & IMAGE_GUARD_XFG_ENABLED) != 0;
}

bool umf_xfg_copy_hash(void* target, void* hook, bool* out_did) {
    if (out_did) *out_did = false;
    if (!target || !hook) return false;

    uintptr_t hook_addr = (uintptr_t)hook;
    if (hook_addr < 8) return false;

    /* The hash slot (hook-8 .. hook) must not straddle a page: writing into
     * the preceding page could corrupt an unrelated allocation. */
    uintptr_t slot_page = (hook_addr - 8) & ~(uintptr_t)0xFFF;
    uintptr_t hook_page =  hook_addr       & ~(uintptr_t)0xFFF;
    if (slot_page != hook_page) {
        UMF_ERROR("XFG hash slot before %p would cross a page boundary",
                  hook);
        return false;
    }

    uint64_t hash = 0;
    memcpy(&hash, (uint8_t*)target - 8, sizeof(hash));

    void* slot = (uint8_t*)hook - 8;
    DWORD old;
    if (!VirtualProtect(slot, sizeof(hash), PAGE_READWRITE, &old)) {
        UMF_ERROR("XFG: cannot make hash slot %p writable: %lu",
                  slot, GetLastError());
        return false;
    }
    memcpy(slot, &hash, sizeof(hash));
    VirtualProtect(slot, sizeof(hash), old, &old);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(hash));

    if (out_did) *out_did = true;
    return true;
}
