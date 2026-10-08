import { BRAND } from "../brand";
import { useStudio } from "../state/store";

export function StatusBar() {
  const s = useStudio();
  return (
    <footer className="statusbar">
      <div className="sb-item">
        <span className={"led " + (s.connecting ? "busy" : s.connected ? "ok" : "idle")} />
        <span>{s.connecting ? "connecting…" : s.connected ? "Connected" : "Disconnected"}</span>
      </div>
      <div className="sb-sep" />
      <div className="sb-item">
        pipe&nbsp;<span className="mono">{s.pipe ?? "—"}</span>
      </div>
      <div className="sb-sep" />
      <div className="sb-item">
        pid&nbsp;<span className="mono">{s.pid ?? "—"}</span>
      </div>
      <div className="sb-sep" />
      <div className="sb-item accent">{s.hooks.length} hooks</div>
      <div className="sb-item accent">{s.mods.length} mods</div>
      <div className="sb-grow" />
      {s.error && (
        <>
          <div className="sb-item" style={{ color: "var(--err)" }}>
            ⚠ {s.error}
          </div>
          <div className="sb-sep" />
        </>
      )}
      <div className="sb-item brandline">
        {BRAND.product} · v{BRAND.version}
      </div>
    </footer>
  );
}
