import GlassCard from "../components/GlassCard";
import { useNavigation } from "../navigation/NavigationContext";
import styles from "./HomePage.module.css";

// The primary landing screen: exactly two boxes (idealist.md), Download and Convert &
// Compress -- conversion is the app's primary use case, download is secondary, but
// Download is listed first here since it's also the entry point for the URL-paste flow.
// Each box is both a click target and a drop target, so dragging a file straight onto
// "Convert & Compress" skips the menu entirely (also a hard requirement).
export default function HomePage() {
  const { navigate } = useNavigation();

  const handleDownloadDrop = () => {
    // A dropped file on the Download box has no meaning (Download only takes a URL) --
    // just navigate there so the user can paste a link.
    navigate({ kind: "download" });
  };

  const handleConvertDrop = (files: FileList) => {
    const first = files[0];
    // Electron/Tauri file drops expose a real filesystem path via `.path` in Tauri's
    // webview; fall back to just navigating with nothing prefilled if that's absent.
    const path = (first as File & { path?: string }).path;
    navigate({ kind: "convert", prefillFilePath: path, mode: "convert" });
  };

  return (
    <div className={styles.wrap}>
      <div className={styles.heading}>
        <h1 className={styles.title}>What are we doing today?</h1>
        <p className={styles.subtitle}>Drag a file in, or pick a card to get started.</p>
      </div>
      <div className={styles.boxes}>
        <GlassCard
          className={styles.box}
          onClick={() => navigate({ kind: "download" })}
          onFilesDropped={handleDownloadDrop}
          ariaLabel="Download from a URL"
        >
          <div className={styles.icon}>&#8595;</div>
          <h2 className={styles.boxTitle}>Download</h2>
          <p className={styles.boxSubtitle}>Paste a link. MP4, MP3, or almost anything else.</p>
        </GlassCard>
        <GlassCard
          className={styles.box}
          onClick={() => navigate({ kind: "convert", mode: "convert" })}
          onFilesDropped={handleConvertDrop}
          ariaLabel="Convert or compress a local file"
        >
          <div className={styles.icon}>&#8646;</div>
          <h2 className={styles.boxTitle}>Convert &amp; Compress</h2>
          <p className={styles.boxSubtitle}>Drop a file. Change its format, shrink its size.</p>
        </GlassCard>
      </div>
    </div>
  );
}
