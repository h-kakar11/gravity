#include "core/media/MockMediaEngine.h"

#include "core/media/DeferredOperations.h"

#include "core/errors/MediaToolException.h"

namespace mediatool::media {

filesystem::FileInfo MockMediaEngine::Probe(const std::string& path) {
    if (probeError.has_value()) {
        throw errors::MediaToolException(*probeError);
    }
    filesystem::FileInfo result = probeResult;
    result.path = path;
    return result;
}

void MockMediaEngine::RunScripted(const std::string& inputPath, const std::string& outputPath,
                                  const nlohmann::json& options, bool isCompress,
                                  ProgressCallback onProgress, CancelledCallback isCancelled) {
    lastInputPath = inputPath;
    lastOutputPath = outputPath;
    lastOptions = options;
    lastCallWasCompress = isCompress;

    if (onProcessingStart) onProcessingStart(outputPath);

    for (const auto& progress : progressSequence) {
        if (isCancelled && isCancelled()) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_MOCK_PROCESSING_CANCELLED", errors::ErrorCategory::Cancelled, "cancelled"));
        }
        if (onProgress) onProgress(progress);
    }

    if (processingError.has_value()) {
        throw errors::MediaToolException(*processingError);
    }
}

void MockMediaEngine::Convert(const std::string& inputPath, const std::string& outputPath,
                              const nlohmann::json& options, ProgressCallback onProgress,
                              CancelledCallback isCancelled) {
    RunScripted(inputPath, outputPath, options, /*isCompress=*/false, std::move(onProgress),
                std::move(isCancelled));
}

void MockMediaEngine::Compress(const std::string& inputPath, const std::string& outputPath,
                               const nlohmann::json& options, ProgressCallback onProgress,
                               CancelledCallback isCancelled) {
    RunScripted(inputPath, outputPath, options, /*isCompress=*/true, std::move(onProgress),
                std::move(isCancelled));
}

// The same deferral contract the real engine reports (core/media/DeferredOperations.h),
// not a mock-specific "not scripted" message -- a test that asserts on the deferral must
// be asserting on the shipped behavior.
void MockMediaEngine::ExtractAudio(const std::string&, const std::string&, ProgressCallback,
                                   CancelledCallback) {
    throw errors::MediaToolException(MakeNotImplementedError(kExtractAudioOperation));
}

void MockMediaEngine::ExtractFrames(const std::string&, const std::string&, const nlohmann::json&,
                                    ProgressCallback, CancelledCallback) {
    throw errors::MediaToolException(MakeNotImplementedError(kExtractFramesOperation));
}

}  // namespace mediatool::media
