import { useEffect, useMemo, useState } from "react";
import { Crosshair, Info, Plug, PlugZap, Power, RefreshCw, Search, TriangleAlert } from "lucide-react";
import { BRAND } from "../brand";
import { useStudio } from "../state/store";
import { Badge, Card, EmptyState } from "../components/ui";
import { BrandLockup } from "../components/Logo";

export function Connection() {
  const s = useStudio();
  const [q, setQ] = useState("");
  const [onlyTargets, setOnlyTargets] = useState(true);

  useEffect(() => {
    if (s.kind === "tauri" && !s.connected) void s.refreshProcesses();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const ql = q.trim().toLowerCase();
  const rows = useMemo(() => {
    return s.processes.filter((p) => {
      if (onlyTargets && !p.attachable) return false;
      if (!ql) return true;
      return p.name.toLowerCase().includes(ql) || String(p.pid).includes(ql);
    });
  }, [s.processes, onlyTargets, ql]);

  const manualPid = /^\d+$/.test(q.trim()) ? parseInt(q.trim(), 10) : NaN;
  const manualValid = Number.isFinite(manualPid) && manualPid > 0;
  const targetCount = s.processes.filter((p) => p.attachable).length;

  const doConnect = async (pid: number) => {
    const ok = await s.connect(pid);
    if (ok) s.setView("dashboard");
  };

  return (
    <div className="view fade-in">
      <div className="view-head">
        <span className="vh-icon">
          <PlugZap size={20} />
        </span>
        <div>
          <div className="view-title">Connection</div>
          <div className="view-sub">Attach to a running Hexforge runtime</div>
        </div>
        <span className="spacer" />
        <Badge tone={s.kind === "tauri" ? "ok" : "warn"}>{s.kind === "tauri" ? "LIVE BRIDGE" : "BROWSER"}</Badge>
        {s.kind === "tauri" && !s.connected && (
          <button className="btn btn-sm" onClick={() => void s.refreshProcesses()}>
            <RefreshCw size={14} /> Rescan
          </button>
        )}
      </div>

      <div className="view-body">
        {s.kind === "web" ? (
          <Card icon={<TriangleAlert size={16} />} title="Desktop app required">
            <div className="col" style={{ gap: 10 }}>
              <p className="muted" style={{ fontSize: 13, lineHeight: 1.6 }}>
                You are viewing the Studio UI in a browser, which has no access to the runtime pipe. There is no
                demo data. Launch the desktop app to enumerate processes and attach for real:
              </p>
              <div className="input mono" style={{ color: "var(--accent-2)" }}>
                npm run tauri dev
              </div>
            </div>
          </Card>
        ) : s.connected ? (
          <Card icon={<Plug size={16} />} title="Attached">
            <div className="col" style={{ gap: 12 }}>
              <div className="row gap-6">
                <span className="led ok" />
                <span className="mono">{s.process?.path ?? "runtime"}</span>
              </div>
              <div className="stat-row">
                <span className="k">PID</span>
                <span className="v">{s.pid}</span>
              </div>
              <div className="stat-row">
                <span className="k">Architecture</span>
                <span className="v">{s.process?.arch ?? "—"}</span>
              </div>
              <div className="stat-row">
                <span className="k">Pipe</span>
                <span className="v">{s.pipe}</span>
              </div>
              <div className="row">
                <button className="btn btn-danger" onClick={() => void s.disconnect()}>
                  <Power size={16} /> Disconnect
                </button>
                <span className="faint" style={{ fontSize: 12 }}>
                  Disconnect to attach to a different target.
                </span>
              </div>
            </div>
          </Card>
        ) : (
          <div style={{ display: "grid", gridTemplateColumns: "1.7fr 1fr", gap: 16 }}>
            <Card icon={<Crosshair size={16} />} title="Select a target">
              <div className="toolbar">
                <div className="search" style={{ flex: 1 }}>
                  <Search size={15} />
                  <input
                    className="input"
                    placeholder="Search by process name or PID…"
                    value={q}
                    onChange={(e) => setQ(e.target.value)}
                    autoFocus
                  />
                </div>
                <span
                  className={"chip" + (onlyTargets ? " on" : "")}
                  style={{ cursor: "pointer" }}
                  onClick={() => setOnlyTargets((v) => !v)}
                  title="Only show processes exposing a Hexforge runtime pipe"
                >
                  runtime only
                </span>
              </div>

              {manualValid && rows.every((r) => r.pid !== manualPid) && (
                <div className="row" style={{ marginBottom: 12 }}>
                  <button className="btn btn-primary btn-sm" disabled={s.connecting} onClick={() => void doConnect(manualPid)}>
                    <PlugZap size={14} /> Attach to PID {manualPid}
                  </button>
                  <span className="faint" style={{ fontSize: 12 }}>
                    direct attach (no pipe detected until connected)
                  </span>
                </div>
              )}

              {s.error && (
                <div className="badge err" style={{ marginBottom: 12 }}>
                  {s.error}
                </div>
              )}

              {rows.length === 0 ? (
                <EmptyState
                  icon={<Crosshair size={30} />}
                  title={onlyTargets ? "No runtime targets found" : "No matching processes"}
                  hint={
                    onlyTargets
                      ? "No process currently exposes a Hexforge pipe. Inject the runtime, or turn off 'runtime only' to browse all processes."
                      : "Adjust your search."
                  }
                />
              ) : (
                <div className="table-wrap" style={{ maxHeight: 440, overflowY: "auto" }}>
                  <table className="data">
                    <thead>
                      <tr>
                        <th>Process</th>
                        <th style={{ textAlign: "right" }}>PID</th>
                        <th>Runtime</th>
                        <th style={{ width: 1 }} />
                      </tr>
                    </thead>
                    <tbody>
                      {rows.map((p) => (
                        <tr key={p.pid}>
                          <td className="mono">{p.name}</td>
                          <td className="num">{p.pid}</td>
                          <td>
                            {p.attachable ? <Badge tone="ok">detected</Badge> : <Badge tone="muted">none</Badge>}
                          </td>
                          <td>
                            <button
                              className={"btn btn-sm" + (p.attachable ? " btn-primary" : "")}
                              disabled={s.connecting}
                              onClick={() => void doConnect(p.pid)}
                            >
                              <PlugZap size={13} /> Connect
                            </button>
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}

              <div className="faint" style={{ fontSize: 11.5, marginTop: 10 }}>
                {s.processes.length} processes · {targetCount} with runtime detected
              </div>
            </Card>

            <Card icon={<Info size={16} />} title="About">
              <div className="col" style={{ gap: 14 }}>
                <BrandLockup size={34} />
                <p className="muted" style={{ fontSize: 13, lineHeight: 1.6 }}>
                  {BRAND.product} is the control console for the {BRAND.name} universal mod framework. It speaks
                  JSON-RPC 2.0 to the injected runtime over a per-process named pipe to inspect hooks, mods and
                  mitigations, stream logs, and evaluate sandboxed Lua.
                </p>
                <div className="stat-row">
                  <span className="k">Transport</span>
                  <span className="v">named pipe · JSON-RPC 2.0</span>
                </div>
                <div className="stat-row">
                  <span className="k">Pipe</span>
                  <span className="v">{"\\\\.\\pipe\\umf-studio-<pid>"}</span>
                </div>
                <div className="stat-row">
                  <span className="k">Version</span>
                  <span className="v">{BRAND.version}</span>
                </div>
              </div>
            </Card>
          </div>
        )}
      </div>
    </div>
  );
}
