import { useEffect } from "react";
import AppShell from "./components/AppShell";
import { ThemeProvider } from "./context/ThemeContext";
import { useNotifications } from "./hooks/useNotifications";
import { NavigationProvider, useNavigation } from "./navigation/NavigationContext";
import ConvertPage from "./pages/ConvertPage";
import DevConsole from "./pages/DevConsole";
import DownloaderPage from "./pages/DownloaderPage";
import HomePage from "./pages/HomePage";
import QueuePage from "./pages/QueuePage";
import ScheduledTasksPage from "./pages/ScheduledTasksPage";
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

  // Windows context menu CLI contract (Phase 5.3): a cold start with --convert/--compress
  // "<path>" (checked once, after mount, so the Rust side has had a chance to stash it --
  // see cli.rs's module doc for why this is a command instead of an event for this case),
  // and a second launch redirected here while already running (a live event instead, since
  // this component -- and therefore its listener -- is already mounted by then).
  useEffect(() => {
    coreClient.getStartupFileAction().then((action) => {
      if (action) navigate({ kind: "convert", prefillFilePath: action.path, mode: action.mode });
    }).catch(() => {});

    return coreClient.subscribeToCliFileOpened((action) => {
      navigate({ kind: "convert", prefillFilePath: action.path, mode: action.mode });
    });
  }, [navigate]);

  switch (screen.kind) {
    case "home":
      return <HomePage />;
    case "download":
      return <DownloaderPage />;
    case "queue":
      return <QueuePage />;
    case "settings":
      return <SettingsPage />;
    case "scheduledTasks":
      return <ScheduledTasksPage />;
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
    <ThemeProvider>
      <NavigationProvider>
        <AppShell>
          <Screens />
        </AppShell>
      </NavigationProvider>
    </ThemeProvider>
  );
}
