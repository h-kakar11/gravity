// Restrained notifications (spec section 15). Toasts fire for outcomes a user would want to
// know about even when they've looked away -- a job finishing, failing, being cancelled, an
// automatic retry being scheduled, the queue coming back after a restart -- and never for
// routine progress. One place produces them (useQueueNotifications, wired in App.tsx) so no
// two components can double-fire the same event.

import { createContext, useCallback, useContext, useRef, useState, type ReactNode } from "react";
import { AlertTriangleIcon, CheckCircleIcon, InfoIcon, XIcon } from "../icons";

export type ToastTone = "success" | "error" | "info";

interface Toast {
  id: number;
  tone: ToastTone;
  title: string;
  detail?: string;
}

interface ToastContextValue {
  push: (toast: Omit<Toast, "id">) => void;
}

const ToastContext = createContext<ToastContextValue | null>(null);

const TONE_ICON: Record<ToastTone, React.ComponentType<{ size?: number }>> = {
  success: CheckCircleIcon,
  error: AlertTriangleIcon,
  info: InfoIcon,
};

const AUTO_DISMISS_MS = 6000;

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const nextId = useRef(1);

  const dismiss = useCallback((id: number) => {
    setToasts((prev) => prev.filter((t) => t.id !== id));
  }, []);

  const push = useCallback(
    (toast: Omit<Toast, "id">) => {
      const id = nextId.current++;
      setToasts((prev) => [...prev.slice(-3), { ...toast, id }]);
      window.setTimeout(() => dismiss(id), AUTO_DISMISS_MS);
    },
    [dismiss],
  );

  return (
    <ToastContext.Provider value={{ push }}>
      {children}
      <div className="gv-toast-stack" role="region" aria-label="Notifications">
        {toasts.map((toast) => {
          const Glyph = TONE_ICON[toast.tone];
          return (
            <div
              key={toast.id}
              className={`gv-toast gv-toast--${toast.tone}`}
              role="status"
              aria-live="polite"
            >
              <Glyph size={16} />
              <div className="gv-toast__body">
                <div className="gv-toast__title">{toast.title}</div>
                {toast.detail ? <div className="gv-toast__detail">{toast.detail}</div> : null}
              </div>
              <button
                type="button"
                className="gv-toast__close"
                aria-label="Dismiss notification"
                onClick={() => dismiss(toast.id)}
              >
                <XIcon size={13} />
              </button>
            </div>
          );
        })}
      </div>
    </ToastContext.Provider>
  );
}

export function useToasts(): ToastContextValue {
  const ctx = useContext(ToastContext);
  if (!ctx) throw new Error("useToasts must be used within a ToastProvider");
  return ctx;
}
