import AppShell from "./components/AppShell";
import { NavigationProvider, useNavigation } from "./navigation/NavigationContext";
import ConvertPage from "./pages/ConvertPage";
import DevConsole from "./pages/DevConsole";
import DownloaderPage from "./pages/DownloaderPage";
import HomePage from "./pages/HomePage";
import QueuePage from "./pages/QueuePage";
import SettingsPage from "./pages/SettingsPage";

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
