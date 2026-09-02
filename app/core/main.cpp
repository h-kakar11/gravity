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
#include "core/filesystem/ToolPathResolver.h"
#include "core/hardware/HardwareInfo.h"
#include "core/hardware/WindowsHardwareDetector.h"
#include "core/ipc/LineReader.h"
#include "core/ipc/RequestExecutor.h"
#include "core/ipc/RequestValidation.h"
#include "core/jobs/DownloadJob.h"
#include "core/jobs/InProgressJobStore.h"
#include "core/jobs/JobArtifactCleanup.h"
#include "core/jobs/JobHistoryStore.h"
#include "core/jobs/JobSpec.h"
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
#include "engines/downloader/YtDlpFormatSelector.h"
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

// Issue #79. Both of these used to be an environment variable or a CWD-relative literal,
// handed straight to CreateProcess with no existence check at all, so the two ways they
// could go wrong -- a working directory that isn't the repository root, and an environment
// variable whose value carries the quotes a batch file wrapped it in -- both surfaced as
// the same opaque "The system cannot find the file specified" naming a path that reads as
// perfectly valid. Candidates are now anchored to the core executable's own directory
// (see ToolPathResolver.h) and checked before launch.
//
// A tool that resolves to nothing does NOT abort startup: downloading is one feature of
// several, and a missing Python interpreter should not stop conversion, compression or
// settings from working. The empty string propagates to YtDlpProvider, and
// EnsureDownloaderAvailable below turns the next download/inspect attempt into an error
// that names every path that was tried.
const std::vector<std::string> kPythonRelativeCandidates = {
    // Packaged layout (app/desktop/src-tauri/src/core_bridge.rs bundles it here).
    "python/python.exe",
    // Dev layout, Windows venv...
    "python/downloader/.venv/Scripts/python.exe",
    // ...and POSIX venv, so a non-Windows dev build resolves the same way.
    "python/downloader/.venv/bin/python3",
};

const std::vector<std::string> kDownloaderScriptRelativeCandidates = {
    "python/downloader/downloader.py",
};

struct ResolvedTool {
    std::string path;                     // empty when nothing existed
    std::vector<std::string> candidates;  // everything tried, in order, for diagnostics
};

ResolvedTool ResolveTool(const char* envName, const std::vector<std::string>& relativeCandidates) {
    ResolvedTool resolved;
    resolved.candidates = filesystem::BuildToolCandidates(
        EnvOr(envName, ""), filesystem::ExecutableDirectory(), relativeCandidates);
    std::error_code ec;
    resolved.path = filesystem::FirstExisting(resolved.candidates,
                                               [&ec](const std::string& candidate) {
                                                   return stdfs::exists(candidate, ec);
                                               })
                        .value_or(std::string());
    return resolved;
}

ResolvedTool ResolvePythonExecutable() {
    return ResolveTool("MEDIATOOL_PYTHON_PATH", kPythonRelativeCandidates);
}

ResolvedTool ResolveDownloaderScript() {
    return ResolveTool("MEDIATOOL_DOWNLOADER_SCRIPT", kDownloaderScriptRelativeCandidates);
}

// Turns "nothing resolved" into an error a user can act on, at the point a download is
// actually requested. Lists every candidate rather than only the last one, because the
// useful information is which layout the core thought it was running in.
[[noreturn]] void ThrowDownloaderToolMissing(const char* what, const ResolvedTool& tool) {
    std::string details = "Tried, in order:";
    for (const std::string& candidate : tool.candidates) details += "\n  " + candidate;
    details += "\nSet MEDIATOOL_PYTHON_PATH / MEDIATOOL_DOWNLOADER_SCRIPT to override.";
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_DOWNLOADER_NOT_FOUND", errors::ErrorCategory::EngineFailure,
        std::string("The downloader's ") + what + " could not be found.", details));
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
    // Resolved once at startup and kept, not just consumed: the candidate list is what
    // makes a "downloader not found" failure diagnosable (issue #79). Declared before
    // ytDlpProvider so member-initialization order fills them in first.
    ResolvedTool pythonTool{ResolvePythonExecutable()};
    ResolvedTool downloaderScriptTool{ResolveDownloaderScript()};
    downloader::YtDlpProvider ytDlpProvider;
    jobs::JobHistoryStore jobHistoryStore{jobs::DefaultJobHistoryFilePath()};
    // The jobs that have NOT finished, as recipes rather than status reports, so a crash
    // does not lose the queue -- see core/jobs/InProgressJobStore.h.
    jobs::InProgressJobStore inProgressJobStore{jobs::DefaultInProgressJobsFilePath()};
    settings::PresetStore presetStore{settings::DefaultPresetsFilePath()};

    // The downloader health probe starts a process, so it runs at most once and only when
    // something actually asks. Cached rather than probed at startup: a probe on every
    // launch would slow a cold start for information most sessions never need, and it
    // would run before the user has had a chance to point `advanced.ytDlpPath` somewhere
    // that works.
    std::once_flag downloaderInfoOnce;
    downloads::DownloaderInfo downloaderInfo;

    // Tracks each job's previous state purely to classify the Running state as either
    // "resumed from pause" or "(re)started" when JobManager reports a transition -- see
    // the comment on PublishJobStateChanged below.
    std::mutex previousStateMutex;
    std::unordered_map<jobs::JobId, jobs::JobState> previousState;

    // DECLARED LAST, DELIBERATELY, and it is the only member whose position matters.
    // ~JobManager cancels every queued job and joins the worker pool, which fires
    // state-changed callbacks -- and those callbacks touch jobHistoryStore,
    // inProgressJobStore, previousState and previousStateMutex. Members are destroyed in
    // reverse declaration order, so anything declared after this one would already be
    // gone by the time those callbacks run. It used to sit above the two stores, which
    // meant a shutdown with jobs still queued wrote history into a destroyed object.
    jobs::JobManager jobManager;

    explicit AppContext(const settings::Settings& settings)
        : ffmpegEngine(processRunner, EffectiveFfmpegOverride(settings), EffectiveFfprobeOverride()),
          // Resolved once at startup (not per-download) and handed to yt-dlp so it merges
          // separate video/audio streams via the SAME ffmpeg binary the rest of the app
          // already uses -- see docs/decisions.md "Video/audio merge strategy".
          ytDlpProvider(processRunner, pythonTool.path, downloaderScriptTool.path,
                        media::DiscoverFfmpegPath(processRunner, EffectiveFfmpegOverride(settings))
                            .value_or("")),
          jobManager(static_cast<std::size_t>(std::max(1, settings.processing.concurrentJobs)),
                      jobs::RetryPolicy{
                          .maxAttempts = std::max(1, settings.processing.maxRetryAttempts)}) {}
};

// Called before anything that would launch the Python downloader. Startup deliberately
// does not fail when these are missing (issue #79) -- this is where a user finds out, with
// the full candidate list, instead of getting CreateProcess's bare "cannot find the file
// specified" pointing at a path that looks correct.
void EnsureDownloaderAvailable(const AppContext& app) {
    if (app.pythonTool.path.empty()) ThrowDownloaderToolMissing("Python interpreter", app.pythonTool);
    if (app.downloaderScriptTool.path.empty())
        ThrowDownloaderToolMissing("downloader.py script", app.downloaderScriptTool);
}

// Probes the downloader once per process and remembers the answer. The warning is logged
// from here rather than from a startup path so it appears the first time it is relevant --
// which is also the first time a user could act on it.
const downloads::DownloaderInfo& DownloaderInfoCached(AppContext& app) {
    std::call_once(app.downloaderInfoOnce, [&app] {
        app.downloaderInfo = app.ytDlpProvider.Info();
        if (!app.downloaderInfo.available) {
            logging::Log::Warning("downloader",
                                   "The downloader backend is not usable: yt-dlp could not be "
                                   "loaded by the configured Python interpreter. Downloads will "
                                   "fail until it is installed.");
        } else if (app.downloaderInfo.stale) {
            logging::Log::Warning(
                "downloader",
                "yt-dlp " + app.downloaderInfo.version.value_or("(unknown version)") + " is " +
                    std::to_string(app.downloaderInfo.ageDays.value_or(0)) +
                    " days old. Site extractors break within weeks, so downloads are likely to "
                    "fail with errors that look like the video's fault. Update it.");
        }
    });
    return app.downloaderInfo;
}

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
            // Terminal means there is nothing left to recover. Leaving the record would
            // re-run finished work on the next launch.
            app.inProgressJobStore.Remove(id);
            return;
        }
        case JobState::Failed: {
            auto snapshot = app.jobManager.GetJob(id);
            json data{{"state", "FAILED"}};
            if (snapshot.error) data["error"] = snapshot.error->ToJson();
            app.eventBus.Publish(events::MakeEvent(events::EventType::JobFailed, data, id));
            app.jobHistoryStore.Append(snapshot.ToJson());
            app.inProgressJobStore.Remove(id);
            return;
        }
        case JobState::Cancelled: {
            app.eventBus.Publish(
                events::MakeEvent(events::EventType::JobCancelled, {{"state", "CANCELLED"}}, id));
            app.jobHistoryStore.Append(app.jobManager.GetJob(id).ToJson());
            app.inProgressJobStore.Remove(id);
            return;
        }
        case JobState::Retrying: {
            // Not a terminal event, and deliberately not a jobFailed: the attempt failed,
            // the job did not. The frontend needs all four facts to say something honest
            // ("attempt 2 of 3, retrying in 4s, because ...") instead of showing a job
            // flicker through FAILED and back.
            const auto snapshot = app.jobManager.GetJob(id);
            json data{{"state", "RETRYING"},
                       {"attempt", snapshot.attempts},
                       {"maxAttempts", app.jobManager.GetRetryPolicy().maxAttempts},
                       {"retryInMs", 0}};
            if (snapshot.error) data["error"] = snapshot.error->ToJson();
            app.eventBus.Publish(events::MakeEvent(events::EventType::JobRetrying, data, id));
            // The attempt budget has to survive a crash too, or relaunching the app hands
            // a permanently-broken job a fresh three attempts every time.
            app.inProgressJobStore.SetAttemptCount(id, snapshot.attempts);
            return;
        }
        case JobState::Queued:
            // Announced explicitly by the createJob handler: JobManager has no
            // "transitioned into Queued" callback, since a Job starts Queued at
            // construction.
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

// Mirrors _MAX_PLAYLIST_ENTRIES in python/downloader/downloader.py -- the enumeration cap
// there is what actually bounds a fan-out; this bounds the numbering params a caller may
// send, so a hand-rolled createJob call cannot ask for a 2-billion-wide zero-padding.
constexpr std::int64_t kMaxPlaylistEntries = 500;

// The two scheduling parameters every job type accepts, applied identically for all of
// them (jobs::SchedulerCore is what interprets them). Both are optional: absent means
// priority 0 and no dependencies, which is the plain FIFO behavior. Invalid dependency ids
// are rejected later, by JobManager::SubmitJob -- this only validates the shape.
void ApplySchedulingParams(jobs::Job& job, const json& jobParams) {
    job.SetPriority(static_cast<int>(
        OptionalInt(jobParams, "priority", 0, kMinJobPriority, kMaxJobPriority)));
    job.SetDependsOn(ipc::OptionalStringArray(jobParams, "dependsOn", kMaxJobDependencies));
    // Bounded by the playlist cap rather than kMaxJobDependencies: a playlist chain is the
    // reason this exists, and each link names exactly one predecessor, so the list is
    // short -- but the bound has to admit the longest chain a playlist can legitimately
    // build if a caller ever declares one in a single call.
    job.SetRunAfter(ipc::OptionalStringArray(jobParams, "runAfter",
                                              static_cast<std::size_t>(kMaxPlaylistEntries)));
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
    EnsureDownloaderAvailable(app);

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

// Enumerates a playlist so the frontend can fan it out into one createJob call per entry
// (docs/decisions.md, "Playlist URLs"). Deliberately does NOT create jobs itself: which
// entries to take, what to name the folder, and which quality to apply are all choices the
// user makes between this call and the createJob calls that follow.
json HandleInspectPlaylistUrl(AppContext& app, const json& params) {
    const std::string url = RequireNonEmptyString(params, "url");
    ValidateDownloadUrl(app, url);
    EnsureDownloaderAvailable(app);

    const auto deadline = std::chrono::steady_clock::now() + kInspectDeadline;
    auto isCancelled = [deadline] { return std::chrono::steady_clock::now() >= deadline; };

    try {
        const downloads::PlaylistInfo info = app.ytDlpProvider.InspectPlaylist(url, isCancelled);
        return {{"playlist", info.ToJson()}};
    } catch (const errors::MediaToolException& e) {
        if (e.Info().code == "E_INSPECT_PLAYLIST_CANCELLED") {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_INSPECT_PLAYLIST_TIMEOUT", errors::ErrorCategory::NetworkError,
                "Reading this playlist took too long and was cancelled.", "url=" + url,
                /*recoverable=*/true));
        }
        throw;
    }
}

// How many "playlist #n" candidates to try before giving up. A user with 500 playlist
// folders in one directory is past the point where an auto-generated name helps, and this
// keeps a pathological directory from turning name suggestion into an unbounded stat loop.
constexpr int kMaxPlaylistFolderProbes = 500;

// Suggests the default name for a playlist's destination subfolder: "playlist #n" for the
// lowest n not already present in `outputDirectory`. Only a suggestion -- the user is
// expected to overwrite it with the real playlist name, and nothing reserves it, so two
// suggestions taken concurrently can collide. That is acceptable because the actual
// collision guard is downstream: each entry's DownloadJob still runs its filename through
// FilenameReservationRegistry.
json HandleSuggestPlaylistFolder(AppContext& app, const json& params) {
    const std::string outputDirectory = RequireNonEmptyString(params, "outputDirectory");

    for (int n = 1; n <= kMaxPlaylistFolderProbes; ++n) {
        const std::string candidate = "playlist #" + std::to_string(n);
        if (!app.fileSystem.Exists(filesystem::paths::Join(outputDirectory, candidate))) {
            return {{"name", candidate}};
        }
    }
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_PLAYLIST_FOLDER_UNAVAILABLE", errors::ErrorCategory::InvalidFile,
        "Could not find an unused playlist folder name. Name the folder yourself.",
        "tried \"playlist #1\"..\"playlist #" + std::to_string(kMaxPlaylistFolderProbes) +
            "\" in " + outputDirectory));
}

// The three builders below validate their params and hand back a constructed Job. They
// deliberately do NOT submit, persist or publish: SubmitJobOfType() does all three for
// every type, so the createJob path and the crash-recovery path cannot drift apart.
//
// `onArtifactLocation` is threaded through because it is the only thing a job knows that
// the recovery store cannot work out for itself -- see core/jobs/JobSpec.h.
using ArtifactHook = std::function<void(const std::string&, const std::string&)>;

std::unique_ptr<jobs::Job> BuildDownloadJob(AppContext& app, const json& jobParams,
                                             ArtifactHook onArtifactLocation) {
    const std::string url = RequireNonEmptyString(jobParams, "url");
    const std::string outputDirectory = RequireNonEmptyString(jobParams, "outputDirectory");
    ValidateDownloadUrl(app, url);
    // Fail here, with the candidate list, rather than letting the job start and die inside
    // a worker thread with CreateProcess's opaque message (issue #79).
    EnsureDownloaderAvailable(app);
    // The paths exist; whether the backend behind them actually works is a separate
    // question, and this is the first moment it matters. Probed once per process, and
    // never fatal -- a stale yt-dlp still works for many sites, so this warns rather than
    // refusing a download the user might well get.
    (void)DownloaderInfoCached(app);
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
        // This value reaches yt-dlp's -f verbatim, and -f is an expression language, not
        // a name (see downloader::IsSafeFormatSelector). Rejected here so the caller gets
        // the error synchronously from createJob instead of discovering it once a worker
        // thread has already started the job.
        if (!downloader::IsSafeFormatSelector(*value)) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_INVALID_FORMAT_ID", errors::ErrorCategory::UnsupportedFormat,
                "That stream selection is not a valid format id.",
                "field=formatId value=" + *value +
                    " (expected up to 8 '+'-joined ids of [A-Za-z0-9_.-])"));
        }
        formatId = *value;
    }

    jobs::DownloadJob::Options options;
    options.url = url;
    options.outputDirectory = outputDirectory;
    options.quality = quality;
    options.formatId = formatId;

    // Playlist numbering (issue #41). Both or neither: an index with no total has no digit
    // width to pad to, and a total with no index has nothing to place. Rejected rather than
    // silently ignored, because a caller sending one of them is asking for numbering it
    // would not get.
    const bool hasIndex = jobParams.contains("playlistIndex") && !jobParams.at("playlistIndex").is_null();
    const bool hasCount = jobParams.contains("playlistCount") && !jobParams.at("playlistCount").is_null();
    if (hasIndex != hasCount) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INVALID_PARAM_VALUE", errors::ErrorCategory::Unknown,
            "playlistIndex and playlistCount must be provided together.",
            std::string("field=") + (hasIndex ? "playlistCount" : "playlistIndex") + " is missing"));
    }
    if (hasIndex) {
        const auto index = OptionalInt(jobParams, "playlistIndex", 0, 1, kMaxPlaylistEntries);
        const auto count = OptionalInt(jobParams, "playlistCount", 0, 1, kMaxPlaylistEntries);
        if (index > count) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_INVALID_PARAM_VALUE", errors::ErrorCategory::Unknown,
                "playlistIndex must not exceed playlistCount.",
                "playlistIndex=" + std::to_string(index) + " playlistCount=" + std::to_string(count)));
        }
        options.playlistIndex = static_cast<int>(index);
        options.playlistCount = static_cast<int>(count);
    }

    options.onArtifactLocation = std::move(onArtifactLocation);

    return std::make_unique<jobs::DownloadJob>(options, app.ytDlpProvider, app.fileSystem,
                                                &app.ffmpegEngine, app.reservationRegistry);
}

// Shared by HandleCreateConversionJob/HandleCreateCompressionJob -- they differ only in
// `isCompression` (which JobType the job runs as and which IMediaEngine method it calls);
// per engines/ffmpeg/FFmpegArgBuilder.h, Compress is Convert with different default
// option VALUES (supplied by the caller, i.e. the frontend's preset), not a different
// code path here either.
std::unique_ptr<jobs::Job> BuildMediaProcessingJob(AppContext& app, const json& jobParams,
                                                    bool isCompression,
                                                    ArtifactHook onArtifactLocation) {
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

    // The same coarse floor DOWNLOAD already had. A conversion writes a file too, and
    // running out of space mid-encode wastes however long the encode had been running --
    // catching "the drive is already essentially full" here costs one stat call.
    constexpr std::uint64_t kMinFreeBytesForProcessing = 100ull * 1024 * 1024;
    if (auto available = app.fileSystem.GetAvailableDiskSpace(outputDirectory);
        available && *available < kMinFreeBytesForProcessing) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_INSUFFICIENT_DISK_SPACE", errors::ErrorCategory::DiskSpaceError,
            "Not enough free disk space at the selected output directory.",
            "available=" + std::to_string(*available) + " bytes"));
    }

    const json& processingOptions = OptionalObject(jobParams, "options");
    const std::string outputFormat = RequireNonEmptyString(processingOptions, "outputFormat");
    // `quality` used to reach the arg builder unvalidated, where an unrecognized value
    // silently degraded to "medium" -- a typo in a preset produced a job that ran with a
    // quality the caller never asked for. Issue #82 also removed the Pro tier that used to
    // reject "lossless" here outright, so the whole tier list is now simply the allowed set.
    if (processingOptions.contains("quality") && !processingOptions.at("quality").is_null()) {
        (void)ipc::RequireEnum(processingOptions, "quality",
                                {"lowest", "low", "medium", "high", "ultra", "lossless"});
    }

    jobs::MediaProcessingJob::Options options;
    options.inputPath = inputPath;
    options.outputDirectory = outputDirectory;
    options.outputFormat = outputFormat;
    options.engineOptions = processingOptions;
    options.isCompression = isCompression;
    options.onArtifactLocation = std::move(onArtifactLocation);

    return std::make_unique<jobs::MediaProcessingJob>(std::move(options), app.ffmpegEngine,
                                                      app.fileSystem, app.reservationRegistry);
}

// Builds, persists and submits one job, whatever its type. The single place that does
// so: createJob calls it with a fresh request, and the startup recovery pass calls it
// with a stored one, so a recovered job is constructed and validated exactly like a new
// one rather than through a second path that can drift.
//
// `recoveryCount` is carried through from a recovered spec (0 for a fresh request) so a
// job that takes the process down on every attempt eventually stops being recovered --
// see jobs::kMaxRecoveryAttempts. `attempts` likewise carries the retry budget already
// spent, so a relaunch does not hand a permanently-broken job a fresh three attempts.
jobs::JobId SubmitJobOfType(AppContext& app, const std::string& typeWire, const json& jobParams,
                             int recoveryCount = 0, int attempts = 0) {
    // The hook fires from a worker thread once the job has reserved its output filename,
    // which is necessarily after SubmitJob() below -- so capturing an id that is filled in
    // a few lines further down is safe, and is the only way to hand a job its own id
    // through a constructor argument.
    auto idHolder = std::make_shared<jobs::JobId>();
    ArtifactHook onArtifactLocation = [&app, idHolder](const std::string& directory,
                                                        const std::string& filenameBase) {
        app.inProgressJobStore.SetArtifactLocation(*idHolder, {directory, filenameBase});
    };

    std::unique_ptr<jobs::Job> job;
    if (typeWire == "DOWNLOAD") {
        job = BuildDownloadJob(app, jobParams, std::move(onArtifactLocation));
    } else if (typeWire == "CONVERSION") {
        job = BuildMediaProcessingJob(app, jobParams, /*isCompression=*/false,
                                       std::move(onArtifactLocation));
    } else if (typeWire == "COMPRESSION") {
        job = BuildMediaProcessingJob(app, jobParams, /*isCompression=*/true,
                                       std::move(onArtifactLocation));
    } else if (typeWire == "TEST") {
        job = std::make_unique<jobs::TestJob>();
    } else {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_JOB_TYPE_NOT_IMPLEMENTED", errors::ErrorCategory::UnsupportedFormat,
            "Only TEST, DOWNLOAD, CONVERSION and COMPRESSION jobs are implemented so far -- " +
                typeWire + " is scaffolded (see docs/roadmap.md) but not runnable yet.",
            "", false));
    }

    ApplySchedulingParams(*job, jobParams);
    job->SetAttemptCount(attempts);
    const jobs::JobId id = job->Id();
    *idHolder = id;

    jobs::JobSpec spec;
    spec.id = id;
    spec.type = job->Type();
    spec.params = jobParams;
    spec.createdAt = job->CreatedAt();
    spec.recoveryCount = recoveryCount;
    spec.attempts = attempts;
    // Persisted BEFORE the submission, not after: a crash in between then re-queues a job
    // that never ran, which is the harmless direction. The other order can lose a job
    // that is already running.
    app.inProgressJobStore.Put(spec);

    try {
        app.jobManager.SubmitJob(std::move(job));
    } catch (...) {
        // The scheduler refused it (an unknown or already-failed dependency), so there is
        // no job to recover -- the record would otherwise be resurrected on every launch.
        app.inProgressJobStore.Remove(id);
        throw;
    }
    return id;
}

json HandleCreateJob(AppContext& app, const json& params) {
    const std::string typeWire = RequireNonEmptyString(params, "type");
    // Absent/empty job params are legitimate for TEST; the per-type builders require
    // whatever they actually need out of this object.
    const json& jobParams = OptionalObject(params, "params");

    const jobs::JobId id = SubmitJobOfType(app, typeWire, jobParams);
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
    // `capabilities` is what will actually be attempted; `deferredCapabilities` is what
    // applies to this file but cannot run yet, each with a reason the UI can show on a
    // disabled control. Attempting one is guaranteed to fail with E_NOT_IMPLEMENTED --
    // see core/media/DeferredOperations.h.
    json deferred = json::array();
    for (const filesystem::DeferredCapability& capability :
         filesystem::DeferredCapabilitiesFor(info.category, info.extension)) {
        deferred.push_back(capability.ToJson());
    }
    return {{"capabilities", filesystem::CapabilitiesFor(info.category, info.extension)},
            {"deferredCapabilities", std::move(deferred)}};
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

json HandleGetDownloaderInfo(AppContext& app, const json&) {
    return {{"downloaderInfo", DownloaderInfoCached(app).ToJson()}};
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
        {"inspectPlaylistUrl", HandleInspectPlaylistUrl},
        {"suggestPlaylistFolder", HandleSuggestPlaylistFolder},
        {"getCapabilities", HandleGetCapabilities},
        {"getSettings", HandleGetSettings},
        {"updateSettings", HandleUpdateSettings},
        {"getHardwareInfo", HandleGetHardwareInfo},
        {"getDownloaderInfo", HandleGetDownloaderInfo},
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
    return command == "inspectDownloadUrl" || command == "inspectPlaylistUrl" ||
           command == "inspectFile";
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

// Rebuilds the jobs an earlier run did not finish.
//
// What this restores is INTENT, not progress. A job that was RUNNING when the process
// died is re-queued from the start: its ffmpeg or yt-dlp child died with it, its partial
// output is not a checkpoint, and neither subprocess has a resume protocol this app could
// drive. What the user actually loses today is the queue itself -- twenty things lined up
// and nothing to say what they were -- and that is what comes back.
//
// Order matters in three places:
//   * Specs are replayed in submission order, so a `dependsOn` edge still points at a job
//     that was submitted before it.
//   * Each job's leftovers are deleted BEFORE it is resubmitted, so the re-run allocates a
//     clean filename instead of tripping over a half-written file from the run that died.
//   * The store is cleared first and each replayed job re-adds itself through the normal
//     submission path, so a spec that can no longer be built (its input file is gone, say)
//     does not sit in the file being retried on every launch forever.
// The gap that leaves is a crash DURING recovery, which loses the specs not yet replayed.
// Narrowing it would mean a write per spec; a crash inside the recovery pass is not the
// failure this exists to survive.
void RecoverInProgressJobs(AppContext& app) {
    const std::vector<jobs::JobSpec> specs = app.inProgressJobStore.Load();
    if (specs.empty()) {
        return;
    }
    app.inProgressJobStore.Clear();

    logging::Log::Info("recovery", "Rebuilding " + std::to_string(specs.size()) +
                                        " job(s) left unfinished by a previous run.");

    // Ids are generated per Job object, so a rebuilt job is a new id. Dependencies are
    // remapped through this as we go; an id that is NOT in here belonged to a job that
    // already reached a terminal state (its record was dropped then), so the edge is
    // either already satisfied or points at something that can never report an outcome --
    // in both cases dropping it is what lets the dependent run at all.
    std::unordered_map<jobs::JobId, jobs::JobId> rebuiltIds;

    for (const jobs::JobSpec& spec : specs) {
        if (spec.recoveryCount >= jobs::kMaxRecoveryAttempts) {
            logging::Log::Warning("recovery",
                                   "Giving up on job " + spec.id + " after " +
                                       std::to_string(spec.recoveryCount) +
                                       " recovery attempts; it is not being re-queued.");
            continue;
        }

        if (spec.artifact) {
            // The orphan case: files a killed run wrote and no live job owns. Scoped by
            // filenameBase through the same IsJobArtifactOf match every failure path uses,
            // so it is never a bare prefix delete and never recursive.
            jobs::CleanupJobArtifacts(app.fileSystem, spec.artifact->outputDirectory,
                                       spec.artifact->filenameBase);
        }

        json jobParams = spec.params;
        // Both edge kinds are remapped identically -- a playlist's sequencing chain has to
        // survive a restart the same way a workflow's dependency chain does, or a recovered
        // playlist would resume downloading every remaining entry at once.
        for (const char* edgeKey : {"dependsOn", "runAfter"}) {
            if (!jobParams.contains(edgeKey) || !jobParams.at(edgeKey).is_array()) continue;
            json remapped = json::array();
            for (const auto& dependency : jobParams.at(edgeKey)) {
                if (!dependency.is_string()) continue;
                auto rebuilt = rebuiltIds.find(dependency.get<std::string>());
                if (rebuilt != rebuiltIds.end()) remapped.push_back(rebuilt->second);
            }
            jobParams[edgeKey] = std::move(remapped);
        }

        try {
            const jobs::JobId id =
                SubmitJobOfType(app, jobs::ToWireString(spec.type), jobParams,
                                 spec.recoveryCount + 1, spec.attempts);
            rebuiltIds.emplace(spec.id, id);
            app.eventBus.Publish(
                events::MakeEvent(events::EventType::JobCreated, {{"state", "QUEUED"}}, id));
        } catch (const errors::MediaToolException& e) {
            // A job whose input has since been deleted, whose output directory is gone, or
            // whose params a newer build rejects. Dropped with a reason rather than
            // retried forever.
            logging::Log::Warning("recovery", "Could not rebuild job " + spec.id + ": " +
                                                   e.Info().code + " " + e.Info().message);
        }
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

    // After the callbacks and the event sink are wired, so a recovered job publishes the
    // same jobCreated/jobStarted/jobProgress stream a fresh one does -- and before the
    // read loop, so the frontend's first listJobs already includes them.
    RecoverInProgressJobs(app);

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
            app.pythonTool.path, {app.downloaderScriptTool.path, "--selftest"}, {},
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
