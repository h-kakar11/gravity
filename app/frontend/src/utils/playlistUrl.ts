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
  // The `list=` names one of YouTube's auto-generated mixes (radios) rather than a real
  // playlist: an endless stream synthesized around a seed video, which has no "all of it"
  // to download. Offering "the whole playlist" for one of these is how a 40-song playlist
  // came to be reported as hundreds of videos -- the enumeration only stops at the cap.
  // Same caveat as the rest of this file: a hint, not the gate. The backend rejects a mix
  // with E_PLAYLIST_IS_MIX regardless of what this says.
  isMix: boolean;
}

// Path-based single-video forms that carry the id in the path rather than a `v=` param.
const VIDEO_PATH_PREFIXES = ["/shorts/", "/embed/", "/live/", "/v/"];

// Mirrors normalize_playlist_url / auto_generated_mix_id in python/downloader/downloader.py,
// which is where the reasoning behind each prefix is written down. Kept scoped to YouTube
// hosts for the same reason it is there: "an id starting with RD" is a fact about YouTube's
// URL scheme, not about playlist ids generally.
const YOUTUBE_HOSTS = new Set([
  "youtube.com",
  "www.youtube.com",
  "m.youtube.com",
  "music.youtube.com",
  "youtu.be",
  "www.youtu.be",
  "youtube-nocookie.com",
  "www.youtube-nocookie.com",
]);

// RDCLAK... are YouTube Music's curated playlists -- RD-prefixed but genuinely finite.
// RDAMPL<playlist id> is the radio seeded from a real playlist, which the backend rewrites
// back to that playlist, so the whole-playlist choice stays meaningful for it.
const NOT_A_MIX_PREFIXES = ["RDCLAK", "RDAMPL"];

function isMixListId(listId: string): boolean {
  if (!listId.startsWith("RD")) return false;
  return !NOT_A_MIX_PREFIXES.some((prefix) => listId.startsWith(prefix));
}

export function analyzePlaylistUrl(raw: string): PlaylistUrlShape {
  let parsed: URL;
  try {
    parsed = new URL(raw.trim());
  } catch {
    // Not a URL yet (the user is still typing, or pasted something else) -- no hint to give.
    return { hasPlaylist: false, hasVideo: false, isMix: false };
  }

  const listId = parsed.searchParams.get("list");
  if (listId === null) return { hasPlaylist: false, hasVideo: false, isMix: false };

  // youtu.be/<id> puts the video id in the path with nothing to distinguish it from a
  // playlist path, so treat any non-empty path on that host as a video reference.
  const isShortHost = parsed.hostname === "youtu.be" || parsed.hostname === "www.youtu.be";
  const hasVideo =
    parsed.searchParams.has("v") ||
    (isShortHost && parsed.pathname.replace(/\/+$/, "").length > 1) ||
    VIDEO_PATH_PREFIXES.some(
      (prefix) => parsed.pathname.startsWith(prefix) && parsed.pathname.length > prefix.length,
    );

  const isMix = YOUTUBE_HOSTS.has(parsed.hostname.toLowerCase()) && isMixListId(listId);

  return { hasPlaylist: true, hasVideo, isMix };
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
