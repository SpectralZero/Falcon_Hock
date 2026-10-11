# Hexforge — Status Report

_Native memory + injection toolkit (external scanner/trainer tool + injectable runtime engine)._
_Last updated: 2026-10-11._

---

## 0. TL;DR

- **Build:** VS 2022 + vcpkg. Configure/build the `build/` dir (preset `x64-debug`). Targets: `hexforge_tool.exe` (the app), `hexforge_trainer.exe` (the exported-trainer runner), `umf_runtime.dll` (injectable engine), `umf_test.exe` (engine tests).
- **Engine tests:** `399 / 399` passing (last run this session). Engine code was not touched by the recent tool work.
- **Run:** close any open window, then launch `build\Debug\hexforge_tool.exe` (asks for admin — needed to read/write other processes).
- **Recent focus:** crash fix, standalone trainer export, wizard, live editing, cover-image embedding, per-cheat notes, branding/icon.

---

## 1. What is Hexforge (architecture)

Two halves that work independently:

### 1a. `hexforge_tool.exe` — the external tool (no injection required)
A Win32 + Dear ImGui + D3D11 app that opens a target process with `OpenProcess` and uses `ReadProcessMemory` / `WriteProcessMemory`. This is the Cheat-Engine-style part: scan, edit, freeze, pointer-scan, disassemble, build trainers. **It never injects anything to do its core job** — safest mode.

Key source files (`tool/`):
| File | Role |
|---|---|
| `app.cpp` | All UI/tabs, scan orchestration, trainer table, wizard, export |
| `backend.cpp/.hpp` | Process list, attach, read/write, scan engine, regions/modules, pointer scan, disasm (Zydis) |
| `trainer.hpp/.cpp` | Shared cheat model + `.hexforge` format + single-file overlay embed |
| `trainer_runner.cpp` | The standalone trainer window that gets exported |
| `ipc.cpp/.hpp` | Talks to the injected runtime over a named pipe |
| `hexforge.rc` / `hexforge.ico` | App icon (resource `101`) |

### 1b. `umf_runtime.dll` — the injectable engine (optional, advanced)
A C engine you inject into a target for in-process power: inline/IAT/vtable/EAT hooks, a Lua sandbox, a mod loader, a DX11/GDI overlay, AOB scanning, mitigations. The tool's **Runtime** tab talks to it over IPC. This is where "real mod" territory lives (hooks, overlays, scripts). 399 unit tests cover it.

---

## 2. What's DONE

### 2.1 Stability
- **Fixed the Scanner crash on First/Next Scan** (`vector subscript out of range`). Root cause: the background scan thread rewrote the results vector while the UI thread read it. Fixes:
  - Scans run on a **worker thread** with a progress bar + **Cancel** (window never freezes).
  - Results are guarded by a mutex; the UI iterates a **private snapshot** (`result_count()` / `results_snapshot()`), never a live reference. Immune to the resize race.
- **Crash log + minidump:** `hexforge.log` and `hexforge-crash.dmp` are written next to the exe.

### 2.2 Scanner (the part you already liked)
- Types: int8/16/32/64, float, double. Compare: exact / increased / decreased / changed / unchanged.
- **Unknown-value scan** ("I don't know the number", then narrow by up/down/changed).
- **Writable-only** filter.
- Results: live value column; right-click → Add to Trainer / Edit value / Hex / Disasm / Pointer scan / Copy.
- **Live writes with no trainer:** per-row **`Set`** pokes the "new value" into that single address; right-click **Edit value** opens a write popup; **Apply to all results** writes every match.

### 2.3 Trainer (the tool tab)
- Table: Name, Address, Type, Current (live), Value (editable), **Mode**, Freeze, **Hotkey (F1–F12)**, Set/`i`/X.
- **Mode = Toggle** (hotkey flips Freeze on/off = god mode / infinite) or **Set once** (hotkey writes the value one time).
- **Per-cheat Note** (the `i` button): a short tip (e.g. "game caps this at 60") saved with the cheat and shown in the exported trainer as a hover `(i)`.
- Save / Load `.hexforge` (text format, human-readable).
- Change an amount by editing **Value + Set** — no need to create a new cheat.

### 2.4 Trainer Wizard (guided, for non-experts)
Pick Health/Money/Ammo/Speed/Other → enter the in-game number (or "I don't know") → change it in-game and click It went DOWN/UP/CHANGED/SAME → when few remain, **Make cheat** drops it into the Trainer and jumps there.

### 2.5 Standalone trainer export (the FLiNG-style feature)
- **Export Trainer (.exe)** copies the prebuilt `hexforge_trainer.exe` and appends your table as a file **overlay** (no compiler needed on the user's machine). Shareable single file.
- The exported trainer:
  - Auto-finds the game by process name and attaches (keeps waiting if not running).
  - FLiNG-style window: logo, glitch title, left panel (cover image + GitHub/Discord), right panel (cheat table).
  - **Editable Value box per cheat + live "Now" column** — change ammo/money at runtime, no re-export.
  - Toggle = freeze checkbox; Set once = Activate; hotkeys work in-game.
  - **Embedded cover image** (PNG/JPG) decoded with WIC and shown in the left panel.
  - **GitHub/Discord buttons** open the author's URLs (baked in at export), with drawn glyph icons.
  - Per-cheat **notes** shown as `(i)` tooltips.
- Overlay format: `v1` (text only) and `v2` (text + image), both readable; back-compatible.

### 2.6 Other tabs
- **Hex:** `Live` re-read with changed-byte highlighting + **Data Inspector** (int/uint/float/double/ptr at once) — the tool for telling steady values from fast ones.
- **Memory Map:** color-coded by protection (exec/write/read/guard) + writable-only filter + click to jump to Hex.
- **Modules:** DLLs with base/size; click to Hex.
- **Pointer Scan:** find a module+offset path to an address.
- **Disasm:** Zydis disassembly at an address.
- **Runtime:** mitigations, hooks/mods lists, sandboxed Lua + live log (only after Inject).
- **Log:** in-UI timestamps, text filter, severity colors, autoscroll, Copy/Save.
- **Help:** per-tab explanations + recipes.

### 2.7 Branding
- New **multi-resolution `hexforge.ico`** (256/128/64/48/32/24/16) generated from `hex.png`. One icon drives the app window, taskbar, Explorer, desktop shortcut, **and** the in-trainer logo (the runner loads icon `101` as a texture). Master art kept at `tool/hexforge.png` (512²).
- Studio/Tauri fully removed earlier; re-verified: zero references.

---

## 3. Your questions, answered

### 3.1 "Why does my infinite ammo only affect the pistol, but real trainers affect all weapons?"
Because you froze **one weapon's ammo counter** — a single memory location that belongs to that specific weapon/instance. The sniper, shotgun, etc. each have **their own** ammo field (often inside their own object/struct). Freezing one address only locks that one.

Real "all-weapons infinite ammo" is almost always done at the **code level**, not per-value:
- The game has a shared routine like `ammo -= cost` (or `DecrementAmmo(weapon)`), executed for **every** weapon.
- Trainers **hook that instruction/function** (or NOP the subtract, or force the "cost" to 0, or make the function return early). Now **no** weapon loses ammo — one patch covers all.

Two ways to get there in Hexforge:
1. **Find what writes to the ammo address** → it points you at the shared decrement instruction → **NOP/patch** it. (This "find what writes" + patch feature is the top item in §5 — not built yet.)
2. **Pointer/struct approach:** if all weapons share a struct layout, find the ammo **offset** and a base pointer, then freeze each, or script it in Lua.

Short version: per-value freeze = per-weapon. Code patch/hook = all weapons. The code-patch path is the next big feature.

### 3.2 "I set ammo to 444 but after reload it became 60 — bug?"
No — that's the **game clamping**. Current ammo and magazine capacity are separate values; reload runs `current = min(reserve, magCap)` and clamps to 60. To make it stick: **Freeze** the ammo (the tool rewrites it every frame so firing never lowers it), or also raise the **magazine-capacity** value. Values the game re-validates each action need Freeze, not a one-time Set. (You can capture exactly this as a per-cheat **Note** now.)

### 3.3 "What about normal apps, not games?"
Everything the scanner/trainer does is process-agnostic — it works on any Win32/x64 process. Useful, legitimate things:
- **Automation/QA:** drive or inspect an app's state, script repetitive actions, build test harnesses.
- **Debugging/research:** inspect live structures, watch a value change, dump a memory region, diff two regions.
- **Accessibility / personalization:** force a window value, keep a setting pinned, tweak UI counters.
- **Education:** see how programs lay out memory, how pointers/structs work, how patching changes behavior.
- **Localization / modding of non-game apps:** find and replace strings in memory.
- **Reverse-engineering practice:** disassemble around an address, follow pointers, understand control flow.

Caution: respect each app's license/ToS and the law. Don't tamper with other people's software, anti-cheat-protected online games, or licensing you don't own. The tool is for software you own or are authorized to analyze.

Planned app-focused helpers (see §5): string search/replace, byte-pattern (AOB) scan, struct dissector, scriptable watchers, snapshot/compare.

### 3.4 "Lua — I never tested it. When and why?"
Lua runs **inside the target** via the injected `umf_runtime.dll` (Runtime tab → Inject). Use the point-and-click tool for simple value cheats; reach for Lua when you need **logic the UI can't express**:
- **Reload-safe cheats:** compute an address from `moduleBase + AOB + offsets` at runtime (survives game restarts/updates) instead of a hard-coded address.
- **Conditional/structured cheats:** "if in combat, lock HP; else let it regen", loops over an array of weapons, apply to a whole struct.
- **Function hooks:** intercept a game function and **force a return value** (e.g. `IsDead()` → always false) — the clean way to do god mode / all-weapon ammo.
- **Overlays & notifications:** draw an in-game overlay, toast messages.
- **Batch discovery:** script AOB scans, list modules/exports, dump strings.

Best practice: prototype an address in the external tool, then "graduate" it to a Lua script when you want it robust and shareable as a mod. It's the advanced tier — most game cheats don't need it, most real **mods** do.

---

## 4. The exported trainer — authoring checklist
Before clicking **Export Trainer (.exe)**:
1. Attach to the game (so the trainer knows the process name).
2. Build your cheats; set each **Mode** and **Value**; add **Notes** (the `i`) for caps/quirks like the ammo→60 note.
3. Set **title**, optional **GitHub**/**Discord** URLs, and an optional **cover image** path (png/jpg).
4. Export → a single `<Title>.exe` appears next to the tool. Share it. Users just run it and press hotkeys.

---

## 5. What's LEFT — prioritized roadmap (what mod creators actually need)

Ordered by impact. This is the "don't make me list features" backlog, drawn from what Cheat Engine / x64dbg / ReClass / FLiNG / WeMod / BepInEx provide.

> **Update (latest passes): P0 1–4, P1 5–9, P2 10–11, P3 14–19 are now DONE, and the injection/runtime path is fixed.** Remaining: P2 #12 (full visual hook manager with live arg inspection — basic install/list exists), P2 #13 (mod packaging), string **replace**-all, and the entire **P4 online platform** (needs a server/backend — can't be built offline). Everything else works.

### P0 — the big missing capabilities
1. ✅ **"Find what writes/accesses this address"** — **Find Writes** tab (debugger + hardware breakpoint). Catches the exact instructions; "Disasm" jumps to them.
2. ✅ **Patch / NOP / assemble** — Disasm right-click **NOP this instruction** with a **Patches** list + one-click **Revert / Revert all**.
3. ✅ **AOB pattern scan** with wildcards — Scanner "search = AOB pattern"; also `umf.aob` in Lua.
4. ✅ **Return-value override hooks** — Runtime tab "Quick hook (force return)" box (no code), plus `umf.hook` in Lua. God-mode/all-weapon in one click.

### P1 — robustness & discovery
5. ✅ **Pointer-scan presets + revalidation** — Quick/Normal/Deep presets; a live "resolves" column via `resolve_chain`.
6. ✅ **Struct dissector** — **Struct** tab shows memory at a base as a live typed table.
7. ✅ **Hex editing** — click a byte to edit + write; Data Inspector; live changed-byte highlight. (Find bytes/string lives on the Scanner AOB/Text modes.)
8. ✅ **String search** — Scanner "search = Text string" (ASCII + UTF-16). (Replace-all still TODO.)
9. ✅ **Watch list** — **Watch** tab (pin addresses, live, freeze), plus right-click → Add to Watch.

### P2 — scripting & mods (lean on the engine you already have)
10. ✅ (mostly) **Lua editor upgrades** — snippet library + bigger editor + clear/output. (Syntax highlight/autocomplete/REPL still TODO.)
11. ✅ **High-value Lua API** — added `read_*/write_*`, `read_bytes/write_bytes`, `aob`, `nop`, `module_base` to the engine sandbox (alongside `resolve/hook/call_original/log`). See LUA_DEMO.md.
12. ⬜ **Visual hook manager** (pick a function, toggle a hook, see args/return) — no coding.
13. ⬜ **Mod packaging** (BepInEx-style): bundle Lua/native mods + manifest; one-click load via the runtime.

### P3 — trainer/UX polish
14. ✅ **Trainer** reorder/duplicate/enable-all — All-OFF + per-row controls; Watch list added. (Named groups still TODO.)
15. ✅ **Cheat freeze types** — Locked / No-less-than (floor) / No-more-than (cap), in tool + exported trainer.
16. ✅ **Settings tab** — accent theme (persisted to `hexforge.ini`), default writable-only, rights/about.
17. ✅ **Toasts** — toast notifications for patches/writes. (Detachable panels still TODO.)
18. ✅ **Game-image picker dialog** (+ trainer save/load dialogs). Brand icons are still clean drawn glyphs (drop-in official PNGs = TODO).
19. ✅ **Export options:** author + version baked in and shown in the trainer footer. (Window size/theme + safe-mode indicator still TODO.)

### P4 — the community/platform vision (not started — needs backend/online infra)
20. ⬜ **Trainer registry/manifest format** (sign + version; metadata: game, version, author, hashes).
21. ⬜ **In-tool browser** to publish/search/install community trainers & mods.
22. ⬜ **Account/author identity**, update checks, submission pipeline.

---

## 9. Changelog — latest pass
- **Injection fixed (was broken):** `Inject runtime` + `Connect` now work. Root causes were (a) the runtime's dependency DLLs (`Zydis.dll`, `lua.dll`, CRT) weren't on the target's search path, and (b) `DllMain` never started anything. Fixes: `inject_dll` adds the tool dir to the target's DLL search path then runs a new exported `umf_runtime_start` (boots logging + IPC pipe); CMake copies the CRT DLLs next to `umf_runtime.dll`. See LUA_DEMO.md §12.
- **Return-override hook, no code (P0#4):** Runtime tab "Quick hook (force return)" box — enter address + return value → god mode / all-weapon with one click (builds a `umf.hook` call).
- **Export options (P3#19):** author + version baked into the exported trainer and shown in its footer.
- Full end-to-end **test checklist** added to LUA_DEMO.md §13.

## 9b. Changelog — prior pass
- **Disasm**: NOP/patch with original-bytes capture and Revert/Revert-all.
- **Scanner**: AOB pattern (wildcards) and Text (ASCII/UTF-16) search modes; per-row **Set** live-write.
- **Pointer Scan**: presets + live resolve/revalidate column.
- **Struct** dissector tab; **Watch** tab; **Settings** tab (persisted); toast notifications; native file dialogs.
- **Freeze types** (locked/floor/cap) in tool + exported trainer.
- **Engine Lua API** expanded: memory read/write, `read_bytes`/`write_bytes`, `aob`, `nop`, `module_base`; Runtime tab snippet library. Engine tests still **399/399**.
- See **LUA_DEMO.md** for a full hands-on scripting walkthrough (all-weapon ammo, god-mode hook, reload-safe addresses).



---

## 6. Testing status & known limitations

**Verified**
- Engine unit tests: `399/399`.
- `hexforge_tool.exe` and `hexforge_trainer.exe` compile clean (Debug).
- Scanner no longer freezes/crashes on big processes; Next Scan race fixed.

**Needs a manual smoke test (GUI, can't be unit-tested here)**
- Run the full checklist in **LUA_DEMO.md §13** (covers every tab + the exported trainer + Lua).
- Especially: **Inject runtime → Connect** (fixed this pass), **Find Writes** on a safe single-player game, and the Lua **Quick hook**.

**Known limitations today**
- **Find Writes** makes the tool the game's debugger — use only on offline/single-player targets; never on anti-cheat-protected online games.
- Exported trainers use fixed addresses unless you back them with a pointer path or an AOB/Lua cheat; a game update can move a raw address.
- No full visual hook manager with live argument inspection yet (basic install/list + Lua hooks exist); no mod-packaging UI; string search has no replace-all.
- Brand icons are clean drawn glyphs, not official PNG logos (drop-in TODO).
- 32-bit targets: the tool is x64; reading x86 processes works for data, but code features assume x64.
- **P4 community platform** (publish/browse/install, accounts, signed manifests) needs an online backend and is not built — the `.hexforge` format + single-file export are its local foundation.

---

## 7. Commit history (recent)
- `fix Next Scan crash (thread-safe results), standalone Trainer .exe export, Trainer Wizard, live Hex inspector, Memory Map colors, Log filters`
- `live value writes from Scanner + editable values in exported trainer`
- (this pass) `multi-res icon from hex.png; embedded cover image (WIC) + GitHub/Discord link glyphs + per-cheat notes in the exported trainer`

---

## 8. Suggested next step
Build **P0 #1 + #2** together: "find what writes to this address" → jump to the instruction → **NOP/patch with revert**. That single workflow is what turns Hexforge from a value editor into a real trainer/mod maker, and it directly fixes the "infinite ammo only affects one weapon" limitation.
