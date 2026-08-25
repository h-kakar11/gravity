import type { ReactNode } from "react";
import styles from "./ProLockedBadge.module.css";

// Marks an option that belongs to the future "Pro" tier (idealist.md: lossless/LZMA2
// compression, WebP/AVIF high-efficiency images, batch resize+convert, watermark overlay,
// video trim before download). Visibly present but inert -- no backend call is ever wired
// to a control wrapped this way, and no "coming soon" copy that implies it half-works.
export default function ProLockedBadge() {
  return (
    <span className={styles.badge} aria-hidden="true">
      Pro
    </span>
  );
}

// Wraps a control (e.g. a <select> option row, a checkbox) so it renders visibly but is
// non-interactive, with the badge alongside it.
export function ProLockedControl({ children, label }: { children: ReactNode; label: string }) {
  return (
    <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
      <div className={styles.disabledControl} aria-disabled="true" title={`${label} (Pro)`}>
        {children}
      </div>
      <ProLockedBadge />
    </div>
  );
}
