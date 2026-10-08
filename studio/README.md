# Hexforge Studio

The desktop control console for the **Hexforge** universal mod framework
(the injected `umf_runtime`). Built with **Tauri 2 + React + TypeScript**.

Studio attaches to a running target over the runtime's per-process named pipe
and speaks **newline-delimited JSON-RPC 2.0** to inspect hooks, mods and
security mitigations, stream the live log, and evaluate sandboxed Lua.

```
┌─────────────┐   invoke / events   ┌──────────────┐   \\.\pipe\umf-studio-<pid>   ┌─────────────┐
│ React (UI)  │ ◄─────────────────► │ Rust (Tauri) │ ◄───── JSON-RPC 2.0 ────────► │ umf_runtime │
└─────────────┘                     └──────────────┘        (named pipe)            └─────────────┘
```

## Prerequisites

- Node.js 18+ and npm
- Rust (stable) + the Tauri 2 prerequisites for Windows (MSVC build tools,
  WebView2 runtime — present on Windows 10/11 by default)

## Develop & build

```bash
npm install          # install frontend deps
npm run dev          # Vite dev server only (browser, mock runtime)
npm run tauri dev    # full desktop app with the live named-pipe bridge
npm run build        # type-check + build the frontend bundle
npm run tauri build  # produce the distributable desktop app
```

Running in a plain browser (`npm run dev`) uses a **mock backend** that serves
representative data and a live log stream, so the whole UI is explorable
without an injected target. Inside Tauri it uses the real bridge.

## IPC contract (served by `umf_runtime`)

Pipe: `\\.\pipe\umf-studio-<pid>` · one JSON object per line.

| Method           | Params           | Result                                                            |
|------------------|------------------|-------------------------------------------------------------------|
| `ping`           | –                | `{ pong: true }`                                                  |
| `getProcessInfo` | –                | `{ pid, path, arch }`                                             |
| `getMitigations` | –                | `{ acg, hvci, cfg, cet_ss, cet_ibt }`                             |
| `listHooks`      | –                | `[{ name, address, strategy, chain, installed }]`                 |
| `listMods`       | –                | `[{ name, version, type, caps, priority, active, category, audience }]` |
| `evalLua`        | `{ code }`       | `{ ok }`                                                          |
| `subscribe`      | –                | `{ subscribed: true }` then streams `{"method":"log","params":{level,msg}}` |

The Rust bridge (`src-tauri/src/lib.rs`) exposes `umf_connect`, `umf_disconnect`
and `umf_rpc` commands and re-emits log notifications as the `umf://log` Tauri
event (and `umf://disconnect` on drop).

## Structure

```
src/
  brand.ts              brand constants (one place to rebrand)
  styles.css            design system (dark mod-tool theme)
  components/           Logo, WindowChrome, Sidebar, StatusBar, ui kit
  ipc/                  types, client (Tauri | mock), mock backend
  state/store.tsx       connection + data store (React context)
  views/                Dashboard, Hooks, Mods, LuaConsole, Logs, Connection
src-tauri/              Rust: named-pipe JSON-RPC bridge + Tauri shell
app-icon.svg            logo source for `npm run tauri icon`
```
