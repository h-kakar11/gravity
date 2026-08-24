#include "core/jobs/DownloadJob.h"

#include <utility>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/OutputNameRegistry.h"
#include "core/filesystem/PathUtils.h"

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
                          filesystem::IFileSystem& fileSystem, media::IMediaEngine* mediaEngine)
    : Job(JobType::Download),
      options_(std::move(options)),
      provider_(provider),
      fileSystem_(fileSystem),
      mediaEngine_(mediaEngine) {}

DownloadJob::DownloadJob(Options options, downloads::IDownloadProvider& provider,
                          filesystem::IFileSystem& fileSystem, media::IMediaEngine* mediaEngine,
                          common::IClock& clock)
    : Job(JobType::Download, clock),
      options_(std::move(options)),
      provider_(provider),
      fileSystem_(fileSystem),
      mediaEngine_(mediaEngine) {}

void DownloadJob::CleanupArtifacts(const std::string& filenameBase) {
    try {
        for (const auto& name : fileSystem_.ListDirectory(options_.outputDirectory)) {
            if (name.rfind(filenameBase, 0) == 0) {
                fileSystem_.Delete(filesystem::paths::Join(options_.outputDirectory, name));
            }
        }
    } catch (...) {
        // Best-effort cleanup; a cleanup failure must never mask the real job error.
    }
}

void DownloadJob::Execute() {
    Progress fetching;
    fetching.statusMessage = "Fetching metadata";
    ReportProgress(fetching);

    if (IsCancellationRequested()) ThrowCancelled();

    const downloads::DownloadMetadata metadata =
        provider_.Inspect(options_.url, [this] { return IsCancellationRequested(); });
    SetMetadata(MetadataToJson(metadata));

    const std::string safeTitle = filesystem::SanitizeWindowsFilename(metadata.title);
    fileSystem_.CreateDirectory(options_.outputDirectory);
    // Reserved, not merely deduplicated. Two downloads of the same title running at once
    // would otherwise both find the name free and both write to it, and the second would
    // silently destroy the first (see OutputNameRegistry.h). The reservation is held until
    // this function returns, which covers the whole window between choosing the name and
    // yt-dlp having written the file.
    auto reservation = filesystem::OutputNameRegistry::Instance().ReserveBaseName(
        options_.outputDirectory, safeTitle, fileSystem_);
    const std::string filenameBase = reservation.Value();

    Progress starting;
    starting.statusMessage = "Starting download";
    ReportProgress(starting);

    downloads::DownloadOptions downloadOptions;
    downloadOptions.url = options_.url;
    downloadOptions.outputDirectory = options_.outputDirectory;
    downloadOptions.quality = options_.quality;
    downloadOptions.filenameBase = filenameBase;

    std::string outputPath;
    try {
        provider_.Download(
            downloadOptions, [](const downloads::DownloadMetadata&) { /* already have it */ },
            [this](const Progress& progress) { ReportProgress(progress); },
            [&outputPath](const std::string& path) { outputPath = path; },
            [this] { return IsCancellationRequested(); });
    } catch (const errors::MediaToolException&) {
        CleanupArtifacts(filenameBase);
        throw;
    }

    if (outputPath.empty() || !fileSystem_.Exists(outputPath)) {
        CleanupArtifacts(filenameBase);
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_DOWNLOAD_OUTPUT_MISSING", errors::ErrorCategory::DownloadFailure,
            "The downloader reported success but the output file is missing.",
            "expected output at: " + outputPath));
    }

    filesystem::FileInfo info = fileSystem_.Inspect(outputPath);
    if (info.sizeBytes == 0) {
        CleanupArtifacts(filenameBase);
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
            CleanupArtifacts(filenameBase);
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_DOWNLOAD_VERIFICATION_FAILED", errors::ErrorCategory::InvalidFile,
                "The downloaded file failed media verification.", e.Info().details));
        }
    }

    nlohmann::json result;
    result["outputPath"] = outputPath;
    result["fileInfo"] = info.ToJson();
    SetResult(result);
}

}  // namespace mediatool::jobs
