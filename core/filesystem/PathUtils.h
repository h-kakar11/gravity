#pragma once

// Platform-safe path helpers built on std::filesystem (spec section 11) -- never
// manually concatenate path strings; go through Join()/Normalize() instead so
// separators, "." / ".." segments, and absolute-path overrides are handled by the
// standard library rather than ad-hoc string logic.

#include <string>
#include <vector>

namespace mediatool::filesystem::paths {

// Joins `base` with `component` the way std::filesystem::path::operator/ does: if
// `component` is itself absolute, it replaces `base` entirely (mirrors
// std::filesystem's own semantics rather than surprising callers with two different
// behaviors).
std::string Join(const std::string& base, const std::string& component);

// Left-to-right fold of Join() over `components`.
std::string Join(const std::string& base, const std::vector<std::string>& components);

bool IsAbsolute(const std::string& path);

// Windows path-shape checks implemented as plain string logic rather than via
// std::filesystem::path::is_absolute()/IsAbsolute() above: that call's result depends on
// the *host* platform's path grammar (e.g. it does not recognize "C:\Users\..." as
// absolute when built on a POSIX host), which is wrong for software that only ever runs
// on Windows and needs a consistent answer regardless of what platform it happens to be
// built/tested on.
bool LooksAbsoluteWindowsPath(const std::string& path);
bool IsUncPath(const std::string& path);

// Gate for any path an IPC caller supplies directly (an output directory, a file to
// inspect) before it's handed to the filesystem (spec/audit #11: these were previously
// accepted completely unvalidated, including "../" traversal and UNC paths, letting
// inspectFile stat arbitrary files anywhere the process has permission to read). Requires
// an absolute, well-formed Windows-style path with no ".." segment anywhere in it
// (checked as literal path segments split on either separator, not resolved against the
// real filesystem or the process's CWD -- a relative path is rejected outright rather
// than resolved, since "relative to what?" is itself the kind of fragile CWD assumption
// #7 already flags). UNC paths (`\\server\share\...`) are rejected unless
// `allowNetworkPaths` is true (wired to AdvancedSettings::allowNetworkPaths, off by
// default).
bool IsSafeUserSuppliedPath(const std::string& path, bool allowNetworkPaths);

// Lexically collapses "." / ".." segments and redundant separators without touching
// the filesystem (safe to call on paths that don't exist yet, e.g. an output path
// that hasn't been created). Does not resolve symlinks.
std::string Normalize(const std::string& path);

// No leading dot, lowercase (matches IFileSystem::GetExtension's contract).
std::string GetExtension(const std::string& path);

// Filename with extension, no directory component.
std::string GetFilename(const std::string& path);

std::string GetParentDirectory(const std::string& path);

}  // namespace mediatool::filesystem::paths
