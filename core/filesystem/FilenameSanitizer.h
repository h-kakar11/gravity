#pragma once

// Filename hygiene for downloaded/generated files (spec sections 21, 36): making an
// arbitrary video/playlist title safe as a Windows filename, resolving collisions, and
// numbering playlist entries.

#include <string>

#include "core/filesystem/IFileSystem.h"

namespace mediatool::filesystem {

// Strips/replaces the Windows-illegal characters (< > : " / \ | ? * and ASCII control
// characters), trims trailing dots/spaces (Windows rejects names ending in either), and
// caps length to ~200 characters. Never touches non-ASCII bytes, so Unicode/emoji in
// `rawTitle` pass through untouched. Returns "untitled" if nothing legal survives.
std::string SanitizeWindowsFilename(const std::string& rawTitle);

// Returns `desiredPath` unchanged if it doesn't already exist; otherwise finds the
// first free "<name> (N).<ext>" variant by probing fs.Exists() with increasing N.
std::string DeduplicateFilename(const std::string& desiredPath, const IFileSystem& fs);

// Prefixes `filename` with `index` zero-padded to totalCount's digit width, e.g.
// index 3 of 42 -> "03 - <filename>".
std::string WithPlaylistIndex(const std::string& filename, int index, int totalCount);

}  // namespace mediatool::filesystem
