# Hexforge — Engineering Handoff

_Last updated: 2026-10-13. Head commit: `69cd9c1`. Repo: `SpectralZero/Falcon_Hock` (branch `main`)._

This is a deep, self-contained handoff for whoever continues Hexforge (human or AI).
Read `HEXFORGE_REPORT.md` for the product/status overview, `LUA_DEMO.md` for the
scripting walkthrough + end-to-end test checklist, and `TEST_REPORT.md` (bottom
section) for the test-by-test resolution status.

---

## 1. Goal

Build an **all-in-one native memory & game-hacking toolkit** for Windows x64 that a
single user can use end-to-end:

- **External tool** (`hexforge_tool.exe`): attach to any process, scan/edit memory
  (Cheat-Engine style), pointer-scan, disassemble/patch, hardware-breakpoint
  "find what writes", build a trainer, and **export a standalone single-file
  trainer `.exe`** (FLiNG-style) that non-technical users can run.
- **Injectable engine** (`umf_runtime.dll`): in-process hooks (inline/IAT/vtable/
  EAT/HWBP), a sandboxed **Lua** API (read/write/aob/nop/hook), overlay, mod loader,
  talking to the tool over a named pipe.
- **Long-term vision**: a community platform where mid-level+ users create and share
  trainers/mods under the tool's brand, and normal users browse/install.

Guiding principle from the owner: **be proactive** — research what real mod creators
need (Cheat Engine / x64dbg / ReClass / FLiNG / BepInEx) and build toward that, not
just one-off requests. Keep every build green and the engine tests at **399/399**.

---

## 2. Current state (what works today)

**Build:** VS 2022 + vcpkg. Configure/build the `build/` dir.
- `cmake --build build --target hexforge_tool --config Debug` builds the tool **and**
  `hexforge_trainer.exe` (export runner).
- `cmake --build build --target umf_test --config Debug` + run `build\Debug\umf_test.exe`
  → `399 checks, 0 failed`.
- `VCPKG_ROOT` for this machine: `C:/Users/RTX/AppData/Local/Temp/kilo/vcpkg`.
- Run `hexforge_tool.exe` **as admin**.

**Verified by the owner's manual test (TEST_REPORT.md): 54/61 pass, 0 fail, 7 partial.**
All 7 partials + the 9 "Issues Found" + the GUI-overflow note have since been
addressed (see TEST_REPORT.md → "Resolution Status"). Highlights now working:

- Scanner: value / unknown / increased-decreased-changed-unchanged / **AOB** / **text**,
  now **multi-threaded** (was 26 min on RE3 → should be seconds; re-test).
- Live per-row writes, Watch tab, Trainer (freeze modes, hotkeys F1–F12, notes),
  **standalone trainer export** with embedded cover image + GitHub/Discord + author/version.
- **Find Writes** (DebugActiveProcess + DR0/DR7 hardware breakpoint) → **Make AOB**
  (real signature from the caught instruction) → **+NOP cheat** (restart-proof).
- **Disasm**: reactive, follow jump/call, back-stack, NOP/patch + revert, Copy-as-AOB.
- **Hex**: paged/scrollable (256 B–4 KB), wheel/arrow paging, robust per-row reads,
  click-to-edit, live changed-byte highlight, data inspector.
- **Pointer scan**: presets + live "resolves" column; fixed to return chains on 64-bit.
- **Struct** dissector (viewer), **Memory Map** (color-coded), **Modules** (+ exports
  with Disasm/Hex jump), **Settings** (accent persisted to `hexforge.ini`), toasts.
- **Runtime/Lua**: injection fixed; Connect works; snippet library; **Quick hook
  (force return)** no-code god-mode; persistent lock-protected Lua sandbox so hooks survive.

**Known-good invariants to preserve:**
- Scanner results are mutex-guarded; the UI reads a **snapshot** (`results_snapshot`),
  never a live reference. Don't reintroduce a live-reference read (it caused the
  original "vector subscript out of range" crash).
- `umf_runtime.dll` must stay **self-contained at load**: its deps (`Zydis.dll`,
  `lua.dll`, CRT DLLs) are resolved via `SetDllDirectoryW(toolDir)` at inject time +
  the CRT copy step in CMake. Don't add a new runtime dependency without updating both.
- `DebugWatch` must always `disarm_all()` + `DebugActiveProcessStop()` on stop, and
  keep `DebugSetProcessKillOnExit(FALSE)`, or stopping the watch will kill the game.

---

## 3. Active files (where everything lives)

### External tool — `tool/`
| File | Lines (approx) | Role |
|---|---|---|
| `app.cpp` | ~2300 | **All UI** (ImGui tabs), the `App` god-struct, scan orchestration, trainer/export, wizard, disasm/hex/struct/watch/find-writes/modules/settings, runtime/Lua panel. Start here for UI work. |
| `backend.hpp` / `backend.cpp` | ~950 | Engine: process list, attach, read/write, **parallel scanner** (`run_parallel`), `first_scan`/`first_scan_unknown`/`next_scan`/`aob_scan`/`string_scan`/`aob_find_first`, `pointer_scan`/`resolve_chain`, `module_exports`, regions/modules, `inject_dll`, `DebugWatch`. No ImGui, no Zydis here (keep it that way so the trainer links it). |
| `trainer.hpp` / `trainer.cpp` | ~400 | Shared `Cheat`/`TrainerDoc` model, `.hexforge` text format (pipe-delimited, back-compat parse), single-file **overlay** embed (v1 text, v2 text+image), `apply_patch_cheat`, `write_typed`/`fmt_value_at`, hotkey helpers. Shared by tool + trainer runner. |
| `trainer_runner.cpp` | ~690 | The exported trainer window (FLiNG-style): logo, glitch title, cover image (WIC), GitHub/Discord glyph buttons, cheat table (editable values, freeze modes, patch cheats), auto-attach, hotkeys. Reads its embedded overlay. |
| `ipc.hpp` / `ipc.cpp` | — | JSON-RPC-ish client to the runtime over a named pipe. |
| `hexforge.rc` / `hexforge.ico` / `hexforge.png` | — | App icon (resource `101`, multi-res) + 512px master. Feeds taskbar/shortcut/trainer logo. |

### Injectable engine — `src/`, headers in `include/umf/umf.h`
| File | Role |
|---|---|
| `src/umf_init.c` | `umf_init`/`umf_shutdown`, `DllMain`, and the exported **`umf_runtime_start`** (bootstrap the injector runs remotely). |
| `src/ipc/umf_ipc.c` | Named-pipe server + method dispatch (`evalLua`, `listHooks`, `listMods`, `getMitigations`, `getProcessInfo`, `ping`, `subscribe`). `evalLua` uses the **persistent** sandbox. |
| `src/sandbox/umf_lua_hook.c` | The `umf.*` Lua API: `log/resolve/hook/call_original` + added `read_*/write_*/read_bytes/write_bytes/aob/nop/module_base`. Persistent eval state + exec lock live here. |
| `src/sandbox/umf_lua_sandbox.c` | Sandbox creation (removes os/io/debug/ffi; memory + instruction budget). |
| `src/core/...`, `src/mem/umf_aob.c`, hooks | Trampoline pool, IAT/EAT/vtable/HWBP hooks, Zydis relocator, AOB (`umf_aob_scan`), overlay. |
| `tests/` | 399 checks via `umf_test`. |

### Build / docs
- `CMakeLists.txt` — targets `umf_runtime` (DLL, + CRT-copy step), `umf_test`,
  `hexforge_tool` (links imgui/Zydis/d3d11/dbghelp/comdlg32), `hexforge_trainer`
  (links imgui/d3d11/**windowscodecs/ole32/shell32**, no Zydis).
- `HEXFORGE_REPORT.md`, `LUA_DEMO.md`, `TEST_REPORT.md`, this `HANDOFF.md`.

---

## 4. Changes made (chronological, by commit)

| Commit | Summary |
|---|---|
| `2b7d5ea` | Fixed Next-Scan crash (thread-safe snapshot results); background scan + progress + cancel; first **trainer export**; **Wizard**; live Hex inspector; Memory-Map colors; Log filters. |
| `d8f90a0` | Live single-address writes from Scanner; **editable values** in the exported trainer. |
| `09c728e` | Multi-res **icon** from `hex.png`; embedded **cover image** (WIC) in trainer; GitHub/Discord glyph buttons; per-cheat **notes**. |
| `2ea305b` | **Find Writes** (`DebugWatch` HWBP); Disasm **NOP/patch + revert**; **AOB** + **text** scan; Watch/Struct/Settings tabs; pointer-scan presets + resolve; freeze types; **engine Lua memory API**; Runtime snippet library. |
| `fb102b6` | **Injection fixed** (`SetDllDirectoryW` + exported `umf_runtime_start` + CRT copy); **persistent Lua sandbox** (hooks survive); **Quick hook (force return)**; trainer author/version; LUA_DEMO §12–13. |
| `afbd22f` | **Multi-threaded scanner** (work-stealing pool, 16 MB reads, first-byte memchr); **tab auto-switch** (Browse-in-Hex / Memory-Map / Modules); clickable Modules; horizontal-scroll safety net. |
| `8bbcbed` | **Pointer scanner fixed** (committed-pointer filter + 8M cap → chains on 64-bit); **Find Writes → Make AOB** (real signature) + **+NOP cheat**; **AOB code-patch cheat kind** in tool + exported trainer. |
| `d438ba0` | **Scrollable/paged Hex** editor; **interactive Disasm** (follow jump/call, back-stack, Copy-as-AOB, reactive). |
| `69cd9c1` | **Module PE exports** (`module_exports`) with filter + Disasm/Hex jump; Help text refresh. |

Format/compat note: `.hexforge` cheat line is now
`label|addrHex|type|value|freeze|hotkey|mode|note|freezeMode|kind|aob`
plus `#TITLE/#TARGET/#GITHUB/#DISCORD/#AUTHOR/#VERSION/#IMAGE` meta lines. Parsing is
back-compatible (missing trailing fields default sensibly). Overlay magic: `HXFTRN1`
(text) / `HXFTRN2` (text+image).

---

## 5. Failed attempts & course corrections (don't repeat these)

- **Single-threaded scanner** (1 MB chunks, byte-by-byte): 26 min on RE3. Replaced by
  `run_parallel` (one worker/core, 16 MB jobs) + a first-fixed-byte `memchr` skip for
  AOB. _Lesson: RPM overhead + one core dominates; batch + parallelise._
- **Pointer scanner indexing any 48-bit value**: filled the 4M cap in early heaps and
  returned **0 chains** on 64-bit. Fixed by indexing **only values that point into a
  committed region** (binary-search over sorted ranges) + raising the cap to 8M.
  _Lesson: most "pointer-like" qwords are noise; filter to real pointers._
- **`evalLua` created+destroyed the sandbox each call**: `umf.hook` stored that
  `lua_State*`, so when the hooked function later fired it used freed memory → crash.
  Fixed with a **persistent** eval sandbox + a recursive **exec lock** shared by the
  hook dispatcher. _Lesson: hook callbacks outlive the eval call; the state must too._
- **Injection "just LoadLibrary"**: failed because (a) `Zydis.dll`/`lua.dll`/Debug-CRT
  weren't on the target's search path, and (b) `DllMain` started nothing. Fixed with
  remote `SetDllDirectoryW(toolDir)` + exported `umf_runtime_start` run via
  `CreateRemoteThread` + a CMake step copying CRT DLLs next to the runtime.
  _Lesson: an injected DLL must be self-contained at load and self-bootstrapping._
- **Fake AOB in the Lua snippet** (`29 41 ?? 48 8B`) matched the wrong instruction in
  RE3. Replaced the "trust the example" flow with **Find Writes → Make AOB** that
  reconstructs the real signature (Zydis backward-decode to the RIP boundary).
- **`(int[]){...}[idx]` compound literal** for the hex-size combo: compiles in C, not
  C++ (MSVC). Replaced with a `static const int[]` lookup.
- **Hex `hexBase` as a second source of truth** next to `hexAddr`: fought external
  "Browse in Hex" navigation and clobbered typing. Reverted to **`hexAddr` only**;
  paging rewrites the text.
- **Build-time gotchas:** `OPENFILENAMEW` needs `<commdlg.h>` + `comdlg32`; WIC needs
  `<wincodec.h>` + `windowscodecs`/`ole32`; the trainer must **not** link Zydis
  (so `backend.cpp` stays Zydis-free — disasm lives only in `app.cpp`).
- **Relinking while the tool is running** fails (exe locked). Close `hexforge_tool.exe`
  before building, or expect an MSBuild "cannot open for writing" error.

---

## 6. Next steps (prioritised, with concrete approach)

**Tier 1 — biggest remaining pro features**
1. **Auto Assembler + code caves.** Add an assembler (vcpkg `keystone`), a CE-style
   `[ENABLE]/[DISABLE]` script with `alloc/dealloc/label`, and `VirtualAllocEx` caves +
   a 5-byte `jmp` detour with original-byte save/revert. Expose `umf.alloc` /
   `umf.assemble` in the Lua API too. _This is the #1 thing pros want._
2. **Disk-backed pointer maps + cross-session rescan.** Snapshot pointer index to a
   file; diff two snapshots (restart game, re-map, intersect) to find stable paths on
   multi-GB games. Keep the current in-memory path as "quick scan".
3. **Struct dissector upgrade (ReClass-lite):** name fields, follow-pointer inline
   (click a pointer → open sub-struct), save/load layout (JSON), C++ struct codegen.

**Tier 2 — competitive polish**
4. **Speedhack**: in the engine, hook `QueryPerformanceCounter`/`GetTickCount64`/
   `timeGetTime`, keep a virtual clock scaled by a factor; expose a Runtime slider +
   `umf.speed(x)`.
5. **Register + callstack at a Find-Writes hit** (capture the thread CONTEXT in the
   debug loop and surface it); support **DR0–DR3** (up to 4 simultaneous watches).
6. **Module imports + sections**; **AVX2 SIMD** compare in `run_parallel` for exact
   int32/float scans; **per-tab layout audit** on narrow windows (some rows still rely
   on the horizontal-scroll safety net).

**Tier 3 — reach**
7. **CE `.CT` import/export**, Lua **syntax highlighting**, in-game **D3D overlay**
   wired to Lua draw calls, and the **P4 online platform** (needs a real backend:
   accounts, signed trainer manifests, a browse/install client).

**Testing workflow that works well:** the owner runs the whole suite in one pass and
writes findings into `TEST_REPORT.md` (phase, test#, expected, result, repro). Keep
appending resolution status rather than rewriting. Always finish a change with a green
`hexforge_tool` build and, if the engine changed, `umf_test` = 399/399.
