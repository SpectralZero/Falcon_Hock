import { useMemo, useState } from "react";
import { RefreshCw, Search, Webhook } from "lucide-react";
import { useStudio } from "../state/store";
import { Badge, EmptyState } from "../components/ui";
import { strategyLabel, strategyTone } from "../ipc/types";

export function Hooks() {
  const s = useStudio();
  const [q, setQ] = useState("");

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase();
    if (!needle) return s.hooks;
    return s.hooks.filter((h) => h.name.toLowerCase().includes(needle));
  }, [s.hooks, q]);

  return (
    <div className="view fade-in">
      <div className="view-head">
        <span className="vh-icon">
          <Webhook size={20} />
        </span>
        <div>
          <div className="view-title">Hooks</div>
          <div className="view-sub">Installed hook targets &amp; strategies</div>
        </div>
        <span className="spacer" />
        <button className="btn btn-sm" disabled={!s.connected || s.busy} onClick={() => void s.refreshHooks()}>
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
          <EmptyState icon={<Webhook size={30} />} title="No hooks" hint="No installed hook targets match." />
        ) : (
          <div className="table-wrap">
            <table className="data">
              <thead>
                <tr>
                  <th>Target</th>
                  <th>Address</th>
                  <th>Strategy</th>
                  <th style={{ textAlign: "right" }}>Chain</th>
                  <th>State</th>
                </tr>
              </thead>
              <tbody>
                {rows.map((h, i) => (
                  <tr key={h.name + i}>
                    <td className="mono">{h.name}</td>
                    <td className="mono t-addr">{h.address}</td>
                    <td>
                      <Badge tone={strategyTone(h.strategy)}>{strategyLabel(h.strategy)}</Badge>
                    </td>
                    <td className="num">{h.chain}</td>
                    <td>
                      {h.installed ? <Badge tone="ok">installed</Badge> : <Badge tone="warn">pending</Badge>}
                    </td>
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
