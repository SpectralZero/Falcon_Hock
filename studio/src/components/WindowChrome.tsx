import { getCurrentWindow } from "@tauri-apps/api/window";
import { Minus, Square, X } from "lucide-react";
import { isTauri } from "../ipc/client";
import { useStudio } from "../state/store";
import { BrandLockup } from "./Logo";

type Dir =
  | "North" | "South" | "East" | "West"
  | "NorthEast" | "NorthWest" | "SouthEast" | "SouthWest";

async function startResize(dir: Dir) {
  if (!isTauri()) return;
  const w = getCurrentWindow();
  await (w.startResizeDragging as unknown as (d: string) => Promise<void>)(dir);
}

function winCtl(action: "min" | "max" | "close") {
  if (!isTauri()) return;
  const w = getCurrentWindow();
  if (action === "min") void w.minimize();
  else if (action === "max") void w.toggleMaximize();
  else void w.close();
}

export function WindowChrome() {
  const { connected, kind, pid } = useStudio();
  return (
    <>
      <div className="chrome">
        <div className="chrome-left">
          <BrandLockup size={26} />
        </div>

        <div className="chrome-drag" data-tauri-drag-region />

        <div className="chrome-right">
          <span className="badge" style={{ gap: 7 }}>
            <span className={"led " + (connected ? "ok" : "idle")} />
            {connected ? `pid ${pid}` : "offline"}
          </span>
          <span className="badge muted">{kind === "tauri" ? "LIVE" : "DEMO"}</span>
          <div className="win-btns">
            <button className="win-btn" title="Minimize" onClick={() => winCtl("min")}>
              <Minus size={15} />
            </button>
            <button className="win-btn" title="Maximize" onClick={() => winCtl("max")}>
              <Square size={13} />
            </button>
            <button className="win-btn close" title="Close" onClick={() => winCtl("close")}>
              <X size={15} />
            </button>
          </div>
        </div>
      </div>

      {isTauri() && (
        <>
          <div className="rh rh-t" onMouseDown={() => startResize("North")} />
          <div className="rh rh-b" onMouseDown={() => startResize("South")} />
          <div className="rh rh-l" onMouseDown={() => startResize("West")} />
          <div className="rh rh-r" onMouseDown={() => startResize("East")} />
          <div className="rh rh-tl" onMouseDown={() => startResize("NorthWest")} />
          <div className="rh rh-tr" onMouseDown={() => startResize("NorthEast")} />
          <div className="rh rh-bl" onMouseDown={() => startResize("SouthWest")} />
          <div className="rh rh-br" onMouseDown={() => startResize("SouthEast")} />
        </>
      )}
    </>
  );
}
