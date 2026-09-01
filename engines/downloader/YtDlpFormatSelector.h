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

// True if `selector` is safe to hand to yt-dlp's -f verbatim as a USER-SUPPLIED value.
//
// The selectors FormatSelectorForQuality() builds are ours and are not subject to this --
// they legitimately contain the bracket/slash expression syntax this rejects. What this
// gates is downloads::DownloadOptions::formatId, which comes straight from the frontend
// after a user picks a stream out of Inspect()'s list (issue #31) and reaches -f with no
// transformation at all. yt-dlp's -f is a small expression language (filters, arithmetic,
// fallbacks, `all`, and format-id sets), so an unvalidated value there is not a free-text
// field, it is code: "all" downloads every stream on the page, and a filter expression can
// select something the user never saw.
//
// The accepted grammar is therefore the smallest thing that covers a real format id and
// nothing else: one or more '+'-joined ids, each 1-64 characters of [A-Za-z0-9_.-], at
// most 8 of them. That is exactly the shape of what Inspect() reports ("137", "140",
// "hls-1080p", "dash_video-2") and of the "137+140" combo the caller may build from two of
// them. Everything the expression language needs -- whitespace, brackets, parentheses,
// slashes, commas, comparison operators, quotes -- is absent from it by construction.
bool IsSafeFormatSelector(const std::string& selector);

}  // namespace mediatool::downloader
