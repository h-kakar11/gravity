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
        setColors(JSON.parse(saved));
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
