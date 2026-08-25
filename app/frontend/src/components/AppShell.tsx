import type { ReactNode } from "react";
import { useNavigation } from "../navigation/NavigationContext";
import styles from "./AppShell.module.css";

// Replaces the old inline <nav> two-tab switch. A slim top bar only -- no sidebar
// (idealist.md: "no sidebars, no useless crap"). The active screen is whatever
// NavigationContext says; App.tsx decides which page component that maps to.
export default function AppShell({ children }: { children: ReactNode }) {
  const { screen, navigate } = useNavigation();

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
      <main className={styles.content}>{children}</main>
    </div>
  );
}
