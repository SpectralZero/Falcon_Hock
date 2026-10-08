/*
 * umf_lua_reload.c — §LUARELOAD: fail-safe Lua script hot-reload
 *
 * Watches one .lua file by last-write time + size and, on a change, loads it
 * into a fresh sandbox. The reload is atomic and fail-safe: a new sandbox is
 * built and the chunk run *before* the old one is retired, so a script that
 * fails to compile or run leaves the previous good state live and only records
 * the error. Change detection is edge-triggered — the file stamp is updated
 * after every attempt — so a persistently broken file is retried only when it
 * changes again, not on every poll.
 *
 * This module treats lua_State as opaque and goes through the public umf_lua_*
 * API, so it pulls in no Lua headers of its own.
 */

#include "umf/umf.h"

struct UmfLuaReloader {
    char              path[MAX_PATH];
    UmfMod*           owner;
    struct lua_State* L;
    FILETIME          last_write;
    uint64_t          last_size;
    int               generation;
    bool              loaded;
    char              err[256];
};

static bool stat_file(const char* path, FILETIME* mtime, uint64_t* size) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return false;
    if (mtime) *mtime = fad.ftLastWriteTime;
    if (size)  *size  = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return true;
}

static void stamp(UmfLuaReloader* r) {
    stat_file(r->path, &r->last_write, &r->last_size);
}

static bool read_file(const char* path, char** out, size_t* out_n) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 ||
        sz.QuadPart > (1024 * 1024)) {          /* 1 MiB script cap */
        CloseHandle(h);
        return false;
    }
    size_t n = (size_t)sz.QuadPart;
    char* b = (char*)malloc(n + 1);
    if (!b) { CloseHandle(h); return false; }

    DWORD rd = 0;
    BOOL ok = ReadFile(h, b, (DWORD)n, &rd, NULL);
    CloseHandle(h);
    if (!ok || rd != n) { free(b); return false; }

    b[n] = 0;
    *out = b;
    *out_n = n;
    return true;
}

UmfLuaReloader* umf_lua_reload_create(const char* script_path, UmfMod* owner) {
    if (!script_path || !script_path[0]) return NULL;
    UmfLuaReloader* r = (UmfLuaReloader*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    snprintf(r->path, sizeof(r->path), "%s", script_path);
    r->owner = owner;
    return r;
}

void umf_lua_reload_destroy(UmfLuaReloader* r) {
    if (!r) return;
    if (r->L) umf_lua_destroy_sandbox(r->L);
    free(r);
}

bool umf_lua_reload_changed(UmfLuaReloader* r) {
    if (!r) return false;
    FILETIME mt; uint64_t sz;
    if (!stat_file(r->path, &mt, &sz)) return !r->loaded;  /* missing: retry only pre-load */
    if (!r->loaded) return true;
    if (CompareFileTime(&mt, &r->last_write) != 0) return true;
    return sz != r->last_size;
}

int umf_lua_reload_force(UmfLuaReloader* r) {
    if (!r) return -1;

    char* src = NULL;
    size_t n = 0;
    if (!read_file(r->path, &src, &n)) {
        snprintf(r->err, sizeof(r->err), "cannot read script '%s'", r->path);
        return -1;
    }

    struct lua_State* nl = umf_lua_create_sandbox();
    if (!nl) {
        free(src);
        snprintf(r->err, sizeof(r->err), "sandbox creation failed");
        return -1;
    }
    umf_lua_setup_api(nl, r->owner);
    bool ok = umf_lua_run_string(nl, src);
    free(src);

    stamp(r);   /* edge-trigger: record the stamp whether or not it ran */

    if (!ok) {
        umf_lua_destroy_sandbox(nl);
        snprintf(r->err, sizeof(r->err),
                 "script '%s' failed to compile or run", r->path);
        return -1;
    }

    if (r->L) umf_lua_destroy_sandbox(r->L);   /* retire the old one on success */
    r->L = nl;
    r->loaded = true;
    r->generation++;
    r->err[0] = 0;
    UMF_INFO("Lua hot-reload: '%s' loaded (generation %d)", r->path, r->generation);
    return 1;
}

int umf_lua_reload_poll(UmfLuaReloader* r) {
    if (!r) return -1;
    if (!umf_lua_reload_changed(r)) return 0;
    return umf_lua_reload_force(r);
}

struct lua_State* umf_lua_reload_state(UmfLuaReloader* r) {
    return r ? r->L : NULL;
}

int umf_lua_reload_generation(UmfLuaReloader* r) {
    return r ? r->generation : 0;
}

const char* umf_lua_reload_error(UmfLuaReloader* r) {
    return r ? r->err : "";
}
