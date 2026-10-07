/*
 * umf_mod_loader.c — §MOD: mod registry + loader
 *
 * Loads native mods (DLLs exporting umf_mod_init / umf_mod_shutdown) from a
 * mod.json manifest, tracks them in a stable registry (so UmfMod* passed as
 * a hook owner stays valid), orders directory loads by dependency, and tears
 * everything down on shutdown.
 *
 * Lua mods are recognised by the manifest but their hook bindings are a
 * follow-up; umf_mod_load reports them as unsupported for now.
 */

#include "umf/umf.h"
#include <stdio.h>

typedef struct {
    UmfMod           mod;        /* stable: handed to hooks as owner_mod */
    UmfModManifest   manifest;
    UmfModShutdownFn shutdown;
    bool             used;
} UmfLoadedMod;

#define UMF_MAX_MODS 64

static UmfLoadedMod g_mods[UMF_MAX_MODS];
static int          g_mod_count = 0;
static SRWLOCK      g_mod_lock  = SRWLOCK_INIT;

/* ── Dependency ordering (DFS post-order) ── */

static bool topo_dfs(const UmfModManifest* mods, int n, int i,
                     int* state, int* order, int* oi) {
    if (state[i] == 2) return true;
    if (state[i] == 1) return false;        /* cycle */
    state[i] = 1;

    for (int d = 0; d < mods[i].dep_count; d++) {
        int di = -1;
        for (int j = 0; j < n; j++) {
            if (strcmp(mods[j].name, mods[i].deps[d]) == 0) { di = j; break; }
        }
        if (di >= 0) {
            if (!topo_dfs(mods, n, di, state, order, oi)) return false;
        } else {
            UMF_WARN("Mod '%s' depends on missing '%s'",
                     mods[i].name, mods[i].deps[d]);
        }
    }

    state[i] = 2;
    order[(*oi)++] = i;
    return true;
}

bool umf_mod_topo_sort(const UmfModManifest* mods, int n, int* out_order) {
    if (n <= 0 || n > UMF_MAX_MODS) return false;
    int state[UMF_MAX_MODS];
    memset(state, 0, sizeof(state));
    int oi = 0;
    for (int i = 0; i < n; i++) {
        if (state[i] == 0 && !topo_dfs(mods, n, i, state, out_order, &oi))
            return false;
    }
    return oi == n;
}

/* ── Registry helpers ── */

UmfMod* umf_mod_find(const char* name) {
    UmfMod* found = NULL;
    AcquireSRWLockShared(&g_mod_lock);
    for (int i = 0; i < g_mod_count; i++) {
        if (g_mods[i].used && strcmp(g_mods[i].mod.name, name) == 0) {
            found = &g_mods[i].mod;
            break;
        }
    }
    ReleaseSRWLockShared(&g_mod_lock);
    return found;
}

int umf_mod_count(void) {
    int n = 0;
    AcquireSRWLockShared(&g_mod_lock);
    for (int i = 0; i < g_mod_count; i++) if (g_mods[i].used) n++;
    ReleaseSRWLockShared(&g_mod_lock);
    return n;
}

int umf_mod_list(UmfModInfo* out, int max) {
    if (!out || max <= 0) return 0;

    AcquireSRWLockShared(&g_mod_lock);
    int n = 0;
    for (int i = 0; i < g_mod_count && n < max; i++) {
        if (!g_mods[i].used) continue;
        UmfMod* m = &g_mods[i].mod;

        strncpy(out[n].name, m->name, sizeof(out[n].name) - 1);
        out[n].name[sizeof(out[n].name) - 1] = '\0';
        strncpy(out[n].version, m->version, sizeof(out[n].version) - 1);
        out[n].version[sizeof(out[n].version) - 1] = '\0';
        out[n].type         = (int)m->type;
        out[n].capabilities = m->capabilities;
        out[n].priority     = m->priority;
        out[n].active       = m->active;
        n++;
    }
    ReleaseSRWLockShared(&g_mod_lock);
    return n;
}

/* Directory of a file path (everything up to the last slash), into dst. */
static void dir_of(const char* path, char* dst, size_t dstlen) {
    strncpy(dst, path, dstlen - 1);
    dst[dstlen - 1] = '\0';
    char* last = NULL;
    for (char* s = dst; *s; s++) if (*s == '\\' || *s == '/') last = s;
    if (last) *last = '\0';
    else      dst[0] = '\0';
}

/* ── Native mod loading ── */

static bool load_native(UmfLoadedMod* slot, const char* manifest_dir) {
    char dll_path[MAX_PATH];
    if (manifest_dir[0])
        snprintf(dll_path, sizeof(dll_path), "%s\\%s",
                 manifest_dir, slot->manifest.entry);
    else
        snprintf(dll_path, sizeof(dll_path), "%s", slot->manifest.entry);

    HMODULE h = LoadLibraryA(dll_path);
    if (!h) {
        UMF_ERROR("Mod '%s': cannot load '%s' (%lu)",
                  slot->manifest.name, dll_path, GetLastError());
        return false;
    }

    UmfModInitFn init = (UmfModInitFn)(void*)GetProcAddress(h, "umf_mod_init");
    if (!init) {
        UMF_ERROR("Mod '%s': missing umf_mod_init export", slot->manifest.name);
        FreeLibrary(h);
        return false;
    }
    slot->shutdown =
        (UmfModShutdownFn)(void*)GetProcAddress(h, "umf_mod_shutdown");

    /* Populate the stable UmfMod handed to the mod (and to hooks). */
    memset(&slot->mod, 0, sizeof(slot->mod));
    strncpy(slot->mod.name, slot->manifest.name, sizeof(slot->mod.name) - 1);
    strncpy(slot->mod.version, slot->manifest.version,
            sizeof(slot->mod.version) - 1);
    slot->mod.module_handle = h;
    slot->mod.type          = UMF_MOD_NATIVE;
    slot->mod.capabilities  = slot->manifest.capabilities;
    slot->mod.priority      = slot->manifest.priority;
    slot->mod.active        = true;

    if (!init(&slot->mod)) {
        UMF_ERROR("Mod '%s': umf_mod_init returned false", slot->manifest.name);
        slot->mod.active = false;
        FreeLibrary(h);
        return false;
    }

    UMF_INFO("Mod '%s' v%s loaded (native, caps=0x%02X)",
             slot->mod.name, slot->mod.version, slot->mod.capabilities);
    return true;
}

bool umf_mod_load(const char* manifest_path) {
    UmfModManifest manifest;
    if (!umf_parse_manifest_file(manifest_path, &manifest)) return false;

    if (umf_mod_find(manifest.name)) {
        UMF_INFO("Mod '%s' already loaded — skipping", manifest.name);
        return true;
    }

    AcquireSRWLockExclusive(&g_mod_lock);
    if (g_mod_count >= UMF_MAX_MODS) {
        ReleaseSRWLockExclusive(&g_mod_lock);
        UMF_ERROR("Mod registry full (%d)", UMF_MAX_MODS);
        return false;
    }
    UmfLoadedMod* slot = &g_mods[g_mod_count];
    memset(slot, 0, sizeof(*slot));
    slot->manifest = manifest;
    slot->used = true;
    int my_index = g_mod_count;
    g_mod_count++;
    ReleaseSRWLockExclusive(&g_mod_lock);

    char dir[MAX_PATH];
    dir_of(manifest_path, dir, sizeof(dir));

    bool ok;
    if (manifest.type == UMF_MOD_LUA) {
        UMF_WARN("Mod '%s': Lua mods are not yet supported", manifest.name);
        ok = false;
    } else {
        ok = load_native(slot, dir);
    }

    if (!ok) {
        AcquireSRWLockExclusive(&g_mod_lock);
        slot->used = false;
        if (my_index == g_mod_count - 1) g_mod_count--;  /* reclaim tail slot */
        ReleaseSRWLockExclusive(&g_mod_lock);
        return false;
    }
    return true;
}

int umf_mod_load_dir(const char* mods_dir) {
    /* Collect "<mods_dir>\*\mod.json" manifests. */
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", mods_dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    static UmfModManifest manifests[UMF_MAX_MODS];
    static char           paths[UMF_MAX_MODS][MAX_PATH];
    int n = 0;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;

        char mpath[MAX_PATH];
        snprintf(mpath, sizeof(mpath), "%s\\%s\\mod.json",
                 mods_dir, fd.cFileName);
        if (n < UMF_MAX_MODS &&
            umf_parse_manifest_file(mpath, &manifests[n])) {
            strncpy(paths[n], mpath, MAX_PATH - 1);
            paths[n][MAX_PATH - 1] = '\0';
            n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    if (n == 0) return 0;

    int order[UMF_MAX_MODS];
    if (!umf_mod_topo_sort(manifests, n, order)) {
        UMF_ERROR("Mod dependency cycle in '%s' — not loading", mods_dir);
        return 0;
    }

    int loaded = 0;
    for (int i = 0; i < n; i++) {
        if (umf_mod_load(paths[order[i]])) loaded++;
    }
    return loaded;
}

void umf_mod_unload_all(void) {
    AcquireSRWLockExclusive(&g_mod_lock);
    /* Unload in reverse order (dependents before dependencies). */
    for (int i = g_mod_count - 1; i >= 0; i--) {
        UmfLoadedMod* m = &g_mods[i];
        if (!m->used) continue;

        if (m->shutdown) m->shutdown(&m->mod);
        if (m->mod.module_handle) FreeLibrary(m->mod.module_handle);
        UMF_INFO("Mod '%s' unloaded", m->mod.name);

        m->mod.active = false;
        m->used = false;
    }
    g_mod_count = 0;
    ReleaseSRWLockExclusive(&g_mod_lock);
}
