import { render, screen, waitFor } from "@testing-library/react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import DownloaderPage from "./DownloaderPage";
import { NavigationProvider } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";

// The Download page's paste handling is the one piece of real behavior on this screen that
// isn't a straight pass-through to the core, and issue #81 lived in it.
vi.mock("../services/coreClient", () => ({
  getSettings: vi.fn(),
  inspectDownloadUrl: vi.fn(),
  listJobs: vi.fn(),
  getJob: vi.fn(),
  subscribeToJobEvents: vi.fn(),
  createDownloadJob: vi.fn(),
  cancelJob: vi.fn(),
  pauseJob: vi.fn(),
  resumeJob: vi.fn(),
  retryJob: vi.fn(),
  removeJob: vi.fn(),
  openContainingFolder: vi.fn(),
  listPresets: vi.fn(),
  savePreset: vi.fn(),
  deletePreset: vi.fn(),
}));

const URL_UNDER_TEST = "https://example.com/watch?v=abc123";

// jsdom does not implement the paste event's DEFAULT ACTION -- it dispatches the event and
// stops -- so a test here cannot observe the doubling itself. The mechanism was confirmed
// separately in real Chromium against React 18.3.1: with a controlled input, pasting a
// 34-character URL left a 68-character field without preventDefault() and a 34-character
// one with it. (`paste` is a discrete event, so React flushes the handler's setState
// synchronously, which writes the value into the DOM node and parks the caret at the end;
// the browser's default insertion then appends the same text again.)
//
// What is testable here, and what these assert, is the contract that experiment showed to
// be load-bearing: the handler takes over insertion for a URL, and leaves it alone for
// anything else.
function firePaste(input: HTMLInputElement, text: string): Event {
  const event = new Event("paste", { bubbles: true, cancelable: true });
  Object.defineProperty(event, "clipboardData", { value: { getData: () => text } });
  input.dispatchEvent(event);
  return event;
}

function renderPage() {
  return render(
    <NavigationProvider>
      <DownloaderPage />
    </NavigationProvider>,
  );
}

describe("DownloaderPage paste handling", () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(coreClient.subscribeToJobEvents).mockReturnValue(() => {});
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [] } as never);
    vi.mocked(coreClient.getSettings).mockRejectedValue(new Error("no settings in this test"));
    // PresetBar mounts alongside the URL field and loads its own list on mount.
    vi.mocked(coreClient.listPresets).mockResolvedValue({ presets: [] } as never);
    vi.mocked(coreClient.inspectDownloadUrl).mockResolvedValue({
      metadata: { title: "Example", formats: [] },
    } as never);
  });

  it("takes over insertion for a pasted URL so the browser cannot insert it a second time", async () => {
    // Issue #81: "Ctrl+V pastes URL twice". The handler sets the field's state itself, so
    // it must also suppress the browser's own insertion -- otherwise both run and the URL
    // lands in the field twice over.
    renderPage();
    const input = screen.getByPlaceholderText("https://...") as HTMLInputElement;

    const event = firePaste(input, URL_UNDER_TEST);

    expect(event.defaultPrevented).toBe(true);
    await waitFor(() => expect(input.value).toBe(URL_UNDER_TEST));
  });

  it("auto-inspects the pasted URL exactly once", async () => {
    // Issue #62's auto-inspect has to survive the #81 fix.
    renderPage();
    const input = screen.getByPlaceholderText("https://...") as HTMLInputElement;

    firePaste(input, URL_UNDER_TEST);

    await waitFor(() => expect(coreClient.inspectDownloadUrl).toHaveBeenCalledTimes(1));
    expect(coreClient.inspectDownloadUrl).toHaveBeenCalledWith(URL_UNDER_TEST);
  });

  it("leaves non-URL text to the browser's own paste behavior", () => {
    // The handler deliberately ignores arbitrary text. It must NOT preventDefault there,
    // or pasting a search term into the field would insert nothing at all -- turning the
    // #81 fix into a worse bug than the one it replaced.
    renderPage();
    const input = screen.getByPlaceholderText("https://...") as HTMLInputElement;

    const event = firePaste(input, "not a url");

    expect(event.defaultPrevented).toBe(false);
    expect(coreClient.inspectDownloadUrl).not.toHaveBeenCalled();
  });
});
