#pragma once

// Phase 1 implementation of media::IMediaEngine (spec section 16). The only thing in the
// codebase allowed to invoke ffmpeg/ffprobe, always through the injected IProcessRunner
// with a structured argv vector -- never a shell string. Probe() is fully implemented;
// Convert/Compress/ExtractAudio/ExtractFrames intentionally throw
// errors::MediaToolException{ErrorCategory::UnsupportedFormat, ...} -- out of scope for
// Phase 1 (spec section 16: "do NOT implement every operation now").

#include <optional>
#include <string>

#include "core/media/IMediaEngine.h"
#include "core/process/IProcessRunner.h"

namespace mediatool::media {

class FFmpegEngine : public IMediaEngine {
public:
    // `overrideFfmpegPath`/`overrideFfprobePath` let a caller (e.g. Settings) pin an
    // explicit binary path; leave empty to resolve via PATH through `runner`
    // (see FFmpegDiscovery.h). `runner` must outlive this engine.
    explicit FFmpegEngine(process::IProcessRunner& runner,
                          std::optional<std::string> overrideFfmpegPath = std::nullopt,
                          std::optional<std::string> overrideFfprobePath = std::nullopt);

    bool IsAvailable() const override;
    std::optional<std::string> Version() const override;

    filesystem::FileInfo Probe(const std::string& path) override;

    void Convert(const std::string& inputPath, const std::string& outputPath,
                const nlohmann::json& options, ProgressCallback onProgress,
                CancelledCallback isCancelled) override;

    void Compress(const std::string& inputPath, const std::string& outputPath,
                 const nlohmann::json& options, ProgressCallback onProgress,
                 CancelledCallback isCancelled) override;

    void ExtractAudio(const std::string& inputPath, const std::string& outputPath,
                      ProgressCallback onProgress, CancelledCallback isCancelled) override;

    void ExtractFrames(const std::string& inputPath, const std::string& outputDir,
                       const nlohmann::json& options, ProgressCallback onProgress,
                       CancelledCallback isCancelled) override;

private:
    process::IProcessRunner& runner_;
    std::optional<std::string> overrideFfmpegPath_;
    std::optional<std::string> overrideFfprobePath_;

    std::optional<std::string> ResolveFfmpegPath() const;
    std::optional<std::string> ResolveFfprobePath() const;

    [[noreturn]] void ThrowNotImplemented(const std::string& operation) const;
};

}  // namespace mediatool::media
