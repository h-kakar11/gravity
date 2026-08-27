#pragma once

// One of the five mockable interfaces called out in spec section 37. FFmpegEngine
// (engines/ffmpeg) is the implementation; MockMediaEngine (tests) is a scripted stand-in
// so job/engine wiring can be tested without a real ffmpeg binary.
//
// Phase 1 scope (spec section 16) required only Probe() to work end-to-end against a real
// ffprobe; Convert/Compress/ExtractAudio/ExtractFrames were declared for interface
// completeness with no obligation to do real work yet. That's now only true for
// ExtractAudio/ExtractFrames -- Convert and Compress were implemented for real in Phase
// 2.6 (see FFmpegEngine.h). The original rule still holds for whatever remains
// unimplemented: throw errors::MediaToolException with ErrorCategory::UnsupportedFormat
// and a message that says so plainly, never fake a success result for an operation that
// doesn't actually run ffmpeg.

#include <functional>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/filesystem/FileInfo.h"
#include "core/jobs/Progress.h"

namespace mediatool::media {

using ProgressCallback = std::function<void(const jobs::Progress&)>;
using CancelledCallback = std::function<bool()>;

class IMediaEngine {
public:
    virtual ~IMediaEngine() = default;

    // False if ffmpeg/ffprobe could not be discovered on this machine (spec section 22:
    // the app must still function, just without media processing capability).
    virtual bool IsAvailable() const = 0;
    virtual std::optional<std::string> Version() const = 0;

    // Throws errors::MediaToolException on failure (missing file, ffprobe error, etc).
    virtual filesystem::FileInfo Probe(const std::string& path) = 0;

    virtual void Convert(const std::string& inputPath, const std::string& outputPath,
                         const nlohmann::json& options, ProgressCallback onProgress,
                         CancelledCallback isCancelled) = 0;

    virtual void Compress(const std::string& inputPath, const std::string& outputPath,
                          const nlohmann::json& options, ProgressCallback onProgress,
                          CancelledCallback isCancelled) = 0;

    virtual void ExtractAudio(const std::string& inputPath, const std::string& outputPath,
                             ProgressCallback onProgress, CancelledCallback isCancelled) = 0;

    virtual void ExtractFrames(const std::string& inputPath, const std::string& outputDir,
                               const nlohmann::json& options, ProgressCallback onProgress,
                               CancelledCallback isCancelled) = 0;
};

}  // namespace mediatool::media
