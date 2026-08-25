#pragma once

// Locates the ffmpeg/ffprobe executables (spec section 16, spec section 42: both the
// "found" and "not found" paths must be tested without a real binary).
//
// Discovery strategy: if an explicit override path is supplied (e.g. pinned via
// Settings), it is trusted verbatim and returned without touching the process runner.
// Otherwise the bare command name is resolved by shelling out to the Windows `where`
// utility through the injected IProcessRunner -- `where <name>` prints the absolute path
// of the first match on PATH to stdout and exits non-zero when nothing is found. Using
// `where` (a real executable, not a shell builtin) keeps this a structured-argv process
// launch like every other invocation in the codebase, and keeps discovery fully
// testable against a fake IProcessRunner (spec section 37) with no real ffmpeg install.
//
// Never throws: any launch failure or "not found" result becomes std::nullopt.

#include <optional>
#include <set>
#include <string>

#include "core/process/IProcessRunner.h"

namespace mediatool::media {

std::optional<std::string> DiscoverFfmpegPath(
    process::IProcessRunner& runner,
    const std::optional<std::string>& overridePath = std::nullopt);

std::optional<std::string> DiscoverFfprobePath(
    process::IProcessRunner& runner,
    const std::optional<std::string>& overridePath = std::nullopt);

// Runs `<ffmpegPath> -hide_banner -encoders` and returns the set of encoder names it
// reports (e.g. "libx264", "libopenh264", "h264_nvenc") -- used once at FFmpegEngine
// construction, cached, and never re-probed per job (spec/audit #20's "one discovery
// path, one lifetime" principle applied to encoder capability the same way it already
// applies to the binary path itself). Returns an empty set (never throws) if `ffmpegPath`
// can't be launched or produces output this can't parse -- callers must treat an empty
// set as "assume nothing but the bundled default is available", not as an error.
std::set<std::string> DiscoverAvailableEncoders(process::IProcessRunner& runner, const std::string& ffmpegPath);

}  // namespace mediatool::media
