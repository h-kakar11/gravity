#pragma once

// Locating the sidecar executables and scripts the core launches (the Python downloader
// interpreter and downloader.py today) -- issue #79.
//
// The failure this exists to prevent: every path was resolved as either a raw environment
// variable or a *CWD-relative* literal ("python/downloader/.venv/Scripts/python.exe"),
// handed straight to CreateProcess, and never checked. The core's working directory is
// whatever the Tauri shell happened to inherit -- app/desktop/src-tauri under `tauri dev`,
// the shortcut's "Start in" for an installed build, C:\Windows\system32 for a few launch
// paths -- and essentially never the repository root the relative default was written
// against. When it didn't resolve, the only thing the user saw was CreateProcess's own
// "The system cannot find the file specified", naming a path that (from the repo root)
// looks perfectly valid.
//
// Two changes, both here: candidates are anchored to the CORE EXECUTABLE'S OWN DIRECTORY
// rather than the CWD, and every candidate is existence-checked before launch so a
// failure can name what was tried instead of what CreateProcess happened to be handed.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mediatool::filesystem {

// Strips surrounding whitespace and one matched pair of surrounding double quotes from an
// environment variable's value. `set MEDIATOOL_PYTHON_PATH="C:\...\python.exe"` in a batch
// file stores the quotes as part of the value, which then reaches CreateProcess as part of
// the filename and cannot possibly resolve -- while `echo %MEDIATOOL_PYTHON_PATH%` looks
// exactly right to whoever set it.
std::string CleanEnvPathValue(const std::string& raw);

// How far up from the executable's own directory to look for a `relativeCandidates` match.
// A packaged build resolves at level 0 (resources sit beside the exe); a dev build's core
// lives at build/<preset>/app/core/, four levels below the repository root.
inline constexpr int kMaxAncestorLevels = 6;

// The ordered list of absolute-or-relative paths to try for one tool:
//   1. `envValue` (cleaned), when non-empty -- an explicit override always wins.
//   2. each of `relativeCandidates` joined onto `executableDirectory` and each ancestor
//      directory up to kMaxAncestorLevels.
//   3. each of `relativeCandidates` unchanged -- i.e. CWD-relative, the pre-#79 behavior,
//      kept last so nothing that resolved before stops resolving now.
// Duplicates are removed while preserving first-occurrence order, so the diagnostic list
// a failure prints doesn't repeat itself.
std::vector<std::string> BuildToolCandidates(const std::string& envValue,
                                             const std::string& executableDirectory,
                                             const std::vector<std::string>& relativeCandidates);

// The first candidate `exists` accepts, or nullopt if none do.
std::optional<std::string> FirstExisting(const std::vector<std::string>& candidates,
                                          const std::function<bool(const std::string&)>& exists);

// The directory containing this process's own executable, or "" if the platform wouldn't
// say. Deliberately not derived from argv[0], which is whatever the parent process chose
// to pass and need not be a path at all.
std::string ExecutableDirectory();

}  // namespace mediatool::filesystem
