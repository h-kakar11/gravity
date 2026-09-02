import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { act } from "react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import HomePage from "./HomePage";
import { NavigationProvider } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";

// HomePage became the app's primary download surface when the download flow moved here
// from DownloaderPage, and arrived with no tests at all. The regressions these cover are
// all the same shape: state and handlers were carried over, but nothing rendered them, so
// the feature was reachable in the source and unreachable in the product.

vi.mock("../services/coreClient", () => ({
  getSettings: vi.fn(),
  inspectDownloadUrl: vi.fn(),
  inspectPlaylistUrl: vi.fn(),
  suggestPlaylistFolder: vi.fn(),
  listJobs: vi.fn(),
  listJobHistory: vi.fn(),
  getJob: vi.fn(),
  subscribeToJobEvents: vi.fn(),
  createDownloadJob: vi.fn(),
  cancelJob: vi.fn(),
  pauseJob: vi.fn(),
  resumeJob: vi.fn(),
  retryJob: vi.fn(),
  removeJob: vi.fn(),
  openContainingFolder: vi.fn(),
}));

const SETTINGS = {
  settings: {
    general: { defaultOutputDirectory: "D:\\Videos" },
    downloads: { defaultQuality: "1080p", speedUnits: "MBps" },
  },
};

function renderHome() {
  return render(
    <NavigationProvider>
      <HomePage />
    </NavigationProvider>,
  );
}

async function inspect(url: string) {
  const input = screen.getByPlaceholderText("Paste a URL...");
  fireEvent.change(input, { target: { value: url } });
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name: "Inspect" }));
  });
}

async function click(name: string | RegExp) {
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name }));
  });
}

beforeEach(() => {
  vi.clearAllMocks();
  vi.mocked(coreClient.subscribeToJobEvents).mockReturnValue(() => {});
  vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [] } as never);
  vi.mocked(coreClient.listJobHistory).mockResolvedValue({ jobs: [] } as never);
  vi.mocked(coreClient.getSettings).mockResolvedValue(SETTINGS as never);
});

describe("HomePage playlist flow", () => {
  const PLAYLIST_URL = "https://example.com/playlist?list=abc";

  function scriptPlaylist() {
    vi.mocked(coreClient.inspectDownloadUrl).mockRejectedValue({
      code: "E_PLAYLIST_NOT_SUPPORTED",
      category: "UNSUPPORTED_FORMAT",
      message: "This URL is a playlist, not a single video.",
      details: "",
      recoverable: false,
    });
    vi.mocked(coreClient.inspectPlaylistUrl).mockResolvedValue({
      playlist: {
        title: "My Playlist",
        uploader: "Some Channel",
        count: 3,
        truncated: false,
        entries: [
          { index: 1, url: "https://example.com/watch?v=a", title: "First", durationSeconds: 61 },
          { index: 2, url: "https://example.com/watch?v=b", title: "Second" },
          { index: 3, url: "https://example.com/watch?v=c", title: "Third" },
        ],
      },
    } as never);
    vi.mocked(coreClient.suggestPlaylistFolder).mockResolvedValue({ name: "playlist #1" } as never);
  }

  it("enumerates a playlist link and shows what it found", async () => {
    // The regression: E_PLAYLIST_NOT_SUPPORTED correctly routed to inspectPlaylistUrl and
    // the result landed in state, but no JSX read it -- so the card expanded to show
    // nothing and the flow dead-ended with no error and no way forward.
    scriptPlaylist();
    renderHome();
    await inspect(PLAYLIST_URL);

    await waitFor(() => expect(screen.getByText("My Playlist")).toBeTruthy());
    expect(screen.getByText(/downloaded one at a time/)).toBeTruthy();
    expect(screen.getByRole("button", { name: /Download all 3/ })).toBeTruthy();
    // The list itself, so "it enumerated" means the entries are actually on screen.
    expect(screen.getByText(/Show the 3 videos/)).toBeTruthy();
    expect(screen.getByText(/First/)).toBeTruthy();
    // Entry durations come back from the core as `durationSeconds`; 61s renders as 1:01.
    expect(screen.getByText(/1:01/)).toBeTruthy();
  });

  it("fans the playlist out into one job per entry, chained to run in order", async () => {
    scriptPlaylist();
    let created = 0;
    vi.mocked(coreClient.createDownloadJob).mockImplementation(
      async () => ({ jobId: `job-${++created}` }) as never,
    );

    renderHome();
    await inspect(PLAYLIST_URL);
    await waitFor(() => expect(screen.getByText("My Playlist")).toBeTruthy());
    await click(/Download all 3/);

    await waitFor(() => expect(coreClient.createDownloadJob).toHaveBeenCalledTimes(3));
    const calls = vi.mocked(coreClient.createDownloadJob).mock.calls.map(([p]) => p);

    // Into the named subfolder, numbered, and each entry waiting on the previous one.
    expect(calls.every((c) => c.outputDirectory === "D:\\Videos\\playlist #1")).toBe(true);
    expect(calls.map((c) => c.playlistIndex)).toEqual([1, 2, 3]);
    expect(calls.every((c) => c.playlistCount === 3)).toBe(true);
    expect(calls[0].runAfter).toBeUndefined();
    expect(calls[1].runAfter).toEqual(["job-1"]);
    expect(calls[2].runAfter).toEqual(["job-2"]);
  });

  it("uses runAfter rather than dependsOn, so one dead video does not cancel the rest", async () => {
    // dependsOn requires the predecessor to COMPLETE and transitively cancels dependents;
    // a private or deleted video is routine in a long playlist, so that edge kind would
    // throw away every entry after the first failure. See SchedulerCore::Submission.
    scriptPlaylist();
    let created = 0;
    vi.mocked(coreClient.createDownloadJob).mockImplementation(
      async () => ({ jobId: `job-${++created}` }) as never,
    );

    renderHome();
    await inspect(PLAYLIST_URL);
    await waitFor(() => expect(screen.getByText("My Playlist")).toBeTruthy());
    await click(/Download all 3/);

    await waitFor(() => expect(coreClient.createDownloadJob).toHaveBeenCalledTimes(3));
    for (const [params] of vi.mocked(coreClient.createDownloadJob).mock.calls) {
      expect(params).not.toHaveProperty("dependsOn");
    }
  });

  it("offers the video-or-playlist choice for a link that is both", async () => {
    // `comboChoiceUrl` was set on this page but never rendered, so a watch?v=X&list=Y link
    // silently resolved to just the one video with no way to ask for the list.
    vi.mocked(coreClient.inspectDownloadUrl).mockResolvedValue({
      metadata: { title: "Shared From A List", formats: [] },
    } as never);
    scriptPlaylist();
    vi.mocked(coreClient.inspectDownloadUrl).mockResolvedValue({
      metadata: { title: "Shared From A List", formats: [] },
    } as never);

    renderHome();
    await inspect("https://example.com/watch?v=a&list=abc");

    await waitFor(() => expect(screen.getByText("This link is part of a playlist.")).toBeTruthy());
    await click("The whole playlist");
    await waitFor(() => expect(screen.getByText("My Playlist")).toBeTruthy());
  });
});

describe("HomePage single-video download", () => {
  const METADATA = {
    metadata: {
      title: "A Real Video",
      uploader: "Some Channel",
      durationSeconds: 212,
      formats: [
        { formatId: "137", extension: "mp4", resolution: "1920x1080", hasVideo: true, hasAudio: false },
        { formatId: "140", extension: "m4a", hasVideo: false, hasAudio: true },
      ],
    },
  };

  it("lets the user pick an exact stream, and sends it as formatId", async () => {
    // `selectedFormatId` was read by handleDownload and disabled the quality preset, but
    // with no format list rendered it could never become non-null (issue #31's feature was
    // unreachable here).
    vi.mocked(coreClient.inspectDownloadUrl).mockResolvedValue(METADATA as never);
    vi.mocked(coreClient.createDownloadJob).mockResolvedValue({ jobId: "job-1" } as never);

    renderHome();
    await inspect("https://example.com/watch?v=abc");
    await waitFor(() => expect(screen.getByText("A Real Video")).toBeTruthy());

    await click(/137/);
    await click("Download");

    await waitFor(() => expect(coreClient.createDownloadJob).toHaveBeenCalled());
    expect(vi.mocked(coreClient.createDownloadJob).mock.calls[0][0].formatId).toBe("137");
  });

  it("sends the quality preset when no explicit stream is chosen", async () => {
    vi.mocked(coreClient.inspectDownloadUrl).mockResolvedValue(METADATA as never);
    vi.mocked(coreClient.createDownloadJob).mockResolvedValue({ jobId: "job-1" } as never);

    renderHome();
    await inspect("https://example.com/watch?v=abc");
    await waitFor(() => expect(screen.getByText("A Real Video")).toBeTruthy());
    await click("Download");

    await waitFor(() => expect(coreClient.createDownloadJob).toHaveBeenCalled());
    const params = vi.mocked(coreClient.createDownloadJob).mock.calls[0][0];
    expect(params.formatId).toBeUndefined();
    // Seeded from settings.downloads.defaultQuality ("1080p"), uppercased to the wire value.
    expect(params.quality).toBe("1080P");
  });
});

describe("HomePage error presentation", () => {
  it("gives a connectivity failure plain language instead of the raw category/code", async () => {
    vi.mocked(coreClient.inspectDownloadUrl).mockRejectedValue({
      code: "E_NETWORK",
      category: "NETWORK_ERROR",
      message: "urlopen error [Errno -3] Temporary failure in name resolution",
      details: "Traceback (most recent call last): ...",
      recoverable: true,
    });

    renderHome();
    await inspect("https://example.com/watch?v=dead");

    await waitFor(() => expect(screen.getByRole("alert")).toBeTruthy());
    expect(screen.getByRole("alert").textContent).toMatch(/Can't reach the network/);
    // A Python traceback is not something to put in front of a user.
    expect(screen.getByRole("alert").textContent).not.toMatch(/Traceback/);
  });

  it("collapses raw diagnostics behind a disclosure for other failures", async () => {
    vi.mocked(coreClient.inspectDownloadUrl).mockRejectedValue({
      code: "E_VIDEO_PRIVATE",
      category: "PERMISSION_ERROR",
      message: "Private video. Sign in if you've been granted access.",
      details: "yt_dlp.utils.DownloadError: ...",
      recoverable: false,
    });

    renderHome();
    await inspect("https://example.com/watch?v=priv");

    await waitFor(() => expect(screen.getByRole("alert")).toBeTruthy());
    expect(screen.getByRole("alert").textContent).toMatch(/Private video/);
    expect(screen.getByText("Technical details")).toBeTruthy();
  });
});
