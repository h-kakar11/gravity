// Shared inline styles for the queue screen. The project styles with inline `CSSProperties`
// objects rather than a CSS framework (see DownloaderPage/DevConsole) -- this keeps the
// queue in the same language instead of introducing a second one. The final visual design
// is a later phase (docs/roadmap.md "UI"); this is functional, accessible, and consistent.

import type { CSSProperties } from "react";
import type { JobState } from "../types/job";

// Status colours are paired with a text label everywhere they are used, never used alone
// to carry meaning (spec section 37).
export const STATE_COLORS: Record<JobState, string> = {
  QUEUED: "#6b7280",
  WAITING: "#7c3aed",
  STARTING: "#2563eb",
  RUNNING: "#2563eb",
  PAUSED: "#b45309",
  RETRY_WAIT: "#b45309",
  RETRYING: "#2563eb",
  COMPLETED: "#15803d",
  FAILED: "#b91c1c",
  CANCELLED: "#6b7280",
  SKIPPED: "#a16207",
};

export const styles: Record<string, CSSProperties> = {
  page: { padding: "1.5rem", fontFamily: "system-ui, sans-serif", maxWidth: 1100, margin: "0 auto" },
  h1: { fontSize: "1.5rem", margin: "0 0 0.25rem" },
  subtitle: { color: "#555", margin: "0 0 1.25rem", fontSize: "0.9rem" },

  toolbar: {
    display: "flex",
    flexWrap: "wrap",
    gap: "0.5rem",
    alignItems: "center",
    padding: "0.75rem",
    border: "1px solid #ddd",
    borderRadius: 8,
    marginBottom: "1rem",
  },
  toolbarGroup: { display: "flex", gap: "0.35rem", alignItems: "center" },
  toolbarLabel: { fontSize: "0.8rem", color: "#555" },
  spacer: { flex: 1 },

  button: {
    padding: "0.35rem 0.7rem",
    fontSize: "0.85rem",
    borderRadius: 6,
    border: "1px solid #ccc",
    background: "#fff",
    cursor: "pointer",
  },
  buttonPrimary: {
    padding: "0.35rem 0.7rem",
    fontSize: "0.85rem",
    borderRadius: 6,
    border: "1px solid #2563eb",
    background: "#2563eb",
    color: "#fff",
    cursor: "pointer",
  },
  buttonDanger: {
    padding: "0.35rem 0.7rem",
    fontSize: "0.85rem",
    borderRadius: 6,
    border: "1px solid #b91c1c",
    background: "#fff",
    color: "#b91c1c",
    cursor: "pointer",
  },
  // Disabled controls stay visible and keep their label -- the user can see the action
  // exists and is simply not available right now (spec section 31).
  buttonDisabled: { opacity: 0.45, cursor: "not-allowed" },
  iconButton: {
    padding: "0.2rem 0.45rem",
    fontSize: "0.8rem",
    borderRadius: 4,
    border: "1px solid #ccc",
    background: "#fff",
    cursor: "pointer",
    minWidth: 28,
  },

  statsRow: { display: "flex", flexWrap: "wrap", gap: "1rem", marginBottom: "1rem" },
  stat: {
    padding: "0.5rem 0.85rem",
    border: "1px solid #e5e7eb",
    borderRadius: 8,
    minWidth: 78,
  },
  statValue: { fontSize: "1.35rem", fontWeight: 600, lineHeight: 1.1 },
  statLabel: { fontSize: "0.75rem", color: "#555", textTransform: "uppercase", letterSpacing: "0.03em" },

  filterBar: { display: "flex", flexWrap: "wrap", gap: "0.35rem", marginBottom: "0.75rem" },
  filterTab: {
    padding: "0.3rem 0.7rem",
    fontSize: "0.85rem",
    borderRadius: 999,
    border: "1px solid #ddd",
    background: "#fff",
    cursor: "pointer",
  },
  filterTabActive: {
    padding: "0.3rem 0.7rem",
    fontSize: "0.85rem",
    borderRadius: 999,
    border: "1px solid #2563eb",
    background: "#eff6ff",
    color: "#1d4ed8",
    cursor: "pointer",
    fontWeight: 600,
  },

  list: { display: "flex", flexDirection: "column", gap: "0.5rem" },
  row: {
    display: "flex",
    gap: "0.75rem",
    alignItems: "flex-start",
    padding: "0.7rem 0.85rem",
    border: "1px solid #e5e7eb",
    borderRadius: 8,
    background: "#fff",
  },
  rowSelected: {
    display: "flex",
    gap: "0.75rem",
    alignItems: "flex-start",
    padding: "0.7rem 0.85rem",
    border: "1px solid #2563eb",
    borderRadius: 8,
    background: "#f8fafc",
  },
  rowMain: { flex: 1, minWidth: 0 },
  // minWidth:0 plus these three is what stops a 300-character filename from stretching the
  // row past the viewport (spec section 37).
  ellipsis: {
    overflow: "hidden",
    textOverflow: "ellipsis",
    whiteSpace: "nowrap",
  },
  rowTitle: {
    fontWeight: 600,
    fontSize: "0.95rem",
    overflow: "hidden",
    textOverflow: "ellipsis",
    whiteSpace: "nowrap",
  },
  rowSubtitle: {
    fontSize: "0.8rem",
    color: "#555",
    overflow: "hidden",
    textOverflow: "ellipsis",
    whiteSpace: "nowrap",
  },
  rowMeta: { fontSize: "0.75rem", color: "#6b7280", display: "flex", gap: "0.75rem", flexWrap: "wrap" },
  rowControls: { display: "flex", gap: "0.25rem", alignItems: "center", flexShrink: 0 },

  badge: {
    display: "inline-block",
    padding: "0.1rem 0.45rem",
    borderRadius: 999,
    fontSize: "0.7rem",
    fontWeight: 600,
    border: "1px solid currentColor",
  },
  typeBadge: {
    display: "inline-block",
    padding: "0.1rem 0.45rem",
    borderRadius: 4,
    fontSize: "0.7rem",
    fontWeight: 600,
    background: "#f3f4f6",
    color: "#374151",
  },

  progressTrack: {
    height: 6,
    background: "#f1f5f9",
    borderRadius: 999,
    overflow: "hidden",
    marginTop: "0.35rem",
  },
  progressFill: { height: "100%", background: "#2563eb", borderRadius: 999 },
  progressIndeterminate: { height: "100%", background: "#cbd5e1", width: "100%" },

  detail: {
    marginTop: "1rem",
    padding: "1rem",
    border: "1px solid #ddd",
    borderRadius: 8,
    background: "#fafafa",
  },
  detailGrid: {
    display: "grid",
    gridTemplateColumns: "minmax(120px, max-content) 1fr",
    gap: "0.3rem 1rem",
    fontSize: "0.85rem",
  },
  detailLabel: { color: "#555" },
  detailValue: { wordBreak: "break-all" },

  errorBanner: {
    padding: "0.6rem 0.85rem",
    borderRadius: 6,
    border: "1px solid #fca5a5",
    background: "#fef2f2",
    color: "#7f1d1d",
    fontSize: "0.85rem",
    marginBottom: "0.75rem",
  },
  empty: { padding: "2rem", textAlign: "center", color: "#6b7280", fontSize: "0.9rem" },
  srOnly: {
    position: "absolute",
    width: 1,
    height: 1,
    padding: 0,
    margin: -1,
    overflow: "hidden",
    clip: "rect(0,0,0,0)",
    whiteSpace: "nowrap",
    border: 0,
  },
};
