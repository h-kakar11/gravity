import { describe, expect, it } from "vitest";
import { analyzePlaylistUrl, joinWindowsPath, withoutPlaylistParam } from "./playlistUrl";

describe("analyzePlaylistUrl", () => {
  it("treats a bare playlist link as a playlist with no specific video", () => {
    const shape = analyzePlaylistUrl("https://www.youtube.com/playlist?list=PL123");
    expect(shape).toEqual({ hasPlaylist: true, hasVideo: false });
  });

  it("detects the shared-from-a-playlist combo link", () => {
    // The case the whole prompt exists for: downloading the entire list here would be a
    // wrong guess, so the page has to ask instead.
    const shape = analyzePlaylistUrl("https://www.youtube.com/watch?v=abc123&list=PL123");
    expect(shape).toEqual({ hasPlaylist: true, hasVideo: true });
  });

  it("detects a combo link on the youtu.be short host", () => {
    expect(analyzePlaylistUrl("https://youtu.be/abc123?list=PL123")).toEqual({
      hasPlaylist: true,
      hasVideo: true,
    });
  });

  it("detects a combo link for path-based video forms", () => {
    expect(analyzePlaylistUrl("https://www.youtube.com/shorts/abc123?list=PL123").hasVideo).toBe(true);
    expect(analyzePlaylistUrl("https://www.youtube.com/embed/abc123?list=PL123").hasVideo).toBe(true);
  });

  it("reports no playlist for an ordinary single-video link", () => {
    expect(analyzePlaylistUrl("https://www.youtube.com/watch?v=abc123")).toEqual({
      hasPlaylist: false,
      hasVideo: false,
    });
  });

  it("does not throw on text that is not a URL yet", () => {
    expect(analyzePlaylistUrl("not a url")).toEqual({ hasPlaylist: false, hasVideo: false });
    expect(analyzePlaylistUrl("")).toEqual({ hasPlaylist: false, hasVideo: false });
  });
});

describe("withoutPlaylistParam", () => {
  it("strips list/index so the URL names only the video", () => {
    const stripped = withoutPlaylistParam("https://www.youtube.com/watch?v=abc123&list=PL123&index=4");
    expect(stripped).toContain("v=abc123");
    expect(stripped).not.toContain("list=");
    expect(stripped).not.toContain("index=");
  });

  it("returns non-URL input unchanged rather than throwing", () => {
    expect(withoutPlaylistParam("  not a url  ")).toBe("not a url");
  });
});

describe("joinWindowsPath", () => {
  it("joins with a single separator", () => {
    expect(joinWindowsPath("C:\\out", "playlist #1")).toBe("C:\\out\\playlist #1");
  });

  it("does not double the separator when the directory already ends with one", () => {
    expect(joinWindowsPath("C:\\out\\", "playlist #1")).toBe("C:\\out\\playlist #1");
    expect(joinWindowsPath("C:/out/", "playlist #1")).toBe("C:/out\\playlist #1");
  });
});
