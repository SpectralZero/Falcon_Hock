/*
 * umf_proxy.c — §PROXY: proxy / forwarder DLL generator
 *
 * Reads a DLL's export directory and emits forwarders that point every export
 * at a renamed copy of the original. Two output shapes:
 *
 *   .def     — an EXPORTS section (feed to the linker with /DEF)
 *   pragma   — C source using #pragma comment(linker, "/export:...") lines
 *
 * Named exports forward by name ("Foo=orig.Foo"); unnamed (NONAME) exports
 * forward by ordinal ("ord7=orig.#7 @7 NONAME"). The module only produces
 * text — it never writes a file, builds, or injects.
 *
 * On-disk DLLs are mapped with DONT_RESOLVE_DLL_REFERENCES: the image is laid
 * out so RVAs resolve exactly like a loaded module, but DllMain never runs and
 * imports are not resolved, so reading an arbitrary DLL is side-effect free.
 */

#include "umf/umf.h"
#include <stdio.h>
#include <stdarg.h>

/* ────────────────────────────────────────────────────────────────
 * Export-table parsing
 * ──────────────────────────────────────────────────────────────── */

static bool parse_image_exports(const uint8_t* base, const char* modname,
                                UmfExportTable* out) {
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    IMAGE_DATA_DIRECTORY dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return false;

    IMAGE_EXPORT_DIRECTORY* exp =
        (IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
    DWORD* funcs = (DWORD*)(base + exp->AddressOfFunctions);
    DWORD* names = (DWORD*)(base + exp->AddressOfNames);
    WORD*  ords  = (WORD*)(base + exp->AddressOfNameOrdinals);

    out->count        = 0;
    out->base_ordinal = exp->Base;
    snprintf(out->module, sizeof(out->module), "%s", modname ? modname : "");

    DWORD exp_start = dir.VirtualAddress;
    DWORD exp_end   = dir.VirtualAddress + dir.Size;

    for (DWORD i = 0; i < exp->NumberOfFunctions; i++) {
        DWORD frva = funcs[i];
        if (frva == 0) continue;                       /* unused slot (hole) */
        if (out->count >= UMF_PROXY_MAX_EXPORTS) break;

        UmfExport* e = &out->exports[out->count];
        memset(e, 0, sizeof(*e));
        e->ordinal   = (uint16_t)(exp->Base + i);
        e->forwarder = (frva >= exp_start && frva < exp_end);

        const char* nm = NULL;                         /* resolve the name */
        for (DWORD k = 0; k < exp->NumberOfNames; k++) {
            if (ords[k] == i) { nm = (const char*)(base + names[k]); break; }
        }
        if (nm) {
            snprintf(e->name, sizeof(e->name), "%s", nm);
            e->by_ordinal = false;
        } else {
            e->name[0]    = 0;
            e->by_ordinal = true;
        }
        out->count++;
    }
    return true;
}

static void base_name_of(const char* path, char* out, size_t outlen) {
    const char* base = path;
    for (const char* p = path; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    snprintf(out, outlen, "%s", base);
}

bool umf_exports_from_module(HMODULE module, const char* module_name,
                             UmfExportTable* out) {
    if (!module || !out) return false;

    char derived[128] = {0};
    if (!module_name || !module_name[0]) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(module, path, MAX_PATH))
            base_name_of(path, derived, sizeof(derived));
        module_name = derived;
    }

    bool ok = false;
    __try {
        ok = parse_image_exports((const uint8_t*)module, module_name, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

bool umf_exports_from_file(const char* dll_path, UmfExportTable* out) {
    if (!dll_path || !dll_path[0] || !out) return false;

    wchar_t wpath[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, dll_path, -1, wpath, MAX_PATH) == 0)
        return false;

    HMODULE h = LoadLibraryExW(wpath, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!h) return false;

    char base[128];
    base_name_of(dll_path, base, sizeof(base));

    bool ok = false;
    __try {
        ok = parse_image_exports((const uint8_t*)h, base, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    FreeLibrary(h);
    return ok;
}

/* ────────────────────────────────────────────────────────────────
 * Text generation (snprintf-style bounded append)
 * ──────────────────────────────────────────────────────────────── */

static int emit(char* out, size_t outlen, int total, const char* fmt, ...) {
    char*  dst = (total < (int)outlen) ? out + total : NULL;
    size_t rem = (total < (int)outlen) ? outlen - (size_t)total : 0;

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(dst, rem, fmt, ap);
    va_end(ap);

    if (n < 0) return total;
    return total + n;
}

int umf_proxy_generate_def(const UmfExportTable* tbl, const char* real_module,
                           char* out, size_t outlen) {
    if (!out || outlen == 0) return 0;
    out[0] = 0;
    if (!tbl) return 0;
    const char* real = (real_module && real_module[0]) ? real_module : "orig";

    int t = emit(out, outlen, 0, "EXPORTS\n");
    for (int i = 0; i < tbl->count; i++) {
        const UmfExport* e = &tbl->exports[i];
        if (e->by_ordinal)
            t = emit(out, outlen, t, "    ord%u=%s.#%u @%u NONAME\n",
                     e->ordinal, real, e->ordinal, e->ordinal);
        else
            t = emit(out, outlen, t, "    %s=%s.%s\n", e->name, real, e->name);
    }
    out[outlen - 1] = 0;
    return t;
}

int umf_proxy_generate_pragma(const UmfExportTable* tbl, const char* real_module,
                              char* out, size_t outlen) {
    if (!out || outlen == 0) return 0;
    out[0] = 0;
    if (!tbl) return 0;
    const char* real = (real_module && real_module[0]) ? real_module : "orig";

    int t = emit(out, outlen, 0,
                 "/* UMF proxy forwarders for %s (%d exports) */\n",
                 tbl->module, tbl->count);
    for (int i = 0; i < tbl->count; i++) {
        const UmfExport* e = &tbl->exports[i];
        if (e->by_ordinal)
            t = emit(out, outlen, t,
                     "#pragma comment(linker, \"/export:ord%u=%s.#%u,@%u,NONAME\")\n",
                     e->ordinal, real, e->ordinal, e->ordinal);
        else
            t = emit(out, outlen, t,
                     "#pragma comment(linker, \"/export:%s=%s.%s\")\n",
                     e->name, real, e->name);
    }
    out[outlen - 1] = 0;
    return t;
}
