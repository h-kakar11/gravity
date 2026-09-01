#include "core/jobs/MediaProcessingJob.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/PathUtils.h"
#include "core/jobs/JobArtifactCleanup.h"
#include "core/media/BitrateTarget.h"

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

// Default audio budget when the user hasn't named one. Subtracted from the overall target
// before the video encoder gets its share, so `-b:v` plus the audio stream together land
// near the target rather than overshooting it by the size of the audio.
constexpr int kDefaultAudioBudgetKbps = 128;
constexpr int kMinAudioBitrateKbps = 32;

// Fills in the rate-control target the engine needs to make this job's output SMALLER
// than its input (issue #80). ffmpeg's CRF knob targets a perceptual quality, not a size,
// so re-encoding an already-compressed file at a fixed CRF regularly inflates it -- and on
// a stock install the default H.264 encoder (libopenh264) doesn't implement -crf at all,
// so the quality tier silently did nothing and every encode ran at that encoder's own
// hardcoded default. Both failure modes go away once the caller supplies an explicit
// bitrate derived from what the source actually is.
//
// `sourceBitrateBps` is ffprobe's overall container bitrate for the input. A source we
// couldn't probe leaves the options untouched: no target is better than one invented from
// a number we don't have.
void ApplyBitrateTarget(nlohmann::json& engineOptions, const std::string& outputFormat,
                        bool isCompression, std::optional<std::int64_t> sourceBitrateBps) {
    if (!sourceBitrateBps.has_value() || *sourceBitrateBps <= 0) return;

    const std::string quality = engineOptions.value("quality", std::string("medium"));
    const int sourceKbps = static_cast<int>(*sourceBitrateBps / 1000);
    const std::optional<int> overallTarget =
        media::TargetBitrateKbps(sourceKbps, quality, isCompression);
    if (!overallTarget.has_value()) return;

    const bool audioOnly = media::IsAudioOnlyOutputFormat(outputFormat);
    const bool audioBitrateAlreadyChosen = engineOptions.contains("audioBitrateKbps") &&
                                            !engineOptions.at("audioBitrateKbps").is_null();

    if (audioOnly) {
        // No video stream to size: the audio bitrate IS the whole budget. An explicit
        // user choice still wins -- they asked for a specific bitrate, not a ratio.
        if (audioBitrateAlreadyChosen) return;
        engineOptions["audioBitrateKbps"] =
            std::max(kMinAudioBitrateKbps, *overallTarget);
        return;
    }

    const int audioBudget = audioBitrateAlreadyChosen
                                ? engineOptions.at("audioBitrateKbps").get<int>()
                                : kDefaultAudioBudgetKbps;
    if (!audioBitrateAlreadyChosen && isCompression) {
        // Re-encoding audio at ffmpeg's default while shrinking the video by 4x would let
        // the audio dominate the output; give it the same explicit budget the video gets.
        engineOptions["audioBitrateKbps"] = kDefaultAudioBudgetKbps;
    }
    engineOptions["videoBitrateKbps"] =
        std::max(media::kMinTargetBitrateKbps, *overallTarget - audioBudget);
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

    const std::string outputPath = filesystem::paths::Join(
        options_.outputDirectory, filenameBase + "." + options_.outputFormat);

    // Probe the INPUT before encoding, not just the output: the source's own bitrate is
    // the only thing that lets a compression job promise a smaller file (issue #80).
    // Best-effort -- a source ffprobe can't read (or no ffprobe at all) simply leaves the
    // engine on its previous behavior rather than failing a job that would have worked.
    nlohmann::json engineOptions = options_.engineOptions;
    try {
        const filesystem::FileInfo source = mediaEngine_.Probe(options_.inputPath);
        ApplyBitrateTarget(engineOptions, options_.outputFormat, options_.isCompression,
                           source.bitrate);
    } catch (const errors::MediaToolException&) {
        // Intentionally swallowed -- see above.
    }

    Progress processing;
    processing.statusMessage = options_.isCompression ? "Compressing" : "Converting";
    ReportProgress(processing);

    auto onProgress = [this](const Progress& progress) { ReportProgress(progress); };
    auto isCancelled = [this] { return IsCancellationRequested(); };

    try {
        if (options_.isCompression) {
            mediaEngine_.Compress(options_.inputPath, outputPath, engineOptions, onProgress,
                                  isCancelled);
        } else {
            mediaEngine_.Convert(options_.inputPath, outputPath, engineOptions, onProgress,
                                 isCancelled);
        }
    } catch (const errors::MediaToolException&) {
        CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
        throw;
    }

    if (!fileSystem_.Exists(outputPath)) {
        CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_MEDIA_PROCESSING_OUTPUT_MISSING", errors::ErrorCategory::EngineFailure,
            "ffmpeg reported success but the output file is missing.",
            "expected output at: " + outputPath));
    }

    filesystem::FileInfo info = fileSystem_.Inspect(outputPath);
    if (info.sizeBytes == 0) {
        CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_MEDIA_PROCESSING_OUTPUT_EMPTY", errors::ErrorCategory::EngineFailure,
            "The processed file is empty.", outputPath));
    }

    // Cross-check via ffprobe (the same "don't trust exit code 0 alone" instinct
    // DownloadJob applies) -- a corrupt/truncated output should fail here even though
    // ffmpeg itself reported success.
    try {
        const filesystem::FileInfo probed = mediaEngine_.Probe(outputPath);
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

    nlohmann::json result;
    result["outputPath"] = outputPath;
    result["fileInfo"] = info.ToJson();
    SetResult(result);
}

}  // namespace mediatool::jobs
