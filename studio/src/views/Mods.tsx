import { useMemo, useState } from "react";
import { Boxes, RefreshCw, Search } from "lucide-react";
import { useStudio } from "../state/store";
import { Badge, EmptyState } from "../components/ui";
import { capList, modTypeLabel } from "../ipc/types";

export function Mods() {
  const s = useStudio();
  const [q, setQ] = useState("");

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase();
    if (!needle) return s.mods;
    return s.mods.filter((m) => m.name.toLowerCase().includes(needle));
  }, [s.mods, q]);

  return (
    <div className="view fade-in">
      <div className="view-head">
        <span className="vh-icon">
          <Boxes size={20} />
        </span>
        <div>
          <div className="view-title">Mods</div>
          <div className="view-sub">Loaded mods, capabilities &amp; metadata</div>
        </div>
        <span className="spacer" />
        <button className="btn btn-sm" disabled={!s.connected || s.busy} onClick={() => void s.refreshMods()}>
          <RefreshCw size={14} className={s.busy ? "spin" : undefined} />
          Refresh
        </button>
      </div>

      <div className="view-body">
        <div className="toolbar">
          <div className="search">
            <Search size={15} />
            <input
              className="input"
              placeholder="Filter by name…"
              value={q}
              onChange={(e) => setQ(e.target.value)}
            />
          </div>
          <span className="spacer" />
          <Badge tone="accent">{rows.length} shown</Badge>
        </div>

        {rows.length === 0 ? (
          <EmptyState icon={<Boxes size={30} />} title="No mods" hint="No loaded mods match." />
        ) : (
          <div className="table-wrap">
            <table className="data">
              <thead>
                <tr>
                  <th>Mod</th>
                  <th>Version</th>
                  <th>Type</th>
                  <th>Capabilities</th>
                  <th style={{ textAlign: "right" }}>Priority</th>
                  <th>Category</th>
                  <th>State</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((m, i) => (
                  <tr key={m.name + i}>
                    <td className="mono">{m.name}</td>
                    <td className="mono">{m.version}</td>
                    <td>
                      <Badge tone={m.type === 1 ? "accent" : "info"}>{modTypeLabel(m.type)}</Badge>
                    </td>
                    <td>
                      <div className="chips">
                        {capList(m.caps).map((c) => (
                          <span className="chip on" key={c}>
                            {c}
                          </span>
                        ))}
                        {capList(m.caps).length === 0 && <span className="chip">none</span>}
                      </div>
                    </td>
                    <td className="num">{m.priority}</td>
                    <td>
                      <span className="faint" style={{ fontSize: 12 }}>
                        {m.category} · {m.audience}
                      </span>
                    </td>
                    <td>{m.active ? <Badge tone="ok">active</Badge> : <Badge tone="muted">idle</Badge>}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </div>
  );
}
