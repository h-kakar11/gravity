import { type ReactNode } from "react";
import { useTauriDragDrop } from "../hooks/useTauriDragDrop";
import styles from "./GlassCard.module.css";

interface GlassCardProps {
  children: ReactNode;
  onClick?: () => void;
  onFilesDropped?: (paths: string[]) => void;
  className?: string;
  ariaLabel?: string;
}

// The reusable "Glassmorphism Lite" card. Doubles as a drag-and-drop target when
// `onFilesDropped` is supplied -- drag-and-drop is a hard requirement throughout the app,
// and the home screen's two boxes are the primary place it needs to work first. Uses
// `useTauriDragDrop` rather than HTML5 drag events: Tauri v2 intercepts OS-level
// drag-and-drop by default, so plain `onDrop`/`dataTransfer.files` never receives real
// file paths here (see issue #57).
export default function GlassCard({ children, onClick, onFilesDropped, className, ariaLabel }: GlassCardProps) {
  const { ref, isDragOver } = useTauriDragDrop<HTMLDivElement>((paths) => {
    onFilesDropped?.(paths);
  });
  const interactive = Boolean(onClick || onFilesDropped);
  const dragActive = Boolean(onFilesDropped) && isDragOver;

  const classes = [styles.card, interactive ? styles.interactive : "", dragActive ? styles.dragActive : "", className]
    .filter(Boolean)
    .join(" ");

  return (
    <div
      ref={onFilesDropped ? ref : undefined}
      className={classes}
      onClick={onClick}
      role={onClick ? "button" : undefined}
      tabIndex={onClick ? 0 : undefined}
      aria-label={ariaLabel}
      onKeyDown={
        onClick
          ? (event) => {
              if (event.key === "Enter" || event.key === " ") {
                event.preventDefault();
                onClick();
              }
            }
          : undefined
      }
    >
      {children}
    </div>
  );
}
