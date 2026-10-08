import { useEffect, useMemo, useRef, useState } from "react";
import { Pause, Play, ScrollText, Search, Trash2 } from "lucide-react";
import { useStudio } from "../state/store";
import { LOG_LEVELS } from "../ipc/types";
import { EmptyState } from "../components/ui";

export function Logs() {
  const s = useStudio();
  const [levels, setLevels] = useState<Set<number>>(new Set([0, 1, 2, 3, 4, 5]));
  const [q, setQ] = useState("");
  const [paused, setPaused] = useState(false);
  const consoleRef = useRef<HTMLDivElement>(null);

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase();
    return s.logs.filter(
      (l) => levels.has(l.level) && (!needle || l.msg.toLowerCase().includes(needle)),
    );
  }, [s.logs, levels, q]);

  useEffect(() => {
    if (!paused && consoleRef.current) {
      consoleRef.current.scrollTop = consoleRef.current.scrollHeight;
    }
  }, [rows, paused]);

  const toggleLevel = (lvl: number) => {
    setLevels((prev) => {
      const next = new Set(prev);
      if (next.has(lvl)) next.delete(lvl);
      else next.add(lvl);
      return next;
    });
  };

  return (
    <div className="view fade-in">
      <div className="view-head">
        <span className="vh-icon">
          <ScrollText size={20} />
        </span>
        <div>
          <div className="view-title">Logs</div>
          <div className="view-sub">Live runtime log stream (subscribe)</div>
        </div>
        <span className="spacer" />
        <button className="btn btn-sm" onClick={() => setPaused((p) => !p)}>
          {paused ? <Play size={14} /> : <Pause size={14} />}
          {paused ? "Resume" : "Pause"}
        </button>
        <button className="btn btn-sm" onClick={() => s.clearLogs()} disabled={s.logs.length === 0}>
          <Trash2 size={14} /> Clear
        </button>
      </div>

      <div className="view-body" style={{ display: "flex", flexDirection: "column", minHeight: 0 }}>
        <div className="toolbar">
          <div className="search">
            <Search size={15} />
            <input className="input" placeholder="Filter messages…" value={q} onChange={(e) => setQ(e.target.value)} />
          </div>
          <span className="spacer" />
          <div className="chips">
            {LOG_LEVELS.map((name, lvl) => (
              <span
                key={name}
                className={"chip" + (levels.has(lvl) ? " on" : "")}
                style={{ cursor: "pointer", opacity: levels.has(lvl) ? 1 : 0.45 }}
                onClick={() => toggleLevel(lvl)}
              >
                {name}
              </span>
            ))}
          </div>
        </div>

        {rows.length === 0 ? (
          <EmptyState icon={<ScrollText size={30} />} title="No log lines" hint="Waiting for runtime events…" />
        ) : (
          <div className="console" ref={consoleRef}>
            {rows.map((l) => (
              <div key={l.id} className={"log-line lvl-" + l.levelName}>
                <span className="log-time">{l.time}</span>
                <span className="log-lvl">{l.levelName}</span>
                <span className="log-msg">{l.msg}</span>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}
