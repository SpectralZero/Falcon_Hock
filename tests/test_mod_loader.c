/*
 * test_mod_loader.c — §MOD loader + manifest + capability tests.
 *
 * Writes a mod.json next to the test exe (pointing at the native test-mod
 * DLL), loads it, verifies the mod's hook is live, exercises capability
 * enforcement and dependency ordering, then unloads and checks restoration.
 */
#include "umf/umf.h"
#include "test_framework.h"
#include <stdio.h>

#define NATIVE_MOD_SENTINEL 0x4D4F4421u

/* ── capability allow-path target/hook ── */
typedef int (*add2_fn)(int, int);
static int     cap_target(int a, int b) { volatile int r = a + b; return r; }
static add2_fn g_cap_orig = NULL;
static int     cap_hook(int a, int b)  { return g_cap_orig ? g_cap_orig(a, b) + 1 : -1; }

static void exe_dir(char* buf, size_t len) {
    GetModuleFileNameA(NULL, buf, (DWORD)len);
    char* last = NULL;
    for (char* s = buf; *s; s++) if (*s == '\\' || *s == '/') last = s;
    if (last) *last = '\0';
}

void run_mod_loader_tests(void) {
    if (!umf_init()) { CHECK(0, "umf_init for mod loader"); return; }

    char dir[MAX_PATH];
    exe_dir(dir, sizeof(dir));

    char manifest_path[MAX_PATH];
    snprintf(manifest_path, sizeof(manifest_path),
             "%s\\native_demo.mod.json", dir);

    /* Write a manifest pointing at the native test-mod DLL. */
    FILE* f = NULL;
    fopen_s(&f, manifest_path, "wb");
    CHECK(f != NULL, "write mod manifest");
    if (f) {
        fprintf(f,
            "{\n"
            "  \"name\": \"native_demo\",\n"
            "  \"version\": \"1.0.0\",\n"
            "  \"type\": \"native\",\n"
            "  \"entry\": \"umf_test_mod_native.dll\",\n"
            "  \"priority\": 10,\n"
            "  \"dependencies\": [],\n"
            "  \"capabilities\": [\"hook\"]\n"
            "}\n");
        fclose(f);
    }

    DWORD real_pid = GetCurrentProcessId();

    /* ── Load the mod ── */
    bool loaded = umf_mod_load(manifest_path);
    CHECK(loaded, "umf_mod_load native mod");
    CHECK(umf_mod_count() == 1, "one mod registered");

    UmfMod* m = umf_mod_find("native_demo");
    CHECK(m != NULL, "umf_mod_find locates the mod");
    CHECK(m && (m->capabilities & UMF_CAP_HOOK), "manifest capability parsed");

    CHECK(GetCurrentProcessId() == NATIVE_MOD_SENTINEL,
          "mod's IAT hook is live after load");

    /* ── Capability enforcement ── */
    UmfMod mod_bad;  memset(&mod_bad, 0, sizeof(mod_bad));
    strcpy_s(mod_bad.name, sizeof(mod_bad.name), "nocaps");
    mod_bad.capabilities = 0;
    bool denied = umf_register_hook_addr((void*)&cap_target, "cap",
                                         (void*)&cap_hook, 0, &mod_bad, NULL);
    CHECK(!denied, "hook denied for mod without 'hook' capability");

    UmfMitigationStatus mit;
    umf_detect_all_mitigations(&mit);
    bool inline_ok = (umf_viable_strategies(&mit) & UMF_STRAT_INLINE) &&
                     umf_can_write_code_page(&mit);
    if (inline_ok) {
        UmfMod mod_ok; memset(&mod_ok, 0, sizeof(mod_ok));
        strcpy_s(mod_ok.name, sizeof(mod_ok.name), "capmod");
        mod_ok.capabilities = UMF_CAP_HOOK;

        volatile add2_fn call = cap_target;
        bool allowed = umf_register_hook_addr((void*)call, "cap",
                                              (void*)&cap_hook, 0, &mod_ok,
                                              (void**)&g_cap_orig);
        CHECK(allowed, "hook allowed for mod with 'hook' capability");
        umf_apply_pending_batch();
        CHECK(call(2, 3) == 6, "capability-gated hook runs (2+3+1)");

        UmfHookTarget* t = umf_hook_registry_find((void*)call);
        if (t) { umf_queue_hook_disable(t); umf_apply_pending_batch(); }
        CHECK(call(2, 3) == 5, "allow-path hook cleaned up");
    }

    /* ── Dependency topological order (pure function) ── */
    UmfModManifest ms[3];
    memset(ms, 0, sizeof(ms));
    strcpy_s(ms[0].name, sizeof(ms[0].name), "C");
    strcpy_s(ms[0].deps[0], sizeof(ms[0].deps[0]), "B"); ms[0].dep_count = 1;
    strcpy_s(ms[1].name, sizeof(ms[1].name), "B");
    strcpy_s(ms[1].deps[0], sizeof(ms[1].deps[0]), "A"); ms[1].dep_count = 1;
    strcpy_s(ms[2].name, sizeof(ms[2].name), "A");

    int order[3];
    bool sorted = umf_mod_topo_sort(ms, 3, order);
    CHECK(sorted, "topo-sort succeeds (no cycle)");

    int posA = -1, posB = -1, posC = -1;
    for (int i = 0; i < 3; i++) {
        if (strcmp(ms[order[i]].name, "A") == 0) posA = i;
        if (strcmp(ms[order[i]].name, "B") == 0) posB = i;
        if (strcmp(ms[order[i]].name, "C") == 0) posC = i;
    }
    CHECK(posA < posB && posB < posC, "dependencies ordered before dependents");

    /* ── Unload ── */
    umf_mod_unload_all();
    CHECK(umf_mod_count() == 0, "all mods unloaded");
    CHECK(GetCurrentProcessId() == real_pid, "mod hook removed on unload");

    umf_shutdown();
    CHECK(1, "umf_shutdown after mod loader tests");
}
