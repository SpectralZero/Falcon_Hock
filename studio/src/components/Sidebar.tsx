import { Boxes, LayoutDashboard, PlugZap, ScrollText, SquareTerminal, Webhook, type LucideIcon } from "lucide-react";
import { BRAND } from "../brand";
import { useStudio, type View } from "../state/store";
import { BrandLockup } from "./Logo";

const NAV: { id: View; label: string; Icon: LucideIcon; countKey?: "hooks" | "mods" | "logs" }[] = [
  { id: "dashboard", label: "Dashboard", Icon: LayoutDashboard },
  { id: "hooks", label: "Hooks", Icon: Webhook, countKey: "hooks" },
  { id: "mods", label: "Mods", Icon: Boxes, countKey: "mods" },
  { id: "lua", label: "Lua Console", Icon: SquareTerminal },
  { id: "logs", label: "Logs", Icon: ScrollText, countKey: "logs" },
  { id: "connection", label: "Connection", Icon: PlugZap },
];

export function Sidebar() {
  const s = useStudio();
  const counts = { hooks: s.hooks.length, mods: s.mods.length, logs: s.logs.length };

  return (
    <aside className="sidebar">
      <div className="sidebar-brand">
        <BrandLockup size={30} />
        <div className="sidebar-tagline">{BRAND.tagline}</div>
      </div>

      <nav className="nav">
        <div className="nav-label">Workspace</div>
        {NAV.map(({ id, label, Icon, countKey }) => {
          const n = countKey ? counts[countKey] : 0;
          return (
            <div
              key={id}
              className={"nav-item" + (s.view === id ? " active" : "")}
              onClick={() => s.setView(id)}
            >
              <Icon size={17} />
              <span>{label}</span>
              {n > 0 && <span className="nav-badge">{n}</span>}
            </div>
          );
        })}
      </nav>

      <div className="sidebar-foot">
        <div className="conn-mini">
          <div className="row">
            <span className="lbl">Status</span>
            <span className="row gap-6">
              <span className={"led " + (s.connecting ? "busy" : s.connected ? "ok" : "idle")} />
              <span className="val">{s.connecting ? "connecting" : s.connected ? "online" : "offline"}</span>
            </span>
          </div>
          <div className="row">
            <span className="lbl">PID</span>
            <span className="val">{s.pid ?? "—"}</span>
          </div>
          <div className="row">
            <span className="lbl">Arch</span>
            <span className="val">{s.process?.arch ?? "—"}</span>
          </div>
        </div>
      </div>
    </aside>
  );
}
