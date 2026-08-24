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

#include <chrono>
#include <algorithm>
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

#include "core/GravityVersion.h"
#include "core/downloads/IDownloadProvider.h"
#include "core/downloads/NdjsonLineProtocol.h"
#include "core/downloads/QualityPreset.h"
#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/events/Event.h"
#include "core/events/EventBus.h"
#include "core/filesystem/ExecutablePath.h"
#include "core/filesystem/FileInfo.h"
#include "core/filesystem/LocalFileSystem.h"
#include "core/filesystem/PathUtils.h"
#include "core/hardware/HardwareInfo.h"
#include "core/hardware/WindowsHardwareDetector.h"
#include "core/jobs/CompressionJob.h"
#include "core/jobs/ConversionJob.h"
#include "core/jobs/DownloadJob.h"
#include "core/jobs/JobManager.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/Progress.h"
#include "core/jobs/TestJob.h"
#include "core/logging/Logger.h"
#include "core/process/IProcessRunner.h"
#include "core/media/ProcessingOptions.h"
#include "core/process/RealProcessRunner.h"
#include "core/queue/QueuePersistence.h"
#include "core/queue/QueueTypes.h"
#include "core/settings/JsonFileSettingsStore.h"
#include "core/settings/Settings.h"
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

// Monotonic counter stamped on every event line. Assigned INSIDE g_stdoutMutex (see
// WriteEventLine) rather than where the event is constructed: several threads publish
// concurrently, so numbering at construction time would hand out increasing numbers that
// then reach the wire out of order. Numbering at the write point makes the sequence and the
// byte order on stdout the same ordering, which is the property the frontend's event
// reconciliation actually relies on (spec section 57).
std::int64_t g_eventSequence = 0;

// nlohmann's default dump() is strict about UTF-8 and THROWS if a string anywhere in the
// payload contains an invalid byte sequence (json.exception.type_error.316) -- found by
// Phase 8 IPC fuzzing: a request line containing raw invalid UTF-8 bytes causes
// json::parse() to fail with a diagnostic message that itself embeds a snippet of the
// offending input, that message lands in this response's "error" field, and dump()
// throwing on IT, uncaught, was an unconditional process crash reachable by anything sent
// over stdin -- a remote DoS with a two-line reproduction. error_handler_t::replace makes
// dump() substitute U+FFFD for an invalid byte instead of throwing, so serialization can
// never fail regardless of what ended up in a string (a message, a filename, anything).
constexpr auto kJsonDumpErrorHandler = nlohmann::json::error_handler_t::replace;

void WriteLine(const json& payload) {
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    std::cout << payload.dump(-1, ' ', false, kJsonDumpErrorHandler) << std::endl;
}

// Reads the counter without consuming a number. Used by getQueueSnapshot to tell the
// frontend how far the snapshot already accounts for.
std::int64_t CurrentEventSequence() {
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    return g_eventSequence;
}

void WriteEventLine(const events::Event& event) {
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    json payload = event.ToJson();
    payload["seq"] = ++g_eventSequence;
    std::cout << payload.dump(-1, ' ', false, kJsonDumpErrorHandler) << std::endl;
}

// --- path resolution ---------------------------------------------------------------------
// Env var overrides mirror the same MEDIATOOL_CORE_PATH-style pattern used on the Rust
// side (app/desktop/src-tauri) -- see docs/development.md. Below the override, defaults
// resolve relative to this executable's own directory, never the process's current
// working directory (Phase 7, "no CWD dependency" -- a Start Menu shortcut, a Desktop
// shortcut, and `npm run tauri dev` all set CWD differently, and a packaged install must
// work regardless of which one launched it). Falls back to a repo-relative dev path only
// when the executable directory itself can't be determined at all.
std::string EnvOr(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    return (value && *value) ? std::string(value) : fallback;
}

// A literal (non-bare-command) path is passed straight through to CreateProcess on
// Windows without a PATH search, and CreateProcess does not reliably accept a
// forward-slash path there the way POSIX-style tools do -- construct through
// std::filesystem::path and normalize to the native separator so the resolved default
// actually resolves instead of failing with "cannot find the file specified".
std::string NativePath(const std::string& value) {
    return stdfs::path(value).make_preferred().string();
}

// Where a packaged install's bundled resources live. MEDIATOOL_RESOURCE_DIR, when set, is
// authoritative -- it is how the Rust shell (core_bridge.rs) tells this process exactly
// where Tauri placed bundle.resources, which is the one place that genuinely knows (it
// wrote tauri.conf.json). Absent that -- this binary launched directly, e.g. --selftest
// during development -- falls back to <executable directory>/resources, and finally to
// the working directory if even the executable's own location can't be determined.
stdfs::path ResourceDirectory() {
    if (const char* fromShell = std::getenv("MEDIATOOL_RESOURCE_DIR"); fromShell && *fromShell) {
        return stdfs::path(fromShell);
    }
    if (auto exeDir = filesystem::ExecutableDirectory(); exeDir.has_value()) {
        return stdfs::path(*exeDir) / "resources";
    }
    return stdfs::path(".");
}

std::string ResolvePythonExecutable() {
    if (const char* override = std::getenv("MEDIATOOL_PYTHON_PATH"); override && *override) {
        return NativePath(override);
    }
#ifdef _WIN32
    return NativePath((ResourceDirectory() / "python" / "python.exe").string());
#else
    return NativePath((ResourceDirectory() / "python" / "bin" / "python3").string());
#endif
}

std::string ResolveDownloaderScript() {
    if (const char* override = std::getenv("MEDIATOOL_DOWNLOADER_SCRIPT"); override && *override) {
        return NativePath(override);
    }
    return NativePath((ResourceDirectory() / "downloader" / "downloader.py").string());
}

// --- the wired-up application -----------------------------------------------------------

// --- IPC input validation ----------------------------------------------------------------
// Everything below the IPC boundary treats request params as untrusted (spec section 54).
// These helpers are the single place a raw JSON value becomes a typed, range-checked value;
// no handler reads params.at(...) and uses the result directly.

[[noreturn]] void ThrowInvalidParams(const std::string& message, const std::string& details = "") {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_INVALID_PARAMS", errors::ErrorCategory::Unknown, message, details));
}

std::string RequireString(const json& params, const char* key) {
    if (!params.contains(key) || !params.at(key).is_string())
        ThrowInvalidParams(std::string("Missing or non-string parameter: ") + key);
    return params.at(key).get<std::string>();
}

std::string RequireJobId(const json& params, const char* key = "jobId") {
    const std::string id = RequireString(params, key);
    // Job ids are opaque, but they are also map keys and log tokens: bound the length and
    // reject control characters rather than passing an arbitrary blob straight through.
    if (id.empty() || id.size() > 128)
        ThrowInvalidParams("A job id must be between 1 and 128 characters.");
    for (unsigned char c : id) {
        if (c < 0x20 || c == 0x7f) ThrowInvalidParams("A job id must not contain control characters.");
    }
    return id;
}

// Validates a filesystem path that arrived over IPC. This process only ever receives paths
// the user picked in their own file dialog, so the point is not to sandbox them -- it is to
// reject the malformed values that would otherwise reach std::filesystem or a child
// process argv, and to refuse relative ".." traversal outright since nothing legitimate
// sends it (spec section 54).
std::string RequirePath(const json& params, const char* key) {
    const std::string path = RequireString(params, key);
    if (path.empty()) ThrowInvalidParams(std::string(key) + " must not be empty.");
    if (path.size() > 4096) ThrowInvalidParams(std::string(key) + " is unreasonably long.");
    if (path.find('\0') != std::string::npos)
        ThrowInvalidParams(std::string(key) + " must not contain a null character.");
    for (const auto& part : stdfs::path(path)) {
        if (part == "..")
            ThrowInvalidParams("Paths containing \"..\" are not accepted.", key + std::string("=") + path);
    }
    return path;
}

// --- job construction --------------------------------------------------------------------
// createJob and restart recovery must build a job the SAME way -- a job restored from the
// state file has to behave exactly like the one the user originally asked for. So both go
// through these builders, and the validated params are what get persisted.

struct JobDependencies {
    downloads::IDownloadProvider& provider;
    filesystem::IFileSystem& fileSystem;
    media::IMediaEngine& mediaEngine;
};

// Real URLs run from a handful of characters to a few thousand (query strings, tracking
// parameters); nothing legitimate approaches this. Bounds the same class of problem
// RequirePath already bounds for paths -- found by Phase 8 IPC fuzzing: an unbounded url
// string was accepted whole (duplicate-key computation, persisted queue state, eventually
// a subprocess argv) with no upfront rejection.
constexpr std::size_t kMaxUrlLength = 8192;

// Shared by createJob{type:DOWNLOAD}, the job factory, and inspectDownloadUrl -- all three
// take a raw URL from the frontend and must reject it up front (spec section 4) rather than
// letting an obviously-unsupported string reach a subprocess launch.
void ValidateDownloadUrlValue(downloads::IDownloadProvider& provider, const std::string& url) {
    if (url.size() > kMaxUrlLength) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_DOWNLOAD_URL", errors::ErrorCategory::UnsupportedFormat,
            "That URL is unreasonably long.", "length=" + std::to_string(url.size())));
    }
    if (url.empty() || !provider.CanHandle(url)) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_DOWNLOAD_URL", errors::ErrorCategory::UnsupportedFormat,
            "This URL is not a supported http/https media URL.", "url=" + url));
    }
}

std::unique_ptr<jobs::Job> BuildDownloadJob(JobDependencies deps, const json& params) {
    jobs::DownloadJob::Options options;
    options.url = RequireString(params, "url");
    options.outputDirectory = RequirePath(params, "outputDirectory");
    ValidateDownloadUrlValue(deps.provider, options.url);
    if (params.contains("quality")) {
        try {
            options.quality =
                downloads::QualityPresetFromWireString(RequireString(params, "quality"));
        } catch (const std::invalid_argument& e) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_INVALID_QUALITY_PRESET", errors::ErrorCategory::UnsupportedFormat, e.what()));
        }
    }
    return std::make_unique<jobs::DownloadJob>(options, deps.provider, deps.fileSystem,
                                               &deps.mediaEngine);
}

// A processing job's input is either a path the caller gave us, or -- for a pipeline stage --
// another job's output, which is not knowable until that job has run. In the second case the
// path is left empty here and JobManager fills it in immediately before the job executes
// (see Job::ApplyResolvedInput).
std::string ReadProcessingInputPath(const json& params) {
    if (params.contains("inputFromJobId")) {
        if (params.contains("inputPath")) {
            ThrowInvalidParams(
                "Give either inputPath or inputFromJobId, not both.",
                "a pipeline stage takes its input from the job it follows");
        }
        return std::string();
    }
    return RequirePath(params, "inputPath");
}

std::unique_ptr<jobs::Job> BuildConversionJob(JobDependencies deps, const json& params) {
    jobs::ConversionJob::Options options;
    options.common.inputPath = ReadProcessingInputPath(params);
    options.common.outputDirectory = RequirePath(params, "outputDirectory");
    if (params.contains("outputFilenameBase"))
        options.common.outputFilenameBase = RequireString(params, "outputFilenameBase");
    // FromJson does the enum/range checking and throws UnsupportedFormat on anything it
    // does not recognize -- deliberately no silent fallback to a default format.
    options.request = media::ConversionRequest::FromJson(params);
    return std::make_unique<jobs::ConversionJob>(options, deps.mediaEngine, deps.fileSystem);
}

std::unique_ptr<jobs::Job> BuildCompressionJob(JobDependencies deps, const json& params) {
    jobs::CompressionJob::Options options;
    options.common.inputPath = ReadProcessingInputPath(params);
    options.common.outputDirectory = RequirePath(params, "outputDirectory");
    if (params.contains("outputFilenameBase"))
        options.common.outputFilenameBase = RequireString(params, "outputFilenameBase");
    options.request = media::CompressionRequest::FromJson(params);
    // Compression keeps the source container by default; the output still needs a concrete
    // extension before the encode starts. When the input is a pipeline stage's output the
    // source extension is not known yet, so an explicit outputExtension is the only way to
    // say anything but mp4.
    const std::string sourceExtension = filesystem::paths::GetExtension(options.common.inputPath);
    options.outputExtension = params.contains("outputExtension")
                                  ? RequireString(params, "outputExtension")
                                  : (sourceExtension.empty() ? "mp4" : sourceExtension);
    return std::make_unique<jobs::CompressionJob>(options, deps.mediaEngine, deps.fileSystem);
}

// The production IJobFactory. Restart recovery hands it a persisted JobSpec and gets back a
// job wired to the real provider/filesystem/engine -- the same objects createJob uses.
class RealJobFactory final : public jobs::IJobFactory {
public:
    RealJobFactory(downloads::IDownloadProvider& provider, filesystem::IFileSystem& fileSystem,
                   media::IMediaEngine& mediaEngine)
        : deps_{provider, fileSystem, mediaEngine} {}

    std::unique_ptr<jobs::Job> Create(const queue::JobSpec& spec) override {
        switch (spec.type) {
            case jobs::JobType::Download: return BuildDownloadJob(deps_, spec.params);
            case jobs::JobType::Conversion: return BuildConversionJob(deps_, spec.params);
            case jobs::JobType::Compression: return BuildCompressionJob(deps_, spec.params);
            case jobs::JobType::Test: return std::make_unique<jobs::TestJob>();
            case jobs::JobType::Batch:
            case jobs::JobType::Workflow:
                break;
        }
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_JOB_TYPE_NOT_IMPLEMENTED", errors::ErrorCategory::UnsupportedFormat,
            jobs::ToWireString(spec.type) + " jobs are not implemented yet.",
            "see docs/roadmap.md"));
    }

private:
    JobDependencies deps_;
};

struct AppContext {
    process::RealProcessRunner processRunner;
    events::EventBus eventBus;
    settings::JsonFileSettingsStore settingsStore{settings::DefaultSettingsFilePath()};
    hardware::WindowsHardwareDetector hardwareDetector;
    filesystem::LocalFileSystem fileSystem;
    media::FFmpegEngine ffmpegEngine;
    downloader::YtDlpProvider ytDlpProvider;
    // Declared before jobManager: the manager takes a pointer to it at construction, so it
    // has to be alive first (and outlive it, which reverse-order destruction gives us).
    RealJobFactory jobFactory;
    jobs::JobManager jobManager;

    // Tracks each job's previous state purely to classify the Running state as either
    // "resumed from pause" or "(re)started" when JobManager reports a transition -- see
    // the comment on PublishJobStateChanged below.
    std::mutex previousStateMutex;
    std::unordered_map<jobs::JobId, jobs::JobState> previousState;

    static jobs::JobManager::Options MakeJobManagerOptions(const settings::Settings& settings) {
        jobs::JobManager::Options options;
        options.maxConcurrentJobs =
            static_cast<std::size_t>(std::max(1, settings.processing.concurrentJobs));
        options.stateFilePath = queue::QueuePersistence::DefaultStateFilePath();
        return options;
    }

    explicit AppContext(const settings::Settings& settings)
        : ffmpegEngine(processRunner,
                       settings.advanced.ffmpegPath.empty()
                           ? std::nullopt
                           : std::optional<std::string>(settings.advanced.ffmpegPath),
                       std::nullopt),
          // Resolved once at startup (not per-download) and handed to yt-dlp so it merges
          // separate video/audio streams via the SAME ffmpeg binary the rest of the app
          // already uses -- see docs/decisions.md "Video/audio merge strategy".
          ytDlpProvider(processRunner, ResolvePythonExecutable(), ResolveDownloaderScript(),
                        media::DiscoverFfmpegPath(processRunner, settings.advanced.ffmpegPath.empty()
                                                                      ? std::nullopt
                                                                      : std::optional<std::string>(
                                                                            settings.advanced.ffmpegPath))
                            .value_or("")),
          jobFactory(ytDlpProvider, fileSystem, ffmpegEngine),
          jobManager(MakeJobManagerOptions(settings), &jobFactory) {}
};

// --- event publishing from JobManager callbacks -----------------------------------------

json ProgressAndState(const jobs::Progress& progress, const char* stateWire) {
    json data = progress.ToJson();
    data["state"] = stateWire;
    return data;
}

// Every event this process publishes gets a monotonic sequence number from the JobManager,
// so the frontend can drop anything that arrives after a newer event it already applied
// (spec section 57). Publishing goes through here so no call site can forget.
void Publish(AppContext& app, events::EventType type, json data,
             std::optional<std::string> jobId = std::nullopt) {
    app.eventBus.Publish(events::MakeEvent(type, std::move(data), std::move(jobId)));
}

json QueueStateData(AppContext& app) {
    const auto snapshot = app.jobManager.GetQueueSnapshot();
    return {{"runState", queue::ToWireString(snapshot.runState)},
            {"maxConcurrency", snapshot.maxConcurrency},
            {"statistics", snapshot.statistics.ToJson()},
            {"pendingOrder", snapshot.pendingOrder}};
}

void PublishQueueChanged(AppContext& app) {
    Publish(app, events::EventType::QueueChanged, QueueStateData(app));
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

    // Every job event carries the job's revision alongside its state. Combined with the
    // event sequence number that gives the frontend two independent ways to reject a stale
    // update, which matters because progress and state events travel the same channel.
    json data{{"state", jobs::ToWireString(state)}};
    std::optional<jobs::JobManager::JobSnapshot> snapshot;
    try {
        snapshot = app.jobManager.GetJob(id);
        data["revision"] = snapshot->revision;
    } catch (const errors::MediaToolException&) {
        // The job was cleared from history between the transition and this lookup. The
        // event is still worth publishing; it just carries no revision.
    }

    switch (state) {
        case JobState::Starting:
            Publish(app, events::EventType::JobStarted, data, id);
            return;
        case JobState::Running:
            if (previous == JobState::Paused) {
                Publish(app, events::EventType::JobResumed, data, id);
            } else {
                // Covers the normal Starting->Running step and a Retrying->Running restart.
                Publish(app, events::EventType::JobStarted, data, id);
            }
            return;
        case JobState::Paused:
            Publish(app, events::EventType::JobPaused, data, id);
            return;
        case JobState::Completed:
            if (snapshot && snapshot->result) data["result"] = *snapshot->result;
            Publish(app, events::EventType::JobCompleted, data, id);
            return;
        case JobState::Failed:
            if (snapshot && snapshot->error) data["error"] = snapshot->error->ToJson();
            Publish(app, events::EventType::JobFailed, data, id);
            return;
        case JobState::Cancelled:
            Publish(app, events::EventType::JobCancelled, data, id);
            return;
        case JobState::Skipped:
            if (snapshot && snapshot->error) data["error"] = snapshot->error->ToJson();
            Publish(app, events::EventType::JobSkipped, data, id);
            return;
        case JobState::Queued:
        case JobState::Waiting:
            // A job becoming runnable, or becoming blocked again, as its dependencies
            // resolve. jobQueued carries both -- the payload's `state` distinguishes them.
            Publish(app, events::EventType::JobQueued, data, id);
            return;
        case JobState::RetryWait:
            // The jobRetryScheduled event (published from the retry callback) carries the
            // attempt count and delay; this transition on its own would say less.
            return;
        case JobState::Retrying:
            // A momentary internal state with no wire event of its own -- the follow-up
            // Retrying->Running transition above covers it.
            return;
    }
}

void PublishJobProgress(AppContext& app, const jobs::JobId& id, const jobs::Progress& progress) {
    // Already throttled by JobManager (spec section 28) -- this is not the place to add a
    // second, competing rate limit.
    Publish(app, events::EventType::JobProgress, ProgressAndState(progress, "RUNNING"), id);
}

void PublishRetryScheduled(AppContext& app, const jobs::JobId& id, int attempt,
                           std::int64_t delayMs, const std::string& reason) {
    json data{{"state", "RETRY_WAIT"},
              {"attempt", attempt},
              {"delayMs", delayMs},
              {"reason", reason}};
    try {
        const auto snapshot = app.jobManager.GetJob(id);
        data["revision"] = snapshot.revision;
        data["maxRetries"] = snapshot.maxRetries;
        if (snapshot.nextRetryAtMs) data["nextRetryAtMs"] = *snapshot.nextRetryAtMs;
        if (snapshot.error) data["error"] = snapshot.error->ToJson();
    } catch (const errors::MediaToolException&) {
    }
    Publish(app, events::EventType::JobRetryScheduled, data, id);
}

// --- command handlers --------------------------------------------------------------------

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

void ValidateDownloadUrl(AppContext& app, const std::string& url) {
    ValidateDownloadUrlValue(app.ytDlpProvider, url);
}

json HandleInspectDownloadUrl(AppContext& app, const json& params) {
    const std::string url = params.at("url").get<std::string>();
    ValidateDownloadUrl(app, url);
    const downloads::DownloadMetadata metadata = app.ytDlpProvider.Inspect(url, [] { return false; });
    return {{"metadata", DownloadMetadataToJson(metadata)}};
}

// A coarse floor, not a real "will this download fit" check (that needs the file size,
// which isn't known until Inspect() runs inside the job) -- catches the "drive is already
// essentially full" case up front (spec section 11).
void RequireSomeFreeSpace(AppContext& app, const std::string& outputDirectory) {
    constexpr std::uint64_t kMinFreeBytes = 100ull * 1024 * 1024;
    if (auto available = app.fileSystem.GetAvailableDiskSpace(outputDirectory);
        available && *available < kMinFreeBytes) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INSUFFICIENT_DISK_SPACE", errors::ErrorCategory::DiskSpaceError,
            "Not enough free disk space at the selected output directory.",
            "available=" + std::to_string(*available) + " bytes"));
    }
}

// Reads the scheduling options every job type accepts, independent of what the job does.
jobs::JobManager::SubmitRequest ParseSubmitRequest(AppContext& app, jobs::JobType type,
                                                   const json& params, const json& jobParams) {
    jobs::JobManager::SubmitRequest request;
    request.spec.type = type;
    request.spec.params = jobParams;

    if (params.contains("priority"))
        request.priority = queue::JobPriorityFromWireString(RequireString(params, "priority"));

    if (params.contains("dependsOn")) {
        const auto& dependsOn = params.at("dependsOn");
        if (!dependsOn.is_array()) ThrowInvalidParams("dependsOn must be an array of job ids.");
        if (dependsOn.size() > 32) ThrowInvalidParams("A job may declare at most 32 dependencies.");
        for (const auto& entry : dependsOn) {
            if (!entry.is_string()) ThrowInvalidParams("dependsOn entries must be strings.");
            const std::string dependencyId = entry.get<std::string>();
            // Checked here as well as in the scheduler so the caller gets a message naming
            // the unknown id rather than a generic insert failure.
            app.jobManager.GetJob(dependencyId);
            request.dependencies.push_back(dependencyId);
        }
    }
    if (params.contains("parentJobId")) {
        const std::string parent = RequireJobId(params, "parentJobId");
        app.jobManager.GetJob(parent);
        request.parentJobId = parent;
    }
    if (params.contains("retryPolicy")) {
        // FromJson validates and throws on out-of-range values; nothing is clamped silently.
        request.retryPolicy = queue::RetryPolicy::FromJson(params.at("retryPolicy"));
    }
    if (params.contains("allowDuplicate")) {
        if (!params.at("allowDuplicate").is_boolean())
            ThrowInvalidParams("allowDuplicate must be a boolean.");
        request.allowDuplicate = params.at("allowDuplicate").get<bool>();
    }
    // Taking input from another job necessarily means depending on it. Adding the edge here
    // rather than making the caller remember both is what stops a pipeline stage from being
    // dispatched before the job it reads from has produced anything.
    if (jobParams.contains("inputFromJobId")) {
        const std::string sourceId = RequireJobId(jobParams, "inputFromJobId");
        app.jobManager.GetJob(sourceId);  // throws E_JOB_NOT_FOUND with a clear message
        if (std::find(request.dependencies.begin(), request.dependencies.end(), sourceId) ==
            request.dependencies.end()) {
            request.dependencies.push_back(sourceId);
        }
    }

    request.duplicateKey = queue::MakeDuplicateKey(type, jobParams);
    return request;
}

json HandleCreateJob(AppContext& app, const json& params) {
    const std::string typeWire = RequireString(params, "type");
    jobs::JobType type;
    try {
        type = jobs::JobTypeFromWireString(typeWire);
    } catch (const std::invalid_argument&) {
        ThrowInvalidParams("Unknown job type: " + typeWire);
    }

    const json jobParams = params.value("params", json::object());
    if (!jobParams.is_object()) ThrowInvalidParams("params must be an object.");

    const std::string duplicateKey = queue::MakeDuplicateKey(type, jobParams);
    const bool allowDuplicate =
        params.contains("allowDuplicate") && params.at("allowDuplicate").is_boolean() &&
        params.at("allowDuplicate").get<bool>();

    JobDependencies deps{app.ytDlpProvider, app.fileSystem, app.ffmpegEngine};
    std::unique_ptr<jobs::Job> job;
    switch (type) {
        case jobs::JobType::Download:
            RequireSomeFreeSpace(app, RequirePath(jobParams, "outputDirectory"));
            job = BuildDownloadJob(deps, jobParams);
            break;
        case jobs::JobType::Conversion:
            RequireSomeFreeSpace(app, RequirePath(jobParams, "outputDirectory"));
            job = BuildConversionJob(deps, jobParams);
            break;
        case jobs::JobType::Compression:
            RequireSomeFreeSpace(app, RequirePath(jobParams, "outputDirectory"));
            job = BuildCompressionJob(deps, jobParams);
            break;
        case jobs::JobType::Test:
            job = std::make_unique<jobs::TestJob>();
            break;
        case jobs::JobType::Batch:
        case jobs::JobType::Workflow:
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_JOB_TYPE_NOT_IMPLEMENTED", errors::ErrorCategory::UnsupportedFormat,
                typeWire + " jobs are scaffolded (see docs/roadmap.md) but not runnable yet.",
                "", false));
    }

    auto request = ParseSubmitRequest(app, type, params, jobParams);
    const jobs::JobId id = job->Id();
    try {
        app.jobManager.SubmitJob(std::move(job), request);
    } catch (const errors::MediaToolException& e) {
        if (e.Info().code == "E_DUPLICATE_JOB" && !allowDuplicate) {
            // Policy: reject the duplicate and name the job it collided with, so the UI can
            // focus the existing one rather than quietly starting a second identical
            // download (spec section 20).
            errors::ErrorInfo info = e.Info();
            info.recoverable = true;
            throw errors::MediaToolException(info);
        }
        throw;
    }

    json created{{"state", "QUEUED"}, {"type", typeWire}};
    try {
        const auto snapshot = app.jobManager.GetJob(id);
        created["state"] = jobs::ToWireString(snapshot.state);
        created["revision"] = snapshot.revision;
    } catch (const errors::MediaToolException&) {
    }
    Publish(app, events::EventType::JobCreated, created, id);
    PublishQueueChanged(app);
    return {{"jobId", id}, {"duplicateKey", duplicateKey}};
}

json HandleGetJob(AppContext& app, const json& params) {
    return {{"job", app.jobManager.GetJob(params.at("jobId").get<std::string>()).ToJson()}};
}

json HandleListJobs(AppContext& app, const json&) {
    json jobsArray = json::array();
    for (const auto& snapshot : app.jobManager.ListJobs()) jobsArray.push_back(snapshot.ToJson());
    return {{"jobs", jobsArray}};
}

json HandleCancelJob(AppContext& app, const json& params) {
    app.jobManager.CancelJob(RequireJobId(params));
    return json::object();
}

json HandlePauseJob(AppContext& app, const json& params) {
    app.jobManager.PauseJob(RequireJobId(params));
    return json::object();
}

json HandleResumeJob(AppContext& app, const json& params) {
    app.jobManager.ResumeJob(RequireJobId(params));
    return json::object();
}

json HandleRetryJob(AppContext& app, const json& params) {
    app.jobManager.RetryJob(RequireJobId(params));
    return json::object();
}

json HandleRemoveJob(AppContext& app, const json& params) {
    app.jobManager.RemoveJob(RequireJobId(params));
    return json::object();
}

// --- queue commands ------------------------------------------------------------------------
// The complete queue state in one round trip. The frontend calls this on start and on
// reconnect; incremental events keep it current in between (spec section 29).
json HandleGetQueueSnapshot(AppContext& app, const json&) {
    // Sequence read BEFORE the snapshot on purpose. The frontend applies events with
    // seq > sequence, so this direction can only ever make it re-apply an event the
    // snapshot already contains -- harmless, since every event assigns state rather than
    // mutating it incrementally. Reading afterwards could make it skip an event whose
    // effect had not landed in the snapshot yet, which would leave the two out of step.
    const std::int64_t sequence = CurrentEventSequence();
    auto queueJson = app.jobManager.GetQueueSnapshot().ToJson();
    queueJson["sequence"] = sequence;
    return {{"queue", queueJson}};
}

json HandleSetJobPriority(AppContext& app, const json& params) {
    app.jobManager.SetJobPriority(RequireJobId(params),
                                  queue::JobPriorityFromWireString(RequireString(params, "priority")));
    PublishQueueChanged(app);
    return json::object();
}

json HandleMoveJob(AppContext& app, const json& params) {
    app.jobManager.MoveJob(RequireJobId(params),
                           queue::MoveDirectionFromWireString(RequireString(params, "direction")));
    PublishQueueChanged(app);
    return json::object();
}

json HandlePauseQueue(AppContext& app, const json&) {
    app.jobManager.PauseQueue();
    PublishQueueChanged(app);
    return {{"runState", "PAUSED"}};
}

json HandleResumeQueue(AppContext& app, const json&) {
    app.jobManager.ResumeQueue();
    PublishQueueChanged(app);
    return {{"runState", "RUNNING"}};
}

json HandleSetConcurrency(AppContext& app, const json& params) {
    if (!params.contains("maxConcurrency") || !params.at("maxConcurrency").is_number_integer())
        ThrowInvalidParams("maxConcurrency must be a whole number.");
    const std::int64_t requested = params.at("maxConcurrency").get<std::int64_t>();
    // Bounded rather than trusted: this crosses the IPC boundary, and an absurd value would
    // mean an absurd number of concurrent ffmpeg processes (spec section 54).
    if (requested < 1 || requested > 16)
        ThrowInvalidParams("maxConcurrency must be between 1 and 16.",
                           "got " + std::to_string(requested));

    app.jobManager.SetMaxConcurrency(static_cast<std::size_t>(requested));

    // Keep the persisted setting in step, so the choice survives a restart the same way the
    // rest of Settings does.
    settings::Settings updated = app.settingsStore.Load();
    updated.processing.concurrentJobs = static_cast<int>(requested);
    app.settingsStore.Save(updated);

    PublishQueueChanged(app);
    return {{"maxConcurrency", requested}};
}

json HandleClearHistory(AppContext& app, const json& params) {
    const queue::HistoryScope scope =
        params.contains("scope") ? queue::HistoryScopeFromWireString(RequireString(params, "scope"))
                                 : queue::HistoryScope::All;
    // Queue history and media files are separate concerns: this removes queue entries and
    // never touches a file on disk (spec section 27).
    const auto removed = app.jobManager.ClearHistory(scope);
    PublishQueueChanged(app);
    return {{"removedJobIds", removed}, {"removedCount", removed.size()}};
}

json HandleRetryFailedJobs(AppContext& app, const json&) {
    const auto retried = app.jobManager.RetryAllFailed();
    PublishQueueChanged(app);
    return {{"retriedJobIds", retried}, {"retriedCount", retried.size()}};
}

// The closed sets of enum values the frontend is allowed to send, so it can build its
// pickers from the backend's truth rather than a hand-copied list that can drift.
json HandleGetProcessingCapabilities(AppContext& app, const json&) {
    return {{"targetFormats", media::AllTargetFormatWireStrings()},
            {"compressionPresets", json::array({"LOW", "MEDIUM", "HIGH"})},
            {"priorities", json::array({"LOW", "NORMAL", "HIGH"})},
            {"ffmpegAvailable", app.ffmpegEngine.IsAvailable()}};
}

json HandleInspectFile(AppContext& app, const json& params) {
    return {{"fileInfo", InspectFileEnriched(app, params.at("path").get<std::string>()).ToJson()}};
}

json HandleGetCapabilities(AppContext& app, const json& params) {
    const std::string path = params.at("path").get<std::string>();
    filesystem::FileInfo info = app.fileSystem.Inspect(path);
    return {{"capabilities", filesystem::CapabilitiesFor(info.category, info.extension)}};
}

json HandleGetSettings(AppContext& app, const json&) {
    return {{"settings", app.settingsStore.Load().ToJson()}};
}

json HandleUpdateSettings(AppContext& app, const json& params) {
    json merged = app.settingsStore.Load().ToJson();
    merged.merge_patch(params.at("settings"));
    settings::Settings updated = settings::Settings::FromJson(merged);
    app.settingsStore.Save(updated);
    return {{"settings", updated.ToJson()}};
}

json HandleGetHardwareInfo(AppContext& app, const json&) {
    return {{"hardwareInfo", app.hardwareDetector.Detect().ToJson()}};
}

// Runs the downloader script's own --version mode (a single line on stdout, not NDJSON --
// see python/downloader/downloader.py's run_version()) and returns whatever it printed, or
// nullopt if the process couldn't be launched or produced nothing usable. Never throws:
// this is diagnostic information for an About panel, not something a missing Python
// installation should turn into a hard IPC failure.
std::optional<std::string> DiscoverYtDlpVersion(AppContext& app) {
    try {
        std::vector<std::string> lines;
        auto proc = app.processRunner.Start(
            ResolvePythonExecutable(), {ResolveDownloaderScript(), "--version"}, {},
            [&lines](const std::string& line) { lines.push_back(line); }, [](const std::string&) {});
        const auto result = proc->WaitFor(5000);
        if (!result.has_value() || result->exitCode != 0 || lines.empty()) return std::nullopt;
        return lines.front();
    } catch (...) {
        return std::nullopt;
    }
}

// About panel data (spec: "Gravity version, FFmpeg version, yt-dlp version"). Every field
// is best-effort -- a missing FFmpeg or Python install is a normal, already-surfaced state
// elsewhere (Settings, the processing capabilities check), not a reason for this command to
// fail; absent fields are simply omitted rather than reported as an error.
json HandleGetVersionInfo(AppContext& app, const json&) {
    json result = {{"gravityVersion", kGravityVersion}};
    if (auto ffmpegVersion = app.ffmpegEngine.Version(); ffmpegVersion.has_value()) {
        result["ffmpegVersion"] = *ffmpegVersion;
    }
    if (auto ytDlpVersion = DiscoverYtDlpVersion(app); ytDlpVersion.has_value()) {
        result["ytDlpVersion"] = *ytDlpVersion;
    }
    return {{"versionInfo", result}};
}

using Handler = std::function<json(AppContext&, const json&)>;

const std::unordered_map<std::string, Handler>& CommandTable() {
    static const std::unordered_map<std::string, Handler> table{
        {"createJob", HandleCreateJob},
        {"getJob", HandleGetJob},
        {"listJobs", HandleListJobs},
        {"cancelJob", HandleCancelJob},
        {"pauseJob", HandlePauseJob},
        {"resumeJob", HandleResumeJob},
        {"retryJob", HandleRetryJob},
        {"removeJob", HandleRemoveJob},
        {"getQueueSnapshot", HandleGetQueueSnapshot},
        {"setJobPriority", HandleSetJobPriority},
        {"moveJob", HandleMoveJob},
        {"pauseQueue", HandlePauseQueue},
        {"resumeQueue", HandleResumeQueue},
        {"setConcurrency", HandleSetConcurrency},
        {"clearHistory", HandleClearHistory},
        {"retryFailedJobs", HandleRetryFailedJobs},
        {"getProcessingCapabilities", HandleGetProcessingCapabilities},
        {"inspectFile", HandleInspectFile},
        {"inspectDownloadUrl", HandleInspectDownloadUrl},
        {"getCapabilities", HandleGetCapabilities},
        {"getSettings", HandleGetSettings},
        {"updateSettings", HandleUpdateSettings},
        {"getHardwareInfo", HandleGetHardwareInfo},
        {"getVersionInfo", HandleGetVersionInfo},
    };
    return table;
}

// --- the IPC loop --------------------------------------------------------------------

void RunIpcLoop(AppContext& app) {
    app.jobManager.OnJobStateChanged(
        [&app](const jobs::JobId& id, jobs::JobState state) { PublishJobStateChanged(app, id, state); });
    app.jobManager.OnJobProgress(
        [&app](const jobs::JobId& id, const jobs::Progress& progress) { PublishJobProgress(app, id, progress); });
    app.jobManager.OnRetryScheduled([&app](const jobs::JobId& id, int attempt, std::int64_t delayMs,
                                           const std::string& reason) {
        PublishRetryScheduled(app, id, attempt, delayMs, reason);
    });
    app.jobManager.OnQueueChanged([&app] { PublishQueueChanged(app); });
    app.eventBus.Subscribe([](const events::Event& event) { WriteEventLine(event); });
    logging::Logger::SetEventSink([&app](events::Event event) { app.eventBus.Publish(event); });

    logging::Log::Info("mediatool-core", "IPC loop starting");

    // Restored after the subscribers are attached so the frontend sees the recovery events,
    // and before the first request is read so a getQueueSnapshot cannot race it.
    const auto recovery = app.jobManager.RestoreFromDisk();
    if (recovery.status == queue::LoadOutcome::Status::Recovered) {
        logging::Log::Warning("mediatool-core",
                              "queue state could not be read: " + recovery.diagnostic);
    } else if (recovery.restoredJobs > 0 || recovery.interruptedJobs > 0) {
        logging::Log::Info("mediatool-core",
                           "restored " + std::to_string(recovery.restoredJobs) + " queued jobs (" +
                               std::to_string(recovery.interruptedJobs) + " interrupted)");
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        std::string id;
        try {
            json request = json::parse(line);
            id = request.at("id").get<std::string>();
            const std::string command = request.at("command").get<std::string>();
            const json params = request.value("params", json::object());

            const auto& table = CommandTable();
            const auto it = table.find(command);
            if (it == table.end()) {
                WriteLine({{"id", id},
                          {"ok", false},
                          {"error", errors::ErrorInfo::Make("E_UNKNOWN_COMMAND", errors::ErrorCategory::Unknown,
                                                            "Unknown command: " + command)
                                        .ToJson()}});
                continue;
            }

            const json result = it->second(app, params);
            WriteLine({{"id", id}, {"ok", true}, {"result", result}});
        } catch (const errors::MediaToolException& e) {
            WriteLine({{"id", id}, {"ok", false}, {"error", e.Info().ToJson()}});
        } catch (const std::exception& e) {
            WriteLine({{"id", id},
                      {"ok", false},
                      {"error", errors::ErrorInfo::Make("E_UNHANDLED_EXCEPTION", errors::ErrorCategory::Unknown,
                                                        e.what())
                                    .ToJson()}});
        }
    }

    logging::Log::Info("mediatool-core", "stdin closed, shutting down");
    // Stops the scheduler, waits for in-flight jobs, and writes final queue state. Without
    // this the last few transitions would only exist in memory.
    app.jobManager.Shutdown();
}

// --- self-test --------------------------------------------------------------------------

void PrintStep(const std::string& title) {
    std::cout << "\n== " << title << " ==" << std::endl;
}

void RunSelfTest(AppContext& app) {
    std::cout << "Gravity core self-test\n";
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

}  // namespace

int main(int argc, char** argv) {
    logging::Logger::Init(logging::DefaultLogDirectory() + "/application.log", logging::LogLevel::Info);

    settings::JsonFileSettingsStore bootstrapStore(settings::DefaultSettingsFilePath());
    AppContext app(bootstrapStore.Load());

    const bool selfTest = argc > 1 && std::string(argv[1]) == "--selftest";
    if (selfTest) {
        RunSelfTest(app);
        return 0;
    }

    RunIpcLoop(app);
    return 0;
}
