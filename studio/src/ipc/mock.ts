import type { Backend, ConnectInfo } from "./types";

// A self-contained mock runtime so the full UI is alive in a plain browser
// (no injected target required) — useful for development and demos.
export class MockBackend implements Backend {
  readonly kind = "mock" as const;
  private connected = false;
  private pid = 0;
  private logCbs = new Set<(level: number, msg: string) => void>();
  private discCbs = new Set<() => void>();
  private timer: ReturnType<typeof setInterval> | null = null;
  private tick = 0;

  async connect(pid: number): Promise<ConnectInfo> {
    this.connected = true;
    this.pid = pid;
    this.startLogStream();
    return { pid, pipe: `\\\\.\\pipe\\umf-studio-${pid}` };
  }

  async disconnect(): Promise<void> {
    this.connected = false;
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
    this.discCbs.forEach((cb) => cb());
  }

  async rpc<T = unknown>(method: string, params?: Record<string, unknown>): Promise<T> {
    const data = this.dispatch(method, params);
    // small simulated latency
    await new Promise((r) => setTimeout(r, 40 + Math.random() * 60));
    return data as T;
  }

  onLog(cb: (level: number, msg: string) => void): () => void {
    this.logCbs.add(cb);
    return () => this.logCbs.delete(cb);
  }

  onDisconnect(cb: () => void): () => void {
    this.discCbs.add(cb);
    return () => this.discCbs.delete(cb);
  }

  private dispatch(method: string, params?: Record<string, unknown>): unknown {
    switch (method) {
      case "ping":
        return { pong: true };
      case "getProcessInfo":
        return { pid: this.pid, path: "C:\\Games\\Starfell\\Starfell.exe", arch: "x64" };
      case "getMitigations":
        return { acg: false, hvci: false, cfg: true, cet_ss: true, cet_ibt: false };
      case "listHooks":
        return MOCK_HOOKS;
      case "listMods":
        return MOCK_MODS;
      case "evalLua": {
        const code = String(params?.code ?? "");
        const ok = !/error|undefined_global|\bnil\(/i.test(code);
        this.emit(ok ? 2 : 4, ok ? "evalLua: chunk ran ok" : "evalLua: chunk raised an error");
        return { ok };
      }
      case "subscribe":
        return { subscribed: true };
      default:
        throw new Error(`method not found: ${method}`);
    }
  }

  private startLogStream() {
    if (this.timer) return;
    this.timer = setInterval(() => {
      if (!this.connected) return;
      const [level, msg] = MOCK_LOG_LINES[this.tick % MOCK_LOG_LINES.length];
      this.tick++;
      this.emit(level, msg);
    }, 1400);
    // a couple of immediate lines so the console isn't empty
    this.emit(2, "IPC client subscribed to log stream");
    this.emit(1, "overlay: present hook active (DXGI vtable[8])");
  }

  private emit(level: number, msg: string) {
    this.logCbs.forEach((cb) => cb(level, msg));
  }
}

const MOCK_HOOKS: unknown = [
  { name: "d3d11.dll!Present", address: "0x00007FFC12AB3400", strategy: 2, chain: 2, installed: true },
  { name: "kernel32.dll!CreateFileW", address: "0x00007FFC0F221180", strategy: 1, chain: 1, installed: true },
  { name: "ws2_32.dll!recv", address: "0x00007FFC0E8820C0", strategy: 1, chain: 1, installed: true },
  { name: "game.exe!PlayerTakeDamage", address: "0x000000014029A6E0", strategy: 0, chain: 3, installed: true },
  { name: "game.exe!UpdateCamera", address: "0x00000001402C1120", strategy: 0, chain: 1, installed: true },
  { name: "user32.dll!GetAsyncKeyState", address: "0x00007FFC10551A40", strategy: 5, chain: 1, installed: true },
  { name: "game.exe!ResolveShopPrice", address: "0x00000001403F8880", strategy: 3, chain: 1, installed: false },
];

const MOCK_MODS: unknown = [
  { name: "InfiniteHealth", version: "1.2.0", type: 1, caps: 0x07, priority: 10, active: true, category: "game", audience: "beginner" },
  { name: "NoClip", version: "0.9.1", type: 1, caps: 0x06, priority: 5, active: true, category: "game", audience: "expert" },
  { name: "FOVUnlock", version: "1.0.0", type: 0, caps: 0x03, priority: 0, active: true, category: "game", audience: "beginner" },
  { name: "NetInspector", version: "0.4.2", type: 0, caps: 0x02, priority: 20, active: false, category: "research", audience: "expert" },
  { name: "OverlayHUD", version: "2.0.0", type: 0, caps: 0x0b, priority: 8, active: true, category: "app", audience: "beginner" },
];

const MOCK_LOG_LINES: [number, string][] = [
  [2, "scan: 4 results narrowed to 1 (i32 == 100)"],
  [1, "mem: wrote 4 bytes @ 0x14029A6E4 (InfiniteHealth)"],
  [2, "mod 'NoClip' toggled: active"],
  [3, "hwbp: Dr0 shared with debugger — re-armed"],
  [1, "ipc: listHooks -> 7 targets"],
  [2, "overlay: frame callback 1200 frames"],
  [1, "aob: 'E8 ?? ?? ?? ?? 48 8B' matched @ game.exe+0x2C110"],
  [3, "lua: script instruction budget at 60%"],
  [2, "profiler: PlayerTakeDamage avg 1.8us over 420 calls"],
  [1, "watchdog: module d3d11.dll stable"],
];
