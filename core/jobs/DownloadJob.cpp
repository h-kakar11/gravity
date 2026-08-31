#include "core/jobs/DownloadJob.h"

#include <utility>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/PathUtils.h"
#include "core/jobs/JobArtifactCleanup.h"

namespace mediatool::jobs {

namespace {

[[noreturn]] void ThrowCancelled() {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_DOWNLOAD_JOB_CANCELLED", errors::ErrorCategory::Cancelled, "Download job was cancelled."));
}

nlohmann::json MetadataToJson(const downloads::DownloadMetadata& metadata) {
    nlohmann::json json;
    json["title"] = metadata.title;
    if (metadata.uploader) json["uploader"] = *metadata.uploader;
    if (metadata.durationSeconds) json["durationSeconds"] = *metadata.durationSeconds;
    if (metadata.webpageUrl) json["webpageUrl"] = *metadata.webpageUrl;
    if (metadata.thumbnailUrl) json["thumbnailUrl"] = *metadata.thumbnailUrl;
    return json;
}

}  // namespace

DownloadJob::DownloadJob(Options options, downloads::IDownloadProvider& provider,
                          filesystem::IFileSystem& fileSystem, media::IMediaEngine* mediaEngine,
                          filesystem::FilenameReservationRegistry& reservationRegistry)
    : Job(JobType::Download),
      options_(std::move(options)),
      provider_(provider),
      fileSystem_(fileSystem),
      mediaEngine_(mediaEngine),
      reservationRegistry_(reservationRegistry) {}

DownloadJob::DownloadJob(Options options, downloads::IDownloadProvider& provider,
                          filesystem::IFileSystem& fileSystem, media::IMediaEngine* mediaEngine,
                          filesystem::FilenameReservationRegistry& reservationRegistry,
                          common::IClock& clock)
    : Job(JobType::Download, clock),
      options_(std::move(options)),
      provider_(provider),
      fileSystem_(fileSystem),
      mediaEngine_(mediaEngine),
      reservationRegistry_(reservationRegistry) {}

void DownloadJob::CleanupArtifacts(const std::string& filenameBase) {
    CleanupJobArtifacts(fileSystem_, options_.outputDirectory, filenameBase);
}

void DownloadJob::Execute() {
    Progress fetching;
    fetching.statusMessage = "Fetching metadata";
    ReportProgress(fetching);

    if (IsCancellationRequested()) ThrowCancelled();

    const downloads::DownloadMetadata metadata =
        provider_.Inspect(options_.url, [this] { return IsCancellationRequested(); });
    SetMetadata(MetadataToJson(metadata));

    std::string safeTitle = filesystem::SanitizeWindowsFilename(metadata.title);
    safeTitle = filesystem::TruncateBaseNameForMaxPath(options_.outputDirectory, safeTitle);
    fileSystem_.CreateDirectory(options_.outputDirectory);
    // Reserve (not just probe) the output base name: DeduplicateBaseName alone only
    // checks the disk, which is a TOCTOU race once concurrency > 1 -- two jobs racing to
    // download videos with the same title could both compute the same "next free" name
    // (#12). The reservation is released automatically when it goes out of scope at the
    // end of this function (success or exception alike), freeing the name for reuse once
    // this job is no longer using it.
    auto reservation =
        reservationRegistry_.Reserve(options_.outputDirectory, safeTitle, fileSystem_);
    const std::string& filenameBase = reservation.BaseName();

    Progress starting;
    starting.statusMessage = "Starting download";
    ReportProgress(starting);

    // Download under a temp-marked base name, not the clean reserved one directly (#10):
    // yt-dlp's own intermediate/partial artifacts (fragments, ".part", the pre-merge
    // streams) would otherwise land at the exact name the user expects a *finished* file
    // to have -- a crash or kill mid-download previously left a partial file sitting right
    // there with nothing to distinguish it from a completed one. Every verification step
    // below runs against the temp name; only a fully-verified file gets promoted to the
    // clean name, in one rename, as the very last step before SetResult. The reservation
    // itself still claims the CLEAN base name (so no other job can pick the same eventual
    // title), independent of what marker the in-progress write uses.
    const std::string tempFilenameBase = filenameBase + ".partial";

    downloads::DownloadOptions downloadOptions;
    downloadOptions.url = options_.url;
    downloadOptions.outputDirectory = options_.outputDirectory;
    downloadOptions.quality = options_.quality;
    downloadOptions.formatId = options_.formatId;
    downloadOptions.filenameBase = tempFilenameBase;

    std::string outputPath;
    try {
        provider_.Download(
            downloadOptions, [](const downloads::DownloadMetadata&) { /* already have it */ },
            [this](const Progress& progress) { ReportProgress(progress); },
            [&outputPath](const std::string& path) { outputPath = path; },
            [this] { return IsCancellationRequested(); });
    } catch (const errors::MediaToolException&) {
        CleanupArtifacts(tempFilenameBase);
        throw;
    }

    if (outputPath.empty() || !fileSystem_.Exists(outputPath)) {
        CleanupArtifacts(tempFilenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DOWNLOAD_OUTPUT_MISSING", errors::ErrorCategory::DownloadFailure,
            "The downloader reported success but the output file is missing.",
            "expected output at: " + outputPath));
    }

    filesystem::FileInfo info = fileSystem_.Inspect(outputPath);
    if (info.sizeBytes == 0) {
        CleanupArtifacts(tempFilenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DOWNLOAD_OUTPUT_EMPTY", errors::ErrorCategory::DownloadFailure,
            "The downloaded file is empty.", outputPath));
    }

    if (mediaEngine_ && mediaEngine_->IsAvailable()) {
        try {
            const filesystem::FileInfo probed = mediaEngine_->Probe(outputPath);
            info.durationSeconds = probed.durationSeconds;
            info.width = probed.width;
            info.height = probed.height;
            info.videoCodec = probed.videoCodec;
            info.audioCodec = probed.audioCodec;
            info.bitrate = probed.bitrate;
            info.fps = probed.fps;
        } catch (const errors::MediaToolException& e) {
            CleanupArtifacts(tempFilenameBase);
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_DOWNLOAD_VERIFICATION_FAILED", errors::ErrorCategory::InvalidFile,
                "The downloaded file failed media verification.", e.Info().details));
        }
    }

    // Promote: strip the ".partial" marker from just the leaf filename (outputPath's
    // extension is only known now, post-merge -- yt-dlp chooses it) and rename in place.
    // Same directory, so this is a single cheap, atomic filesystem rename, not a copy.
    const std::string leafName = filesystem::paths::GetFilename(outputPath);
    if (leafName.compare(0, tempFilenameBase.size(), tempFilenameBase) == 0) {
        const std::string cleanLeafName = filenameBase + leafName.substr(tempFilenameBase.size());
        try {
            fileSystem_.Rename(outputPath, cleanLeafName);
        } catch (const errors::MediaToolException&) {
            CleanupArtifacts(tempFilenameBase);
            throw;
        }
        outputPath = filesystem::paths::Join(options_.outputDirectory, cleanLeafName);
    }

    nlohmann::json result;
    result["outputPath"] = outputPath;
    result["fileInfo"] = info.ToJson();
    SetResult(result);
}

}  // namespace mediatool::jobs
