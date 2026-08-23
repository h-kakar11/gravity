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
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "core/downloads/IDownloadProvider.h"
#include "core/downloads/NdjsonLineProtocol.h"
#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/events/Event.h"
#include "core/events/EventBus.h"
#include "core/filesystem/FileInfo.h"
#include "core/filesystem/LocalFileSystem.h"
#include "core/hardware/HardwareInfo.h"
#include "core/hardware/WindowsHardwareDetector.h"
#include "core/jobs/JobManager.h"
#include "core/jobs/JobTypes.h"
#include "core/jobs/Progress.h"
#include "core/jobs/TestJob.h"
#include "core/logging/Logger.h"
#include "core/process/IProcessRunner.h"
#include "core/process/RealProcessRunner.h"
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

void WriteLine(const json& payload) {
    std::lock_guard<std::mutex> lock(g_stdoutMutex);
    std::cout << payload.dump() << std::endl;
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

// --- the wired-up application -----------------------------------------------------------

struct AppContext {
    process::RealProcessRunner processRunner;
    events::EventBus eventBus;
    settings::JsonFileSettingsStore settingsStore{settings::DefaultSettingsFilePath()};
    hardware::WindowsHardwareDetector hardwareDetector;
    filesystem::LocalFileSystem fileSystem;
    media::FFmpegEngine ffmpegEngine;
    downloader::YtDlpProvider ytDlpProvider;
    jobs::JobManager jobManager;

    // Tracks each job's previous state purely to classify the Running state as either
    // "resumed from pause" or "(re)started" when JobManager reports a transition -- see
    // the comment on PublishJobStateChanged below.
    std::mutex previousStateMutex;
    std::unordered_map<jobs::JobId, jobs::JobState> previousState;

    explicit AppContext(const settings::Settings& settings)
        : ffmpegEngine(processRunner,
                       settings.advanced.ffmpegPath.empty()
                           ? std::nullopt
                           : std::optional<std::string>(settings.advanced.ffmpegPath),
                       std::nullopt),
          ytDlpProvider(processRunner, ResolvePythonExecutable(), ResolveDownloaderScript()),
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
            return;
        }
        case JobState::Failed: {
            auto snapshot = app.jobManager.GetJob(id);
            json data{{"state", "FAILED"}};
            if (snapshot.error) data["error"] = snapshot.error->ToJson();
            app.eventBus.Publish(events::MakeEvent(events::EventType::JobFailed, data, id));
            return;
        }
        case JobState::Cancelled:
            app.eventBus.Publish(
                events::MakeEvent(events::EventType::JobCancelled, {{"state", "CANCELLED"}}, id));
            return;
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

json HandleCreateJob(AppContext& app, const json& params) {
    const std::string typeWire = params.at("type").get<std::string>();
    if (typeWire != "TEST") {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_JOB_TYPE_NOT_IMPLEMENTED", errors::ErrorCategory::UnsupportedFormat,
            "Only TEST jobs are implemented in Phase 1 -- " + typeWire +
                " is scaffolded (see docs/roadmap.md) but not runnable yet.",
            "", false));
    }

    auto job = std::make_unique<jobs::TestJob>();
    const jobs::JobId id = job->Id();
    job->SetCallbacks(
        [&app, id](jobs::JobState state) { PublishJobStateChanged(app, id, state); },
        [&app, id](const jobs::Progress& progress) { PublishJobProgress(app, id, progress); });

    app.jobManager.SubmitJob(std::move(job));
    app.eventBus.Publish(events::MakeEvent(events::EventType::JobCreated, {{"state", "QUEUED"}}, id));
    return {{"jobId", id}};
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
    app.jobManager.CancelJob(params.at("jobId").get<std::string>());
    return json::object();
}

json HandlePauseJob(AppContext& app, const json& params) {
    app.jobManager.PauseJob(params.at("jobId").get<std::string>());
    return json::object();
}

json HandleResumeJob(AppContext& app, const json& params) {
    app.jobManager.ResumeJob(params.at("jobId").get<std::string>());
    return json::object();
}

json HandleRetryJob(AppContext& app, const json& params) {
    app.jobManager.RetryJob(params.at("jobId").get<std::string>());
    return json::object();
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
        {"inspectFile", HandleInspectFile},
        {"getCapabilities", HandleGetCapabilities},
        {"getSettings", HandleGetSettings},
        {"updateSettings", HandleUpdateSettings},
        {"getHardwareInfo", HandleGetHardwareInfo},
    };
    return table;
}

// --- the IPC loop --------------------------------------------------------------------

void RunIpcLoop(AppContext& app) {
    app.jobManager.OnJobStateChanged(
        [&app](const jobs::JobId& id, jobs::JobState state) { PublishJobStateChanged(app, id, state); });
    app.jobManager.OnJobProgress(
        [&app](const jobs::JobId& id, const jobs::Progress& progress) { PublishJobProgress(app, id, progress); });
    app.eventBus.Subscribe([](const events::Event& event) { WriteLine(event.ToJson()); });
    logging::Logger::SetEventSink([&app](events::Event event) { app.eventBus.Publish(event); });

    logging::Log::Info("mediatool-core", "IPC loop starting");

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
