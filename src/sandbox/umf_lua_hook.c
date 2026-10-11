/*
 * umf_lua_hook.c — §LUA: bind a Lua function as a C hook
 *
 * A Lua mod calls umf.hook(target, fn) and the engine installs a real hook.
 * Because a Lua callback is generic (variadic), a per-hook x64 dispatch stub
 * is generated at runtime. The stub spills the four integer argument
 * registers (RCX/RDX/R8/R9) into a small array, calls the C dispatcher with
 * that array + a hook id, and returns the dispatcher's RAX as the hook's
 * return value. The dispatcher runs the Lua callback with those up to four
 * integer arguments and returns its first result.
 *
 * Scope: integer register arguments only (up to 4). Stack arguments and
 * floating-point arguments are not forwarded — documented limitation.
 * A single Lua state is assumed (not reentrant across threads).
 */

#include "umf/umf.h"

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <string.h>

#define UMF_MAX_LUA_HOOKS 32

typedef struct {
    int          id;
    lua_State*   L;
    void*        original;     /* trampoline (call original)       */
    void*        target;       /* hooked address                   */
    uint8_t*     stub;         /* dispatch stub code               */
    UmfTrampolineSlot* stub_slot;
    bool         active;
} LuaHook;

static LuaHook   g_lua_hooks[UMF_MAX_LUA_HOOKS];
static int       g_lua_hook_count = 0;
static SRWLOCK   g_lua_lock = SRWLOCK_INIT;
static __declspec(thread) int t_current_hook = -1;

/* Persistent eval sandbox + a recursive lock serializing all Lua execution
 * (interactive eval on the pipe thread vs. hook callbacks on game threads). */
static lua_State*       g_eval_L = NULL;
static CRITICAL_SECTION g_lua_exec_cs;
static INIT_ONCE        g_exec_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_exec_cs(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_lua_exec_cs);
    return TRUE;
}
void umf_lua_exec_lock(void) {
    InitOnceExecuteOnce(&g_exec_once, init_exec_cs, NULL, NULL);
    EnterCriticalSection(&g_lua_exec_cs);
}
void umf_lua_exec_unlock(void) {
    LeaveCriticalSection(&g_lua_exec_cs);
}
lua_State* umf_lua_eval_state(void) {
    umf_lua_exec_lock();
    if (!g_eval_L) {
        g_eval_L = umf_lua_create_sandbox();
        if (g_eval_L) umf_lua_setup_api(g_eval_L, NULL);
    }
    lua_State* L = g_eval_L;
    umf_lua_exec_unlock();
    return L;
}

/* Anchor table in the Lua registry holding every hook callback, keyed by id,
 * so refs to functions passed to umf.hook() survive the caller returning. */
#define UMF_LUA_HOOKS_TABLE "umf.hook_callbacks"

extern UmfTrampolinePool g_trampoline_pool;

static void push_hooks_table(lua_State* L) {
    luaL_getsubtable(L, LUA_REGISTRYINDEX, UMF_LUA_HOOKS_TABLE);
}

/* ── C dispatcher invoked by the generated stub ── */
static uint64_t lua_dispatch(uint64_t* args, uint64_t id) {
    if (id >= (uint64_t)UMF_MAX_LUA_HOOKS) return 0;
    LuaHook* h = &g_lua_hooks[id];
    if (!h->active || !h->L) return 0;

    umf_lua_exec_lock();   // serialize with interactive eval / other hooks
    lua_State* L = h->L;
    push_hooks_table(L);
    lua_rawgeti(L, -1, (lua_Integer)id + 1);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); umf_lua_exec_unlock(); return 0; }

    for (int i = 0; i < 4; i++)
        lua_pushinteger(L, (lua_Integer)args[i]);

    int prev = t_current_hook;
    t_current_hook = (int)id;
    int rc = umf_lua_sandboxed_pcall(L, 4, 1, 0);
    t_current_hook = prev;

    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        UMF_ERROR("Lua hook %llu error: %s", (unsigned long long)id,
                  err ? err : "?");
        lua_pop(L, 1);
        umf_lua_exec_unlock();
        return 0;
    }

    uint64_t result = (uint64_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    umf_lua_exec_unlock();
    return result;
}

/* ── Generate the x64 dispatch stub ── */
static void write_stub(uint8_t* p, uint64_t id, void* dispatch) {
    size_t o = 0;
    /* sub rsp, 0x58 */
    p[o++] = 0x48; p[o++] = 0x83; p[o++] = 0xEC; p[o++] = 0x58;
    /* mov [rsp+0x20], rcx */
    p[o++] = 0x48; p[o++] = 0x89; p[o++] = 0x4C; p[o++] = 0x24; p[o++] = 0x20;
    /* mov [rsp+0x28], rdx */
    p[o++] = 0x48; p[o++] = 0x89; p[o++] = 0x54; p[o++] = 0x24; p[o++] = 0x28;
    /* mov [rsp+0x30], r8 */
    p[o++] = 0x4C; p[o++] = 0x89; p[o++] = 0x44; p[o++] = 0x24; p[o++] = 0x30;
    /* mov [rsp+0x38], r9 */
    p[o++] = 0x4C; p[o++] = 0x89; p[o++] = 0x4C; p[o++] = 0x24; p[o++] = 0x38;
    /* lea rcx, [rsp+0x20] */
    p[o++] = 0x48; p[o++] = 0x8D; p[o++] = 0x4C; p[o++] = 0x24; p[o++] = 0x20;
    /* mov rdx, imm64 id */
    p[o++] = 0x48; p[o++] = 0xBA; memcpy(p + o, &id, 8); o += 8;
    /* mov rax, imm64 dispatch */
    uint64_t d = (uint64_t)(uintptr_t)dispatch;
    p[o++] = 0x48; p[o++] = 0xB8; memcpy(p + o, &d, 8); o += 8;
    /* call rax */
    p[o++] = 0xFF; p[o++] = 0xD0;
    /* add rsp, 0x58 */
    p[o++] = 0x48; p[o++] = 0x83; p[o++] = 0xC4; p[o++] = 0x58;
    /* ret */
    p[o++] = 0xC3;
}

/* ── "dll!func" resolution ── */
static void* resolve_target(const char* name) {
    const char* bang = strchr(name, '!');
    if (bang) {
        char dll[128];
        size_t n = (size_t)(bang - name);
        if (n >= sizeof(dll)) n = sizeof(dll) - 1;
        memcpy(dll, name, n); dll[n] = '\0';
        return umf_resolve_function(dll, bang + 1);
    }
    return umf_resolve_function("kernel32.dll", name);
}

/* ── umf.hook(target, fn, opts?) ── */
static int l_umf_hook(lua_State* L) {
    UmfMod* mod = (UmfMod*)lua_touserdata(L, lua_upvalueindex(1));

    if (mod && !(mod->capabilities & UMF_CAP_HOOK)) {
        return luaL_error(L, "mod '%s' lacks 'hook' capability", mod->name);
    }

    void* target = NULL;
    if (lua_type(L, 1) == LUA_TSTRING) {
        target = resolve_target(lua_tostring(L, 1));
    } else if (lua_type(L, 1) == LUA_TLIGHTUSERDATA) {
        target = lua_touserdata(L, 1);
    } else {
        return luaL_error(L, "umf.hook: arg 1 must be 'dll!func' or address");
    }
    if (!target) return luaL_error(L, "umf.hook: could not resolve target");

    luaL_checktype(L, 2, LUA_TFUNCTION);

    int priority = 0;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "priority");
        if (lua_isnumber(L, -1)) priority = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    AcquireSRWLockExclusive(&g_lua_lock);
    if (g_lua_hook_count >= UMF_MAX_LUA_HOOKS) {
        ReleaseSRWLockExclusive(&g_lua_lock);
        return luaL_error(L, "too many Lua hooks");
    }
    int id = g_lua_hook_count++;
    LuaHook* h = &g_lua_hooks[id];
    memset(h, 0, sizeof(*h));
    h->id = id; h->L = L; h->target = target;
    h->active = true;
    ReleaseSRWLockExclusive(&g_lua_lock);

    /* Persist the callback in the registry anchor table, keyed by id. */
    push_hooks_table(L);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, (lua_Integer)id + 1);
    lua_pop(L, 1);

    /* Allocate + build the dispatch stub. */
    h->stub_slot = umf_trampoline_pool_allocate_near(
        &g_trampoline_pool, target, UMF_TRAMPOLINE_SLOT_SIZE);
    if (!h->stub_slot) return luaL_error(L, "umf.hook: no trampoline slot");

    h->stub = h->stub_slot->code;
    write_stub(h->stub, (uint64_t)id, (void*)&lua_dispatch);
    h->stub_slot->used_size = 60;
    if (!umf_trampoline_finalize(h->stub_slot, 60, false))
        return luaL_error(L, "umf.hook: finalize stub failed");

    if (!umf_register_hook_addr(target, "lua_hook", (void*)h->stub,
                                priority, mod, (void**)&h->original)) {
        umf_trampoline_pool_release(&g_trampoline_pool, h->stub_slot);
        h->stub_slot = NULL;
        return luaL_error(L, "umf.hook: register failed");
    }
    umf_apply_pending_batch();

    lua_pushinteger(L, id);
    return 1;
}

/* ── umf.call_original(a,b,c,d) — uses the trampoline of the current hook ── */
static int l_umf_call_original(lua_State* L) {
    if (t_current_hook < 0 || t_current_hook >= UMF_MAX_LUA_HOOKS)
        return luaL_error(L, "umf.call_original: not inside a hook");

    LuaHook* h = &g_lua_hooks[t_current_hook];
    if (!h->original) return luaL_error(L, "umf.call_original: no original");

    uint64_t a = (uint64_t)luaL_optinteger(L, 1, 0);
    uint64_t b = (uint64_t)luaL_optinteger(L, 2, 0);
    uint64_t c = (uint64_t)luaL_optinteger(L, 3, 0);
    uint64_t d = (uint64_t)luaL_optinteger(L, 4, 0);

    typedef uint64_t (*fn4)(uint64_t, uint64_t, uint64_t, uint64_t);
    uint64_t r = ((fn4)h->original)(a, b, c, d);

    lua_pushinteger(L, (lua_Integer)r);
    return 1;
}

static int l_umf_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    UMF_INFO("[lua] %s", msg);
    return 0;
}

static int l_umf_resolve(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void* addr = resolve_target(name);
    lua_pushlightuserdata(L, addr);
    return 1;
}

/* ── Memory access (in-process; the runtime is injected into the target) ──
 * Addresses may be passed as integers or as the lightuserdata returned by
 * umf.resolve / umf.aob. Reads/writes are fault-guarded so a bad address
 * returns nil/false instead of crashing the game. */

#include <windows.h>

static uintptr_t lua_addr(lua_State* L, int idx) {
    if (lua_islightuserdata(L, idx)) return (uintptr_t)lua_touserdata(L, idx);
    return (uintptr_t)(uint64_t)luaL_checkinteger(L, idx);
}

static int safe_copy(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int prot_write(uintptr_t a, const void* src, size_t n) {
    DWORD old = 0;
    BOOL changed = VirtualProtect((void*)a, n, PAGE_EXECUTE_READWRITE, &old);
    int ok = safe_copy((void*)a, src, n);
    if (changed) { DWORD tmp; VirtualProtect((void*)a, n, old, &tmp); }
    return ok;
}

#define UMF_LUA_READ(NAME, CTYPE, PUSH)                              \
    static int NAME(lua_State* L) {                                  \
        uintptr_t a = lua_addr(L, 1);                                \
        CTYPE v;                                                     \
        if (!safe_copy(&v, (void*)a, sizeof(v))) { lua_pushnil(L); return 1; } \
        PUSH(L, v);                                                  \
        return 1;                                                    \
    }
UMF_LUA_READ(l_read_i8,  int8_t,   lua_pushinteger)
UMF_LUA_READ(l_read_i16, int16_t,  lua_pushinteger)
UMF_LUA_READ(l_read_i32, int32_t,  lua_pushinteger)
UMF_LUA_READ(l_read_u32, uint32_t, lua_pushinteger)
UMF_LUA_READ(l_read_i64, int64_t,  lua_pushinteger)
UMF_LUA_READ(l_read_f32, float,    lua_pushnumber)
UMF_LUA_READ(l_read_f64, double,   lua_pushnumber)

#define UMF_LUA_WRITE(NAME, CTYPE, GET)                              \
    static int NAME(lua_State* L) {                                  \
        uintptr_t a = lua_addr(L, 1);                                \
        CTYPE v = (CTYPE)GET(L, 2);                                  \
        lua_pushboolean(L, prot_write(a, &v, sizeof(v)));            \
        return 1;                                                    \
    }
UMF_LUA_WRITE(l_write_i8,  int8_t,   luaL_checkinteger)
UMF_LUA_WRITE(l_write_i16, int16_t,  luaL_checkinteger)
UMF_LUA_WRITE(l_write_i32, int32_t,  luaL_checkinteger)
UMF_LUA_WRITE(l_write_i64, int64_t,  luaL_checkinteger)
UMF_LUA_WRITE(l_write_f32, float,    luaL_checknumber)
UMF_LUA_WRITE(l_write_f64, double,   luaL_checknumber)

static int l_read_bytes(lua_State* L) {
    uintptr_t a = lua_addr(L, 1);
    size_t n = (size_t)luaL_checkinteger(L, 2);
    if (n == 0 || n > (1u << 20)) { lua_pushnil(L); return 1; }
    void* tmp = malloc(n);
    if (!tmp) { lua_pushnil(L); return 1; }
    if (!safe_copy(tmp, (void*)a, n)) { free(tmp); lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char*)tmp, n);
    free(tmp);
    return 1;
}

static int l_write_bytes(lua_State* L) {
    uintptr_t a = lua_addr(L, 1);
    size_t n = 0;
    const char* s = luaL_checklstring(L, 2, &n);
    lua_pushboolean(L, n ? prot_write(a, s, n) : 0);
    return 1;
}

static int l_nop(lua_State* L) {
    uintptr_t a = lua_addr(L, 1);
    size_t n = (size_t)luaL_checkinteger(L, 2);
    if (n == 0 || n > 256) { lua_pushboolean(L, 0); return 1; }
    unsigned char* b = (unsigned char*)malloc(n);
    if (!b) { lua_pushboolean(L, 0); return 1; }
    memset(b, 0x90, n);
    int ok = prot_write(a, b, n);
    free(b);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_module_base(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    HMODULE h = GetModuleHandleA(name);
    if (!h) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)(uintptr_t)h);
    return 1;
}

static int l_aob(lua_State* L) {
    const char* pattern = luaL_checkstring(L, 1);
    const char* module = luaL_optstring(L, 2, NULL);
    void* found = umf_aob_scan(pattern, module);
    if (!found) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)(uintptr_t)found);
    return 1;
}

void umf_lua_setup_api(lua_State* L, UmfMod* mod) {
    lua_newtable(L);                       /* umf = {} */

    /* Push `mod` as an upvalue for the capability-aware umf.hook. */
    lua_pushlightuserdata(L, mod);
    lua_pushcclosure(L, l_umf_hook, 1);
    lua_setfield(L, -2, "hook");

    lua_pushcfunction(L, l_umf_call_original); lua_setfield(L, -2, "call_original");
    lua_pushcfunction(L, l_umf_log);           lua_setfield(L, -2, "log");
    lua_pushcfunction(L, l_umf_resolve);       lua_setfield(L, -2, "resolve");

    /* memory access */
    lua_pushcfunction(L, l_read_i8);    lua_setfield(L, -2, "read_i8");
    lua_pushcfunction(L, l_read_i16);   lua_setfield(L, -2, "read_i16");
    lua_pushcfunction(L, l_read_i32);   lua_setfield(L, -2, "read_i32");
    lua_pushcfunction(L, l_read_u32);   lua_setfield(L, -2, "read_u32");
    lua_pushcfunction(L, l_read_i64);   lua_setfield(L, -2, "read_i64");
    lua_pushcfunction(L, l_read_f32);   lua_setfield(L, -2, "read_float");
    lua_pushcfunction(L, l_read_f64);   lua_setfield(L, -2, "read_double");
    lua_pushcfunction(L, l_read_bytes); lua_setfield(L, -2, "read_bytes");
    lua_pushcfunction(L, l_write_i8);   lua_setfield(L, -2, "write_i8");
    lua_pushcfunction(L, l_write_i16);  lua_setfield(L, -2, "write_i16");
    lua_pushcfunction(L, l_write_i32);  lua_setfield(L, -2, "write_i32");
    lua_pushcfunction(L, l_write_i64);  lua_setfield(L, -2, "write_i64");
    lua_pushcfunction(L, l_write_f32);  lua_setfield(L, -2, "write_float");
    lua_pushcfunction(L, l_write_f64);  lua_setfield(L, -2, "write_double");
    lua_pushcfunction(L, l_write_bytes);lua_setfield(L, -2, "write_bytes");
    lua_pushcfunction(L, l_nop);        lua_setfield(L, -2, "nop");
    lua_pushcfunction(L, l_module_base);lua_setfield(L, -2, "module_base");
    lua_pushcfunction(L, l_aob);        lua_setfield(L, -2, "aob");

    lua_setglobal(L, "umf");

    UMF_INFO("Lua 'umf' API installed");
}

bool umf_lua_run_string(lua_State* L, const char* chunk) {
    if (luaL_loadstring(L, chunk) != LUA_OK) {
        UMF_ERROR("Lua load error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    if (umf_lua_sandboxed_pcall(L, 0, 0, 0) != LUA_OK) {
        UMF_ERROR("Lua runtime error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}
