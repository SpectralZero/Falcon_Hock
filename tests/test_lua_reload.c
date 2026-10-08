/*
 * test_lua_reload.c — §LUARELOAD: fail-safe Lua hot-reload mechanics.
 *
 * Writes a .lua file in the temp dir, loads it through the reloader, edits it,
 * and confirms the sandbox is swapped. A deliberately broken edit must keep
 * the previous good sandbox live. Globals are read back via the Lua C API.
 */
#include "umf/umf.h"
#include "test_framework.h"

#include <lua.h>
#include <string.h>

static void write_file(const char* path, const char* content) {
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, content, (DWORD)strlen(content), &w, NULL);
        CloseHandle(h);
    }
}

static lua_Integer global_int(lua_State* L, const char* name) {
    if (!L) return -1;
    lua_getglobal(L, name);
    lua_Integer v = lua_tointeger(L, -1);
    lua_pop(L, 1);
    return v;
}

void run_lua_reload_tests(void) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sumf_reload_test.lua", tmp);

    /* ── First load ── */
    write_file(path, "VALUE = 10\n");
    UmfLuaReloader* r = umf_lua_reload_create(path, NULL);
    CHECK(r != NULL, "create reloader");
    CHECK(umf_lua_reload_changed(r), "changed() is true before first load");
    CHECK(umf_lua_reload_state(r) == NULL, "no state before first load");

    CHECK(umf_lua_reload_poll(r) == 1, "first poll loads the script");
    CHECK(umf_lua_reload_generation(r) == 1, "generation is 1 after first load");
    lua_State* L1 = umf_lua_reload_state(r);
    CHECK(L1 != NULL, "state present after load");
    CHECK(global_int(L1, "VALUE") == 10, "script global VALUE == 10");

    /* ── No change is a no-op ── */
    CHECK(umf_lua_reload_poll(r) == 0, "poll with no change is a no-op");
    CHECK(umf_lua_reload_generation(r) == 1, "generation unchanged on no-op poll");
    CHECK(!umf_lua_reload_changed(r), "changed() is false when unmodified");

    /* ── Edit (different size => detected regardless of mtime resolution) ── */
    write_file(path, "VALUE = 200\n");
    CHECK(umf_lua_reload_changed(r), "changed() is true after an edit");
    CHECK(umf_lua_reload_poll(r) == 1, "poll reloads after an edit");
    CHECK(umf_lua_reload_generation(r) == 2, "generation advances to 2");
    CHECK(global_int(umf_lua_reload_state(r), "VALUE") == 200, "reloaded VALUE == 200");

    /* ── Force reloads unconditionally ── */
    CHECK(umf_lua_reload_force(r) == 1, "force reloads unconditionally");
    CHECK(umf_lua_reload_generation(r) == 3, "generation advances to 3 on force");
    CHECK(global_int(umf_lua_reload_state(r), "VALUE") == 200, "value stable after force");

    /* ── A broken script keeps the previous good state ── */
    write_file(path, "VALUE = (((broken syntax here\n");
    CHECK(umf_lua_reload_poll(r) == -1, "poll reports error on a broken script");
    CHECK(umf_lua_reload_generation(r) == 3, "generation unchanged after a failed reload");
    CHECK(umf_lua_reload_error(r)[0] != 0, "error text is recorded");
    CHECK(global_int(umf_lua_reload_state(r), "VALUE") == 200, "previous good state preserved");

    umf_lua_reload_destroy(r);
    DeleteFileA(path);

    /* ── Missing file + NULL path robustness ── */
    char missing[MAX_PATH];
    snprintf(missing, sizeof(missing), "%sumf_reload_absent_zzz.lua", tmp);
    DeleteFileA(missing);
    UmfLuaReloader* r2 = umf_lua_reload_create(missing, NULL);
    CHECK(r2 != NULL, "create reloader for a missing file");
    CHECK(umf_lua_reload_poll(r2) == -1, "poll errors when the file is missing");
    CHECK(umf_lua_reload_state(r2) == NULL, "no state when the file is missing");
    umf_lua_reload_destroy(r2);

    CHECK(umf_lua_reload_create(NULL, NULL) == NULL, "create rejects a NULL path");
}
