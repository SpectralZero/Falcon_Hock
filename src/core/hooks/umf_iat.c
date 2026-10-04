/*
 * umf_iat.c — §IAT: Import Address Table hooking + GetProcAddress fallback
 *
 * IAT hooking overwrites a function pointer in a module's import table. It is
 * a data-only edit (no code bytes change), so unlike inline hooking it works
 * under ACG and on read-only (RX) code — the only hook path for Chromium /
 * Electron renderers and other hardened processes. A pointer-sized aligned
 * store is atomic on x64, so no trampoline and no thread freeze are required.
 *
 * Forwarder handling (#9): after the loader binds imports, each IAT slot holds
 * the FINAL resolved address, following export forwarders (kernel32!CreateFileW
 * → kernelbase!CreateFileW) and ApiSet redirection. We locate the slot by name
 * first, then fall back to matching the forwarder-resolved address, which also
 * catches direct kernelbase callers and bound/ApiSet imports.
 *
 * GetProcAddress fallback (#4): some software resolves APIs dynamically and
 * never touches the IAT. For those we inline-hook GetProcAddress and swap the
 * returned pointer when a registered target is requested.
 */

#include "umf/umf.h"

/* ════════════════════════════════════════════════════════════════
 * PE helpers
 * ════════════════════════════════════════════════════════════════ */

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

/* Delay-load descriptor (RVA form). Mirrors ImgDelayDescr from delayimp.h. */
typedef struct {
    DWORD grAttrs;        /* dlattrRva (1) when the fields below are RVAs */
    DWORD rvaDLLName;
    DWORD rvaHmod;
    DWORD rvaIAT;
    DWORD rvaINT;
    DWORD rvaBoundIAT;
    DWORD rvaUnloadIAT;
    DWORD dwTimeStamp;
} UmfImgDelayDescr;

/* ════════════════════════════════════════════════════════════════
 * Locate IAT slot — by name, by value, and delay-load
 * ════════════════════════════════════════════════════════════════ */

static bool find_by_name(uint8_t* base, IMAGE_NT_HEADERS* nt,
                         HMODULE module, const char* dll, const char* func,
                         UmfIatLocation* out) {
    IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || !dir->Size) return false;

    IMAGE_IMPORT_DESCRIPTOR* desc =
        (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);

    for (; desc->Name; desc++) {
        /* Name matching requires the original (unbound) import-name table. */
        if (!desc->OriginalFirstThunk) continue;

        const char* mod_name = (const char*)(base + desc->Name);
        if (dll && _stricmp(mod_name, dll) != 0) continue;

        IMAGE_THUNK_DATA* ilt = (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);

        for (; ilt->u1.AddressOfData; ilt++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(ilt->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn =
                (IMAGE_IMPORT_BY_NAME*)(base + ilt->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) == 0) {
                out->iat_slot = (void**)&iat->u1.Function;
                out->original = (void*)iat->u1.Function;
                out->is_delay = false;
                out->module   = module;
                return true;
            }
        }
    }
    return false;
}

static bool find_delay_by_name(uint8_t* base, IMAGE_NT_HEADERS* nt,
                               HMODULE module, const char* dll,
                               const char* func, UmfIatLocation* out) {
    IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir->VirtualAddress || !dir->Size) return false;

    UmfImgDelayDescr* dd = (UmfImgDelayDescr*)(base + dir->VirtualAddress);

    for (; dd->rvaDLLName; dd++) {
        if (!(dd->grAttrs & 1)) continue;      /* only RVA-based descriptors */
        if (!dd->rvaINT || !dd->rvaIAT) continue;

        const char* mod_name = (const char*)(base + dd->rvaDLLName);
        if (dll && _stricmp(mod_name, dll) != 0) continue;

        IMAGE_THUNK_DATA* intt = (IMAGE_THUNK_DATA*)(base + dd->rvaINT);
        IMAGE_THUNK_DATA* iat  = (IMAGE_THUNK_DATA*)(base + dd->rvaIAT);

        for (; intt->u1.AddressOfData; intt++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(intt->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn =
                (IMAGE_IMPORT_BY_NAME*)(base + intt->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) == 0) {
                out->iat_slot = (void**)&iat->u1.Function;
                out->original = (void*)iat->u1.Function;
                out->is_delay = true;
                out->module   = module;
                return true;
            }
        }
    }
    return false;
}

static bool find_by_value(uint8_t* base, IMAGE_NT_HEADERS* nt,
                          HMODULE module, void* target, UmfIatLocation* out) {
    IMAGE_DATA_DIRECTORY* dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || !dir->Size) return false;

    IMAGE_IMPORT_DESCRIPTOR* desc =
        (IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress);

    for (; desc->Name; desc++) {
        IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
        for (; iat->u1.Function; iat++) {
            if ((void*)iat->u1.Function == target) {
                out->iat_slot = (void**)&iat->u1.Function;
                out->original = target;
                out->is_delay = false;
                out->module   = module;
                return true;
            }
        }
    }
    return false;
}

bool umf_find_iat_entry(HMODULE module, const char* dll, const char* func,
                        UmfIatLocation* out) {
    if (!module || !func || !out) return false;

    uint8_t* base;
    IMAGE_NT_HEADERS* nt;
    if (!pe_nt_headers(module, &base, &nt)) return false;

    if (find_by_name(base, nt, module, dll, func, out))       return true;
    if (find_delay_by_name(base, nt, module, dll, func, out)) return true;

    void* resolved = umf_resolve_function(dll, func);
    if (resolved && find_by_value(base, nt, module, resolved, out)) return true;

    return false;
}

/* ════════════════════════════════════════════════════════════════
 * Hook / unhook
 * ════════════════════════════════════════════════════════════════ */

bool umf_hook_iat(HMODULE module, const char* dll, const char* func,
                  void* hook, void** out_original, UmfIatLocation* out_loc) {
    if (!module || !func || !hook) return false;

    UmfIatLocation loc;
    memset(&loc, 0, sizeof(loc));
    if (!umf_find_iat_entry(module, dll, func, &loc)) {
        UMF_WARN("IAT entry %s!%s not found in module %p",
                 dll ? dll : "?", func, (void*)module);
        return false;
    }

    /* Prefer the forwarder/delay-resolved address as the callable original.
     * For a bound standard import this equals the current slot value anyway. */
    void* resolved = umf_resolve_function(dll, func);
    void* original = resolved ? resolved : loc.original;
    loc.original = original;

    DWORD old;
    if (!VirtualProtect(loc.iat_slot, sizeof(void*), PAGE_READWRITE, &old)) {
        UMF_ERROR("VirtualProtect IAT slot %p failed: %lu",
                  (void*)loc.iat_slot, GetLastError());
        return false;
    }
    *loc.iat_slot = hook;                 /* aligned pointer store — atomic */
    VirtualProtect(loc.iat_slot, sizeof(void*), old, &old);

    if (out_original) *out_original = original;
    if (out_loc)      *out_loc = loc;

    UMF_INFO("IAT hook: %s!%s in %p (slot %p -> %p, original %p)%s",
             dll ? dll : "?", func, (void*)module,
             (void*)loc.iat_slot, hook, original,
             loc.is_delay ? " [delay]" : "");
    return true;
}

bool umf_unhook_iat(const UmfIatLocation* loc) {
    if (!loc || !loc->iat_slot) return false;
    DWORD old;
    if (!VirtualProtect(loc->iat_slot, sizeof(void*), PAGE_READWRITE, &old))
        return false;
    *loc->iat_slot = loc->original;
    VirtualProtect(loc->iat_slot, sizeof(void*), old, &old);
    return true;
}

/* ════════════════════════════════════════════════════════════════
 * GetProcAddress fallback (#4)
 * ════════════════════════════════════════════════════════════════ */

typedef FARPROC (WINAPI *PFN_GetProcAddress)(HMODULE, LPCSTR);

#define UMF_MAX_GPA_REDIRECTS 128

static struct { void* target; void* hook; } g_gpa[UMF_MAX_GPA_REDIRECTS];
static int                g_gpa_count = 0;
static SRWLOCK            g_gpa_lock  = SRWLOCK_INIT;
static PFN_GetProcAddress g_orig_GetProcAddress = NULL;
static bool               g_gpa_hooked = false;

static FARPROC WINAPI umf_hook_GetProcAddress(HMODULE module, LPCSTR name) {
    FARPROC real = g_orig_GetProcAddress
                 ? g_orig_GetProcAddress(module, name)
                 : NULL;

    void* redirect = NULL;
    AcquireSRWLockShared(&g_gpa_lock);
    for (int i = 0; i < g_gpa_count; i++) {
        if (g_gpa[i].target == (void*)real) {
            redirect = g_gpa[i].hook;
            break;
        }
    }
    ReleaseSRWLockShared(&g_gpa_lock);

    return redirect ? (FARPROC)redirect : real;
}

bool umf_install_getprocaddress_hook(void* target_resolved, void* hook) {
    if (!target_resolved || !hook) return false;

    AcquireSRWLockExclusive(&g_gpa_lock);
    if (g_gpa_count >= UMF_MAX_GPA_REDIRECTS) {
        ReleaseSRWLockExclusive(&g_gpa_lock);
        UMF_ERROR("GetProcAddress redirect table full");
        return false;
    }
    g_gpa[g_gpa_count].target = target_resolved;
    g_gpa[g_gpa_count].hook   = hook;
    g_gpa_count++;
    bool need_hook = !g_gpa_hooked;
    ReleaseSRWLockExclusive(&g_gpa_lock);

    if (need_hook) {
        void* gpa = umf_resolve_function("kernel32.dll", "GetProcAddress");
        if (!gpa) {
            UMF_ERROR("Cannot resolve GetProcAddress for fallback hook");
            return false;
        }
        if (!umf_register_hook_addr(gpa, "GetProcAddress",
                                    (void*)&umf_hook_GetProcAddress, 0, NULL,
                                    (void**)&g_orig_GetProcAddress)) {
            return false;
        }
        umf_apply_pending_batch();
        g_gpa_hooked = true;
        UMF_INFO("GetProcAddress fallback hook installed");
    }
    return true;
}
