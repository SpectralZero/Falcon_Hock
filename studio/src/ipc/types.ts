// Shared types + enum decoders mirroring the umf_runtime IPC contract
// (see src/ipc/umf_ipc.c and include/umf/umf.h).

export interface ProcessInfo {
  pid: number;
  path: string;
  arch: string;
}

export interface Mitigations {
  acg: boolean;
  hvci: boolean;
  cfg: boolean;
  cet_ss: boolean;
  cet_ibt: boolean;
}

export interface HookInfo {
  name: string;
  address: string;
  strategy: number;
  chain: number;
  installed: boolean;
}

export interface ModInfo {
  name: string;
  version: string;
  type: number;
  caps: number;
  priority: number;
  active: boolean;
  category: string;
  audience: string;
}

export interface LogLine {
  id: number;
  level: number;
  levelName: string;
  msg: string;
  time: string;
}

export interface ConnectInfo {
  pid: number;
  pipe: string;
}

export interface ProcInfo {
  pid: number;
  name: string;
  attachable: boolean;
}

export type BackendKind = "tauri" | "web";

export interface Backend {
  readonly kind: BackendKind;
  connect(pid: number): Promise<ConnectInfo>;
  disconnect(): Promise<void>;
  rpc<T = unknown>(method: string, params?: Record<string, unknown>): Promise<T>;
  listProcesses(): Promise<ProcInfo[]>;
  onLog(cb: (level: number, msg: string) => void): () => void;
  onDisconnect(cb: () => void): () => void;
}

// ── enum decoders (UmfHookStrategy / UmfModType / UmfCapability / UmfLogLevel) ──

export function strategyLabel(s: number): string {
  return ["Inline", "IAT", "VTable", "Gap", "EAT", "HWBP"][s] ?? `#${s}`;
}

export function strategyTone(s: number): string {
  // data-only strategies are "safer"; inline/gap/hwbp patch/stall code
  return [1, 2, 4].includes(s) ? "accent" : "info";
}

export function modTypeLabel(t: number): string {
  return ["Native", "Lua"][t] ?? `#${t}`;
}

export const CAP_FLAGS: { bit: number; label: string }[] = [
  { bit: 0x01, label: "hook" },
  { bit: 0x02, label: "read" },
  { bit: 0x04, label: "write" },
  { bit: 0x08, label: "overlay" },
  { bit: 0x10, label: "file" },
];

export function capList(mask: number): string[] {
  return CAP_FLAGS.filter((c) => (mask & c.bit) !== 0).map((c) => c.label);
}

export const LOG_LEVELS = ["TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL"] as const;

export function logLevelName(n: number): string {
  return LOG_LEVELS[n] ?? "INFO";
}
