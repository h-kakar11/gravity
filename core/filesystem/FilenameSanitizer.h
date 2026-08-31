#pragma once

// Filename hygiene for downloaded/generated files (spec sections 21, 36): making an
// arbitrary video/playlist title safe as a Windows filename, resolving collisions, and
// numbering playlist entries.

#include <string>

#include "core/filesystem/IFileSystem.h"

namespace mediatool::filesystem {

// Strips/replaces the Windows-illegal characters (< > : " / \ | ? * and ASCII control
// characters), trims trailing dots/spaces (Windows rejects names ending in either), caps
// length to ~200 characters, and appends a suffix if the result is one of Windows'
// reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9 -- matched case-insensitively
// against the name with any extension stripped, since those are reserved regardless of
// extension, e.g. "NUL.txt" is just as invalid as "NUL"). Never touches non-ASCII bytes,
// so Unicode/emoji in `rawTitle` pass through untouched. Returns "untitled" if nothing
// legal survives.
std::string SanitizeWindowsFilename(const std::string& rawTitle);

// Legacy MAX_PATH (260 including the terminating null, so 259 usable characters) that
// this codebase targets -- see docs/development.md for why full \\?\ long-path opt-in is
// out of scope for now. Truncates `baseName` (preserving its full UTF-8 codepoints, never
// splitting one) so that `directory + "\" + baseName` leaves headroom for a numbered
// dedup suffix like " (9999)" and a reasonably long extension, without needing to know
// either at truncation time. Returns `baseName` unchanged if it already fits, or if
// `directory` alone is already at/over budget (nothing achievable by trimming the name in
// that case -- Windows will surface its own path-length error at write time).
std::string TruncateBaseNameForMaxPath(const std::string& directory, const std::string& baseName);

// Like DeduplicateBaseName's sibling used to be (DeduplicateFilename, removed -- no
// production caller remained once DeduplicateBaseName superseded it, see docs/decisions.md
// "Dead code cleanup"), but for callers who don't know the final extension yet
// (e.g. a downloader whose output container is chosen by an external tool after the
// fact -- spec section 29). Returns `desiredBaseName` unchanged if no file in
// `directory` starts with it; otherwise finds the first "<name> (N)" variant with no
// existing file of that base name, checked via fs.ListDirectory() rather than
// fs.Exists() (which would need to know the extension).
std::string DeduplicateBaseName(const std::string& directory, const std::string& desiredBaseName,
                                 const IFileSystem& fs);

// Prefixes `filename` with `index` zero-padded to totalCount's digit width, e.g.
// index 3 of 42 -> "03 - <filename>".
std::string WithPlaylistIndex(const std::string& filename, int index, int totalCount);

// Used by cleanup-after-failure logic to decide whether `candidateName` (a bare
// filename, no directory component) was created by the job that owns `filenameBase`,
// as opposed to an unrelated pre-existing file that merely starts with the same text
// (e.g. filenameBase "Vacation" must never match a pre-existing "Vacation Photos.zip").
// True only for an exact match, or a match immediately followed by '.' (covering the
// job's own final output, yt-dlp intermediate artifacts like ".mp4.part"/".f137.mp4",
// and sidecar files like ".info.json") -- never a bare prefix match. DeduplicateBaseName
// already guarantees no file predated the job with this exact base, so anything that
// passes this check was created by this run.
bool IsJobArtifactOf(const std::string& filenameBase, const std::string& candidateName);

}  // namespace mediatool::filesystem
