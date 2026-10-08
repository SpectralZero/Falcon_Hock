import { Sidebar } from "./components/Sidebar";
import { StatusBar } from "./components/StatusBar";
import { WindowChrome } from "./components/WindowChrome";
import { useStudio } from "./state/store";
import { Connection } from "./views/Connection";
import { Dashboard } from "./views/Dashboard";
import { Hooks } from "./views/Hooks";
import { Logs } from "./views/Logs";
import { LuaConsole } from "./views/LuaConsole";
import { Mods } from "./views/Mods";

export default function App() {
  const { view } = useStudio();
  return (
    <div className="app">
      <WindowChrome />
      <div className="app-body">
        <Sidebar />
        <main className="main">
          {view === "dashboard" && <Dashboard />}
          {view === "hooks" && <Hooks />}
          {view === "mods" && <Mods />}
          {view === "lua" && <LuaConsole />}
          {view === "logs" && <Logs />}
          {view === "connection" && <Connection />}
        </main>
      </div>
      <StatusBar />
    </div>
  );
}
