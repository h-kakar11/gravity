#pragma once

// Filename hygiene for downloaded/generated files (spec sections 21, 36): making an
// arbitrary video/playlist title safe as a Windows filename, resolving collisions, and
// numbering playlist entries.

#include <functional>
#include <string>

#include "core/filesystem/IFileSystem.h"

namespace mediatool::filesystem {

// Strips/replaces the Windows-illegal characters (< > : " / \ | ? * and ASCII control
// characters), trims trailing dots/spaces (Windows rejects names ending in either), and
// caps length to ~200 characters. Never touches non-ASCII bytes, so Unicode/emoji in
// `rawTitle` pass through untouched. Returns "untitled" if nothing legal survives.
std::string SanitizeWindowsFilename(const std::string& rawTitle);

// A name that is not on disk but is nonetheless unavailable -- because another in-flight
// job has already claimed it. Deduplication alone cannot see those: two jobs choosing a
// name at the same moment both find it free and both pick it. See OutputNameRegistry.h.
using NameTakenPredicate = std::function<bool(const std::string& candidate)>;

// Returns `desiredPath` unchanged if it doesn't already exist; otherwise finds the
// first free "<name> (N).<ext>" variant by probing fs.Exists() with increasing N.
//
// The `alsoTaken` overload additionally treats any candidate it accepts as occupied. It is
// given the full candidate path.
std::string DeduplicateFilename(const std::string& desiredPath, const IFileSystem& fs);
std::string DeduplicateFilename(const std::string& desiredPath, const IFileSystem& fs,
                                const NameTakenPredicate& alsoTaken);

// Like DeduplicateFilename, but for callers who don't know the final extension yet
// (e.g. a downloader whose output container is chosen by an external tool after the
// fact -- spec section 29). Returns `desiredBaseName` unchanged if no file in
// `directory` starts with it; otherwise finds the first "<name> (N)" variant with no
// existing file of that base name, checked via fs.ListDirectory() rather than
// fs.Exists() (which would need to know the extension).
std::string DeduplicateBaseName(const std::string& directory, const std::string& desiredBaseName,
                                 const IFileSystem& fs);
// As above; `alsoTaken` is given the candidate base name (no directory, no extension).
std::string DeduplicateBaseName(const std::string& directory, const std::string& desiredBaseName,
                                 const IFileSystem& fs, const NameTakenPredicate& alsoTaken);

// Prefixes `filename` with `index` zero-padded to totalCount's digit width, e.g.
// index 3 of 42 -> "03 - <filename>".
std::string WithPlaylistIndex(const std::string& filename, int index, int totalCount);

}  // namespace mediatool::filesystem
