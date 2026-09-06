import { describe, expect, it } from "vitest";
import { analyzePlaylistUrl, joinWindowsPath, withoutPlaylistParam } from "./playlistUrl";

describe("analyzePlaylistUrl", () => {
  it("treats a bare playlist link as a playlist with no specific video", () => {
    const shape = analyzePlaylistUrl("https://www.youtube.com/playlist?list=PL123");
    expect(shape).toEqual({ hasPlaylist: true, hasVideo: false, isMix: false });
  });

  it("detects the shared-from-a-playlist combo link", () => {
    // The case the whole prompt exists for: downloading the entire list here would be a
    // wrong guess, so the page has to ask instead.
    const shape = analyzePlaylistUrl("https://www.youtube.com/watch?v=abc123&list=PL123");
    expect(shape).toEqual({ hasPlaylist: true, hasVideo: true, isMix: false });
  });

  it("detects a combo link on the youtu.be short host", () => {
    expect(analyzePlaylistUrl("https://youtu.be/abc123?list=PL123")).toEqual({
      hasPlaylist: true,
      hasVideo: true,
      isMix: false,
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
      isMix: false,
    });
  });

  it("does not throw on text that is not a URL yet", () => {
    expect(analyzePlaylistUrl("not a url")).toEqual({ hasPlaylist: false, hasVideo: false, isMix: false });
    expect(analyzePlaylistUrl("")).toEqual({ hasPlaylist: false, hasVideo: false, isMix: false });
  });

  // A mix ("radio") is an endless auto-generated stream, not a list -- enumerating one runs
  // to the 500-entry cap, which is how a 40-song playlist got reported as hundreds of
  // videos. The backend refuses these outright (E_PLAYLIST_IS_MIX); this flag is what stops
  // the page offering "the whole playlist" for them in the first place.
  it("flags a video-seeded mix link", () => {
    const shape = analyzePlaylistUrl("https://www.youtube.com/watch?v=abc123&list=RDabc123&start_radio=1");
    expect(shape).toEqual({ hasPlaylist: true, hasVideo: true, isMix: true });
  });

  it("flags a YouTube Music radio link", () => {
    expect(analyzePlaylistUrl("https://music.youtube.com/watch?v=abc123&list=RDAMVMabc123").isMix).toBe(true);
  });

  it("does not flag a curated RD-prefixed music playlist", () => {
    // RDCLAK... is RD-prefixed but a real, finite list.
    expect(analyzePlaylistUrl("https://music.youtube.com/playlist?list=RDCLAK5uy_abc").isMix).toBe(false);
  });

  it("does not flag a playlist-seeded mix, which the backend rewrites to its playlist", () => {
    expect(analyzePlaylistUrl("https://music.youtube.com/watch?v=abc&list=RDAMPLPL123").isMix).toBe(false);
  });

  it("does not flag an RD-prefixed list on a non-YouTube host", () => {
    expect(analyzePlaylistUrl("https://example.com/watch?v=abc&list=RDabc").isMix).toBe(false);
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
