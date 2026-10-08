import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { createBackend } from "../ipc/client";
import {
  logLevelName,
  type Backend,
  type BackendKind,
  type HookInfo,
  type LogLine,
  type Mitigations,
  type ModInfo,
  type ProcInfo,
  type ProcessInfo,
} from "../ipc/types";

export type View = "dashboard" | "hooks" | "mods" | "lua" | "logs" | "connection";

interface Studio {
  kind: BackendKind;
  view: View;
  setView: (v: View) => void;

  connected: boolean;
  connecting: boolean;
  busy: boolean;
  error: string | null;

  pid: number | null;
  pipe: string | null;
  process: ProcessInfo | null;
  mitigations: Mitigations | null;
  hooks: HookInfo[];
  mods: ModInfo[];
  logs: LogLine[];
  processes: ProcInfo[];

  connect: (pid: number) => Promise<boolean>;
  disconnect: () => Promise<void>;
  refreshAll: () => Promise<void>;
  refreshHooks: () => Promise<void>;
  refreshMods: () => Promise<void>;
  refreshProcesses: () => Promise<void>;
  evalLua: (code: string) => Promise<boolean>;
  clearLogs: () => void;
}

const Ctx = createContext<Studio | null>(null);

export function useStudio(): Studio {
  const c = useContext(Ctx);
  if (!c) throw new Error("useStudio must be used within <StudioProvider>");
  return c;
}

const MAX_LOGS = 2000;

export function StudioProvider({ children }: { children: ReactNode }) {
  const backendRef = useRef<Backend | null>(null);
  if (!backendRef.current) backendRef.current = createBackend();
  const backend = backendRef.current;

  const [view, setView] = useState<View>("connection");
  const [connected, setConnected] = useState(false);
  const [connecting, setConnecting] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const [pid, setPid] = useState<number | null>(null);
  const [pipe, setPipe] = useState<string | null>(null);
  const [process, setProcess] = useState<ProcessInfo | null>(null);
  const [mitigations, setMitigations] = useState<Mitigations | null>(null);
  const [hooks, setHooks] = useState<HookInfo[]>([]);
  const [mods, setMods] = useState<ModInfo[]>([]);
  const [logs, setLogs] = useState<LogLine[]>([]);
  const [processes, setProcesses] = useState<ProcInfo[]>([]);

  const logId = useRef(0);
  const unsubs = useRef<Array<() => void>>([]);

  const pushLog = useCallback((level: number, msg: string) => {
    const line: LogLine = {
      id: logId.current++,
      level,
      levelName: logLevelName(level),
      msg,
      time: new Date().toLocaleTimeString("en-GB", { hour12: false }),
    };
    setLogs((prev) => {
      const next = prev.length >= MAX_LOGS ? prev.slice(prev.length - MAX_LOGS + 1) : prev.slice();
      next.push(line);
      return next;
    });
  }, []);

  const clearSubs = useCallback(() => {
    unsubs.current.forEach((u) => u());
    unsubs.current = [];
  }, []);

  const refreshHooks = useCallback(async () => {
    try {
      setHooks(await backend.rpc<HookInfo[]>("listHooks"));
    } catch {
      /* keep previous list */
    }
  }, [backend]);

  const refreshMods = useCallback(async () => {
    try {
      setMods(await backend.rpc<ModInfo[]>("listMods"));
    } catch {
      /* keep previous list */
    }
  }, [backend]);

  const refreshAll = useCallback(async () => {
    setBusy(true);
    try {
      const [p, m] = await Promise.all([
        backend.rpc<ProcessInfo>("getProcessInfo"),
        backend.rpc<Mitigations>("getMitigations"),
      ]);
      setProcess(p);
      setMitigations(m);
      await Promise.all([refreshHooks(), refreshMods()]);
    } finally {
      setBusy(false);
    }
  }, [backend, refreshHooks, refreshMods]);

  const refreshProcesses = useCallback(async () => {
    try {
      setProcesses(await backend.listProcesses());
      setError(null);
    } catch (e) {
      setProcesses([]);
      setError(String((e as Error)?.message ?? e));
    }
  }, [backend]);

  const handleDisconnected = useCallback(() => {
    clearSubs();
    setConnected(false);
    pushLog(4, "disconnected from runtime");
  }, [clearSubs, pushLog]);

  const connect = useCallback(
    async (targetPid: number): Promise<boolean> => {
      setConnecting(true);
      setError(null);
      try {
        const info = await backend.connect(targetPid);
        setPid(info.pid);
        setPipe(info.pipe);
        setConnected(true);
        clearSubs();
        unsubs.current.push(backend.onLog(pushLog));
        unsubs.current.push(backend.onDisconnect(handleDisconnected));
        await backend.rpc("subscribe");
        await refreshAll();
        return true;
      } catch (e) {
        setError(String((e as Error)?.message ?? e));
        setConnected(false);
        return false;
      } finally {
        setConnecting(false);
      }
    },
    [backend, pushLog, handleDisconnected, clearSubs, refreshAll],
  );

  const disconnect = useCallback(async () => {
    try {
      await backend.disconnect();
    } catch {
      /* ignore */
    }
    clearSubs();
    setConnected(false);
    setProcess(null);
    setMitigations(null);
    setHooks([]);
    setMods([]);
  }, [backend, clearSubs]);

  const evalLua = useCallback(
    async (code: string) => {
      try {
        const r = await backend.rpc<{ ok: boolean }>("evalLua", { code });
        return !!r?.ok;
      } catch {
        return false;
      }
    },
    [backend],
  );

  const clearLogs = useCallback(() => setLogs([]), []);

  useEffect(() => () => clearSubs(), [clearSubs]);

  const value: Studio = {
    kind: backend.kind,
    view,
    setView,
    connected,
    connecting,
    busy,
    error,
    pid,
    pipe,
    process,
    mitigations,
    hooks,
    mods,
    logs,
    processes,
    connect,
    disconnect,
    refreshAll,
    refreshHooks,
    refreshMods,
    refreshProcesses,
    evalLua,
    clearLogs,
  };

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}
