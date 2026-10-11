# Hexforge Lua — Hands-On Power Demo

This guide shows what the **injected Lua engine** can do and exactly how to use it.
Lua runs *inside* the target process (via `umf_runtime.dll`), so it can read/write
memory directly, scan byte patterns, patch code, and hook functions — the things
the external tool can't do from outside.

> Use the point-and-click tabs for simple value cheats. Reach for Lua when you
> need logic, reload-safe addresses, all-weapon/code-level cheats, or hooks.

---

## 1. When (and why) to use Lua

| Situation | External tool | Lua (injected) |
|---|---|---|
| Freeze one value you found | ✅ Trainer/Watch | ✅ overkill |
| Change ammo for **all** weapons | ❌ (per-value only) | ✅ NOP the shared decrement |
| God mode (ignore all damage) | ⚠️ freeze HP | ✅ hook `TakeDamage`/`IsDead` to return 0 |
| Address that moves each launch | ⚠️ pointer scan | ✅ `module_base + offsets` or `aob` at runtime |
| React to game state / conditional cheats | ❌ | ✅ full scripting |
| Intercept a function, read its args | ❌ | ✅ `umf.hook` |

Rule of thumb: **values → external tool; code/behaviour → Lua.**

---

## 2. Setup (one time)

1. Build so `umf_runtime.dll` sits next to `hexforge_tool.exe` (it does after a normal build).
2. Run `hexforge_tool.exe` **as admin**.
3. Left panel: pick the game → **Attach**.
4. Left panel: **Inject runtime** (loads `umf_runtime.dll` into the game). The process shows a green `[runtime]` tag.
5. Open the **Runtime** tab → **Connect to runtime**.
6. Pick a **snippet** from the dropdown → **Load snippet** → **Run Lua**. Output shows in Response; `umf.log(...)` lines show in the Runtime log stream.

If "Connect" is greyed out, you haven't injected yet (step 4).

---

## 3. API reference (what the sandbox exposes)

Addresses can be plain integers (e.g. `0x1D9C90A12AC`) or the value returned by
`umf.resolve` / `umf.aob` / `umf.module_base`. All reads/writes are **fault-guarded**:
a bad address returns `nil`/`false` instead of crashing the game.

```
-- discovery
umf.module_base(name)         -> integer base of a loaded module ("re3.exe"), or nil
umf.resolve(module, export)   -> address of an exported function
umf.aob(pattern [, module])   -> address of first byte-pattern match, or nil
                                 pattern: "48 8B ?? C3 90"  (?? = wildcard)

-- read (return number, or nil on fault)
umf.read_i8/read_i16/read_i32/read_u32/read_i64(addr)
umf.read_float(addr)   umf.read_double(addr)
umf.read_bytes(addr, n)       -> string of n raw bytes

-- write (return true/false; auto-handles page protection)
umf.write_i8/write_i16/write_i32/write_i64(addr, value)
umf.write_float(addr, value)  umf.write_double(addr, value)
umf.write_bytes(addr, string)
umf.nop(addr, len)            -> fill len bytes with 0x90 (NOP)

-- hooks
umf.hook(addr, function(a,b,c,d) ... end)   -- intercept a function
umf.call_original(a,b,c,d)                  -- call the original from inside a hook

-- misc
umf.log(text)                 -- prints to the Runtime log stream
```

Sandbox limits: no `os`/`io`/`debug`/`ffi`, text-only `load`, 64 MB memory cap,
100M-instruction budget per run (so an accidental infinite loop can't hang the game).

---

## 4. Demo 1 — Hello / read a value

```lua
local base = umf.module_base("re3.exe")
umf.log(string.format("re3.exe base = %X", base))

-- read the ammo address you found in the Scanner:
local ammo = 0x1D9C90A12AC
umf.log("ammo = " .. tostring(umf.read_i32(ammo)))
```

Run it; the Runtime log shows the base and the current ammo. This proves the
script is live inside the game.

---

## 5. Demo 2 — Write / give yourself ammo

```lua
local ammo = 0x1D9C90A12AC
umf.write_i32(ammo, 999)
umf.log("now = " .. tostring(umf.read_i32(ammo)))
```

Same as the Scanner's Set, but scriptable. Note: if the game clamps on reload
(the 444→60 case), freezing or a code patch is better — see Demo 4.

---

## 6. Demo 3 — Reload-safe address (no hard-coded pointer)

Hard-coded addresses change every launch (ASLR). Compute it at runtime instead:

```lua
-- Option A: module base + static offsets (from the Pointer Scan tab)
local base = umf.module_base("re3.exe")
local p    = umf.read_i64(base + 0x0234ABC0)   -- *(base+offset)
local hp   = p + 0x58                           -- + field offset
umf.write_float(hp, 100.0)
umf.log("HP set via pointer path")

-- Option B: AOB signature (survives game updates)
local hpAddr = umf.aob("F3 0F 10 86 ?? ?? ?? ?? 0F 2F", "re3.exe")
if hpAddr then umf.log(string.format("found near %X", hpAddr)) end
```

This is the robust way to ship a cheat: find the **pattern/path once**, resolve at runtime.

---

## 7. Demo 4 — All-weapon infinite ammo (code patch) ⭐

This is the one the per-value freeze can't do. Steps:

1. In the external tool: Scanner → find your **pistol** ammo address.
2. **Find Writes** tab → paste that address → **Start watching** → fire once.
   It catches the instruction that decrements ammo, e.g. `sub [rcx+0x40], eax`.
3. Click **Disasm** on that hit → read the instruction bytes (e.g. `29 41 40`).
4. Turn the operand into a wildcard to make a stable signature: `29 41 ??`
   (optionally add a few following bytes for uniqueness).
5. Script it:

```lua
-- NOP the shared "ammo -= used" so NO weapon ever loses ammo.
local site = umf.aob("29 41 ?? 48 8B")   -- your signature from Disasm
if site then
  umf.nop(site, 3)                        -- 29 41 40  is 3 bytes
  umf.log(string.format("patched ammo decrement @ %X", site))
else
  umf.log("pattern not found - re-check the signature")
end
```

Because every weapon runs the *same* instruction, one NOP = infinite ammo for
all of them. (In the Disasm tab the same NOP has a one-click **Revert**; from Lua
you'd `umf.write_bytes` the original bytes back.)

---

## 8. Demo 5 — God mode via return-override hook ⭐

Instead of freezing HP, make the damage/death function do nothing:

```lua
-- Find the function start (from Disasm / a tutorial signature).
local fn = umf.aob("48 89 5C 24 08 57 48 83 EC 20")
if fn then
  umf.hook(fn, function(a, b, c, d)
    -- Don't call the original. Just return 0 (= "no damage" / "not dead").
    return 0
  end)
  umf.log("god mode hook installed")
end
```

`umf.hook` redirects the function to your Lua callback. Returning without calling
`umf.call_original` means the real logic never runs — the cleanest god mode.

---

## 9. Demo 6 — Intercept + call original (observe/modify)

```lua
local fn = umf.resolve("user32.dll", "MessageBoxA")
umf.hook(fn, function(hwnd, text, caption, typ)
  umf.log("MessageBoxA was called")
  -- read the C string argument:
  umf.log("text = " .. (umf.read_bytes(text, 32) or "?"))
  return umf.call_original(hwnd, text, caption, typ)  -- let it proceed
end)
```

This pattern — log/modify arguments, then call the original — is how you learn
what a game function does and tweak it without breaking it.

---

## 10. Workflow that ties it together

```
Scanner (find value)
   └─ Find Writes (catch the instruction)
         └─ Disasm (read bytes, make an AOB signature, test NOP + Revert)
               └─ Runtime/Lua (umf.aob + umf.nop / umf.hook to script it for keeps)
```

Prototype in the tabs, then "graduate" the finding into a Lua script that is
reload-safe and shareable as a mod.

---

## 11. Safety / notes
- Lua writes go through page-protection changes automatically; reads/writes are fault-guarded.
- The sandbox blocks file/OS access and caps CPU/memory, so a bad script fails safely.
- Hooks and patches are **in-process** — only after **Inject runtime**. They persist until the game closes or you revert them.
- Only use on software you own / are authorized to modify. Don't touch anti-cheat-protected online games.
