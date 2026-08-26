#pragma once

// Scripted, no-process IMediaEngine for unit tests (spec section 37) -- MediaProcessingJob
// tests use this instead of a real FFmpegEngine/ffmpeg binary, mirroring
// MockDownloadProvider's role for DownloadJob.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/errors/ErrorInfo.h"
#include "core/media/IMediaEngine.h"

namespace mediatool::media {

class MockMediaEngine : public IMediaEngine {
public:
    bool IsAvailable() const override { return available; }
    std::optional<std::string> Version() const override { return version; }

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

    // --- scripting -----------------------------------------------------------------
    bool available = true;
    std::optional<std::string> version = "mock-ffmpeg";

    filesystem::FileInfo probeResult;
    std::optional<errors::ErrorInfo> probeError;  // if set, Probe() throws this instead

    std::vector<jobs::Progress> progressSequence;       // replayed in order during Convert/Compress
    std::optional<errors::ErrorInfo> processingError;   // if set, Convert/Compress throws this
    // Invoked as the first thing Convert()/Compress() does, with the exact output path
    // that will be used -- lets a test simulate the output file "appearing", mirroring
    // MockDownloadProvider::onDownloadStart's role.
    std::function<void(const std::string& outputPath)> onProcessingStart;

    // --- observation -----------------------------------------------------------------
    std::optional<std::string> lastInputPath;
    std::optional<std::string> lastOutputPath;
    std::optional<nlohmann::json> lastOptions;
    bool lastCallWasCompress = false;

private:
    void RunScripted(const std::string& inputPath, const std::string& outputPath,
                     const nlohmann::json& options, bool isCompress, ProgressCallback onProgress,
                     CancelledCallback isCancelled);
};

}  // namespace mediatool::media
