import { fireEvent, render, screen, waitFor } from "@testing-library/react";
import { act } from "react";
import { beforeEach, describe, expect, it, vi } from "vitest";
import ConvertPage from "./ConvertPage";
import { NavigationProvider } from "../navigation/NavigationContext";
import * as coreClient from "../services/coreClient";
import { open as openFilePicker } from "@tauri-apps/plugin-dialog";

// Batch convert. The bug these cover is one shape repeated: the screen accepted exactly
// one file at a time and refused to start anything while a job was running, so converting
// a folder meant driving the whole form once per file. Every test here fails against the
// single-file version of this page.

vi.mock("@tauri-apps/plugin-dialog", () => ({ open: vi.fn() }));

vi.mock("../services/coreClient", () => ({
  getSettings: vi.fn(),
  inspectFile: vi.fn(),
  listFolderFiles: vi.fn(),
  listJobs: vi.fn(),
  getJob: vi.fn(),
  subscribeToJobEvents: vi.fn(),
  createConversionJob: vi.fn(),
  createCompressionJob: vi.fn(),
  cancelJob: vi.fn(),
  pauseJob: vi.fn(),
  resumeJob: vi.fn(),
  retryJob: vi.fn(),
  removeJob: vi.fn(),
  listPresets: vi.fn(),
  savePreset: vi.fn(),
  deletePreset: vi.fn(),
  openContainingFolder: vi.fn(),
}));

const SETTINGS = {
  settings: {
    general: { defaultOutputDirectory: "D:\\Converted" },
    processing: { defaultCompressionQuality: "medium" },
  },
};

function fileInfo(path: string, category: string, extension: string) {
  return {
    path,
    filename: path.split("\\").pop() as string,
    extension,
    category,
    sizeBytes: 1024,
  };
}

function renderConvert() {
  return render(
    <NavigationProvider>
      <ConvertPage />
    </NavigationProvider>,
  );
}

async function click(name: string | RegExp) {
  await act(async () => {
    fireEvent.click(screen.getByRole("button", { name }));
  });
}

// The core-event callback useJobs registers, so a test can push job lifecycle events at
// the page the same way the real core does.
let emitCoreEvent: ((event: unknown) => void) | null = null;

beforeEach(() => {
  vi.clearAllMocks();
  emitCoreEvent = null;
  vi.mocked(coreClient.subscribeToJobEvents).mockImplementation((callback) => {
    emitCoreEvent = callback as (event: unknown) => void;
    return () => {};
  });
  vi.mocked(coreClient.listJobs).mockResolvedValue({ jobs: [] } as never);
  vi.mocked(coreClient.listPresets).mockResolvedValue({ presets: [] } as never);
  vi.mocked(coreClient.getSettings).mockResolvedValue(SETTINGS as never);
  vi.mocked(coreClient.createConversionJob).mockImplementation((params) =>
    Promise.resolve({ jobId: `job-${(params as { inputPath: string }).inputPath}` }) as never,
  );
});

describe("ConvertPage batch selection", () => {
  it("keeps every file from a multi-file pick, not just the first", async () => {
    vi.mocked(openFilePicker).mockResolvedValue(["D:\\a.mp4", "D:\\b.mp4", "D:\\c.mp4"] as never);
    vi.mocked(coreClient.inspectFile).mockImplementation((path) =>
      Promise.resolve({ fileInfo: fileInfo(path, "VIDEO", "mp4") }) as never,
    );

    renderConvert();
    await click("Choose files...");

    expect(await screen.findByText("3 files selected")).toBeTruthy();
    expect(screen.getByText("a.mp4")).toBeTruthy();
    expect(screen.getByText("c.mp4")).toBeTruthy();
    // The picker must be opened in multi-select mode in the first place -- that single
    // argument is the whole difference between "batch" and "one file at a time".
    expect(vi.mocked(openFilePicker).mock.calls[0][0]).toMatchObject({ multiple: true });
  });

  it("expands a chosen folder into its convertible files and reports what it skipped", async () => {
    vi.mocked(openFilePicker).mockResolvedValue("D:\\Clips" as never);
    vi.mocked(coreClient.listFolderFiles).mockResolvedValue({
      files: [fileInfo("D:\\Clips\\one.mp4", "VIDEO", "mp4"), fileInfo("D:\\Clips\\two.mp4", "VIDEO", "mp4")],
      skipped: 3,
      truncated: false,
    } as never);

    renderConvert();
    await click("Choose a folder...");

    expect(await screen.findByText("2 files selected")).toBeTruthy();
    expect(coreClient.listFolderFiles).toHaveBeenCalledWith("D:\\Clips");
    expect(screen.getByText(/3 files skipped/)).toBeTruthy();
  });

  it("expands a path that turns out to be a folder rather than writing it off", async () => {
    // The drag-and-drop payload for "the user dragged a folder in" is just that folder's
    // path -- nothing in it says directory. Inspecting it comes back as a non-convertible
    // category (a folder has no extension), which is the cue to try listing it before
    // giving up on it. Driven here through the picker, which shares the same code path.
    vi.mocked(openFilePicker).mockResolvedValue(["D:\\Clips"] as never);
    vi.mocked(coreClient.inspectFile).mockResolvedValue({
      fileInfo: fileInfo("D:\\Clips", "UNKNOWN", ""),
    } as never);
    vi.mocked(coreClient.listFolderFiles).mockResolvedValue({
      files: [fileInfo("D:\\Clips\\one.mp4", "VIDEO", "mp4")],
      skipped: 0,
      truncated: false,
    } as never);

    renderConvert();
    await click("Choose files...");

    expect(await screen.findByText("1 file selected")).toBeTruthy();
    expect(screen.getByText("one.mp4")).toBeTruthy();
    expect(coreClient.listFolderFiles).toHaveBeenCalledWith("D:\\Clips");
  });

  it("does not queue the same path twice", async () => {
    vi.mocked(coreClient.inspectFile).mockImplementation((path) =>
      Promise.resolve({ fileInfo: fileInfo(path, "VIDEO", "mp4") }) as never,
    );

    renderConvert();
    vi.mocked(openFilePicker).mockResolvedValue(["D:\\a.mp4", "D:\\b.mp4"] as never);
    await click("Choose files...");
    expect(await screen.findByText("2 files selected")).toBeTruthy();

    vi.mocked(openFilePicker).mockResolvedValue(["D:\\b.mp4", "D:\\c.mp4"] as never);
    await click("Add files");
    expect(await screen.findByText("3 files selected")).toBeTruthy();
  });
});

describe("ConvertPage batch submission", () => {
  async function pickThreeVideos() {
    vi.mocked(openFilePicker).mockResolvedValue(["D:\\a.mp4", "D:\\b.mp4", "D:\\c.mp4"] as never);
    vi.mocked(coreClient.inspectFile).mockImplementation((path) =>
      Promise.resolve({ fileInfo: fileInfo(path, "VIDEO", "mp4") }) as never,
    );
    renderConvert();
    await click("Choose files...");
    await screen.findByText("3 files selected");
  }

  it("creates one job per file in list order from a single click", async () => {
    await pickThreeVideos();
    await waitFor(() => expect(screen.getByDisplayValue("D:\\Converted")).toBeTruthy());

    await click("Convert 3 files");

    const calls = vi.mocked(coreClient.createConversionJob).mock.calls;
    expect(calls).toHaveLength(3);
    expect(calls.map((c) => (c[0] as { inputPath: string }).inputPath)).toEqual([
      "D:\\a.mp4",
      "D:\\b.mp4",
      "D:\\c.mp4",
    ]);
    expect(calls[0][0]).toMatchObject({ outputDirectory: "D:\\Converted", options: { outputFormat: "mp4" } });
    expect(await screen.findByText(/0 of 3 done/)).toBeTruthy();
  });

  it("carries on past a file the core rejects instead of losing the rest of the batch", async () => {
    await pickThreeVideos();
    await waitFor(() => expect(screen.getByDisplayValue("D:\\Converted")).toBeTruthy());

    vi.mocked(coreClient.createConversionJob).mockImplementation((params) => {
      const { inputPath } = params as { inputPath: string };
      if (inputPath === "D:\\b.mp4") {
        return Promise.reject({
          code: "E_INPUT_FILE_NOT_FOUND",
          category: "FILE_NOT_FOUND",
          message: "The input file does not exist.",
          details: "",
          recoverable: false,
        }) as never;
      }
      return Promise.resolve({ jobId: `job-${inputPath}` }) as never;
    });

    await click("Convert 3 files");

    expect(vi.mocked(coreClient.createConversionJob).mock.calls).toHaveLength(3);
    expect(await screen.findByText(/1 failed/)).toBeTruthy();
  });

  it("gives a mixed batch one output format per category", async () => {
    vi.mocked(openFilePicker).mockResolvedValue(["D:\\a.mp4", "D:\\song.mp3"] as never);
    vi.mocked(coreClient.inspectFile).mockImplementation((path) =>
      Promise.resolve({
        fileInfo: path.endsWith(".mp3") ? fileInfo(path, "AUDIO", "mp3") : fileInfo(path, "VIDEO", "mp4"),
      }) as never,
    );

    renderConvert();
    await click("Choose files...");
    await screen.findByText("2 files selected");
    await waitFor(() => expect(screen.getByDisplayValue("D:\\Converted")).toBeTruthy());

    // One dropdown each: converting an mp3 "to mp4" is not what a single shared format
    // field would have meant, it is just a job that fails.
    expect(screen.getByLabelText("Output format (Video)")).toBeTruthy();
    fireEvent.change(screen.getByLabelText("Output format (Audio)"), { target: { value: "flac" } });

    await click("Convert 2 files");

    const byPath = new Map(
      vi.mocked(coreClient.createConversionJob).mock.calls.map((c) => {
        const params = c[0] as { inputPath: string; options: { outputFormat: string } };
        return [params.inputPath, params.options.outputFormat];
      }),
    );
    expect(byPath.get("D:\\a.mp4")).toBe("mp4");
    expect(byPath.get("D:\\song.mp3")).toBe("flac");
  });

  it("offers to cancel everything still running, not one job at a time", async () => {
    await pickThreeVideos();
    await waitFor(() => expect(screen.getByDisplayValue("D:\\Converted")).toBeTruthy());

    vi.mocked(coreClient.getJob).mockImplementation((jobId) =>
      Promise.resolve({
        job: {
          id: jobId,
          type: "CONVERSION",
          state: "RUNNING",
          priority: 0,
          attempts: 1,
          progress: { statusMessage: "Converting", percentage: 10 },
          metadata: {},
          createdAt: "2026-01-01T00:00:00Z",
        },
      }) as never,
    );
    vi.mocked(coreClient.cancelJob).mockResolvedValue({} as never);

    await click("Convert 3 files");

    // Three jobs really running, delivered the way the core delivers them.
    await act(async () => {
      for (const path of ["D:\\a.mp4", "D:\\b.mp4", "D:\\c.mp4"]) {
        emitCoreEvent?.({
          event: "jobStarted",
          jobId: `job-${path}`,
          timestamp: "2026-01-01T00:00:00Z",
          data: { state: "RUNNING" },
        });
      }
    });

    const cancelAll = await screen.findByRole("button", { name: /Cancel remaining \(3\)/ });
    await act(async () => {
      fireEvent.click(cancelAll);
    });

    expect(
      vi
        .mocked(coreClient.cancelJob)
        .mock.calls.map((c) => c[0])
        .sort(),
    ).toEqual(["job-D:\\a.mp4", "job-D:\\b.mp4", "job-D:\\c.mp4"]);
  });
});
