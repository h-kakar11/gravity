import { useEffect } from "react";
import AppShell from "./components/AppShell";
import { useNotifications } from "./hooks/useNotifications";
import { NavigationProvider, useNavigation } from "./navigation/NavigationContext";
import ConvertPage from "./pages/ConvertPage";
import DevConsole from "./pages/DevConsole";
import DownloaderPage from "./pages/DownloaderPage";
import HomePage from "./pages/HomePage";
import QueuePage from "./pages/QueuePage";
import SettingsPage from "./pages/SettingsPage";
import * as coreClient from "./services/coreClient";

function Screens() {
  const { screen, navigate } = useNavigation();

  // Global hotkeys (Phase 4.4) fire from Rust regardless of which screen is showing --
  // paste-and-download jumps to the Download screen prefilled with the clipboard URL,
  // focus-queue jumps to the Queue screen. Subscribed once at the top level rather than
  // per-page so a hotkey works no matter where the user currently is.
  useEffect(() => {
    return coreClient.subscribeToHotkeyEvents({
      onPasteAndDownload: (url) => navigate({ kind: "download", prefillUrl: url }),
      onFocusQueue: () => navigate({ kind: "queue" }),
    });
  }, [navigate]);

  // Toast notifications (Phase 4.5), same "fires regardless of active screen" reasoning.
  useNotifications();

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
      return <ConvertPage />;
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
