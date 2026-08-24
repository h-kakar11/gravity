// The application shell (spec section 4): a persistent sidebar naming Gravity's actual
// product concepts, consistent across every screen, with a single clear active-page
// indicator. Tabs are plain buttons in normal DOM order, so keyboard Tab / Shift+Tab and
// screen readers get a sane, native experience for free -- no roving tabindex machinery is
// needed for five top-level destinations.

import type { ReactNode } from "react";
import {
  DownloadIcon,
  ConvertIcon,
  HomeIcon,
  QueueIcon,
  SettingsIcon,
} from "./icons";

// "devConsole" is intentionally not one of NAV_ITEMS below -- it is a diagnostics screen
// reachable only from Settings > Developer, not a primary product destination (spec section
// 4: "do not create navigation entries merely to fill space").
export type Route = "home" | "download" | "process" | "queue" | "settings" | "devConsole";

const NAV_ITEMS: { id: Route; label: string; icon: React.ComponentType<{ size?: number }> }[] = [
  { id: "home", label: "Home", icon: HomeIcon },
  { id: "download", label: "Download", icon: DownloadIcon },
  { id: "process", label: "Convert & Compress", icon: ConvertIcon },
  { id: "queue", label: "Queue", icon: QueueIcon },
  { id: "settings", label: "Settings", icon: SettingsIcon },
];

interface AppShellProps {
  route: Route;
  onNavigate: (route: Route) => void;
  queuedBadge?: number;
  children: ReactNode;
}

export function AppShell({ route, onNavigate, queuedBadge, children }: AppShellProps) {
  return (
    <div className="gv-app">
      <a href="#gv-main-content" className="sr-only">
        Skip to main content
      </a>
      <aside className="gv-sidebar" aria-label="Primary">
        <div className="gv-brand">
          <span className="gv-brand__mark" aria-hidden="true" />
          Gravity
        </div>
        <nav className="gv-nav" aria-label="Sections">
          {NAV_ITEMS.map(({ id, label, icon: Glyph }) => (
            <button
              key={id}
              type="button"
              className="gv-nav__item"
              aria-current={route === id ? "page" : undefined}
              onClick={() => onNavigate(id)}
            >
              <Glyph size={17} />
              <span>{label}</span>
              {id === "queue" && queuedBadge ? (
                <span
                  className="gv-typebadge"
                  style={{ marginLeft: "auto" }}
                  aria-label={`${queuedBadge} active jobs`}
                >
                  {queuedBadge}
                </span>
              ) : null}
            </button>
          ))}
        </nav>
        <div className="gv-sidebar__footer">Local-first media downloader &amp; converter</div>
      </aside>
      <main className="gv-main" id="gv-main-content">
        <div className="gv-main__inner">{children}</div>
      </main>
    </div>
  );
}
