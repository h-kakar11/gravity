import { createContext, useCallback, useContext, useMemo, useState, type ReactNode } from "react";

// Generalizes App.tsx's old two-tab switch into a small discriminated-union screen model
// (idealist.md: no sidebar, two-box home screen navigating into sub-screens). No router
// library -- this is a single-window desktop app with a handful of screens and no
// deep-linking need, so plain React state in a context (letting deeply nested components
// navigate without prop-drilling) is the right amount of machinery, not react-router.

export type Screen =
  | { kind: "home" }
  | { kind: "download"; prefillUrl?: string }
  | { kind: "convert"; prefillFilePath?: string; mode?: "convert" | "compress" }
  | { kind: "queue" }
  | { kind: "settings" }
  | { kind: "scheduledTasks" };

interface NavigationContextValue {
  screen: Screen;
  navigate: (screen: Screen) => void;
}

const NavigationContext = createContext<NavigationContextValue | null>(null);

export function NavigationProvider({ children }: { children: ReactNode }) {
  const [screen, setScreen] = useState<Screen>({ kind: "home" });
  const navigate = useCallback((next: Screen) => setScreen(next), []);
  const value = useMemo(() => ({ screen, navigate }), [screen, navigate]);

  return <NavigationContext.Provider value={value}>{children}</NavigationContext.Provider>;
}

export function useNavigation(): NavigationContextValue {
  const ctx = useContext(NavigationContext);
  if (!ctx) {
    throw new Error("useNavigation() must be called within a NavigationProvider");
  }
  return ctx;
}
