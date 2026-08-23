import { useState } from "react";
import DevConsole from "./pages/DevConsole";
import DownloaderPage from "./pages/DownloaderPage";

type Tab = "download" | "devConsole";

// Minimal tab switch, not a router -- Phase 2 adds a second functional page alongside
// Phase 1's IPC-proving dev console (spec section 34: basic styling is fine, this is not
// the final UI).
export default function App() {
  const [tab, setTab] = useState<Tab>("download");

  return (
    <div>
      <nav style={{ display: "flex", gap: "0.5rem", padding: "0.75rem 1.5rem", borderBottom: "1px solid #ddd" }}>
        <button onClick={() => setTab("download")} disabled={tab === "download"}>
          Download
        </button>
        <button onClick={() => setTab("devConsole")} disabled={tab === "devConsole"}>
          Dev Console
        </button>
      </nav>
      {tab === "download" ? <DownloaderPage /> : <DevConsole />}
    </div>
  );
}
