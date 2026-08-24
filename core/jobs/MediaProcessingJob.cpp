#include "core/jobs/MediaProcessingJob.h"

#include <utility>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/PathUtils.h"

namespace mediatool::jobs {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

[[noreturn]] void ThrowCancelled(const std::string& label) {
    throw MediaToolException(ErrorInfo::Make("E_PROCESSING_CANCELLED", ErrorCategory::Cancelled,
                                              label + " was cancelled."));
}

}  // namespace

MediaProcessingJob::MediaProcessingJob(JobType type, Options options, media::IMediaEngine& engine,
                                       filesystem::IFileSystem& fileSystem)
    : Job(type), options_(std::move(options)), engine_(engine), fileSystem_(fileSystem) {}

MediaProcessingJob::MediaProcessingJob(JobType type, Options options, media::IMediaEngine& engine,
                                       filesystem::IFileSystem& fileSystem, common::IClock& clock)
    : Job(type, clock), options_(std::move(options)), engine_(engine), fileSystem_(fileSystem) {}

media::ProgressCallback MediaProcessingJob::EngineProgressSink() {
    return [this](const Progress& progress) { ReportProgress(progress); };
}

media::CancelledCallback MediaProcessingJob::CancellationProbe() {
    return [this] { return IsCancellationRequested(); };
}

void MediaProcessingJob::SweepPreviousAttempt() {
    if (previousOutputPath_.empty()) return;  // first attempt -- nothing of ours to remove

    const std::string directory = filesystem::paths::GetParentDirectory(previousOutputPath_);
    const std::string filename = filesystem::paths::GetFilename(previousOutputPath_);
    const auto dot = filename.find_last_of('.');
    const std::string stem = dot == std::string::npos ? filename : filename.substr(0, dot);
    const std::string extension = dot == std::string::npos ? std::string() : filename.substr(dot);

    for (const std::string& candidate : {stem + ".processing" + extension, filename}) {
        const std::string path = filesystem::paths::Join(directory, candidate);
        try {
            if (fileSystem_.Exists(path)) fileSystem_.Delete(path);
        } catch (...) {
            // Best-effort: a sweep failure must never mask the real job outcome. If the
            // stale file is genuinely undeletable, deduplication simply picks the next free
            // name instead.
        }
    }
}

void MediaProcessingJob::Execute() {
    const std::string label = OperationLabel();

    Progress preparing;
    preparing.statusMessage = "Preparing";
    ReportProgress(preparing);

    if (options_.inputPath.empty()) {
        throw MediaToolException(ErrorInfo::Make("E_INPUT_NOT_FOUND", ErrorCategory::FileNotFound,
                                                  "No input file was given."));
    }
    // Checked here rather than only at job creation: a queued job can sit behind other work
    // for minutes, and the user may have moved or deleted the file in the meantime.
    if (!fileSystem_.Exists(options_.inputPath)) {
        throw MediaToolException(ErrorInfo::Make(
            "E_INPUT_NOT_FOUND", ErrorCategory::FileNotFound,
            "The input file no longer exists.", "path=" + options_.inputPath));
    }
    if (options_.outputDirectory.empty()) {
        throw MediaToolException(ErrorInfo::Make("E_INVALID_OUTPUT_PATH", ErrorCategory::Unknown,
                                                  "An output folder is required."));
    }

    if (IsCancellationRequested()) ThrowCancelled(label);

    const std::string inputFilename = filesystem::paths::GetFilename(options_.inputPath);
    const std::string sourceExtension = filesystem::paths::GetExtension(options_.inputPath);

    std::string desiredBase = options_.outputFilenameBase;
    if (desiredBase.empty()) {
        const auto dot = inputFilename.find_last_of('.');
        desiredBase = dot == std::string::npos ? inputFilename : inputFilename.substr(0, dot);
    }
    desiredBase = filesystem::SanitizeWindowsFilename(desiredBase);

    fileSystem_.CreateDirectory(options_.outputDirectory);

    const std::string targetExtension = TargetExtension();
    const std::string desiredPath = filesystem::paths::Join(
        options_.outputDirectory, desiredBase + "." + targetExtension);

    // Order matters. Sweep what a previous attempt of this job produced FIRST, so a retry
    // reclaims its own name; then deduplicate, so anything still standing -- which is by
    // definition not ours -- is left alone and worked around.
    SweepPreviousAttempt();
    const std::string outputPath = filesystem::DeduplicateFilename(desiredPath, fileSystem_);
    // Recorded before the engine runs: if this attempt is cancelled or killed partway, the
    // next one still knows which path to clean up.
    previousOutputPath_ = outputPath;

    nlohmann::json metadata = DescribeMetadata();
    metadata["inputPath"] = options_.inputPath;
    metadata["inputFilename"] = inputFilename;
    metadata["sourceFormat"] = sourceExtension;
    metadata["outputPath"] = outputPath;
    metadata["outputFilename"] = filesystem::paths::GetFilename(outputPath);
    metadata["targetFormat"] = targetExtension;
    SetMetadata(metadata);

    if (IsCancellationRequested()) ThrowCancelled(label);

    Progress starting;
    starting.statusMessage = label;
    ReportProgress(starting);

    Invoke(options_.inputPath, outputPath);

    // The engine commits atomically and verifies before committing, but the job owns the
    // final word on "did this actually produce the file we promised the user".
    if (!fileSystem_.Exists(outputPath)) {
        throw MediaToolException(ErrorInfo::Make(
            "E_OUTPUT_MISSING", ErrorCategory::EngineFailure,
            "Processing reported success but the output file is missing.",
            "expected output at: " + outputPath));
    }

    filesystem::FileInfo info = fileSystem_.Inspect(outputPath);
    if (info.sizeBytes == 0) {
        try {
            fileSystem_.Delete(outputPath);
        } catch (...) {
            // Reported below regardless; a failed cleanup must not replace the real error.
        }
        throw MediaToolException(ErrorInfo::Make("E_OUTPUT_EMPTY", ErrorCategory::EngineFailure,
                                                  "The produced file is empty.", outputPath));
    }

    nlohmann::json result;
    result["outputPath"] = outputPath;
    result["fileInfo"] = info.ToJson();
    SetResult(result);
}

}  // namespace mediatool::jobs
