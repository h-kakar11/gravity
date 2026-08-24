import { useEffect, useState } from "react";
import { AppShell, type Route } from "./components/AppShell";
import { ToastProvider } from "./components/ui/ToastProvider";
import * as coreClient from "./services/coreClient";
import DevConsole from "./pages/DevConsole";
import DownloaderPage from "./pages/DownloaderPage";
import HomePage from "./pages/HomePage";
import ProcessPage from "./pages/ProcessPage";
import QueuePage from "./pages/QueuePage";
import SettingsPage from "./pages/SettingsPage";
import { useQueue } from "./state/useQueue";
import { useQueueNotifications } from "./state/useQueueNotifications";

// One queue store for the whole app (spec section 20, "duplicated state" audit finding):
// every screen that needs job data reads this same instance rather than opening its own
// subscription, so there is exactly one place that can disagree with the backend about what
// a job's state is.
function AppContent() {
  const [route, setRoute] = useState<Route>("home");
  const queue = useQueue();

  // Settings' general.showNotifications is the one field this page reads: it gates whether
  // useQueueNotifications is allowed to push toasts at all (spec section 15). Loaded once
  // and kept current locally when Settings saves a change, rather than re-fetched on every
  // render.
  const [notificationsEnabled, setNotificationsEnabled] = useState(true);
  useEffect(() => {
    let active = true;
    coreClient
      .getSettings()
      .then(({ settings }) => {
        if (active) setNotificationsEnabled(settings.general.showNotifications);
      })
      .catch(() => {
        /* Default (enabled) stands if settings can't be read yet. */
      });
    return () => {
      active = false;
    };
  }, []);

  useQueueNotifications(queue.state, notificationsEnabled);

  const activeCount = queue.state.statistics.running + queue.state.statistics.retryWait;

  return (
    <AppShell route={route} onNavigate={setRoute} queuedBadge={activeCount}>
      {route === "home" ? <HomePage queue={queue} onNavigate={setRoute} /> : null}
      {route === "download" ? <DownloaderPage queue={queue} onNavigate={setRoute} /> : null}
      {route === "process" ? <ProcessPage /> : null}
      {route === "queue" ? <QueuePage queue={queue} /> : null}
      {route === "settings" ? (
        <SettingsPage onNavigate={setRoute} onNotificationsEnabledChange={setNotificationsEnabled} />
      ) : null}
      {route === "devConsole" ? <DevConsole queue={queue} /> : null}
    </AppShell>
  );
}

export default function App() {
  return (
    <ToastProvider>
      <AppContent />
    </ToastProvider>
  );
}
