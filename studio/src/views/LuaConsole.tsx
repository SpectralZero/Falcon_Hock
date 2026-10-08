import { useRef, useState } from "react";
import { Play, SquareTerminal, Trash2 } from "lucide-react";
import { useStudio } from "../state/store";
import { Badge, Card } from "../components/ui";

const SAMPLE = `-- Hexforge sandboxed Lua (os/io/ffi removed, 100M instr budget)
local addr = umf.resolve("kernel32.dll", "GetProcAddress")
umf.log("GetProcAddress @ " .. tostring(addr))

for i = 1, 3 do
  umf.log("tick " .. i)
end
`;

interface OutLine {
  id: number;
  ok: boolean;
  text: string;
  time: string;
}

export function LuaConsole() {
  const s = useStudio();
  const [code, setCode] = useState(SAMPLE);
  const [out, setOut] = useState<OutLine[]>([]);
  const [running, setRunning] = useState(false);
  const idc = useRef(0);

  const run = async () => {
    if (!s.connected || running) return;
    setRunning(true);
    const ok = await s.evalLua(code);
    setOut((prev) => [
      ...prev,
      {
        id: idc.current++,
        ok,
        text: ok ? "chunk compiled and ran successfully" : "chunk failed to compile or raised an error",
        time: new Date().toLocaleTimeString("en-GB", { hour12: false }),
      },
    ]);
    setRunning(false);
  };

  return (
    <div className="view fade-in">
      <div className="view-head">
        <span className="vh-icon">
          <SquareTerminal size={20} />
        </span>
        <div>
          <div className="view-title">Lua Console</div>
          <div className="view-sub">Evaluate Lua in a fresh sandboxed state (evalLua)</div>
        </div>
        <span className="spacer" />
        <span className="faint" style={{ fontSize: 12 }}>
          <span className="kbd">Ctrl</span> + <span className="kbd">Enter</span>
        </span>
      </div>

      <div className="view-body">
        <Card
          icon={<SquareTerminal size={16} />}
          title="Sandbox"
          actions={
            <div className="row">
              <button className="btn btn-sm" onClick={() => setOut([])} disabled={out.length === 0}>
                <Trash2 size={14} /> Clear
              </button>
              <button className="btn btn-primary btn-sm" onClick={() => void run()} disabled={!s.connected || running}>
                <Play size={14} /> {running ? "Running…" : "Run"}
              </button>
            </div>
          }
        >
          {!s.connected && (
            <div className="muted" style={{ marginBottom: 10, fontSize: 12.5 }}>
              Connect to a target to evaluate Lua.
            </div>
          )}
          <textarea
            className="editor"
            spellCheck={false}
            value={code}
            onChange={(e) => setCode(e.target.value)}
            onKeyDown={(e) => {
              if (e.ctrlKey && e.key === "Enter") {
                e.preventDefault();
                void run();
              }
            }}
          />
        </Card>

        <div className="mt-16">
          <Card title="Output">
            {out.length === 0 ? (
              <div className="faint" style={{ fontSize: 12.5 }}>No evaluations yet.</div>
            ) : (
              <div className="col" style={{ gap: 8 }}>
                {out.map((o) => (
                  <div key={o.id} className="row" style={{ gap: 10 }}>
                    <span className="mono faint" style={{ fontSize: 11 }}>
                      {o.time}
                    </span>
                    {o.ok ? <Badge tone="ok">ok</Badge> : <Badge tone="err">error</Badge>}
                    <span style={{ fontSize: 12.5 }}>{o.text}</span>
                  </div>
                ))}
              </div>
            )}
          </Card>
        </div>
      </div>
    </div>
  );
}
