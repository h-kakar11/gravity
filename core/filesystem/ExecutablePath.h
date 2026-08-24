#pragma once

// Resolves the directory containing the currently running executable (spec: Phase 7
// "no CWD dependency"). A packaged Gravity install places mediatool-core.exe, its bundled
// FFmpeg/ffprobe binaries, and the Python downloader resources next to (or beneath) the
// executable Windows actually launched -- never relative to whatever directory a shortcut,
// Start Menu entry, or another process happened to set as the current working directory.
//
// This is the one place that answers "where am I actually running from"; every other
// resource-default (FFmpeg discovery, the Python downloader path) is built from it rather
// than each guessing its own relative path.

#include <optional>
#include <string>

namespace mediatool::filesystem {

// Absolute path to the directory containing the running process's own executable, or
// nullopt if the platform API fails (never throws). Cross-platform: Windows via
// GetModuleFileNameW, POSIX via /proc/self/exe (Linux) so the same code path is exercised
// by the Linux dev/CI build this repository is authored and tested on.
std::optional<std::string> ExecutableDirectory();

}  // namespace mediatool::filesystem
