// Classifying a pasted link by shape alone, before any network call, so the Downloader page
// knows whether to offer the "just this video / the whole playlist" choice (issue #41).
//
// This is deliberately only a UI hint, never the gate. The authoritative answer comes from
// the backend: `inspect` fails with E_PLAYLIST_NOT_SUPPORTED for a URL that turns out to be
// a playlist, and `inspectPlaylist` fails with E_NOT_A_PLAYLIST for one that turns out to be
// a single video. That fallback is what makes playlists work on sites whose URL shapes are
// nothing like YouTube's; this function exists so the common YouTube case can present the
// choice up front instead of making the user discover it through an error.

export interface PlaylistUrlShape {
  // The URL carries a playlist reference (YouTube's `list=`).
  hasPlaylist: boolean;
  // The URL also identifies one specific video -- the "shared from a playlist" case, where
  // downloading the whole list would almost certainly be the wrong guess.
  hasVideo: boolean;
}

// Path-based single-video forms that carry the id in the path rather than a `v=` param.
const VIDEO_PATH_PREFIXES = ["/shorts/", "/embed/", "/live/", "/v/"];

export function analyzePlaylistUrl(raw: string): PlaylistUrlShape {
  let parsed: URL;
  try {
    parsed = new URL(raw.trim());
  } catch {
    // Not a URL yet (the user is still typing, or pasted something else) -- no hint to give.
    return { hasPlaylist: false, hasVideo: false };
  }

  const hasPlaylist = parsed.searchParams.has("list");
  if (!hasPlaylist) return { hasPlaylist: false, hasVideo: false };

  // youtu.be/<id> puts the video id in the path with nothing to distinguish it from a
  // playlist path, so treat any non-empty path on that host as a video reference.
  const isShortHost = parsed.hostname === "youtu.be" || parsed.hostname === "www.youtu.be";
  const hasVideo =
    parsed.searchParams.has("v") ||
    (isShortHost && parsed.pathname.replace(/\/+$/, "").length > 1) ||
    VIDEO_PATH_PREFIXES.some(
      (prefix) => parsed.pathname.startsWith(prefix) && parsed.pathname.length > prefix.length,
    );

  return { hasPlaylist, hasVideo };
}

// Strips the playlist reference from a combo URL, leaving the single video it points at.
// Used when the user answers "just this video": the backend's `noplaylist` option already
// resolves a combo URL to the one video, so this is belt-and-braces plus a clearer record of
// what was actually requested.
export function withoutPlaylistParam(raw: string): string {
  try {
    const parsed = new URL(raw.trim());
    parsed.searchParams.delete("list");
    parsed.searchParams.delete("index");
    parsed.searchParams.delete("start_radio");
    return parsed.toString();
  } catch {
    return raw.trim();
  }
}

// Joins a Windows output directory with a chosen subfolder name. The backend takes paths
// verbatim, so normalizing the separator here keeps "C:\out" and "C:\out\" from producing
// two different destinations.
export function joinWindowsPath(directory: string, name: string): string {
  const trimmed = directory.replace(/[\\/]+$/, "");
  return `${trimmed}\\${name}`;
}
