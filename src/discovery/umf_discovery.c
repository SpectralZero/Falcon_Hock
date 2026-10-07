/*
 * umf_discovery.c — §DISCOVERY: RTTI/vtable scanner + symbol resolver
 *
 * Two analysis tools that put names on raw addresses:
 *
 *   RTTI reader — MSVC emits, immediately before each C++ vtable, a pointer to
 *   a _RTTICompleteObjectLocator (COL). On x64 the COL stores image-relative
 *   RVAs plus `pSelf`, the RVA of the COL itself, which lets us recover the
 *   owning module base from any COL address. From the COL we reach the
 *   TypeDescriptor whose trailing string is the decorated class name
 *   (".?AVFoo@@"). Everything is read through SEH guards, so probing a bad
 *   pointer returns false instead of faulting — the same discipline as §AOB.
 *
 *   Symbol resolver — a thin, lock-guarded wrapper over dbghelp. dbghelp is
 *   single-threaded, so every Sym* call is serialised. init is reference
 *   counted so nested init/cleanup pairs are safe.
 *
 * This is data-only analysis: nothing in the target is modified.
 */

#include "umf/umf.h"
#include <dbghelp.h>

/* ────────────────────────────────────────────────────────────────
 * Fault-guarded primitives
 * ──────────────────────────────────────────────────────────────── */

static bool safe_read(const void* src, void* dst, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool safe_read_cstr(const char* src, char* out, size_t outlen) {
    if (outlen == 0) return false;
    for (size_t i = 0; i < outlen - 1; i++) {
        char c;
        if (!safe_read(src + i, &c, 1)) { out[i] = 0; return i > 0; }
        out[i] = c;
        if (c == 0) return true;
    }
    out[outlen - 1] = 0;
    return true;
}

static bool addr_is_exec(const void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return false;
    DWORD prot = mbi.Protect & 0xFF;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

static bool module_range(HMODULE mod, uintptr_t* base, uintptr_t* size) {
    uint8_t* b = (uint8_t*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)b;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    *base = (uintptr_t)b;
    *size = (uintptr_t)nt->OptionalHeader.SizeOfImage;
    return true;
}

/* ────────────────────────────────────────────────────────────────
 * RTTI (MSVC x64 Complete Object Locator)
 * ──────────────────────────────────────────────────────────────── */

/* _RTTICompleteObjectLocator (x64 variant: signature == 1, RVAs + pSelf). */
typedef struct {
    DWORD signature;
    DWORD offset;
    DWORD cd_offset;
    DWORD type_descriptor;    /* RVA of TypeDescriptor      */
    DWORD class_descriptor;   /* RVA of ClassHierarchyDesc  */
    DWORD self;               /* RVA of this COL (x64 only) */
} UmfRttiCol;

/* Count virtual slots: walk forward while each entry points into executable
 * memory. Bounded so a bogus vtable can never loop unbounded. */
static int count_vfuncs(void** vtable) {
    int n = 0;
    for (; n < 4096; n++) {
        void* fn;
        if (!safe_read((char*)vtable + (size_t)n * sizeof(void*), &fn, sizeof(fn)))
            break;
        if (!addr_is_exec(fn)) break;
    }
    return n;
}

/* Validate a candidate COL pointer and, on success, fill name/base/col. */
static bool read_col_class(const void* col_ptr, UmfRttiClass* out) {
    UmfRttiCol col;
    if (!safe_read(col_ptr, &col, sizeof(col))) return false;
    if (col.signature != 1) return false;               /* x64 COL marker */

    uintptr_t base = (uintptr_t)col_ptr - col.self;      /* recover image base */

    IMAGE_DOS_HEADER dos;
    if (!safe_read((const void*)base, &dos, sizeof(dos))) return false;
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return false;

    /* TypeDescriptor: { void* vftable; void* spare; char name[]; } */
    const char* name_ptr =
        (const char*)(base + col.type_descriptor + 2 * sizeof(void*));
    char raw[256];
    if (!safe_read_cstr(name_ptr, raw, sizeof(raw))) return false;
    if (!(raw[0] == '.' && raw[1] == '?' && raw[2] == 'A')) return false;

    out->col        = (void*)col_ptr;
    out->image_base = base;
    snprintf(out->raw_name, sizeof(out->raw_name), "%s", raw);
    umf_rtti_demangle(raw, out->name, sizeof(out->name));
    return true;
}

void umf_rtti_demangle(const char* raw, char* out, size_t outlen) {
    if (!out || outlen == 0) return;
    out[0] = 0;
    if (!raw) return;

    /* Expect ".?A<kind>" (kind: V class, U struct, W enum, ...). */
    if (!(raw[0] == '.' && raw[1] == '?' && raw[2] == 'A' && raw[3] != 0)) return;

    const char* inner = raw + 4;             /* skip ".?AV" */
    size_t len = strlen(inner);
    while (len >= 2 && inner[len - 1] == '@' && inner[len - 2] == '@')
        len -= 2;                            /* strip trailing "@@" */

    /* Scope tokens are '@'-separated and stored innermost-first. */
    const char* starts[32];
    size_t      lens[32];
    int         nt = 0;
    size_t      tok_start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || inner[i] == '@') {
            if (i > tok_start && nt < 32) {
                starts[nt] = inner + tok_start;
                lens[nt]   = i - tok_start;
                nt++;
            }
            tok_start = i + 1;
        }
    }

    size_t pos = 0;
    for (int t = nt - 1; t >= 0; t--) {
        if (pos && pos + 2 < outlen) { out[pos++] = ':'; out[pos++] = ':'; }
        for (size_t k = 0; k < lens[t] && pos + 1 < outlen; k++)
            out[pos++] = starts[t][k];
    }
    out[pos < outlen ? pos : outlen - 1] = 0;
}

bool umf_rtti_from_vtable(void** vtable, UmfRttiClass* out) {
    if (!vtable || !out) return false;
    memset(out, 0, sizeof(*out));

    void* col_ptr;
    if (!safe_read((const char*)vtable - sizeof(void*), &col_ptr, sizeof(col_ptr)))
        return false;
    if (!col_ptr) return false;
    if (!read_col_class(col_ptr, out)) return false;

    out->vtable      = vtable;
    out->vfunc_count = count_vfuncs(vtable);
    return true;
}

bool umf_rtti_from_object(const void* object, UmfRttiClass* out) {
    if (!object || !out) return false;
    void** vtable;
    if (!safe_read(object, &vtable, sizeof(vtable))) return false;
    return umf_rtti_from_vtable(vtable, out);
}

int umf_rtti_scan_module(const char* module_name, UmfRttiClass* out, int max) {
    if (!out || max <= 0) return 0;

    HMODULE mod = (module_name && module_name[0])
                ? GetModuleHandleA(module_name)
                : GetModuleHandleW(NULL);
    if (!mod) return 0;

    uintptr_t base, size;
    if (!module_range(mod, &base, &size)) return 0;
    uintptr_t end = base + size;

    int count = 0;
    for (uintptr_t p = base; p + sizeof(void*) <= end; ) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) == 0) break;

        uintptr_t region_end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (region_end > end) region_end = end;

        DWORD prot = mbi.Protect & 0xFF;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                        (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                         prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                         prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY);

        if (readable) {
            for (uintptr_t s = p; s + sizeof(void*) <= region_end; s += sizeof(void*)) {
                void* v;
                if (!safe_read((const void*)s, &v, sizeof(v))) break;
                if (!v) continue;

                UmfRttiClass cls;
                memset(&cls, 0, sizeof(cls));
                if (!read_col_class(v, &cls)) continue;
                if (cls.image_base != base) continue;   /* COL must be ours */

                /* The slot holding the COL pointer is vtable[-1]; the vtable
                 * proper starts at the next pointer. */
                void** vt = (void**)(s + sizeof(void*));

                bool dup = false;
                for (int i = 0; i < count && i < max; i++)
                    if (out[i].col == cls.col) { dup = true; break; }
                if (dup) continue;

                cls.vtable      = vt;
                cls.vfunc_count = count_vfuncs(vt);
                if (count < max) out[count] = cls;
                count++;
            }
        }

        if (region_end <= p) break;
        p = region_end;
    }

    return count < max ? count : max;
}

/* ────────────────────────────────────────────────────────────────
 * Symbol resolver (dbghelp — serialised, reference counted)
 * ──────────────────────────────────────────────────────────────── */

static SRWLOCK g_sym_lock     = SRWLOCK_INIT;
static int     g_sym_refcount = 0;

bool umf_sym_init(void) {
    AcquireSRWLockExclusive(&g_sym_lock);
    bool ok = true;
    if (g_sym_refcount == 0) {
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                      SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
        ok = SymInitialize(GetCurrentProcess(), NULL, TRUE) != FALSE;
        if (ok) g_sym_refcount = 1;
    } else {
        g_sym_refcount++;
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
    return ok;
}

void umf_sym_cleanup(void) {
    AcquireSRWLockExclusive(&g_sym_lock);
    if (g_sym_refcount > 0 && --g_sym_refcount == 0)
        SymCleanup(GetCurrentProcess());
    ReleaseSRWLockExclusive(&g_sym_lock);
}

bool umf_sym_from_address(const void* addr, UmfSymbol* out) {
    if (!addr || !out) return false;
    memset(out, 0, sizeof(*out));

    AcquireSRWLockExclusive(&g_sym_lock);
    bool ok = false;
    if (g_sym_refcount > 0) {
        char buf[sizeof(SYMBOL_INFO) + UMF_SYM_MAX_NAME];
        SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
        memset(si, 0, sizeof(SYMBOL_INFO));
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen   = UMF_SYM_MAX_NAME - 1;

        DWORD64 disp = 0;
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)addr, &disp, si)) {
            snprintf(out->name, sizeof(out->name), "%s", si->Name);
            out->address      = (uintptr_t)si->Address;
            out->displacement = disp;

            IMAGEHLP_MODULE64 mi;
            memset(&mi, 0, sizeof(mi));
            mi.SizeOfStruct = sizeof(mi);
            if (SymGetModuleInfo64(GetCurrentProcess(), si->ModBase, &mi))
                snprintf(out->module, sizeof(out->module), "%s", mi.ModuleName);
            ok = true;
        }
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
    return ok;
}

uintptr_t umf_sym_resolve(const char* name) {
    if (!name || !name[0]) return 0;

    AcquireSRWLockExclusive(&g_sym_lock);
    uintptr_t addr = 0;
    if (g_sym_refcount > 0) {
        char buf[sizeof(SYMBOL_INFO) + UMF_SYM_MAX_NAME];
        SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
        memset(si, 0, sizeof(SYMBOL_INFO));
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen   = UMF_SYM_MAX_NAME - 1;
        if (SymFromName(GetCurrentProcess(), name, si))
            addr = (uintptr_t)si->Address;
    }
    ReleaseSRWLockExclusive(&g_sym_lock);
    return addr;
}

int umf_undecorate(const char* decorated, char* out, size_t outlen) {
    if (!decorated || !out || outlen == 0) return 0;
    out[0] = 0;
    DWORD n = UnDecorateSymbolName(decorated, out, (DWORD)outlen, UNDNAME_COMPLETE);
    return (int)n;
}
