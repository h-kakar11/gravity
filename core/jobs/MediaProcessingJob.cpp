#include "core/jobs/MediaProcessingJob.h"

#include <filesystem>
#include <utility>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/PathUtils.h"
#include "core/jobs/JobArtifactCleanup.h"

namespace stdfs = std::filesystem;

namespace mediatool::jobs {

namespace {

[[noreturn]] void ThrowCancelled() {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_MEDIA_PROCESSING_JOB_CANCELLED", errors::ErrorCategory::Cancelled,
        "Conversion/compression job was cancelled."));
}

// Smart Rename: the output filename is derived from the INPUT file's own basename, not a
// job id or the input's full path -- e.g. converting "C:\Videos\Vacation Clip.mov" to mp4
// produces "Vacation Clip.mp4", not "job-a1b2c3.mp4". Reuses exactly the same
// SanitizeWindowsFilename machinery DownloadJob applies to a fetched video title -- the
// same hygiene concerns (illegal characters, reserved device names, length) apply to a
// local filename just as much as to arbitrary remote metadata.
std::string InputFileStem(const std::string& inputPath) {
    return stdfs::path(inputPath).stem().string();
}

}  // namespace

MediaProcessingJob::MediaProcessingJob(Options options, media::IMediaEngine& mediaEngine,
                                       filesystem::IFileSystem& fileSystem,
                                       filesystem::FilenameReservationRegistry& reservationRegistry)
    : Job(options.isCompression ? JobType::Compression : JobType::Conversion),
      options_(std::move(options)),
      mediaEngine_(mediaEngine),
      fileSystem_(fileSystem),
      reservationRegistry_(reservationRegistry) {}

MediaProcessingJob::MediaProcessingJob(Options options, media::IMediaEngine& mediaEngine,
                                       filesystem::IFileSystem& fileSystem,
                                       filesystem::FilenameReservationRegistry& reservationRegistry,
                                       common::IClock& clock)
    : Job(options.isCompression ? JobType::Compression : JobType::Conversion, clock),
      options_(std::move(options)),
      mediaEngine_(mediaEngine),
      fileSystem_(fileSystem),
      reservationRegistry_(reservationRegistry) {}

void MediaProcessingJob::Execute() {
    Progress preparing;
    preparing.statusMessage = "Preparing";
    ReportProgress(preparing);

    if (IsCancellationRequested()) ThrowCancelled();

    if (!fileSystem_.Exists(options_.inputPath)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INPUT_FILE_NOT_FOUND", errors::ErrorCategory::FileNotFound,
            "The input file does not exist.", "inputPath=" + options_.inputPath));
    }

    std::string safeBase = filesystem::SanitizeWindowsFilename(InputFileStem(options_.inputPath));
    safeBase = filesystem::TruncateBaseNameForMaxPath(options_.outputDirectory, safeBase);
    fileSystem_.CreateDirectory(options_.outputDirectory);

    // Same TOCTOU-safe reservation DownloadJob uses (#12) -- released automatically when
    // it goes out of scope at the end of this function, success or failure alike.
    auto reservation =
        reservationRegistry_.Reserve(options_.outputDirectory, safeBase, fileSystem_);
    const std::string& filenameBase = reservation.BaseName();

    // Write-then-rename (#10): ffmpeg writes to a ".processing" temp name in the same
    // directory, not the final name directly, so a crash/kill mid-encode never leaves a
    // partial file sitting at the path the user expects a *finished* one to have. The
    // rename at the very end of this function (via fileSystem_, not a raw filesystem
    // call) does the actual promotion, only after every verification step below has
    // passed against the temp file -- and stays mockable in tests the same way every
    // other filesystem operation in this job already is. filenameBase-prefix cleanup
    // matching already covers the temp name too (it starts with filenameBase followed by
    // '.'), so no separate temp-vs-clean bookkeeping is needed the way DownloadJob's
    // multi-stream case requires.
    const std::string finalLeafName = filenameBase + "." + options_.outputFormat;
    const std::string tempLeafName = filenameBase + ".processing." + options_.outputFormat;
    const std::string outputPath = filesystem::paths::Join(options_.outputDirectory, finalLeafName);
    const std::string tempOutputPath = filesystem::paths::Join(options_.outputDirectory, tempLeafName);

    Progress processing;
    processing.statusMessage = options_.isCompression ? "Compressing" : "Converting";
    ReportProgress(processing);

    auto onProgress = [this](const Progress& progress) { ReportProgress(progress); };
    auto isCancelled = [this] { return IsCancellationRequested(); };

    try {
        if (options_.isCompression) {
            mediaEngine_.Compress(options_.inputPath, tempOutputPath, options_.engineOptions, onProgress,
                                  isCancelled);
        } else {
            mediaEngine_.Convert(options_.inputPath, tempOutputPath, options_.engineOptions, onProgress,
                                 isCancelled);
        }
    } catch (const errors::MediaToolException&) {
        CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
        throw;
    }

    if (!fileSystem_.Exists(tempOutputPath)) {
        CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_MEDIA_PROCESSING_OUTPUT_MISSING", errors::ErrorCategory::EngineFailure,
            "ffmpeg reported success but the output file is missing.",
            "expected output at: " + tempOutputPath));
    }

    filesystem::FileInfo info = fileSystem_.Inspect(tempOutputPath);
    if (info.sizeBytes == 0) {
        CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_MEDIA_PROCESSING_OUTPUT_EMPTY", errors::ErrorCategory::EngineFailure,
            "The processed file is empty.", tempOutputPath));
    }

    // Cross-check via ffprobe (the same "don't trust exit code 0 alone" instinct
    // DownloadJob applies) -- a corrupt/truncated output should fail here even though
    // ffmpeg itself reported success.
    try {
        const filesystem::FileInfo probed = mediaEngine_.Probe(tempOutputPath);
        info.durationSeconds = probed.durationSeconds;
        info.width = probed.width;
        info.height = probed.height;
        info.videoCodec = probed.videoCodec;
        info.audioCodec = probed.audioCodec;
        info.bitrate = probed.bitrate;
        info.fps = probed.fps;
    } catch (const errors::MediaToolException& e) {
        CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_MEDIA_PROCESSING_VERIFICATION_FAILED", errors::ErrorCategory::InvalidFile,
            "The processed file failed media verification.", e.Info().details));
    }

    try {
        fileSystem_.Rename(tempOutputPath, finalLeafName);
    } catch (const errors::MediaToolException&) {
        CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
        throw;
    }

    nlohmann::json result;
    result["outputPath"] = outputPath;
    result["fileInfo"] = info.ToJson();
    SetResult(result);
}

}  // namespace mediatool::jobs
