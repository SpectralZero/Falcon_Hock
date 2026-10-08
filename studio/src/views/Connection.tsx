import { useState } from "react";
import { Info, Plug, PlugZap, Power } from "lucide-react";
import { BRAND } from "../brand";
import { useStudio } from "../state/store";
import { Badge, Card } from "../components/ui";
import { BrandLockup } from "../components/Logo";

export function Connection() {
  const s = useStudio();
  const [pidStr, setPidStr] = useState(String(s.pid ?? ""));

  const pidNum = parseInt(pidStr, 10);
  const valid = Number.isFinite(pidNum) && pidNum > 0;
  const preview = valid ? `\\\\.\\pipe\\umf-studio-${pidNum}` : "\\\\.\\pipe\\umf-studio-<pid>";

  return (
    <div className="view fade-in">
      <div className="view-head">
        <span className="vh-icon">
          <PlugZap size={20} />
        </span>
        <div>
          <div className="view-title">Connection</div>
          <div className="view-sub">Attach to a Hexforge runtime over its named pipe</div>
        </div>
        <span className="spacer" />
        <Badge tone={s.kind === "tauri" ? "ok" : "info"}>{s.kind === "tauri" ? "LIVE BRIDGE" : "DEMO (mock)"}</Badge>
      </div>

      <div className="view-body">
        <div className="grid grid-2">
          <Card icon={<Plug size={16} />} title="Attach to process">
            <div className="col" style={{ gap: 14 }}>
              <div className="field">
                <label>Target process ID</label>
                <input
                  className="input mono"
                  placeholder="e.g. 13337"
                  value={pidStr}
                  onChange={(e) => setPidStr(e.target.value.replace(/[^0-9]/g, ""))}
                  disabled={s.connected}
                />
              </div>

              <div className="field">
                <label>Pipe endpoint</label>
                <div className="input mono" style={{ color: "var(--accent-2)", userSelect: "all" }}>
                  {preview}
                </div>
              </div>

              <div className="row">
                {!s.connected ? (
                  <button
                    className="btn btn-primary"
                    disabled={!valid || s.connecting}
                    onClick={() => void s.connect(pidNum)}
                  >
                    <PlugZap size={16} />
                    {s.connecting ? "Connecting…" : "Connect"}
                  </button>
                ) : (
                  <button className="btn btn-danger" onClick={() => void s.disconnect()}>
                    <Power size={16} /> Disconnect
                  </button>
                )}
                <span className="row gap-6">
                  <span className={"led " + (s.connecting ? "busy" : s.connected ? "ok" : "idle")} />
                  <span className="muted" style={{ fontSize: 12.5 }}>
                    {s.connecting ? "connecting…" : s.connected ? "connected" : "not connected"}
                  </span>
                </span>
              </div>

              {s.error && (
                <div className="badge err" style={{ alignSelf: "flex-start" }}>
                  {s.error}
                </div>
              )}
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
                <span className="k">Mode</span>
                <span className="v">{s.kind === "tauri" ? "native bridge" : "in-browser mock"}</span>
              </div>
              <div className="stat-row">
                <span className="k">Version</span>
                <span className="v">{BRAND.version}</span>
              </div>
              {s.kind === "mock" && (
                <p className="faint" style={{ fontSize: 12 }}>
                  Running outside Tauri, so a mock runtime feeds representative data and a live log stream.
                  Launch with <span className="kbd">npm run tauri dev</span> for the real named-pipe bridge.
                </p>
              )}
            </div>
          </Card>
        </div>
      </div>
    </div>
  );
}
