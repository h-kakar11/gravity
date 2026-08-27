import { type ReactNode, useEffect, useState } from "react";
import { useNavigation } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import styles from "./AppShell.module.css";

// Replaces the old inline <nav> two-tab switch. A slim top bar only -- no sidebar
// (idealist.md: "no sidebars, no useless crap"). The active screen is whatever
// NavigationContext says; App.tsx decides which page component that maps to.
export default function AppShell({ children }: { children: ReactNode }) {
  const { screen, navigate } = useNavigation();
  // Mounted once at the top level (like the hotkey/CLI subscriptions in App.tsx) so the
  // banner shows no matter which screen the user is on -- see issue #23/#51.
  const [coreUnavailable, setCoreUnavailable] = useState(false);
  useEffect(() => coreClient.subscribeToCoreUnavailable(() => setCoreUnavailable(true)), []);

  const navItem = (kind: "queue" | "scheduledTasks" | "settings", label: string) => (
    <button
      className={`${styles.navButton} ${screen.kind === kind ? styles.navButtonActive : ""}`}
      onClick={() => navigate({ kind })}
    >
      {label}
    </button>
  );

  return (
    <div className={styles.shell}>
      <header className={styles.topBar}>
        <button className={styles.brand} onClick={() => navigate({ kind: "home" })} aria-label="Gravity home">
          <span className={styles.brandDot} aria-hidden="true" />
          Gravity
        </button>
        <nav className={styles.navGroup}>
          {navItem("queue", "Queue")}
          {navItem("scheduledTasks", "Scheduled")}
          {navItem("settings", "Settings")}
        </nav>
      </header>
      {coreUnavailable && (
        <div className={styles.coreUnavailableBanner} role="alert">
          Gravity's background engine stopped responding. Downloads and conversions won't
          work until you restart the app.
        </div>
      )}
      <main className={styles.content}>{children}</main>
    </div>
  );
}
