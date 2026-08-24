// Navigation behavior (spec section 23): the shell names Gravity's product concepts, marks
// exactly one destination active, and every item is reachable by keyboard because it's a
// plain <button> in normal tab order -- no roving-tabindex machinery to break.

import { render, screen } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { describe, expect, it, vi } from "vitest";
import { AppShell } from "./AppShell";

describe("AppShell", () => {
  it("marks the current route with aria-current and nothing else", () => {
    render(
      <AppShell route="queue" onNavigate={vi.fn()}>
        <div>content</div>
      </AppShell>,
    );
    expect(screen.getByRole("button", { name: "Queue" })).toHaveAttribute("aria-current", "page");
    expect(screen.getByRole("button", { name: "Home" })).not.toHaveAttribute("aria-current");
    expect(screen.getByRole("button", { name: "Settings" })).not.toHaveAttribute("aria-current");
  });

  it("calls onNavigate with the clicked route", async () => {
    const onNavigate = vi.fn();
    const user = userEvent.setup();
    render(
      <AppShell route="home" onNavigate={onNavigate}>
        <div>content</div>
      </AppShell>,
    );
    await user.click(screen.getByRole("button", { name: "Download" }));
    expect(onNavigate).toHaveBeenCalledWith("download");
  });

  it("exposes every primary destination named in the spec, and nothing extra", () => {
    render(
      <AppShell route="home" onNavigate={vi.fn()}>
        <div>content</div>
      </AppShell>,
    );
    const nav = screen.getByRole("navigation", { name: "Sections" });
    const items = Array.from(nav.querySelectorAll("button")).map((b) => b.textContent);
    expect(items).toEqual(["Home", "Download", "Convert & Compress", "Queue", "Settings"]);
  });

  it("renders the active job count badge on Queue when jobs are running", () => {
    render(
      <AppShell route="home" onNavigate={vi.fn()} queuedBadge={3}>
        <div>content</div>
      </AppShell>,
    );
    expect(screen.getByLabelText("3 active jobs")).toBeInTheDocument();
  });

  it("every item is a real button reachable by normal Tab order", () => {
    render(
      <AppShell route="home" onNavigate={vi.fn()}>
        <div>content</div>
      </AppShell>,
    );
    for (const name of ["Home", "Download", "Convert & Compress", "Queue", "Settings"]) {
      const button = screen.getByRole("button", { name });
      expect(button.tabIndex).not.toBe(-1);
    }
  });
});
