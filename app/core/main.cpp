// mediatool-core: the sidecar process. In its default mode it reads NDJSON requests on
// stdin and writes NDJSON responses/events on stdout (docs/ipc-contract.md) -- this is
// the ONLY binary in the product, spawned by the Tauri shell (app/desktop/src-tauri).
// In "--selftest" mode it instead runs a human-readable diagnostic sequence proving every
// Phase 1 subsystem actually works (spec section 42): FFmpeg discovery + a real probe,
// launching the Python downloader and parsing its NDJSON output, and a TestJob run
// end-to-end through JobManager.
//
// This file is the integration point where the nine Phase-1 modules (built independently
// against shared interfaces) are wired together for the first time -- see docs/architecture.md.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/downloads/IDownloadProvider.h"
#include "core/downloads/NdjsonLineProtocol.h"
#include "core/downloads/QualityPreset.h"
#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/events/Event.h"
#include "core/events/EventBus.h"
#include "core/filesystem/FileInfo.h"
#include "core/filesystem/LocalFileSystem.h"
#include "core/filesystem/PathUtils.h"
#include "core/hardware/HardwareInfo.h"
#include "core/hardware/WindowsHardwareDetector.h"
#include "core/ipc/LineReader.h"
#include "core/ipc/RequestExecutor.h"
#include "core/ipc/RequestValidation.h"
#include "core/jobs/DownloadJob.h"
#include "core/jobs/JobHistoryStore.h"
#include "core/jobs/JobManager.h"
#include "core/jobs/MediaProcessingJob.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/Progress.h"
#include "core/jobs/TestJob.h"
#include "core/logging/Logger.h"
#include "core/process/IProcessRunner.h"
#include "core/process/RealProcessRunner.h"
#include "core/settings/JsonFileSettingsStore.h"
#include "core/settings/PresetStore.h"
#include "core/settings/Settings.h"
#include "core/common/Uuid.h"
#include "engines/downloader/YtDlpProvider.h"
#include "engines/ffmpeg/FFmpegDiscovery.h"
#include "engines/ffmpeg/FFmpegEngine.h"

namespace stdfs = std::filesystem;
using json = nlohmann::json;
using namespace mediatool;

namespace {

// --- stdout framing --------------------------------------------------------------------
// Every NDJSON line (IPC responses AND events) goes through here so concurrent writers
// (the request-handling thread and JobManager worker threads publishing progress) never
// interleave a partial line.
std::mutex g_stdoutMutex;

void WriteLine(const json& payload) {
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    // error_handler_t::replace: payload carries externally-influenced text this process
    // doesn't control the encoding of (video titles, ffmpeg/yt-dlp error text, paths) --
    // nlohmann::json::dump()'s default strict handling throws on invalid UTF-8, which
    // would crash the whole core process on a single malformed byte. Substituting the
    // byte instead is the only difference from dump()'s default output for valid input.
    std::cout << payload.dump(-1, ' ', false, json::error_handler_t::replace) << std::endl;
}

// --- path resolution ---------------------------------------------------------------------
// Phase 1 dev-convenience resolution, mirroring the same MEDIATOOL_CORE_PATH-style
// override pattern used on the Rust side (app/desktop/src-tauri) -- see docs/development.md.
std::string EnvOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

// A literal (non-bare-command) path is passed straight through to CreateProcess on
// Windows without a PATH search, and CreateProcess does not reliably accept a
// forward-slash path there the way POSIX-style tools do -- construct through
// std::filesystem::path and normalize to the native separator so the relative dev-mode
// default actually resolves instead of failing with "cannot find the file specified".
std::string NativePath(const std::string& value) {
    return stdfs::path(value).make_preferred().string();
}

std::string ResolvePythonExecutable() {
    return NativePath(EnvOr("MEDIATOOL_PYTHON_PATH", "python/downloader/.venv/Scripts/python.exe"));
}

std::string ResolveDownloaderScript() {
    return NativePath(EnvOr("MEDIATOOL_DOWNLOADER_SCRIPT", "python/downloader/downloader.py"));
}

// A user-supplied path in Settings always wins (they explicitly chose a different
// ffmpeg); absent that, falls back to the bundled ffmpeg the packaged app ships (Phase
// 5.2's resources -- app/desktop/src-tauri/src/core_bridge.rs sets this env var, pointing
// at resource_dir()/ffmpeg/ffmpeg.exe, only once that file actually exists there). Neither
// set means dev mode / no bundled ffmpeg yet, so DiscoverFfmpegPath's own PATH-search
// fallback (pre-Phase-5.2 behavior) is unaffected.
std::optional<std::string> EffectiveFfmpegOverride(const settings::Settings& settings) {
    if (!settings.advanced.ffmpegPath.empty()) return settings.advanced.ffmpegPath;
    const std::string bundled = EnvOr("MEDIATOOL_FFMPEG_PATH", "");
    if (!bundled.empty()) return bundled;
    return std::nullopt;
}

// Same reasoning as EffectiveFfmpegOverride, but ffprobe has no user-facing Settings field
// of its own -- there's never been a reason to let a user override it independently of
// ffmpeg, so this only ever consults the bundled-resource env var.
std::optional<std::string> EffectiveFfprobeOverride() {
    const std::string bundled = EnvOr("MEDIATOOL_FFPROBE_PATH", "");
    if (!bundled.empty()) return bundled;
    return std::nullopt;
}

// --- the wired-up application -----------------------------------------------------------

struct AppContext {
    process::RealProcessRunner processRunner;
    events::EventBus eventBus;
    settings::JsonFileSettingsStore settingsStore{settings::DefaultSettingsFilePath(),
                                                   settings::LegacySettingsFilePath()};
    hardware::WindowsHardwareDetector hardwareDetector;
    filesystem::LocalFileSystem fileSystem;
    // Shared by every job type that allocates an output filename (#12) -- one instance
    // per process, not per job.
    filesystem::FilenameReservationRegistry reservationRegistry;
    media::FFmpegEngine ffmpegEngine;
    downloader::YtDlpProvider ytDlpProvider;
    jobs::JobManager jobManager;
    jobs::JobHistoryStore jobHistoryStore{jobs::DefaultJobHistoryFilePath()};
    settings::PresetStore presetStore{settings::DefaultPresetsFilePath()};

    // Tracks each job's previous state purely to classify the Running state as either
    // "resumed from pause" or "(re)started" when JobManager reports a transition -- see
    // the comment on PublishJobStateChanged below.
    std::mutex previousStateMutex;
    std::unordered_map<jobs::JobId, jobs::JobState> previousState;

    explicit AppContext(const settings::Settings& settings)
        : ffmpegEngine(processRunner, EffectiveFfmpegOverride(settings), EffectiveFfprobeOverride()),
          // Resolved once at startup (not per-download) and handed to yt-dlp so it merges
          // separate video/audio streams via the SAME ffmpeg binary the rest of the app
          // already uses -- see docs/decisions.md "Video/audio merge strategy".
          ytDlpProvider(processRunner, ResolvePythonExecutable(), ResolveDownloaderScript(),
                        media::DiscoverFfmpegPath(processRunner, EffectiveFfmpegOverride(settings))
                            .value_or("")),
          jobManager(static_cast<std::size_t>(std::max(1, settings.processing.concurrentJobs))) {}
};

// --- event publishing from JobManager callbacks -----------------------------------------

json ProgressAndState(const jobs::Progress& progress, const char* stateWire) {
    json data = progress.ToJson();
    data["state"] = stateWire;
    return data;
}

void PublishJobStateChanged(AppContext& app, const jobs::JobId& id, jobs::JobState state) {
    using jobs::JobState;

    jobs::JobState previous;
    {
        std::lock_guard<std::mutex> lock(app.previousStateMutex);
        auto it = app.previousState.find(id);
        previous = it != app.previousState.end() ? it->second : JobState::Queued;
        app.previousState[id] = state;
    }

    switch (state) {
        case JobState::Starting:
            app.eventBus.Publish(events::MakeEvent(events::EventType::JobStarted,
                                                    {{"state", "STARTING"}}, id));
            return;
        case JobState::Running:
            if (previous == JobState::Paused) {
                app.eventBus.Publish(
                    events::MakeEvent(events::EventType::JobResumed, {{"state", "RUNNING"}}, id));
            } else {
                // Covers the normal Starting->Running step and a Retrying->Running restart --
                // the wire protocol has no separate "restarted after retry" event.
                app.eventBus.Publish(
                    events::MakeEvent(events::EventType::JobStarted, {{"state", "RUNNING"}}, id));
            }
            return;
        case JobState::Paused:
            app.eventBus.Publish(
                events::MakeEvent(events::EventType::JobPaused, {{"state", "PAUSED"}}, id));
            return;
        case JobState::Completed: {
            auto snapshot = app.jobManager.GetJob(id);
            json data{{"state", "COMPLETED"}};
            if (snapshot.result) data["result"] = *snapshot.result;
            app.eventBus.Publish(events::MakeEvent(events::EventType::JobCompleted, data, id));
            app.jobHistoryStore.Append(snapshot.ToJson());  // #10/"Session History": every
                                                             // terminal job is recorded
            return;
        }
        case JobState::Failed: {
            auto snapshot = app.jobManager.GetJob(id);
            json data{{"state", "FAILED"}};
            if (snapshot.error) data["error"] = snapshot.error->ToJson();
            app.eventBus.Publish(events::MakeEvent(events::EventType::JobFailed, data, id));
            app.jobHistoryStore.Append(snapshot.ToJson());
            return;
        }
        case JobState::Cancelled: {
            app.eventBus.Publish(
                events::MakeEvent(events::EventType::JobCancelled, {{"state", "CANCELLED"}}, id));
            app.jobHistoryStore.Append(app.jobManager.GetJob(id).ToJson());
            return;
        }
        case JobState::Queued:
        case JobState::Retrying:
            // Queued is announced explicitly by the createJob handler (JobManager has no
            // "transitioned into Queued" callback since a Job starts Queued at
            // construction); Retrying is a momentary internal state with no wire event of
            // its own -- the follow-up Retrying->Running transition above covers it.
            return;
    }
}

void PublishJobProgress(AppContext& app, const jobs::JobId& id, const jobs::Progress& progress) {
    app.eventBus.Publish(
        events::MakeEvent(events::EventType::JobProgress, ProgressAndState(progress, "RUNNING"), id));
}

// --- command handlers --------------------------------------------------------------------

// Every handler below takes its parameters through core/ipc/RequestValidation.h rather
// than indexing into the params object directly (issue #21). Two rules, applied without
// exception: a value is validated before it is used, and the failure says which field was
// wrong and why. Handlers should read as "here are my inputs, here is the work" -- if a
// handler is checking a type or a range inline, that check belongs in the validation tier.
using ipc::OptionalInt;
using ipc::OptionalObject;
using ipc::OptionalString;
using ipc::RequireEnum;
using ipc::RequireInt;
using ipc::RequireNonEmptyString;
using ipc::RequireObject;

// Scheduling priority (issue #17) is an ordering key, not a resource, so the bound is
// about keeping the value legible in logs and snapshots rather than protecting anything.
constexpr std::int64_t kMinJobPriority = -1000;
constexpr std::int64_t kMaxJobPriority = 1000;

// A workflow ("download, then convert, then compress") is a handful of steps. The bound is
// here because `dependsOn` is a list from an untrusted caller, not because 32 is a
// meaningful product limit.
constexpr std::size_t kMaxJobDependencies = 32;

// The two scheduling parameters every job type accepts, applied identically for all of
// them (jobs::SchedulerCore is what interprets them). Both are optional: absent means
// priority 0 and no dependencies, which is the plain FIFO behavior. Invalid dependency ids
// are rejected later, by JobManager::SubmitJob -- this only validates the shape.
void ApplySchedulingParams(jobs::Job& job, const json& jobParams) {
    job.SetPriority(static_cast<int>(
        OptionalInt(jobParams, "priority", 0, kMinJobPriority, kMaxJobPriority)));
    job.SetDependsOn(ipc::OptionalStringArray(jobParams, "dependsOn", kMaxJobDependencies));
}

filesystem::FileInfo InspectFileEnriched(AppContext& app, const std::string& path) {
    filesystem::FileInfo info = app.fileSystem.Inspect(path);

    const bool isMedia = info.category == filesystem::FileCategory::Video ||
                         info.category == filesystem::FileCategory::Audio;
    if (isMedia && app.ffmpegEngine.IsAvailable()) {
        try {
            filesystem::FileInfo probed = app.ffmpegEngine.Probe(path);
            info.durationSeconds = probed.durationSeconds;
            info.width = probed.width;
            info.height = probed.height;
            info.videoCodec = probed.videoCodec;
            info.audioCodec = probed.audioCodec;
            info.bitrate = probed.bitrate;
            info.fps = probed.fps;
        } catch (const errors::MediaToolException&) {
            // Probing is best-effort -- Inspect() must still succeed with the
            // filesystem-only fields if ffprobe fails on this particular file.
        }
    }
    return info;
}

json DownloadFormatToJson(const downloads::DownloadFormat& format) {
    json j;
    j["formatId"] = format.formatId;
    if (format.extension) j["extension"] = *format.extension;
    if (format.resolution) j["resolution"] = *format.resolution;
    if (format.width) j["width"] = *format.width;
    if (format.height) j["height"] = *format.height;
    if (format.fps) j["fps"] = *format.fps;
    if (format.videoCodec) j["videoCodec"] = *format.videoCodec;
    if (format.audioCodec) j["audioCodec"] = *format.audioCodec;
    if (format.videoBitrateKbps) j["videoBitrateKbps"] = *format.videoBitrateKbps;
    if (format.audioBitrateKbps) j["audioBitrateKbps"] = *format.audioBitrateKbps;
    if (format.filesizeBytes) j["filesizeBytes"] = *format.filesizeBytes;
    if (format.approxFilesizeBytes) j["approxFilesizeBytes"] = *format.approxFilesizeBytes;
    j["hasVideo"] = format.hasVideo;
    j["hasAudio"] = format.hasAudio;
    return j;
}

json DownloadMetadataToJson(const downloads::DownloadMetadata& metadata) {
    json j;
    j["title"] = metadata.title;
    if (metadata.uploader) j["uploader"] = *metadata.uploader;
    if (metadata.durationSeconds) j["durationSeconds"] = *metadata.durationSeconds;
    if (metadata.webpageUrl) j["webpageUrl"] = *metadata.webpageUrl;
    if (metadata.thumbnailUrl) j["thumbnailUrl"] = *metadata.thumbnailUrl;
    if (metadata.extractor) j["extractor"] = *metadata.extractor;
    if (metadata.playlistIndex) j["playlistIndex"] = *metadata.playlistIndex;
    if (metadata.playlistCount) j["playlistCount"] = *metadata.playlistCount;
    json formats = json::array();
    for (const auto& f : metadata.formats) formats.push_back(DownloadFormatToJson(f));
    j["formats"] = formats;
    return j;
}

// Shared by createJob{type:DOWNLOAD} and inspectDownloadUrl -- both take a raw URL from
// the frontend and must reject it up front (spec section 4) rather than letting an
// obviously-unsupported string reach a subprocess launch.
void ValidateDownloadUrl(AppContext& app, const std::string& url) {
    if (url.empty() || !app.ytDlpProvider.CanHandle(url)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_DOWNLOAD_URL", errors::ErrorCategory::UnsupportedFormat,
            "This URL is not a supported http/https media URL.", "url=" + url));
    }
}

// Wall-clock bound for a single inspectDownloadUrl call (issue #8). yt-dlp's Inspect()
// runs synchronously on the IPC thread with no independent timeout of its own -- a
// network stall or an unresponsive upstream site used to block the whole backend
// indefinitely. YtDlpProvider::RunPythonCommand already polls an isCancelled() callback
// every 200ms and tears the subprocess down the moment it returns true (see
// engines/downloader/YtDlpProvider.cpp), so a deadline-based callback here is enough to
// turn "hangs forever" into "gives up after a fixed, bounded period" without needing a
// second thread.
constexpr auto kInspectDeadline = std::chrono::seconds(30);

json HandleInspectDownloadUrl(AppContext& app, const json& params) {
    const std::string url = RequireNonEmptyString(params, "url");
    ValidateDownloadUrl(app, url);

    const auto deadline = std::chrono::steady_clock::now() + kInspectDeadline;
    auto isCancelled = [deadline] { return std::chrono::steady_clock::now() >= deadline; };

    try {
        const downloads::DownloadMetadata metadata = app.ytDlpProvider.Inspect(url, isCancelled);
        return {{"metadata", DownloadMetadataToJson(metadata)}};
    } catch (const errors::MediaToolException& e) {
        if (e.Info().code == "E_INSPECT_CANCELLED") {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_INSPECT_TIMEOUT", errors::ErrorCategory::NetworkError,
                "Inspecting this URL took too long and was cancelled.", "url=" + url,
                /*recoverable=*/true));
        }
        throw;
    }
}

json HandleCreateDownloadJob(AppContext& app, const json& jobParams) {
    const std::string url = RequireNonEmptyString(jobParams, "url");
    const std::string outputDirectory = RequireNonEmptyString(jobParams, "outputDirectory");
    ValidateDownloadUrl(app, url);
    // #11: reject traversal and (unless explicitly opted into) UNC output directories --
    // previously any string was accepted as-is.
    const bool allowNetworkPaths = app.settingsStore.Load().advanced.allowNetworkPaths;
    if (!filesystem::paths::IsSafeUserSuppliedPath(outputDirectory, allowNetworkPaths)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_OUTPUT_DIRECTORY", errors::ErrorCategory::Unknown,
            "The output directory must be an absolute path with no \"..\" segments" +
                std::string(allowNetworkPaths ? "." : ", and network (UNC) paths are not enabled."),
            "outputDirectory=" + outputDirectory));
    }

    // A coarse floor, not a real "will this download fit" check (that needs the file
    // size, which isn't known until Inspect() runs inside the job) -- catches the
    // "drive is already essentially full" case up front (spec section 11).
    constexpr std::uint64_t kMinFreeBytesForDownload = 100ull * 1024 * 1024;
    if (auto available = app.fileSystem.GetAvailableDiskSpace(outputDirectory);
        available && *available < kMinFreeBytesForDownload) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INSUFFICIENT_DISK_SPACE", errors::ErrorCategory::DiskSpaceError,
            "Not enough free disk space at the selected output directory.",
            "available=" + std::to_string(*available) + " bytes"));
    }

    downloads::QualityPreset quality = downloads::QualityPreset::Best;
    if (const auto qualityWire = OptionalString(jobParams, "quality")) {
        try {
            quality = downloads::QualityPresetFromWireString(*qualityWire);
        } catch (const std::invalid_argument& e) {
            // QualityPreset owns the list of valid values; re-listing it here as a
            // RequireEnum allowlist would be a second copy to drift out of sync.
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_INVALID_QUALITY_PRESET", errors::ErrorCategory::Unknown, e.what(),
                "field=quality value=" + *qualityWire));
        }
    }

    // Explicit format id from Inspect()'s format list (issue #31) overrides `quality`
    // entirely -- see downloads::DownloadOptions::formatId.
    std::optional<std::string> formatId;
    if (const auto value = OptionalString(jobParams, "formatId")) {
        if (value->empty()) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_INVALID_PARAM_VALUE", errors::ErrorCategory::Unknown,
                "formatId must not be empty.", "field=formatId value is an empty string"));
        }
        formatId = *value;
    }

    jobs::DownloadJob::Options options;
    options.url = url;
    options.outputDirectory = outputDirectory;
    options.quality = quality;
    options.formatId = formatId;

    auto job = std::make_unique<jobs::DownloadJob>(options, app.ytDlpProvider, app.fileSystem,
                                                    &app.ffmpegEngine, app.reservationRegistry);
    ApplySchedulingParams(*job, jobParams);
    const jobs::JobId id = job->Id();
    app.jobManager.SubmitJob(std::move(job));
    app.eventBus.Publish(events::MakeEvent(events::EventType::JobCreated, {{"state", "QUEUED"}}, id));
    return {{"jobId", id}};
}

// Shared by HandleCreateConversionJob/HandleCreateCompressionJob -- they differ only in
// `isCompression` (which JobType the job runs as and which IMediaEngine method it calls);
// per engines/ffmpeg/FFmpegArgBuilder.h, Compress is Convert with different default
// option VALUES (supplied by the caller, i.e. the frontend's preset), not a different
// code path here either.
json HandleCreateMediaProcessingJob(AppContext& app, const json& jobParams, bool isCompression) {
    const std::string inputPath = RequireNonEmptyString(jobParams, "inputPath");
    const std::string outputDirectory = RequireNonEmptyString(jobParams, "outputDirectory");

    const bool allowNetworkPaths = app.settingsStore.Load().advanced.allowNetworkPaths;
    if (!filesystem::paths::IsSafeUserSuppliedPath(inputPath, allowNetworkPaths)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_INPUT_PATH", errors::ErrorCategory::Unknown,
            "The input path must be an absolute path with no \"..\" segments" +
                std::string(allowNetworkPaths ? "." : ", and network (UNC) paths are not enabled."),
            "inputPath=" + inputPath));
    }
    if (!app.fileSystem.Exists(inputPath)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INPUT_FILE_NOT_FOUND", errors::ErrorCategory::FileNotFound,
            "The input file does not exist.", "inputPath=" + inputPath));
    }
    if (!filesystem::paths::IsSafeUserSuppliedPath(outputDirectory, allowNetworkPaths)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_OUTPUT_DIRECTORY", errors::ErrorCategory::Unknown,
            "The output directory must be an absolute path with no \"..\" segments" +
                std::string(allowNetworkPaths ? "." : ", and network (UNC) paths are not enabled."),
            "outputDirectory=" + outputDirectory));
    }

    const json& processingOptions = OptionalObject(jobParams, "options");
    const std::string outputFormat = RequireNonEmptyString(processingOptions, "outputFormat");
    // Server-side Pro-tier gate, independent of the UI never offering this value at all
    // (idealist.md: build the "Pro" affordances as visibly-present-but-inert, not wired
    // to anything real) -- there is no entitlement system, so this is an unconditional
    // rejection, not a toggle.
    if (OptionalString(processingOptions, "quality").value_or("medium") == "lossless") {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_PRO_FEATURE_LOCKED", errors::ErrorCategory::UnsupportedFormat,
            "Lossless quality is a Pro feature and is not available yet."));
    }

    jobs::MediaProcessingJob::Options options;
    options.inputPath = inputPath;
    options.outputDirectory = outputDirectory;
    options.outputFormat = outputFormat;
    options.engineOptions = processingOptions;
    options.isCompression = isCompression;

    auto job = std::make_unique<jobs::MediaProcessingJob>(std::move(options), app.ffmpegEngine,
                                                          app.fileSystem, app.reservationRegistry);
    ApplySchedulingParams(*job, jobParams);
    const jobs::JobId id = job->Id();
    app.jobManager.SubmitJob(std::move(job));
    app.eventBus.Publish(events::MakeEvent(events::EventType::JobCreated, {{"state", "QUEUED"}}, id));
    return {{"jobId", id}};
}

json HandleCreateJob(AppContext& app, const json& params) {
    const std::string typeWire = RequireNonEmptyString(params, "type");
    // Absent/empty job params are legitimate for TEST; the per-type handlers below require
    // whatever they actually need out of this object.
    const json& jobParams = OptionalObject(params, "params");

    if (typeWire == "DOWNLOAD") {
        return HandleCreateDownloadJob(app, jobParams);
    }
    if (typeWire == "CONVERSION") {
        return HandleCreateMediaProcessingJob(app, jobParams, /*isCompression=*/false);
    }
    if (typeWire == "COMPRESSION") {
        return HandleCreateMediaProcessingJob(app, jobParams, /*isCompression=*/true);
    }

    if (typeWire != "TEST") {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_JOB_TYPE_NOT_IMPLEMENTED", errors::ErrorCategory::UnsupportedFormat,
            "Only TEST, DOWNLOAD, CONVERSION and COMPRESSION jobs are implemented so far -- " +
                typeWire + " is scaffolded (see docs/roadmap.md) but not runnable yet.",
            "", false));
    }

    auto job = std::make_unique<jobs::TestJob>();
    ApplySchedulingParams(*job, jobParams);
    const jobs::JobId id = job->Id();
    app.jobManager.SubmitJob(std::move(job));
    app.eventBus.Publish(events::MakeEvent(events::EventType::JobCreated, {{"state", "QUEUED"}}, id));
    return {{"jobId", id}};
}

json HandleGetJob(AppContext& app, const json& params) {
    return {{"job", app.jobManager.GetJob(RequireNonEmptyString(params, "jobId")).ToJson()}};
}

json HandleListJobs(AppContext& app, const json&) {
    json jobsArray = json::array();
    for (const auto& snapshot : app.jobManager.ListJobs()) jobsArray.push_back(snapshot.ToJson());
    return {{"jobs", jobsArray}};
}

json HandleListJobHistory(AppContext& app, const json& params) {
    std::vector<nlohmann::json> entries = app.jobHistoryStore.Load();
    // Load() returns oldest-first; the UI wants most-recent-first ("Session History").
    std::reverse(entries.begin(), entries.end());
    // Bounded, and bounded as a signed integer: `get<std::size_t>()` on a negative limit
    // wrapped to an enormous positive value instead of being rejected.
    constexpr std::int64_t kMaxHistoryLimit = 10000;
    if (ipc::HasParam(params, "limit")) {
        const auto limit = static_cast<std::size_t>(RequireInt(params, "limit", 1, kMaxHistoryLimit));
        if (entries.size() > limit) entries.resize(limit);
    }
    return {{"jobs", json(entries)}};
}

json HandleCancelJob(AppContext& app, const json& params) {
    app.jobManager.CancelJob(RequireNonEmptyString(params, "jobId"));
    return json::object();
}

json HandlePauseJob(AppContext& app, const json& params) {
    app.jobManager.PauseJob(RequireNonEmptyString(params, "jobId"));
    return json::object();
}

json HandleResumeJob(AppContext& app, const json& params) {
    app.jobManager.ResumeJob(RequireNonEmptyString(params, "jobId"));
    return json::object();
}

json HandleRetryJob(AppContext& app, const json& params) {
    app.jobManager.RetryJob(RequireNonEmptyString(params, "jobId"));
    return json::object();
}

// JobManager::RemoveJob already existed (throws ThrowInvalidOperation for a non-terminal
// job, ThrowNotFound for an unknown id -- both surface as the usual structured error), it
// was just never reachable from any IPC command. Completed jobs otherwise accumulate
// forever in JobManager::jobs_, AppContext::previousState, and the frontend's job list
// (issue #29).
json HandleRemoveJob(AppContext& app, const json& params) {
    const std::string jobId = RequireNonEmptyString(params, "jobId");
    app.jobManager.RemoveJob(jobId);
    // JobManager::RemoveJob() already rejects a non-terminal job, so by the time this
    // erase runs no further transition will ever be published for this id -- safe to drop
    // its previousState entry too, for the same "stop accumulating forever" reason.
    std::lock_guard<std::mutex> lock(app.previousStateMutex);
    app.previousState.erase(jobId);
    return json::object();
}

json HandleInspectFile(AppContext& app, const json& params) {
    const std::string path = RequireNonEmptyString(params, "path");
    // #11: same traversal/UNC gate as HandleCreateDownloadJob's output directory --
    // inspectFile previously accepted any absolute path with no restriction at all
    // (e.g. a traversal or UNC path reaching well outside anything the user selected).
    const bool allowNetworkPaths = app.settingsStore.Load().advanced.allowNetworkPaths;
    if (!filesystem::paths::IsSafeUserSuppliedPath(path, allowNetworkPaths)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_PATH", errors::ErrorCategory::Unknown,
            "The path must be an absolute path with no \"..\" segments" +
                std::string(allowNetworkPaths ? "." : ", and network (UNC) paths are not enabled."),
            "path=" + path));
    }
    return {{"fileInfo", InspectFileEnriched(app, path).ToJson()}};
}

json HandleGetCapabilities(AppContext& app, const json& params) {
    const std::string path = RequireNonEmptyString(params, "path");
    filesystem::FileInfo info = app.fileSystem.Inspect(path);
    return {{"capabilities", filesystem::CapabilitiesFor(info.category, info.extension)}};
}

json HandleGetSettings(AppContext& app, const json&) {
    return {{"settings", app.settingsStore.Load().ToJson()}};
}

json HandleUpdateSettings(AppContext& app, const json& params) {
    json merged = app.settingsStore.Load().ToJson();
    merged.merge_patch(RequireObject(params, "settings"));
    settings::Settings updated = settings::Settings::FromJson(merged);
    app.settingsStore.Save(updated);
    return {{"settings", updated.ToJson()}};
}

json HandleGetHardwareInfo(AppContext& app, const json&) {
    return {{"hardwareInfo", app.hardwareDetector.Detect().ToJson()}};
}

// --- Hardware Acceleration UI (#4.7) -----------------------------------------------------
// The engine's encoder probe (2.6's FFmpegEngine::AvailableEncoders(), cached at
// construction) already exists; this just surfaces it as its own command rather than
// overloading the unrelated file-category getCapabilities.

json HandleGetMediaEngineCapabilities(AppContext& app, const json&) {
    const auto& encoders = app.ffmpegEngine.AvailableEncoders();

    json encodersArray = json::array();
    for (const auto& encoder : encoders) encodersArray.push_back(encoder);

    auto hasEncoderSuffix = [&](const std::string& suffix) {
        return std::any_of(encoders.begin(), encoders.end(), [&](const std::string& encoder) {
            return encoder.size() >= suffix.size() &&
                   encoder.compare(encoder.size() - suffix.size(), suffix.size(), suffix) == 0;
        });
    };

    return {
        {"availableEncoders", encodersArray},
        {"hardwareEncodersAvailable",
         {
             {"nvenc", hasEncoderSuffix("_nvenc")},
             {"amf", hasEncoderSuffix("_amf")},
             {"qsv", hasEncoderSuffix("_qsv")},
         }},
    };
}

// --- Multi-Profile Presets (#4.6) --------------------------------------------------------
// Schema/persistence (settings::PresetStore) landed in Phase 3.4; this is the IPC surface
// on top of it.

json HandleListPresets(AppContext& app, const json&) {
    json array = json::array();
    for (const auto& preset : app.presetStore.Load()) array.push_back(preset.ToJson());
    return {{"presets", array}};
}

json HandleSavePreset(AppContext& app, const json& params) {
    const std::string name = RequireNonEmptyString(params, "name");
    const std::string kind = RequireEnum(params, "kind", {"DOWNLOAD", "CONVERSION", "COMPRESSION"});
    const json options = OptionalObject(params, "options");

    std::vector<settings::Preset> presets = app.presetStore.Load();

    // A client-supplied `id` that matches an existing preset means "update this preset in
    // place" (rename / re-save with new options); a missing or non-matching id means
    // "create a new one" -- match-by-identity rather than trusting an index, so a stale id
    // can never silently overwrite the wrong entry.
    const std::string requestedId = OptionalString(params, "id").value_or(std::string());
    auto it = std::find_if(presets.begin(), presets.end(), [&](const settings::Preset& p) {
        return !requestedId.empty() && p.id == requestedId;
    });

    settings::Preset preset;
    preset.id = (it != presets.end()) ? it->id : "preset-" + common::GenerateUuidV4();
    preset.name = name;
    preset.kind = kind;
    preset.options = options;

    if (it != presets.end()) {
        *it = preset;
    } else {
        presets.push_back(preset);
    }

    app.presetStore.Save(presets);
    return {{"preset", preset.ToJson()}};
}

json HandleDeletePreset(AppContext& app, const json& params) {
    const std::string id = RequireNonEmptyString(params, "id");
    std::vector<settings::Preset> presets = app.presetStore.Load();
    const std::size_t before = presets.size();
    presets.erase(
        std::remove_if(presets.begin(), presets.end(),
                        [&](const settings::Preset& p) { return p.id == id; }),
        presets.end());
    if (presets.size() == before) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_PRESET_NOT_FOUND", errors::ErrorCategory::Unknown,
            "No preset with that id exists.", "id=" + id));
    }
    app.presetStore.Save(presets);
    return json::object();
}

using Handler = std::function<json(AppContext&, const json&)>;

const std::unordered_map<std::string, Handler>& CommandTable() {
    static const std::unordered_map<std::string, Handler> table{
        {"createJob", HandleCreateJob},
        {"getJob", HandleGetJob},
        {"listJobs", HandleListJobs},
        {"listJobHistory", HandleListJobHistory},
        {"cancelJob", HandleCancelJob},
        {"pauseJob", HandlePauseJob},
        {"resumeJob", HandleResumeJob},
        {"retryJob", HandleRetryJob},
        {"removeJob", HandleRemoveJob},
        {"inspectFile", HandleInspectFile},
        {"inspectDownloadUrl", HandleInspectDownloadUrl},
        {"getCapabilities", HandleGetCapabilities},
        {"getSettings", HandleGetSettings},
        {"updateSettings", HandleUpdateSettings},
        {"getHardwareInfo", HandleGetHardwareInfo},
        {"getMediaEngineCapabilities", HandleGetMediaEngineCapabilities},
        {"listPresets", HandleListPresets},
        {"savePreset", HandleSavePreset},
        {"deletePreset", HandleDeletePreset},
    };
    return table;
}

// --- the IPC loop --------------------------------------------------------------------

// Commands that wait on something outside this process (a yt-dlp subprocess doing network
// I/O, an ffprobe run over a large file) and therefore must not run on the request loop
// thread -- see core/ipc/RequestExecutor.h for why. Everything else is memory-speed work on
// already-loaded state and is faster to run inline than to hand to another thread.
bool IsBlockingCommand(const std::string& command) {
    return command == "inspectDownloadUrl" || command == "inspectFile";
}

// Four threads is enough to keep a burst of inspects moving without letting a burst turn
// into a swarm of yt-dlp processes; the queue bound is what stops a client that never stops
// asking from growing this process without limit.
constexpr std::size_t kBlockingCommandThreads = 4;
constexpr std::size_t kMaxQueuedBlockingCommands = 64;

// A generous ceiling for a single NDJSON request: the longest legitimate request is a
// createJob carrying two Windows paths and a preset object, which is kilobytes. Anything
// past this is a malformed or hostile stream, not a request (see core/ipc/LineReader.h).
constexpr std::size_t kMaxRequestLineBytes = 1024 * 1024;

json ErrorResponse(const std::string& id, errors::ErrorInfo error) {
    return {{"id", id}, {"ok", false}, {"error", error.ToJson()}};
}

// Runs one command to completion and returns the response envelope to write. Never throws:
// every failure is turned into an `ok:false` response here, because this is called from
// executor threads as well as the loop, and an exception escaping there would be an
// unanswered request rather than a crash the caller can see.
json ExecuteRequest(AppContext& app, const std::string& id, const std::string& command,
                     const json& params) {
    try {
        const auto& table = CommandTable();
        const auto it = table.find(command);
        if (it == table.end()) {
            return ErrorResponse(id, errors::ErrorInfo::Make("E_UNKNOWN_COMMAND",
                                                              errors::ErrorCategory::Unknown,
                                                              "Unknown command: " + command));
        }
        return {{"id", id}, {"ok", true}, {"result", it->second(app, params)}};
    } catch (const errors::MediaToolException& e) {
        return ErrorResponse(id, e.Info());
    } catch (const std::exception& e) {
        return ErrorResponse(id, errors::ErrorInfo::Make("E_UNHANDLED_EXCEPTION",
                                                          errors::ErrorCategory::Unknown, e.what()));
    }
}

void RunIpcLoop(AppContext& app) {
    app.jobManager.OnJobStateChanged(
        [&app](const jobs::JobId& id, jobs::JobState state) { PublishJobStateChanged(app, id, state); });
    app.jobManager.OnJobProgress(
        [&app](const jobs::JobId& id, const jobs::Progress& progress) { PublishJobProgress(app, id, progress); });
    app.eventBus.Subscribe([](const events::Event& event) { WriteLine(event.ToJson()); });
    logging::Logger::SetEventSink([&app](events::Event event) { app.eventBus.Publish(event); });

    logging::Log::Info("mediatool-core", "IPC loop starting");

    // Declared here, so it is torn down (and its threads joined) when this function
    // returns -- before AppContext, which its queued tasks reference.
    ipc::RequestExecutor executor(kBlockingCommandThreads, kMaxQueuedBlockingCommands);

    while (true) {
        const ipc::ReadLineResult read = ipc::ReadBoundedLine(std::cin, kMaxRequestLineBytes);
        if (read.status == ipc::ReadLineStatus::EndOfStream) break;
        if (read.status == ipc::ReadLineStatus::LineTooLong) {
            // No id is recoverable from a line that was never accumulated, so there is
            // nobody to answer -- but the loop survives, which is the entire point.
            logging::Log::Warning("mediatool-core",
                                   "discarded an oversized request line (" +
                                       std::to_string(read.bytesDiscarded) + " bytes, limit " +
                                       std::to_string(kMaxRequestLineBytes) + ")");
            continue;
        }
        if (read.line.empty()) continue;

        // A missing or non-string "id" can't be recovered from by writing a response --
        // the Rust bridge (core_bridge.rs) matches responses to pending requests purely by
        // id, and no pending request will ever be waiting on an empty/garbage one (real
        // ids are always "req-<n>"). Writing one anyway used to just waste a line and leave
        // the actual caller (whatever sent the malformed line) hanging for the full 30s
        // timeout with no faster failure available -- log and skip instead (issue #21).
        json request;
        std::string id;
        try {
            request = json::parse(read.line);
            id = ipc::RequireNonEmptyString(request, "id");
        } catch (const std::exception& e) {
            logging::Log::Warning("mediatool-core",
                                   "rejecting request with missing/malformed id, no response "
                                   "possible: " +
                                       std::string(e.what()));
            continue;
        }

        // From here on a response is always written: the id is routable, so every outcome
        // -- including "your request was malformed" -- reaches the caller as an answer
        // rather than as a timeout.
        std::string command;
        json params;
        try {
            command = ipc::RequireNonEmptyString(request, "command");
            // .value()'s default only applies when the key is absent -- a request that
            // sends "params": null keeps that null right through, which every handler
            // that indexes into params (i.e. all but the no-param ones like listJobs)
            // then throws on. Explicit JSON null and "key absent" should behave the same
            // way here: no params supplied (issue #21).
            params = request.value("params", json::object());
            if (params.is_null()) params = json::object();
            if (!params.is_object()) {
                throw errors::MediaToolException(errors::ErrorInfo::Make(
                    "E_INVALID_PARAM_TYPE", errors::ErrorCategory::Unknown,
                    "params must be an object.",
                    std::string("field=params actualType=") + params.type_name()));
            }
        } catch (const errors::MediaToolException& e) {
            WriteLine(ErrorResponse(id, e.Info()));
            continue;
        }

        if (IsBlockingCommand(command)) {
            const bool queued = executor.TrySubmit([&app, id, command, params] {
                WriteLine(ExecuteRequest(app, id, command, params));
            });
            if (queued) continue;
            // Backpressure rather than a stall: the executor is saturated, so running this
            // inline would block the loop for exactly as long as the request the executor
            // exists to keep off it. Say so, and say it is worth retrying.
            WriteLine(ErrorResponse(
                id, errors::ErrorInfo::Make("E_CORE_BUSY", errors::ErrorCategory::EngineFailure,
                                             "Gravity is busy with too many lookups right now.",
                                             "command=" + command + " queued=" +
                                                 std::to_string(executor.PendingCount()),
                                             /*recoverable=*/true)));
            continue;
        }

        WriteLine(ExecuteRequest(app, id, command, params));
    }

    logging::Log::Info("mediatool-core", "stdin closed, shutting down");
}

// --- self-test --------------------------------------------------------------------------

void PrintStep(const std::string& title) {
    std::cout << "\n== " << title << " ==" << std::endl;
}

void RunSelfTest(AppContext& app) {
    std::cout << "MediaTool Phase 1 self-test\n";
    std::cout << "(proving the foundation, not the finished product -- see docs/architecture.md)\n";

    PrintStep("1. FFmpeg discovery");
    const auto ffmpegPath = media::DiscoverFfmpegPath(app.processRunner);
    const auto ffprobePath = media::DiscoverFfprobePath(app.processRunner);
    if (ffmpegPath) {
        std::cout << "  ffmpeg found: " << *ffmpegPath << "\n";
        std::cout << "  version: " << app.ffmpegEngine.Version().value_or("(unknown)") << "\n";
    } else {
        std::cout << "  ffmpeg NOT found (this is a valid, handled state -- the app still runs)\n";
    }
    std::cout << "  ffprobe: " << (ffprobePath ? *ffprobePath : std::string("NOT found")) << "\n";

    PrintStep("2. FFmpeg probe against a real generated test clip");
    if (ffmpegPath && ffprobePath) {
        const std::string tempClip = (stdfs::temp_directory_path() / "mediatool_selftest_clip.mp4").string();
        try {
            auto proc = app.processRunner.Start(
                *ffmpegPath,
                {"-y", "-f", "lavfi", "-i", "color=c=black:s=64x64:d=1", "-r", "5", tempClip},
                {}, [](const std::string&) {}, [](const std::string&) {});
            const auto result = proc->Wait();
            if (result.exitCode == 0 && stdfs::exists(tempClip)) {
                const filesystem::FileInfo info = app.ffmpegEngine.Probe(tempClip);
                std::cout << "  PASS -- probed generated clip: " << info.width.value_or(-1) << "x"
                          << info.height.value_or(-1) << ", codec="
                          << info.videoCodec.value_or("?") << ", duration="
                          << info.durationSeconds.value_or(-1.0) << "s\n";
            } else {
                std::cout << "  SKIP -- ffmpeg could not generate a test clip (exit code "
                          << result.exitCode << ")\n";
            }
            std::error_code ec;
            stdfs::remove(tempClip, ec);
        } catch (const errors::MediaToolException& e) {
            std::cout << "  FAIL -- " << e.Info().message << "\n";
        }
    } else {
        std::cout << "  SKIP -- ffmpeg/ffprobe not available on this machine\n";
    }

    PrintStep("3. Launch Python downloader subsystem (--selftest, no network)");
    try {
        std::vector<std::string> stdoutLines;
        auto proc = app.processRunner.Start(
            ResolvePythonExecutable(), {ResolveDownloaderScript(), "--selftest"}, {},
            [&stdoutLines](const std::string& line) { stdoutLines.push_back(line); },
            [](const std::string& line) { std::cout << "  [python stderr] " << line << "\n"; });
        const auto result = proc->Wait();

        int validJson = 0;
        std::unordered_map<std::string, int> eventCounts;
        for (const auto& line : stdoutLines) {
            const auto parsed = downloads::ParseNdjsonLine(line);
            if (!parsed) continue;
            ++validJson;
            if (parsed->contains("event")) eventCounts[(*parsed)["event"].get<std::string>()]++;
        }

        std::cout << "  python exit code: " << result.exitCode << "\n";
        std::cout << "  stdout lines: " << stdoutLines.size() << ", valid JSON: " << validJson << "\n";
        for (const auto& [event, count] : eventCounts) std::cout << "    event '" << event << "' x" << count << "\n";
        std::cout << "  " << (validJson > 0 && result.exitCode == 0 ? "PASS" : "FAIL") << "\n";
    } catch (const errors::MediaToolException& e) {
        std::cout << "  FAIL -- " << e.Info().message << " (" << e.Info().details << ")\n";
    }

    PrintStep("4. TestJob through JobManager (job system + state machine + progress)");
    {
        auto job = std::make_unique<jobs::TestJob>();
        const jobs::JobId id = job->Id();
        app.jobManager.OnJobStateChanged([id](const jobs::JobId& changedId, jobs::JobState state) {
            if (changedId == id) std::cout << "  state -> " << jobs::ToWireString(state) << "\n";
        });
        app.jobManager.OnJobProgress([id](const jobs::JobId& changedId, const jobs::Progress& progress) {
            if (changedId == id && progress.percentage)
                std::cout << "  progress: " << *progress.percentage << "% -- " << progress.statusMessage << "\n";
        });

        app.jobManager.SubmitJob(std::move(job));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        jobs::JobState finalState = jobs::JobState::Queued;
        while (std::chrono::steady_clock::now() < deadline) {
            finalState = app.jobManager.GetJob(id).state;
            if (jobs::IsTerminalState(finalState)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        std::cout << "  " << (finalState == jobs::JobState::Completed ? "PASS" : "FAIL")
                  << " -- final state: " << jobs::ToWireString(finalState) << "\n";
    }

    PrintStep("5. Hardware detection");
    const hardware::HardwareInfo hw = app.hardwareDetector.Detect();
    std::cout << "  CPU: " << hw.cpu.name << " (" << hw.cpu.logicalCores << " logical cores)\n";
    std::cout << "  GPUs: " << hw.gpus.size() << "\n";
    for (const auto& gpu : hw.gpus) std::cout << "    - [" << hardware::ToWireString(gpu.vendor) << "] " << gpu.name << "\n";

    PrintStep("6. Settings");
    const settings::Settings settings = app.settingsStore.Load();
    std::cout << "  settings file: " << settings::DefaultSettingsFilePath() << "\n";
    std::cout << "  concurrentJobs: " << settings.processing.concurrentJobs << "\n";

    std::cout << "\nSelf-test complete.\n";
}

// advanced.logLevel is one of Settings::Validate()'s validated enum strings
// ("DEBUG"|"INFO"|"WARNING"|"ERROR") -- unrecognized input can't reach here, but this
// still defends with the same INFO fallback Logger::Init used unconditionally before this
// was wired up (issue #18).
logging::LogLevel ResolveLogLevel(const settings::Settings& settings) {
    const std::string& level = settings.advanced.logLevel;
    if (level == "DEBUG") return logging::LogLevel::Debug;
    if (level == "WARNING") return logging::LogLevel::Warning;
    if (level == "ERROR") return logging::LogLevel::Error;
    return logging::LogLevel::Info;
}

}  // namespace

int main(int argc, char** argv) {
    // Belt-and-suspenders around the whole startup sequence (#5): JsonFileSettingsStore::Load()
    // already falls back to Settings::Defaults() rather than throwing on a corrupt/invalid
    // settings file, so this should never actually fire for that specific case -- but
    // nothing else in AppContext construction gets a free pass to bring down the process
    // with an unhandled exception and no diagnostic either. Logged, not silent.
    try {
        settings::JsonFileSettingsStore bootstrapStore(settings::DefaultSettingsFilePath(),
                                                        settings::LegacySettingsFilePath());
        const settings::Settings settings = bootstrapStore.Load();

        // Loaded before Logger::Init (rather than the previous hardcoded LogLevel::Info)
        // so advanced.logLevel actually takes effect from the first line logged -- see
        // issue #18, which found this setting persisted and validated but never read.
        logging::Logger::Init(logging::DefaultLogDirectory() + "/application.log", ResolveLogLevel(settings));

        AppContext app(settings);

        const bool selfTest = argc > 1 && std::string(argv[1]) == "--selftest";
        if (selfTest) {
            RunSelfTest(app);
            return 0;
        }

        RunIpcLoop(app);
        return 0;
    } catch (const std::exception& e) {
        logging::Log::Error("main", std::string("Fatal error during startup: ") + e.what());
        return 1;
    }
}
