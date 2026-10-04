/*
 * umf_lua_sandbox.c — Lua 5.4 sandboxed scripting environment
 *
 * Removes: os, io, debug, package, ffi, coroutine
 * Replaces: rawget/rawset with protected-key versions
 * Forces: text-only loading (rejects bytecode)
 * Limits: instruction budget per pcall (100M instructions)
 */

#include "umf/umf.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════
 * Protected Keys — cannot be accessed via rawget/rawset
 * ════════════════════════════════════════════════════════════════ */

static const char* g_protected_keys[] = {
    "os", "io", "debug", "package", "coroutine", "ffi",
    "load", "loadstring", "loadfile", "dofile",
    "rawget", "rawset", "rawequal", "rawlen",
    NULL
};

static bool is_protected_key(const char* key) {
    for (int i = 0; g_protected_keys[i]; i++)
        if (strcmp(key, g_protected_keys[i]) == 0)
            return true;
    return false;
}

/* ════════════════════════════════════════════════════════════════
 * Safe Raw Functions (§4.2)
 * ════════════════════════════════════════════════════════════════ */

static int safe_rawget(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Only allow on global environment table */
    lua_pushglobaltable(L);
    int is_global = lua_rawequal(L, 1, -1);
    lua_pop(L, 1);

    if (is_global && lua_type(L, 2) == LUA_TSTRING) {
        const char* key = lua_tostring(L, 2);
        if (is_protected_key(key)) {
            lua_pushnil(L);
            return 1;
        }
    }

    lua_rawget(L, 1);
    return 1;
}

static int safe_rawset(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    lua_pushglobaltable(L);
    int is_global = lua_rawequal(L, 1, -1);
    lua_pop(L, 1);

    if (is_global && lua_type(L, 2) == LUA_TSTRING) {
        const char* key = lua_tostring(L, 2);
        if (is_protected_key(key))
            return luaL_error(L, "rawset: cannot modify protected key '%s'", key);
    }

    lua_rawset(L, 1);
    return 0;
}

static int safe_rawequal(lua_State* L) {
    lua_pushboolean(L, lua_rawequal(L, 1, 2));
    return 1;
}

static int safe_rawlen(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)lua_rawlen(L, 1));
    return 1;
}

/* ════════════════════════════════════════════════════════════════
 * Safe Load — text-only, rejects bytecode
 * ════════════════════════════════════════════════════════════════ */

static int safe_load(lua_State* L) {
    const char* chunk = luaL_checkstring(L, 1);
    const char* name  = luaL_optstring(L, 2, "=(sandbox)");

    /* "t" = text only — rejects compiled bytecode */
    int status = luaL_loadbufferx(L, chunk, strlen(chunk), name, "t");
    if (status != LUA_OK) {
        lua_pushnil(L);
        lua_insert(L, -2);  /* nil, error_msg */
        return 2;
    }
    return 1;  /* compiled function */
}

/* ════════════════════════════════════════════════════════════════
 * Instruction Limit Hook (§4.1)
 * ════════════════════════════════════════════════════════════════ */

static __declspec(thread) int t_instruction_budget = 0;

static void instruction_limit_hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    if (--t_instruction_budget <= 0) {
        luaL_error(L, "[UMF] Script exceeded instruction limit (100M instructions)");
    }
}

/* ════════════════════════════════════════════════════════════════
 * Memory Allocator with Budget
 * ════════════════════════════════════════════════════════════════ */

#define UMF_LUA_MEMORY_LIMIT (64 * 1024 * 1024)  /* 64 MB */

typedef struct {
    size_t current;
    size_t limit;
} UmfLuaAllocState;

static void* lua_budget_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    UmfLuaAllocState* state = (UmfLuaAllocState*)ud;

    if (nsize == 0) {
        /* Free */
        state->current -= osize;
        free(ptr);
        return NULL;
    }

    size_t delta = nsize - osize;
    if (state->current + delta > state->limit) {
        return NULL;  /* Allocation refused — triggers Lua memory error */
    }

    void* new_ptr = realloc(ptr, nsize);
    if (new_ptr) {
        state->current += delta;
    }
    return new_ptr;
}

/* ════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════ */

lua_State* umf_lua_create_sandbox(void) {
    /* Create allocator state */
    UmfLuaAllocState* alloc_state =
        (UmfLuaAllocState*)calloc(1, sizeof(UmfLuaAllocState));
    if (!alloc_state) return NULL;
    alloc_state->limit = UMF_LUA_MEMORY_LIMIT;

    /* Create Lua state with budgeted allocator.
     * Lua 5.5 added a per-state hashing seed parameter. */
#if defined(LUA_VERSION_NUM) && LUA_VERSION_NUM >= 505
    lua_State* L = lua_newstate(lua_budget_alloc, alloc_state, 0);
#else
    lua_State* L = lua_newstate(lua_budget_alloc, alloc_state);
#endif
    if (!L) {
        free(alloc_state);
        return NULL;
    }

    /* Open safe standard libraries */
    luaL_requiref(L, "_G",       luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, "table",    luaopen_table,   1); lua_pop(L, 1);
    luaL_requiref(L, "string",   luaopen_string,  1); lua_pop(L, 1);
    luaL_requiref(L, "math",     luaopen_math,    1); lua_pop(L, 1);
    luaL_requiref(L, "utf8",     luaopen_utf8,    1); lua_pop(L, 1);
    /* NOT opened: os, io, debug, package, coroutine */

    /* Remove dangerous globals */
    const char* remove[] = {
        "os", "io", "debug", "package", "coroutine", "ffi",
        "loadfile", "dofile", "require",
        NULL
    };
    for (int i = 0; remove[i]; i++) {
        lua_pushnil(L);
        lua_setglobal(L, remove[i]);
    }

    /* Replace raw functions */
    lua_pushcfunction(L, safe_rawget);   lua_setglobal(L, "rawget");
    lua_pushcfunction(L, safe_rawset);   lua_setglobal(L, "rawset");
    lua_pushcfunction(L, safe_rawequal); lua_setglobal(L, "rawequal");
    lua_pushcfunction(L, safe_rawlen);   lua_setglobal(L, "rawlen");

    /* Replace load/loadstring with text-only */
    lua_pushcfunction(L, safe_load); lua_setglobal(L, "load");
    lua_pushcfunction(L, safe_load); lua_setglobal(L, "loadstring");

    UMF_INFO("Lua 5.4 sandbox created (memory limit: %d MB)",
             UMF_LUA_MEMORY_LIMIT / (1024 * 1024));
    return L;
}

void umf_lua_destroy_sandbox(lua_State* L) {
    if (!L) return;

    /* Retrieve and free the allocator state */
    void* ud = NULL;
    lua_getallocf(L, &ud);
    lua_close(L);
    free(ud);  /* Free UmfLuaAllocState */

    UMF_INFO("Lua sandbox destroyed");
}

int umf_lua_sandboxed_pcall(lua_State* L, int nargs, int nresults, int errfunc) {
    /* Reset instruction budget */
    t_instruction_budget = 100;  /* 100 * 1M = 100M instructions */
    lua_sethook(L, instruction_limit_hook, LUA_MASKCOUNT, 1000000);

    int rc = lua_pcall(L, nargs, nresults, errfunc);

    /* Remove hook (may not execute if lua_pcall longjmps — self-correcting
     * on next umf_lua_sandboxed_pcall call which resets budget) */
    lua_sethook(L, NULL, 0, 0);

    return rc;
}
