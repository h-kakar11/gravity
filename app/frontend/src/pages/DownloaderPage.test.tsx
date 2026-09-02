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
  inspectPlaylistUrl: vi.fn(),
  suggestPlaylistFolder: vi.fn(),
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

const PLAYLIST_URL = "https://www.youtube.com/playlist?list=PL123";
const COMBO_URL = "https://www.youtube.com/watch?v=abc123&list=PL123";

function playlistOf(count: number) {
  return {
    playlist: {
      title: "My Playlist",
      count,
      truncated: false,
      entries: Array.from({ length: count }, (_, i) => ({
        index: i + 1,
        url: `https://example.com/watch?v=v${i + 1}`,
        title: `Video ${i + 1}`,
      })),
    },
  };
}

// Playlist support (issue #41). The behavior worth pinning here is the decomposition
// contract -- one job per entry, chained so they run one at a time, numbered, into the
// chosen subfolder -- plus the two routing decisions that get a user into that flow.
describe("DownloaderPage playlist handling", () => {
  beforeEach(() => {
    vi.clearAllMocks();
    vi.mocked(coreClient.subscribeToJobEvents).mockReturnValue(() => {});
    vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [] } as never);
    vi.mocked(coreClient.listPresets).mockResolvedValue({ presets: [] } as never);
    vi.mocked(coreClient.getSettings).mockResolvedValue({
      settings: {
        general: { defaultOutputDirectory: "C:\\out" },
        downloads: { defaultQuality: "best", speedUnits: "MBps" },
      },
    } as never);
    vi.mocked(coreClient.suggestPlaylistFolder).mockResolvedValue({ name: "playlist #1" } as never);
    vi.mocked(coreClient.createDownloadJob).mockImplementation(
      (() => {
        let n = 0;
        return () => Promise.resolve({ jobId: `job-${++n}` });
      })() as never,
    );
  });

  it("enumerates a bare playlist link when inspect reports it is a playlist", async () => {
    // The routing signal is the backend's error code, not the URL's shape -- that is what
    // makes this work on sites whose playlist URLs look nothing like YouTube's.
    vi.mocked(coreClient.inspectDownloadUrl).mockRejectedValue({
      code: "E_PLAYLIST_NOT_SUPPORTED",
      category: "UNSUPPORTED_FORMAT",
      message: "This URL is a playlist",
    });
    vi.mocked(coreClient.inspectPlaylistUrl).mockResolvedValue(playlistOf(3) as never);

    renderPage();
    firePaste(screen.getByPlaceholderText("https://...") as HTMLInputElement, PLAYLIST_URL);

    await waitFor(() => expect(coreClient.inspectPlaylistUrl).toHaveBeenCalledWith(PLAYLIST_URL));
    expect(await screen.findByText("My Playlist")).toBeTruthy();
    // The playlist error is a routing signal, not something to show the user.
    expect(screen.queryByText(/This URL is a playlist/)).toBeNull();
  });

  it("asks which to download when the link is one video inside a playlist", async () => {
    vi.mocked(coreClient.inspectDownloadUrl).mockResolvedValue({
      metadata: { title: "Example", formats: [] },
    } as never);

    renderPage();
    firePaste(screen.getByPlaceholderText("https://...") as HTMLInputElement, COMBO_URL);

    expect(await screen.findByText("This link is part of a playlist.")).toBeTruthy();
    // Nothing is enumerated until the user actually chooses the playlist.
    expect(coreClient.inspectPlaylistUrl).not.toHaveBeenCalled();
  });

  it("only enumerates a combo link after the user picks the whole playlist", async () => {
    vi.mocked(coreClient.inspectDownloadUrl).mockResolvedValue({
      metadata: { title: "Example", formats: [] },
    } as never);
    vi.mocked(coreClient.inspectPlaylistUrl).mockResolvedValue(playlistOf(2) as never);

    renderPage();
    firePaste(screen.getByPlaceholderText("https://...") as HTMLInputElement, COMBO_URL);

    (await screen.findByRole("button", { name: "The whole playlist" })).click();

    await waitFor(() => expect(coreClient.inspectPlaylistUrl).toHaveBeenCalledWith(COMBO_URL));
  });

  it("creates one job per entry, chained so they run one at a time", async () => {
    vi.mocked(coreClient.inspectDownloadUrl).mockRejectedValue({
      code: "E_PLAYLIST_NOT_SUPPORTED",
      category: "UNSUPPORTED_FORMAT",
      message: "playlist",
    });
    vi.mocked(coreClient.inspectPlaylistUrl).mockResolvedValue(playlistOf(3) as never);

    renderPage();
    firePaste(screen.getByPlaceholderText("https://...") as HTMLInputElement, PLAYLIST_URL);

    (await screen.findByRole("button", { name: "Download all 3" })).click();

    await waitFor(() => expect(coreClient.createDownloadJob).toHaveBeenCalledTimes(3));
    const calls = vi.mocked(coreClient.createDownloadJob).mock.calls.map(([params]) => params);

    // Every entry lands in the chosen subfolder, numbered against the true total.
    expect(calls.map((c) => c.outputDirectory)).toEqual([
      "C:\\out\\playlist #1",
      "C:\\out\\playlist #1",
      "C:\\out\\playlist #1",
    ]);
    expect(calls.map((c) => c.playlistIndex)).toEqual([1, 2, 3]);
    expect(calls.every((c) => c.playlistCount === 3)).toBe(true);

    // The chain is what enforces "one video at a time": the first is unblocked, and each
    // later job waits on the one before it.
    expect(calls[0].runAfter).toBeUndefined();
    expect(calls[1].runAfter).toEqual(["job-1"]);
    expect(calls[2].runAfter).toEqual(["job-2"]);
    // Emphatically NOT dependsOn: that would cancel every remaining entry the moment one
    // video turned out to be unavailable, which is routine in a long playlist.
    expect(calls.every((c) => c.dependsOn === undefined)).toBe(true);
  });

  it("reports how many were queued when the fan-out fails partway", async () => {
    // Entries created before the failure are already queued -- saying "nothing happened"
    // would be a lie the user could act on.
    vi.mocked(coreClient.inspectDownloadUrl).mockRejectedValue({
      code: "E_PLAYLIST_NOT_SUPPORTED", category: "UNSUPPORTED_FORMAT", message: "playlist",
    });
    vi.mocked(coreClient.inspectPlaylistUrl).mockResolvedValue(playlistOf(3) as never);
    let created = 0;
    vi.mocked(coreClient.createDownloadJob).mockImplementation((() => {
      created += 1;
      if (created === 3) return Promise.reject({ code: "E_BOOM", category: "UNKNOWN", message: "boom" });
      return Promise.resolve({ jobId: `job-${created}` });
    }) as never);

    renderPage();
    firePaste(screen.getByPlaceholderText("https://...") as HTMLInputElement, PLAYLIST_URL);
    (await screen.findByRole("button", { name: "Download all 3" })).click();

    expect(await screen.findByText(/Queued 2 downloads/)).toBeTruthy();
  });
});
