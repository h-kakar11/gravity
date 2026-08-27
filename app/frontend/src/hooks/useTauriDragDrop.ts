import { useEffect, useRef, useState } from "react";
import { getCurrentWebview } from "@tauri-apps/api/webview";
import type { PhysicalPosition } from "@tauri-apps/api/dpi";

// Tauri v2 intercepts native OS drag-and-drop at the webview level by default
// (tauri.conf.json's `dragDropEnabled` is unset, which defaults to `true`), so the
// standard HTML5 `onDrop`/`dataTransfer.files` events never carry real file data in this
// app -- only `getCurrentWebview().onDragDropEvent()` does, delivering real filesystem
// paths via a Tauri-native, window-global (not per-element) event instead (see issue #57).
//
// This hook bridges that global, position-based event back to a per-element drop target:
// attach the returned `ref` to the element that should act as a drop zone, and `onDrop`
// fires with real paths whenever a drop lands inside that element's current bounds.
// `isDragOver` mirrors whether a drag is currently hovering inside those bounds, for
// styling (e.g. a highlighted dropzone).
export function useTauriDragDrop<T extends HTMLElement>(onDrop: (paths: string[]) => void) {
  const ref = useRef<T>(null);
  const [isDragOver, setIsDragOver] = useState(false);
  const onDropRef = useRef(onDrop);
  onDropRef.current = onDrop;

  useEffect(() => {
    let unlisten: (() => void) | undefined;
    let cancelled = false;

    const isInsideBounds = (position: PhysicalPosition) => {
      const el = ref.current;
      if (!el) return false;
      const rect = el.getBoundingClientRect();
      const scale = window.devicePixelRatio || 1;
      const x = position.x / scale;
      const y = position.y / scale;
      return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
    };

    getCurrentWebview()
      .onDragDropEvent((event) => {
        const { payload } = event;
        if (payload.type === "enter" || payload.type === "over") {
          setIsDragOver(isInsideBounds(payload.position));
        } else if (payload.type === "drop") {
          const wasOver = isInsideBounds(payload.position);
          setIsDragOver(false);
          if (wasOver && payload.paths.length > 0) {
            onDropRef.current(payload.paths);
          }
        } else {
          setIsDragOver(false);
        }
      })
      .then((fn) => {
        if (cancelled) {
          fn();
        } else {
          unlisten = fn;
        }
      });

    return () => {
      cancelled = true;
      unlisten?.();
    };
  }, []);

  return { ref, isDragOver };
}
