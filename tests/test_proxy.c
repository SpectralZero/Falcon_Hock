/*
 * test_proxy.c — §PROXY: export-table parse + forwarder generation.
 *
 * Uses umf_runtime.dll itself as the subject: it is already loaded, and its
 * umf_* exports give deterministic names to assert on. A hand-built table
 * exercises the by-ordinal (NONAME) path that umf_runtime's named-only
 * exports do not reach.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <string.h>

/* These live in the function's static storage to avoid a ~1 MB stack frame
 * (UmfExportTable embeds 4096 export records). */
static UmfExportTable g_from_mod;
static UmfExportTable g_from_file;
static UmfExportTable g_syn;

static bool has_named(const UmfExportTable* t, const char* name) {
    for (int i = 0; i < t->count; i++)
        if (!t->exports[i].by_ordinal && strcmp(t->exports[i].name, name) == 0)
            return true;
    return false;
}

void run_proxy_tests(void) {
    HMODULE h = GetModuleHandleA("umf_runtime.dll");
    CHECK(h != NULL, "umf_runtime.dll is loaded");

    /* ── Parse the export table from the loaded module ── */
    bool okm = umf_exports_from_module(h, "umf_runtime.dll", &g_from_mod);
    CHECK(okm, "exports_from_module succeeds");
    CHECK(okm && g_from_mod.count > 0, "module has exports");
    CHECK(okm && has_named(&g_from_mod, "umf_sym_init"),
          "exported umf_sym_init is found");
    CHECK(okm && has_named(&g_from_mod, "umf_rtti_from_object"),
          "exported umf_rtti_from_object is found");

    /* ── Parse the same DLL from disk; results must match ── */
    char path[MAX_PATH];
    DWORD plen = GetModuleFileNameA(h, path, MAX_PATH);
    CHECK(plen > 0, "resolved umf_runtime.dll path");

    bool okf = (plen > 0) && umf_exports_from_file(path, &g_from_file);
    CHECK(okf, "exports_from_file succeeds");
    CHECK(okf && g_from_file.count == g_from_mod.count,
          "file and module export counts match");
    CHECK(okf && has_named(&g_from_file, "umf_sym_init"),
          "file parse finds umf_sym_init too");

    /* ── .def generation from the real table ── */
    static char def[65536];
    int dn = umf_proxy_generate_def(&g_from_mod, "orig", def, sizeof(def));
    CHECK(dn > 0 && strstr(def, "EXPORTS") != NULL, "def has an EXPORTS section");
    CHECK(strstr(def, "umf_sym_init=orig.umf_sym_init") != NULL,
          "def forwards a named export");

    /* ── snprintf-style truncation contract ── */
    char small[16];
    int need = umf_proxy_generate_def(&g_from_mod, "orig", small, sizeof(small));
    CHECK(need > (int)sizeof(small), "def returns the full length it needs");
    CHECK(strlen(small) < sizeof(small), "def NUL-terminates a short buffer");

    /* ── pragma generation ── */
    static char prag[65536];
    int pn = umf_proxy_generate_pragma(&g_from_mod, "orig", prag, sizeof(prag));
    CHECK(pn > 0 &&
          strstr(prag, "#pragma comment(linker, \"/export:umf_sym_init=orig.umf_sym_init\")")
              != NULL,
          "pragma forwards a named export");

    /* ── Synthetic table covers the by-ordinal (NONAME) path ── */
    memset(&g_syn, 0, sizeof(g_syn));
    snprintf(g_syn.module, sizeof(g_syn.module), "synthetic.dll");
    g_syn.count = 3;
    snprintf(g_syn.exports[0].name, sizeof(g_syn.exports[0].name), "Alpha");
    g_syn.exports[0].ordinal = 1;
    snprintf(g_syn.exports[1].name, sizeof(g_syn.exports[1].name), "Beta");
    g_syn.exports[1].ordinal = 2;
    g_syn.exports[2].name[0]    = 0;        /* NONAME */
    g_syn.exports[2].by_ordinal = true;
    g_syn.exports[2].ordinal    = 9;

    static char sdef[1024];
    umf_proxy_generate_def(&g_syn, "real", sdef, sizeof(sdef));
    CHECK(strstr(sdef, "Alpha=real.Alpha") != NULL, "synthetic def named forwarder");
    CHECK(strstr(sdef, "ord9=real.#9 @9 NONAME") != NULL,
          "synthetic def by-ordinal forwarder");

    static char sprag[1024];
    umf_proxy_generate_pragma(&g_syn, "real", sprag, sizeof(sprag));
    CHECK(strstr(sprag, "/export:ord9=real.#9,@9,NONAME") != NULL,
          "synthetic pragma by-ordinal forwarder");

    /* ── Robustness ── */
    CHECK(!umf_exports_from_module(NULL, "x", &g_from_mod),
          "from_module rejects a NULL module");
    CHECK(!umf_exports_from_file("Z:\\no\\such\\umf_missing_zzz.dll", &g_from_file),
          "from_file rejects a missing path");
    char tiny[8] = "keep";
    int zero = umf_proxy_generate_def(NULL, "orig", tiny, sizeof(tiny));
    CHECK(zero == 0 && tiny[0] == 0, "generate_def rejects a NULL table");
}
