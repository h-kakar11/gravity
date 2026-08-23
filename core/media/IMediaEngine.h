#pragma once

// One of the five mockable interfaces called out in spec section 37. FFmpegEngine
// (engines/ffmpeg) is the Phase 1 implementation; MockMediaEngine (tests) is a
// scripted stand-in so job/engine wiring can be tested without a real ffmpeg binary.
//
// Phase 1 scope (spec section 16): Probe() must work end-to-end against a real ffprobe
// when one is discovered. Convert/Compress/ExtractAudio/ExtractFrames are declared for
// interface completeness but are NOT required to do real work yet -- a correct Phase 1
// implementation throws errors::MediaToolException with
// ErrorCategory::UnsupportedFormat and a message that says so plainly. Do not fake a
// success result for an operation that doesn't actually run ffmpeg.

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
