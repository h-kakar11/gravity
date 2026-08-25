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
#include <string>

#include "core/process/IProcessRunner.h"

namespace mediatool::media {

std::optional<std::string> DiscoverFfmpegPath(
    process::IProcessRunner& runner,
    const std::optional<std::string>& overridePath = std::nullopt);

std::optional<std::string> DiscoverFfprobePath(
    process::IProcessRunner& runner,
    const std::optional<std::string>& overridePath = std::nullopt);

}  // namespace mediatool::media
