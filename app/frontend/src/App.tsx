import { useState } from "react";
import DevConsole from "./pages/DevConsole";
import DownloaderPage from "./pages/DownloaderPage";
import ProcessPage from "./pages/ProcessPage";
import QueuePage from "./pages/QueuePage";

type Tab = "download" | "process" | "queue" | "devConsole";

const TABS: { id: Tab; label: string }[] = [
  { id: "download", label: "Download" },
  { id: "process", label: "Convert & Compress" },
  { id: "queue", label: "Queue" },
  { id: "devConsole", label: "Dev Console" },
];

// Minimal tab switch, not a router. Basic styling is fine here -- this is not the final UI
// (docs/roadmap.md "UI").
export default function App() {
  const [tab, setTab] = useState<Tab>("queue");

  return (
    <div>
      <nav
        style={{
          display: "flex",
          gap: "0.5rem",
          padding: "0.75rem 1.5rem",
          borderBottom: "1px solid #ddd",
        }}
        aria-label="Sections"
      >
        {TABS.map(({ id, label }) => (
          <button
            key={id}
            type="button"
            onClick={() => setTab(id)}
            aria-current={tab === id ? "page" : undefined}
            // Disabling the current tab would make it unreachable by keyboard, so the
            // selected one stays focusable and is marked with aria-current instead.
            style={{
              padding: "0.35rem 0.8rem",
              borderRadius: 6,
              border: tab === id ? "1px solid #2563eb" : "1px solid #ddd",
              background: tab === id ? "#eff6ff" : "#fff",
              fontWeight: tab === id ? 600 : 400,
              cursor: "pointer",
            }}
          >
            {label}
          </button>
        ))}
      </nav>
      {tab === "download" ? <DownloaderPage /> : null}
      {tab === "process" ? <ProcessPage /> : null}
      {tab === "queue" ? <QueuePage /> : null}
      {tab === "devConsole" ? <DevConsole /> : null}
    </div>
  );
}
