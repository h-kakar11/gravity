// Issue #99: "the home page does not inherit global theme settings."
//
// The cause was not the context failing to publish the theme -- it published every color
// as a CSS variable correctly. It was that the home page's own styles never read most of
// them: its surfaces, hovers, progress bars and drop targets were written as literal
// rgba(99, 102, 241, ...) / rgba(26, 26, 46, ...) values, i.e. the DEFAULT accent and
// surface with an alpha, because plain CSS cannot add an alpha to a hex variable. So the
// page followed the theme only for as long as the theme was the default.
//
// The fix is the channel variables asserted here; the page now builds every translucent
// shade from them. These tests cover the context half (the variables exist, track the
// theme, and survive a saved theme from an older build); the page half is a CSS change
// jsdom cannot observe, since it does not apply stylesheets from CSS modules.

import { render, screen } from "@testing-library/react";
import { act } from "react";
import { beforeEach, describe, expect, it } from "vitest";

import { ThemeProvider, hexToRgbChannels, useTheme } from "./ThemeContext";

function ThemeProbe() {
  const { colors, updateColors, resetColors } = useTheme();
  return (
    <div>
      <span data-testid="accent">{colors.accent}</span>
      <button onClick={() => updateColors({ accent: "#ff8800" })}>recolor</button>
      <button onClick={resetColors}>reset</button>
    </div>
  );
}

const read = (name: string) => document.documentElement.style.getPropertyValue(name);

describe("hexToRgbChannels", () => {
  it("converts 6- and 3-digit hex to a CSS channel list", () => {
    expect(hexToRgbChannels("#6366f1")).toBe("99, 102, 241");
    expect(hexToRgbChannels("#FFF")).toBe("255, 255, 255");
    expect(hexToRgbChannels("  #000000 ")).toBe("0, 0, 0");
  });

  it("returns null rather than an unparseable value for anything else", () => {
    // A saved theme is user data: it can hold an rgb() string, a named color, or junk.
    // Writing that into a channel variable would produce `rgba(red, 0.1)` -- an invalid
    // declaration the browser drops, i.e. an invisible element.
    expect(hexToRgbChannels("rgb(1,2,3)")).toBeNull();
    expect(hexToRgbChannels("red")).toBeNull();
    expect(hexToRgbChannels("#12345")).toBeNull();
    expect(hexToRgbChannels("")).toBeNull();
  });
});

describe("ThemeProvider", () => {
  beforeEach(() => {
    localStorage.clear();
    document.documentElement.removeAttribute("style");
  });

  it("publishes channel variables alongside the colors themselves", () => {
    render(
      <ThemeProvider>
        <ThemeProbe />
      </ThemeProvider>,
    );

    expect(read("--color-accent")).toBe("#6366f1");
    expect(read("--color-accent-rgb")).toBe("99, 102, 241");
    expect(read("--color-surface-rgb")).toBe("26, 26, 46");
    expect(read("--color-error-rgb")).toBe("239, 68, 68");
  });

  it("moves the channel variables when the accent changes", () => {
    render(
      <ThemeProvider>
        <ThemeProbe />
      </ThemeProvider>,
    );

    act(() => screen.getByText("recolor").click());

    expect(read("--color-accent")).toBe("#ff8800");
    // The whole point of #99: everything drawn as a faded accent moves with it.
    expect(read("--color-accent-rgb")).toBe("255, 136, 0");
  });

  it("fills in colors a theme saved by an older build has never heard of", () => {
    // A saved object missing a key used to reach setProperty() as `undefined`, which sets
    // the variable to the string "undefined" -- that color is then simply gone from the UI.
    localStorage.setItem("gravity-theme-colors", JSON.stringify({ accent: "#00ccff" }));

    render(
      <ThemeProvider>
        <ThemeProbe />
      </ThemeProvider>,
    );

    expect(screen.getByTestId("accent").textContent).toBe("#00ccff");
    expect(read("--color-accent-rgb")).toBe("0, 204, 255");
    expect(read("--color-surface")).toBe("#1a1a2e");
    expect(read("--color-text-primary")).toBe("#f5f5f5");
  });

  it("restores every default on reset", () => {
    render(
      <ThemeProvider>
        <ThemeProbe />
      </ThemeProvider>,
    );

    act(() => screen.getByText("recolor").click());
    act(() => screen.getByText("reset").click());

    expect(read("--color-accent")).toBe("#6366f1");
    expect(read("--color-accent-rgb")).toBe("99, 102, 241");
    expect(localStorage.getItem("gravity-theme-colors")).toBeNull();
  });
});
