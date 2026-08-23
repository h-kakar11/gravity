#include "engines/downloader/YtDlpProvider.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/downloads/IDownloadProvider.h"
#include "core/errors/MediaToolException.h"
#include "core/process/IProcessRunner.h"

namespace {

using mediatool::process::IProcess;
using mediatool::process::IProcessRunner;
using mediatool::process::OutputLineCallback;
using mediatool::process::ProcessOptions;
using mediatool::process::ProcessResult;

// YtDlpProvider::Download() takes ownership of the IProcess Start() returns and destroys
// it before Download() itself returns -- so a test that wants to inspect what happened to
// the process (writtenLines, stdinClosed, ...) *after* Download() returns cannot do so
// through the IProcess object itself, only through state that outlives it. FakeProcessState
// is that state: it's shared (via shared_ptr) between the FakeProcess handle Download()
// owns and destroys, and FakeProcessRunner, which outlives the Download() call and is what
// the test actually asserts against.
struct FakeProcessState {
    std::vector<std::string> stdoutLines;
    int exitCode = 0;
    bool running = false;
    bool finished = false;
    std::vector<std::string> writtenLines;
    bool stdinClosed = false;
    bool terminated = false;
};

class FakeProcess : public IProcess {
public:
    explicit FakeProcess(std::shared_ptr<FakeProcessState> state) : state_(std::move(state)) {}

    void ReplayStdoutAndMaybeFinish(const OutputLineCallback& onStdout) {
        for (const auto& line : state_->stdoutLines) {
            if (onStdout) onStdout(line);
        }
        if (!state_->running) {
            state_->finished = true;
        }
    }

    void WriteLine(const std::string& line) override { state_->writtenLines.push_back(line); }
    void CloseStdin() override { state_->stdinClosed = true; }

    ProcessResult Wait() override {
        state_->running = false;
        state_->finished = true;
        return {state_->exitCode, state_->terminated};
    }

    std::optional<ProcessResult> WaitFor(int /*timeoutMs*/) override {
        if (state_->finished) {
            return ProcessResult{state_->exitCode, state_->terminated};
        }
        return std::nullopt;
    }

    void Terminate() override {
        state_->terminated = true;
        state_->running = false;
        state_->finished = true;
    }

    void Kill() override {
        state_->terminated = true;
        state_->running = false;
        state_->finished = true;
    }

    bool IsRunning() const override { return state_->running; }

private:
    std::shared_ptr<FakeProcessState> state_;
};

class FakeProcessRunner : public IProcessRunner {
public:
    explicit FakeProcessRunner(std::vector<std::string> stdoutLines, int exitCode = 0,
                                bool staysRunning = false)
        : stdoutLines_(std::move(stdoutLines)), exitCode_(exitCode), staysRunning_(staysRunning) {}

    std::unique_ptr<IProcess> Start(const std::string& executable, const std::vector<std::string>& args,
                                     const ProcessOptions& /*options*/, OutputLineCallback onStdout,
                                     OutputLineCallback /*onStderr*/) override {
        lastExecutable = executable;
        lastArgs = args;

        auto state = std::make_shared<FakeProcessState>();
        state->stdoutLines = stdoutLines_;
        state->exitCode = exitCode_;
        state->running = staysRunning_;
        lastProcessState = state;

        auto process = std::make_unique<FakeProcess>(state);
        process->ReplayStdoutAndMaybeFinish(onStdout);
        return process;
    }

    std::string lastExecutable;
    std::vector<std::string> lastArgs;
    std::shared_ptr<FakeProcessState> lastProcessState;

private:
    std::vector<std::string> stdoutLines_;
    int exitCode_;
    bool staysRunning_;
};

mediatool::downloads::DownloadOptions MakeOptions() {
    mediatool::downloads::DownloadOptions options;
    options.url = "https://example.com/watch?v=abc123";
    options.outputDirectory = "C:\\out";
    options.quality = "best";
    return options;
}

}  // namespace

TEST(YtDlpProvider, CanHandleHttpAndHttpsUrlsOnly) {
    FakeProcessRunner runner({});
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    EXPECT_TRUE(provider.CanHandle("https://www.youtube.com/watch?v=abc"));
    EXPECT_TRUE(provider.CanHandle("http://example.com/video.mp4"));
    EXPECT_TRUE(provider.CanHandle("HTTPS://Example.com/x"));
    EXPECT_FALSE(provider.CanHandle("ftp://example.com/video"));
    EXPECT_FALSE(provider.CanHandle("not a url"));
    EXPECT_FALSE(provider.CanHandle(""));
}

TEST(YtDlpProvider, StartsPythonWithCommandStdinFlagAndWritesCommandJson) {
    FakeProcessRunner runner({
        R"({"event":"completed","data":{"outputPath":"C:\\out\\v.mp4"}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "C:\\py\\python.exe", "C:\\src\\downloader.py");

    provider.Download(
        MakeOptions(), [](const auto&) {}, [](const auto&) {}, [](const std::string&) {},
        [] { return false; });

    EXPECT_EQ(runner.lastExecutable, "C:\\py\\python.exe");
    ASSERT_EQ(runner.lastArgs.size(), 2u);
    EXPECT_EQ(runner.lastArgs[0], "C:\\src\\downloader.py");
    EXPECT_EQ(runner.lastArgs[1], "--command-stdin");

    ASSERT_NE(runner.lastProcessState, nullptr);
    ASSERT_EQ(runner.lastProcessState->writtenLines.size(), 1u);
    EXPECT_TRUE(runner.lastProcessState->stdinClosed);

    const auto sentCommand = nlohmann::json::parse(runner.lastProcessState->writtenLines[0]);
    EXPECT_EQ(sentCommand.at("command"), "download");
    EXPECT_EQ(sentCommand.at("params").at("url"), "https://example.com/watch?v=abc123");
    EXPECT_EQ(sentCommand.at("params").at("outputDir"), "C:\\out");
    EXPECT_EQ(sentCommand.at("params").at("quality"), "best");
}

TEST(YtDlpProvider, RoutesEventsToCallbacksAndReturnsCompletedPath) {
    FakeProcessRunner runner({
        R"({"event":"metadata","data":{"title":"Test Video","duration":42,"playlistIndex":null,"playlistCount":null}})",
        R"({"event":"progress","data":{"downloadedBytes":1000,"totalBytes":2000,"speedBytesPerSecond":500,"etaSeconds":2,"statusMessage":"Downloading"}})",
        R"({"event":"progress","data":{"downloadedBytes":2000,"totalBytes":2000,"speedBytesPerSecond":500,"etaSeconds":0,"statusMessage":"Downloading"}})",
        R"({"event":"completed","data":{"outputPath":"C:\\out\\test.mp4"}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    std::optional<mediatool::downloads::DownloadMetadata> metadata;
    std::vector<mediatool::jobs::Progress> progressUpdates;
    std::string completedPath;

    provider.Download(
        MakeOptions(),
        [&](const mediatool::downloads::DownloadMetadata& m) { metadata = m; },
        [&](const mediatool::jobs::Progress& p) { progressUpdates.push_back(p); },
        [&](const std::string& path) { completedPath = path; },
        [] { return false; });

    ASSERT_TRUE(metadata.has_value());
    EXPECT_EQ(metadata->title, "Test Video");
    ASSERT_TRUE(metadata->durationSeconds.has_value());
    EXPECT_DOUBLE_EQ(*metadata->durationSeconds, 42.0);
    EXPECT_FALSE(metadata->playlistIndex.has_value());

    ASSERT_EQ(progressUpdates.size(), 2u);
    ASSERT_TRUE(progressUpdates[0].processedBytes.has_value());
    EXPECT_EQ(*progressUpdates[0].processedBytes, 1000u);
    ASSERT_TRUE(progressUpdates[1].processedBytes.has_value());
    EXPECT_EQ(*progressUpdates[1].processedBytes, 2000u);
    ASSERT_TRUE(progressUpdates[1].percentage.has_value());
    EXPECT_DOUBLE_EQ(*progressUpdates[1].percentage, 100.0);

    EXPECT_EQ(completedPath, "C:\\out\\test.mp4");
}

TEST(YtDlpProvider, ThrowsMediaToolExceptionOnErrorEvent) {
    FakeProcessRunner runner({
        R"({"event":"error","data":{"code":"E_NETWORK","category":"NETWORK_ERROR","message":"boom","details":"stack","recoverable":true}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    bool threw = false;
    try {
        provider.Download(
            MakeOptions(), [](const auto&) {}, [](const auto&) {}, [](const std::string&) {},
            [] { return false; });
    } catch (const mediatool::errors::MediaToolException& ex) {
        threw = true;
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::NetworkError);
        EXPECT_EQ(ex.Info().message, "boom");
    }
    EXPECT_TRUE(threw);
}

TEST(YtDlpProvider, ThrowsEngineFailureWhenProcessExitsWithoutCompletedOrError) {
    FakeProcessRunner runner({}, /*exitCode=*/1, /*staysRunning=*/false);
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    bool threw = false;
    try {
        provider.Download(
            MakeOptions(), [](const auto&) {}, [](const auto&) {}, [](const std::string&) {},
            [] { return false; });
    } catch (const mediatool::errors::MediaToolException& ex) {
        threw = true;
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::EngineFailure);
    }
    EXPECT_TRUE(threw);
}

TEST(YtDlpProvider, CancellationTerminatesProcessAndThrowsCancelled) {
    FakeProcessRunner runner({}, /*exitCode=*/0, /*staysRunning=*/true);
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    bool threw = false;
    try {
        provider.Download(
            MakeOptions(), [](const auto&) {}, [](const auto&) {}, [](const std::string&) {},
            [] { return true; });
    } catch (const mediatool::errors::MediaToolException& ex) {
        threw = true;
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::Cancelled);
    }
    EXPECT_TRUE(threw);
    ASSERT_NE(runner.lastProcessState, nullptr);
    EXPECT_TRUE(runner.lastProcessState->terminated);
}
