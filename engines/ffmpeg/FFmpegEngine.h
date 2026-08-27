#pragma once

// Implementation of media::IMediaEngine (spec section 16). The only thing in the codebase
// allowed to invoke ffmpeg/ffprobe, always through the injected IProcessRunner with a
// structured argv vector -- never a shell string. Probe()/Convert()/Compress() are fully
// implemented (Phase 2.6) -- Compress is Convert with different default option values, not
// a different code path, see RunFfmpegJob(). ExtractAudio/ExtractFrames still intentionally
// throw errors::MediaToolException{ErrorCategory::UnsupportedFormat, ...}; still out of
// scope (spec section 16: "do NOT implement every operation now").

#include <optional>
#include <set>
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

    // The set of encoder names the resolved ffmpeg binary reports (see
    // FFmpegDiscovery::DiscoverAvailableEncoders). Computed once, on first use, and
    // cached -- not part of IMediaEngine (Convert/Compress-specific), but public so the
    // Settings/Hardware-Acceleration UI (Phase 4) can surface exactly which encoder will
    // actually be used rather than a bare on/off toggle.
    const std::set<std::string>& AvailableEncoders() const;

private:
    process::IProcessRunner& runner_;
    std::optional<std::string> overrideFfmpegPath_;
    std::optional<std::string> overrideFfprobePath_;
    mutable std::optional<std::set<std::string>> availableEncodersCache_;
    // Outer optional = "not yet resolved this process lifetime"; inner optional = the
    // resolved path itself (DiscoverFfmpegPath/DiscoverFfprobePath's own "not found" case).
    // Same "one discovery path, one lifetime" principle FFmpegDiscovery.h already documents
    // for DiscoverAvailableEncoders -- resolution previously re-ran (and re-spawned `where`)
    // on every single call (issue #20).
    mutable std::optional<std::optional<std::string>> ffmpegPathCache_;
    mutable std::optional<std::optional<std::string>> ffprobePathCache_;

    std::optional<std::string> ResolveFfmpegPath() const;
    std::optional<std::string> ResolveFfprobePath() const;

    // Shared by Convert() and Compress() -- see FFmpegArgBuilder.h for why these are not
    // structurally different operations.
    void RunFfmpegJob(const std::string& inputPath, const std::string& outputPath,
                      const nlohmann::json& options, ProgressCallback onProgress,
                      CancelledCallback isCancelled);

    [[noreturn]] void ThrowNotImplemented(const std::string& operation) const;
};

}  // namespace mediatool::media
