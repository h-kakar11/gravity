import { type DragEvent, type ReactNode, useState } from "react";
import styles from "./GlassCard.module.css";

interface GlassCardProps {
  children: ReactNode;
  onClick?: () => void;
  onFilesDropped?: (files: FileList) => void;
  className?: string;
  ariaLabel?: string;
}

// The reusable "Glassmorphism Lite" card. Doubles as a drag-and-drop target when
// `onFilesDropped` is supplied -- drag-and-drop is a hard requirement throughout the app,
// and the home screen's two boxes are the primary place it needs to work first.
export default function GlassCard({ children, onClick, onFilesDropped, className, ariaLabel }: GlassCardProps) {
  const [dragActive, setDragActive] = useState(false);
  const interactive = Boolean(onClick || onFilesDropped);

  const handleDragOver = (event: DragEvent<HTMLDivElement>) => {
    if (!onFilesDropped) return;
    event.preventDefault();
    setDragActive(true);
  };

  const handleDragLeave = () => setDragActive(false);

  const handleDrop = (event: DragEvent<HTMLDivElement>) => {
    if (!onFilesDropped) return;
    event.preventDefault();
    setDragActive(false);
    if (event.dataTransfer.files.length > 0) {
      onFilesDropped(event.dataTransfer.files);
    }
  };

  const classes = [styles.card, interactive ? styles.interactive : "", dragActive ? styles.dragActive : "", className]
    .filter(Boolean)
    .join(" ");

  return (
    <div
      className={classes}
      onClick={onClick}
      onDragOver={handleDragOver}
      onDragLeave={handleDragLeave}
      onDrop={handleDrop}
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
