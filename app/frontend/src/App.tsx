import AppShell from "./components/AppShell";
import { NavigationProvider, useNavigation } from "./navigation/NavigationContext";
import DevConsole from "./pages/DevConsole";
import DownloaderPage from "./pages/DownloaderPage";
import HomePage from "./pages/HomePage";
import QueuePage from "./pages/QueuePage";
import SettingsPage from "./pages/SettingsPage";
// ConvertPage lands with Phase 2.6's real Convert/Compress engine.

function Screens() {
  const { screen } = useNavigation();

  switch (screen.kind) {
    case "home":
      return <HomePage />;
    case "download":
      return <DownloaderPage />;
    case "queue":
      return <QueuePage />;
    case "settings":
      return <SettingsPage />;
    case "convert":
      // Placeholder until the real Convert/Compress engine + page land.
      return (
        <div style={{ padding: 48, textAlign: "center", color: "var(--color-text-secondary)" }}>
          <h2 style={{ color: "var(--color-text-primary)" }}>Convert &amp; Compress</h2>
          <p>Coming soon.</p>
        </div>
      );
  }
}

// The DevConsole (IPC-proving diagnostic tool from Phase 1) is a dev-only surface now,
// not a visible tab -- it's not part of the product, per idealist.md's "no useless crap."
// Reached via ?devConsole=1 during `npm run dev` only.
function isDevConsoleRequested(): boolean {
  if (!import.meta.env.DEV) return false;
  return new URLSearchParams(window.location.search).has("devConsole");
}

export default function App() {
  if (isDevConsoleRequested()) {
    return <DevConsole />;
  }

  return (
    <NavigationProvider>
      <AppShell>
        <Screens />
      </AppShell>
    </NavigationProvider>
  );
}
