import { Activity, Boxes, Cpu, LayoutDashboard, RefreshCw, Shield, Webhook } from "lucide-react";
import { useStudio } from "../state/store";
import { Badge, Card } from "../components/ui";
import type { Mitigations } from "../ipc/types";

const MITS: { key: keyof Mitigations; name: string; desc: string }[] = [
  { key: "acg", name: "ACG", desc: "Arbitrary Code Guard" },
  { key: "hvci", name: "HVCI", desc: "Hypervisor Code Integrity" },
  { key: "cfg", name: "CFG", desc: "Control Flow Guard" },
  { key: "cet_ss", name: "CET SS", desc: "Shadow Stack" },
  { key: "cet_ibt", name: "CET IBT", desc: "Indirect Branch Tracking" },
];

export function Dashboard() {
  const s = useStudio();

  return (
    <div className="view fade-in">
      <div className="view-head">
        <span className="vh-icon">
          <LayoutDashboard size={20} />
        </span>
        <div>
          <div className="view-title">Dashboard</div>
          <div className="view-sub">Runtime overview &amp; target posture</div>
        </div>
        <span className="spacer" />
        <button className="btn btn-sm" disabled={!s.connected || s.busy} onClick={() => void s.refreshAll()}>
          <RefreshCw size={14} className={s.busy ? "spin" : undefined} />
          Refresh
        </button>
      </div>

      <div className="view-body">
        {!s.connected ? (
          <Card>
            <div className="empty">
              <Activity size={30} />
              <div style={{ fontWeight: 700, color: "var(--txt)" }}>Not connected</div>
              <div className="faint" style={{ maxWidth: 420 }}>
                Attach to a process running the Hexforge runtime to inspect hooks, mods, mitigations
                and the live log stream.
              </div>
              <button className="btn btn-primary mt-16" onClick={() => s.setView("connection")}>
                Open connection
              </button>
            </div>
          </Card>
        ) : (
          <div className="col" style={{ gap: 16 }}>
            <div className="grid grid-4">
              <Card>
                <div className="stat stat-big">
                  <span className="k">Process ID</span>
                  <span className="v">{s.process?.pid ?? "—"}</span>
                </div>
              </Card>
              <Card>
                <div className="stat stat-big">
                  <span className="k">Architecture</span>
                  <span className="v">{s.process?.arch ?? "—"}</span>
                </div>
              </Card>
              <Card>
                <div className="stat stat-big">
                  <span className="k">Hooks</span>
                  <span className="v">{s.hooks.length}</span>
                </div>
              </Card>
              <Card>
                <div className="stat stat-big">
                  <span className="k">Mods</span>
                  <span className="v">{s.mods.length}</span>
                </div>
              </Card>
            </div>

            <div className="grid grid-2">
              <Card icon={<Cpu size={16} />} title="Target process">
                <div className="stat" style={{ marginBottom: 10 }}>
                  <span className="k">Image path</span>
                  <span className="v" style={{ fontSize: 12.5 }}>{s.process?.path ?? "—"}</span>
                </div>
                <div className="stat-row">
                  <span className="k">PID</span>
                  <span className="v">{s.process?.pid ?? "—"}</span>
                </div>
                <div className="stat-row">
                  <span className="k">Architecture</span>
                  <span className="v">{s.process?.arch ?? "—"}</span>
                </div>
                <div className="stat-row">
                  <span className="k">Transport</span>
                  <span className="v">{s.pipe ?? "—"}</span>
                </div>
              </Card>

              <Card icon={<Shield size={16} />} title="Security mitigations">
                <div className="col" style={{ gap: 8 }}>
                  {MITS.map((m) => {
                    const on = !!s.mitigations?.[m.key];
                    return (
                      <div className="mit" key={m.key}>
                        <span className={"led " + (on ? "warn" : "idle")} />
                        <div>
                          <div className="mit-name">{m.name}</div>
                          <div className="mit-desc">{m.desc}</div>
                        </div>
                        <span className="spacer" />
                        {on ? <Badge tone="warn">enforced</Badge> : <Badge tone="muted">off</Badge>}
                      </div>
                    );
                  })}
                </div>
              </Card>
            </div>

            <div className="grid grid-2">
              <Card icon={<Webhook size={16} />} title="Hook summary">
                <div className="stat-row">
                  <span className="k">Installed targets</span>
                  <span className="v">{s.hooks.filter((h) => h.installed).length}</span>
                </div>
                <div className="stat-row">
                  <span className="k">Pending / detached</span>
                  <span className="v">{s.hooks.filter((h) => !h.installed).length}</span>
                </div>
                <div className="stat-row">
                  <span className="k">Total chains</span>
                  <span className="v">{s.hooks.reduce((a, h) => a + h.chain, 0)}</span>
                </div>
                <button className="btn btn-sm mt-16" onClick={() => s.setView("hooks")}>
                  Inspect hooks
                </button>
              </Card>

              <Card icon={<Boxes size={16} />} title="Mod summary">
                <div className="stat-row">
                  <span className="k">Loaded mods</span>
                  <span className="v">{s.mods.length}</span>
                </div>
                <div className="stat-row">
                  <span className="k">Active</span>
                  <span className="v">{s.mods.filter((m) => m.active).length}</span>
                </div>
                <div className="stat-row">
                  <span className="k">Lua / Native</span>
                  <span className="v">
                    {s.mods.filter((m) => m.type === 1).length} / {s.mods.filter((m) => m.type === 0).length}
                  </span>
                </div>
                <button className="btn btn-sm mt-16" onClick={() => s.setView("mods")}>
                  Manage mods
                </button>
              </Card>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}
