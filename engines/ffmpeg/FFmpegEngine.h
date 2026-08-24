#pragma once

// The implementation of media::IMediaEngine (spec section 16). The only thing in the
// codebase allowed to invoke ffmpeg/ffprobe, always through the injected IProcessRunner
// with a structured argv vector -- never a shell string.
//
// Probe(), Convert() and Compress() are fully implemented. ExtractAudio/ExtractFrames
// still throw errors::MediaToolException{ErrorCategory::UnsupportedFormat, ...}: they are
// declared for interface completeness and are honestly reported as unimplemented rather
// than faked (see docs/roadmap.md).
//
// Convert/Compress both encode into an AtomicWriter temporary path and rename over the
// destination only after the output has been probed back and found valid, so a cancelled,
// failed, or crashed encode never leaves a half-written file where the user expects
// finished output.

#include <optional>
#include <string>
#include <vector>

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

    // How long an encode may go without ffmpeg emitting a progress block before it is
    // treated as hung and killed (spec section 43). ffmpeg emits one roughly every half
    // second while working, so the default is generous rather than tight. <= 0 disables
    // the watchdog. Tests lower it to prove the escalation path.
    void SetStallTimeoutMs(int milliseconds) { stallTimeoutMs_ = milliseconds; }

    // How long a terminate request is given before escalating to a hard kill.
    void SetTerminateGraceMs(int milliseconds) { terminateGraceMs_ = milliseconds; }

private:
    process::IProcessRunner& runner_;
    std::optional<std::string> overrideFfmpegPath_;
    std::optional<std::string> overrideFfprobePath_;
    int stallTimeoutMs_ = 120'000;
    int terminateGraceMs_ = 5'000;

    std::optional<std::string> ResolveFfmpegPath() const;
    std::optional<std::string> ResolveFfprobePath() const;

    // Shared body of Convert() and Compress(): resolve ffmpeg, probe the input for a
    // duration to turn ffmpeg's out_time into a percentage, run `args` (which must already
    // name an AtomicWriter temporary path as its output), stream progress, honour
    // cancellation with a bounded terminate -> kill escalation, verify the produced file,
    // then commit it over `outputPath`. `operation` only labels errors/status messages.
    void RunEncode(const std::string& operation, const std::string& inputPath,
                   const std::string& outputPath,
                   const std::function<std::vector<std::string>(const std::string& tempPath,
                                                                const filesystem::FileInfo& probed)>&
                       buildArgs,
                   ProgressCallback onProgress, CancelledCallback isCancelled);

    [[noreturn]] void ThrowNotImplemented(const std::string& operation) const;
};

}  // namespace mediatool::media
