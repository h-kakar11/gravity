#pragma once

// Translates the application-level QualityPreset (core/downloads/QualityPreset.h) into a
// concrete yt-dlp `-f` format-selector string. This is the ONLY place in the codebase
// allowed to know yt-dlp's selector syntax (spec section 10) -- kept separate from
// YtDlpProvider.cpp so it's unit-testable without spawning a process.

#include <string>

#include "core/downloads/QualityPreset.h"

namespace mediatool::downloader {

// "Best" means the highest-quality appropriate video plus the best compatible audio,
// merged when necessary (spec section 9) -- NOT simply the single highest-quality
// pre-muxed format, which can be lower quality than a separate-stream combination.
// yt-dlp performs the actual merge via ffmpeg when the selector picks two streams; see
// docs/decisions.md "Video/audio merge strategy" for why that's the ffmpeg integration
// point instead of a second one built into FFmpegEngine.
std::string FormatSelectorForQuality(downloads::QualityPreset preset);

}  // namespace mediatool::downloader
