import React, { createContext, useContext, useEffect, useState } from "react";

export interface ThemeColors {
  background: string;
  backgroundRichBlack: string;
  surface: string;
  surfaceHover: string;
  accent: string;
  accentHover: string;
  accentSoft: string;
  textPrimary: string;
  textSecondary: string;
  textDisabled: string;
  topoLineColor: string;
  success: string;
  error: string;
  warning: string;
}

const DEFAULT_COLORS: ThemeColors = {
  background: "#0c0c15",
  backgroundRichBlack: "#000000",
  surface: "#1a1a2e",
  surfaceHover: "#252545",
  accent: "#6366f1",
  accentHover: "#4f46e5",
  accentSoft: "rgba(99, 102, 241, 0.15)",
  textPrimary: "#f5f5f5",
  textSecondary: "#9ca3af",
  textDisabled: "#6b7280",
  topoLineColor: "#6366f1",
  success: "#10b981",
  error: "#ef4444",
  warning: "#f59e0b",
};

const STORAGE_KEY = "gravity-theme-colors";

// "#6366f1" -> "99, 102, 241", the channel-list form CSS needs to build a translucent
// shade of a themed color: `rgba(var(--color-accent-rgb), 0.15)`. Without this, any style
// that wanted a faded accent had to hardcode the DEFAULT accent's channels -- which is
// exactly how the home page came to ignore the theme entirely (issue #99): every one of
// its surfaces, hovers and progress bars was a literal rgba(99, 102, 241, ...).
//
// Returns null for anything that is not a 3- or 6-digit hex color (a saved theme could
// hold an `rgb()` string or something a user typed), in which case the caller leaves the
// existing channel variable alone rather than writing a value CSS cannot parse.
export function hexToRgbChannels(hex: string): string | null {
  const match = /^#([0-9a-f]{3}|[0-9a-f]{6})$/i.exec(hex.trim());
  if (!match) return null;
  let digits = match[1];
  if (digits.length === 3) digits = digits.split("").map((d) => d + d).join("");
  const value = parseInt(digits, 16);
  return `${(value >> 16) & 255}, ${(value >> 8) & 255}, ${value & 255}`;
}

interface ThemeContextType {
  colors: ThemeColors;
  updateColors: (newColors: Partial<ThemeColors>) => void;
  resetColors: () => void;
}

const ThemeContext = createContext<ThemeContextType | undefined>(undefined);

export function ThemeProvider({ children }: { children: React.ReactNode }) {
  const [colors, setColors] = useState<ThemeColors>(DEFAULT_COLORS);

  useEffect(() => {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved) {
      try {
        // Merged over the defaults, never used as-is: a theme saved by an earlier build
        // has no key for a color added since, and the missing key would reach
        // setProperty() as `undefined` -- which sets the CSS variable to the literal
        // string "undefined" and takes that color out of the theme for good.
        setColors({ ...DEFAULT_COLORS, ...(JSON.parse(saved) as Partial<ThemeColors>) });
      } catch {
        setColors(DEFAULT_COLORS);
      }
    }
  }, []);

  useEffect(() => {
    const root = document.documentElement;
    root.style.setProperty("--color-bg", colors.background);
    root.style.setProperty("--color-bg-rich-black", colors.backgroundRichBlack);
    root.style.setProperty("--color-surface", colors.surface);
    root.style.setProperty("--color-surface-hover", colors.surfaceHover);
    root.style.setProperty("--color-accent", colors.accent);
    root.style.setProperty("--color-accent-hover", colors.accentHover);
    root.style.setProperty("--color-accent-soft", colors.accentSoft);
    root.style.setProperty("--color-text-primary", colors.textPrimary);
    root.style.setProperty("--color-text-secondary", colors.textSecondary);
    root.style.setProperty("--color-text-disabled", colors.textDisabled);
    root.style.setProperty("--color-success", colors.success);
    root.style.setProperty("--color-error", colors.error);
    root.style.setProperty("--color-warning", colors.warning);

    // Channel forms of the colors that get used at partial opacity somewhere in the app.
    for (const [variable, value] of [
      ["--color-accent-rgb", colors.accent],
      ["--color-surface-rgb", colors.surface],
      ["--color-error-rgb", colors.error],
    ] as const) {
      const channels = hexToRgbChannels(value);
      if (channels !== null) root.style.setProperty(variable, channels);
    }
  }, [colors]);

  const updateColors = (newColors: Partial<ThemeColors>) => {
    const updated = { ...colors, ...newColors };
    setColors(updated);
    localStorage.setItem(STORAGE_KEY, JSON.stringify(updated));
  };

  const resetColors = () => {
    setColors(DEFAULT_COLORS);
    localStorage.removeItem(STORAGE_KEY);
  };

  return (
    <ThemeContext.Provider value={{ colors, updateColors, resetColors }}>
      {children}
    </ThemeContext.Provider>
  );
}

export function useTheme() {
  const context = useContext(ThemeContext);
  if (context === undefined) {
    throw new Error("useTheme must be used within a ThemeProvider");
  }
  return context;
}
