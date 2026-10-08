import { invoke } from "@tauri-apps/api/core";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import type { Backend, ConnectInfo } from "./types";
import { MockBackend } from "./mock";

export function isTauri(): boolean {
  return typeof window !== "undefined" && "__TAURI_INTERNALS__" in window;
}

interface LogPayload {
  level: number;
  msg: string;
}

// Talks to the Rust side (src-tauri), which owns the named-pipe JSON-RPC
// connection to umf_runtime and forwards log notifications as Tauri events.
class TauriBackend implements Backend {
  readonly kind = "tauri" as const;
  private logCbs = new Set<(level: number, msg: string) => void>();
  private discCbs = new Set<() => void>();
  private unlisteners: UnlistenFn[] = [];
  private ready: Promise<void> | null = null;

  private ensureListeners(): Promise<void> {
    if (this.ready) return this.ready;
    this.ready = (async () => {
      const un1 = await listen<LogPayload>("umf://log", (e) => {
        const p = e.payload;
        if (p) this.logCbs.forEach((cb) => cb(p.level, p.msg));
      });
      const un2 = await listen("umf://disconnect", () => {
        this.discCbs.forEach((cb) => cb());
      });
      this.unlisteners.push(un1, un2);
    })();
    return this.ready;
  }

  async connect(pid: number): Promise<ConnectInfo> {
    await this.ensureListeners();
    return await invoke<ConnectInfo>("umf_connect", { pid });
  }

  async disconnect(): Promise<void> {
    await invoke("umf_disconnect");
  }

  async rpc<T = unknown>(method: string, params?: Record<string, unknown>): Promise<T> {
    return await invoke<T>("umf_rpc", { method, params: params ?? null });
  }

  onLog(cb: (level: number, msg: string) => void): () => void {
    this.logCbs.add(cb);
    return () => this.logCbs.delete(cb);
  }

  onDisconnect(cb: () => void): () => void {
    this.discCbs.add(cb);
    return () => this.discCbs.delete(cb);
  }
}

export function createBackend(): Backend {
  return isTauri() ? new TauriBackend() : new MockBackend();
}
