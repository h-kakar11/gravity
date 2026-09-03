#include "engines/downloader/YtDlpProvider.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/downloads/IDownloadProvider.h"
#include "core/downloads/QualityPreset.h"
#include "core/errors/MediaToolException.h"
#include "core/process/IProcessRunner.h"
#include "engines/downloader/YtDlpFormatSelector.h"

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
    options.quality = mediatool::downloads::QualityPreset::Best;
    options.filenameBase = "Test Video";
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
    EXPECT_EQ(sentCommand.at("params").at("formatSelector"),
              mediatool::downloader::FormatSelectorForQuality(mediatool::downloads::QualityPreset::Best));
    EXPECT_EQ(sentCommand.at("params").at("filenameBase"), "Test Video");
    EXPECT_FALSE(sentCommand.at("params").contains("ffmpegLocation"))
        << "no ffmpeg location was injected into this provider -- the key should be omitted entirely";
}

TEST(YtDlpProvider, UsesExplicitFormatIdInsteadOfQualityPresetWhenSet) {
    FakeProcessRunner runner({
        R"({"event":"completed","data":{"outputPath":"C:\\out\\v.mp4"}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "C:\\py\\python.exe", "C:\\src\\downloader.py");

    // Issue #31: a formatId from Inspect()'s format list must override the quality
    // preset entirely, reaching yt-dlp verbatim as the -f selector string.
    auto options = MakeOptions();
    options.formatId = "137+140";

    provider.Download(
        options, [](const auto&) {}, [](const auto&) {}, [](const std::string&) {}, [] { return false; });

    ASSERT_NE(runner.lastProcessState, nullptr);
    ASSERT_EQ(runner.lastProcessState->writtenLines.size(), 1u);
    const auto sentCommand = nlohmann::json::parse(runner.lastProcessState->writtenLines[0]);
    EXPECT_EQ(sentCommand.at("params").at("formatSelector"), "137+140");
}

TEST(YtDlpProvider, ForwardsFfmpegLocationWhenConfigured) {
    FakeProcessRunner runner({
        R"({"event":"completed","data":{"outputPath":"C:\\out\\v.mp4"}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py", "C:\\tools\\ffmpeg.exe");

    provider.Download(
        MakeOptions(), [](const auto&) {}, [](const auto&) {}, [](const std::string&) {},
        [] { return false; });

    const auto sentCommand = nlohmann::json::parse(runner.lastProcessState->writtenLines[0]);
    EXPECT_EQ(sentCommand.at("params").at("ffmpegLocation"), "C:\\tools\\ffmpeg.exe");
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

TEST(YtDlpProvider, InspectSendsInspectCommandAndReturnsRichMetadata) {
    FakeProcessRunner runner({
        R"({"event":"metadata","data":{)"
        R"("title":"Rich Video","uploader":"Some Channel","duration":99.5,)"
        R"("webpageUrl":"https://example.com/watch?v=abc123","thumbnailUrl":"https://example.com/thumb.jpg",)"
        R"("extractor":"generic","playlistIndex":null,"playlistCount":null,)"
        R"("formats":[)"
        R"({"formatId":"137","extension":"mp4","resolution":"1920x1080","width":1920,"height":1080,)"
        R"("fps":30,"videoCodec":"avc1","audioCodec":null,"videoBitrateKbps":2500,"audioBitrateKbps":null,)"
        R"("filesizeBytes":123456,"approxFilesizeBytes":null,"hasVideo":true,"hasAudio":false},)"
        R"({"formatId":"140","extension":"m4a","resolution":null,"width":null,"height":null,"fps":null,)"
        R"("videoCodec":null,"audioCodec":"mp4a","videoBitrateKbps":null,"audioBitrateKbps":128,)"
        R"("filesizeBytes":45678,"approxFilesizeBytes":null,"hasVideo":false,"hasAudio":true})"
        R"(]}})",
        R"({"event":"completed","data":{}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    const auto metadata = provider.Inspect("https://example.com/watch?v=abc123", [] { return false; });

    EXPECT_EQ(runner.lastArgs[0], "downloader.py");
    EXPECT_EQ(runner.lastArgs[1], "--command-stdin");
    const auto sentCommand = nlohmann::json::parse(runner.lastProcessState->writtenLines[0]);
    EXPECT_EQ(sentCommand.at("command"), "inspect");
    EXPECT_EQ(sentCommand.at("params").at("url"), "https://example.com/watch?v=abc123");

    EXPECT_EQ(metadata.title, "Rich Video");
    ASSERT_TRUE(metadata.uploader.has_value());
    EXPECT_EQ(*metadata.uploader, "Some Channel");
    ASSERT_TRUE(metadata.durationSeconds.has_value());
    EXPECT_DOUBLE_EQ(*metadata.durationSeconds, 99.5);
    ASSERT_TRUE(metadata.webpageUrl.has_value());
    ASSERT_TRUE(metadata.thumbnailUrl.has_value());
    ASSERT_TRUE(metadata.extractor.has_value());
    EXPECT_EQ(*metadata.extractor, "generic");

    ASSERT_EQ(metadata.formats.size(), 2u);
    EXPECT_EQ(metadata.formats[0].formatId, "137");
    EXPECT_TRUE(metadata.formats[0].hasVideo);
    EXPECT_FALSE(metadata.formats[0].hasAudio);
    ASSERT_TRUE(metadata.formats[0].resolution.has_value());
    EXPECT_EQ(*metadata.formats[0].resolution, "1920x1080");
    EXPECT_EQ(metadata.formats[1].formatId, "140");
    EXPECT_FALSE(metadata.formats[1].hasVideo);
    EXPECT_TRUE(metadata.formats[1].hasAudio);
}

TEST(YtDlpProvider, InspectPlaylistSendsPlaylistCommandAndReturnsEntries) {
    // The entry duration key here is "duration", which is what downloader.py's
    // build_playlist_payload actually emits (docs/protocols/downloader.md's `playlist`
    // event). This fixture previously used the C++-side spelling "durationSeconds", so it
    // agreed with the parser's bug instead of with the producer: the test passed while
    // every real playlist listing showed no durations at all. Keep this matching
    // docs/protocols/downloader.md, not IDownloadProvider.h.
    FakeProcessRunner runner({
        R"({"event":"playlist","data":{)"
        R"("title":"My Playlist","uploader":"Some Channel",)"
        R"("webpageUrl":"https://example.com/playlist?list=abc","truncated":false,"count":2,)"
        R"("unavailableCount":1,)"
        R"("entries":[)"
        R"({"index":1,"url":"https://example.com/watch?v=a","title":"First","duration":10.5},)"
        R"({"index":2,"url":"https://example.com/watch?v=b","title":"Second","duration":null})"
        R"(]}})",
        R"({"event":"completed","data":{}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    const auto info =
        provider.InspectPlaylist("https://example.com/playlist?list=abc", [] { return false; });

    const auto sentCommand = nlohmann::json::parse(runner.lastProcessState->writtenLines[0]);
    EXPECT_EQ(sentCommand.at("command"), "inspectPlaylist");
    EXPECT_EQ(sentCommand.at("params").at("url"), "https://example.com/playlist?list=abc");

    EXPECT_EQ(info.title, "My Playlist");
    EXPECT_FALSE(info.truncated);
    EXPECT_EQ(info.unavailableCount, 1);
    ASSERT_EQ(info.entries.size(), 2u);
    EXPECT_EQ(info.entries[0].index, 1);
    EXPECT_EQ(info.entries[0].url, "https://example.com/watch?v=a");
    EXPECT_EQ(info.entries[0].title, "First");
    ASSERT_TRUE(info.entries[0].durationSeconds.has_value());
    EXPECT_DOUBLE_EQ(*info.entries[0].durationSeconds, 10.5);
    EXPECT_EQ(info.entries[1].index, 2);
    EXPECT_FALSE(info.entries[1].durationSeconds.has_value());
}

TEST(YtDlpProvider, InspectPlaylistDropsEntriesWithNoUrl) {
    // downloader.py already filters these; this is the parser refusing to build an
    // undownloadable job out of a malformed payload rather than trusting its input.
    FakeProcessRunner runner({
        R"({"event":"playlist","data":{"title":"P","truncated":false,"count":2,"entries":[)"
        R"({"index":1,"url":"","title":"No URL"},)"
        R"({"index":2,"url":"https://example.com/watch?v=b","title":"Fine"})"
        R"(]}})",
        R"({"event":"completed","data":{}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    const auto info = provider.InspectPlaylist("https://example.com/playlist?list=x", [] { return false; });
    ASSERT_EQ(info.entries.size(), 1u);
    EXPECT_EQ(info.entries[0].title, "Fine");
    // No "unavailableCount" key in this fixture's payload -- defaults to 0 rather than
    // failing to parse, since older/mismatched payload shapes should not break inspection.
    EXPECT_EQ(info.unavailableCount, 0);
}

TEST(YtDlpProvider, InspectPlaylistThrowsEngineFailureWhenNoPlaylistEventArrives) {
    FakeProcessRunner runner({R"({"event":"completed","data":{}})"});
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    bool threw = false;
    try {
        provider.InspectPlaylist("https://example.com/playlist?list=x", [] { return false; });
    } catch (const mediatool::errors::MediaToolException& ex) {
        threw = true;
        EXPECT_EQ(ex.Info().code, "E_INSPECT_PLAYLIST_NO_RESULT");
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::EngineFailure);
    }
    EXPECT_TRUE(threw);
}

TEST(YtDlpProvider, InspectPlaylistSurfacesNotAPlaylistAsAnError) {
    FakeProcessRunner runner({
        R"({"event":"error","data":{"code":"E_NOT_A_PLAYLIST","category":"UNSUPPORTED_FORMAT",)"
        R"("message":"single video","details":"","recoverable":false}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    bool threw = false;
    try {
        provider.InspectPlaylist("https://example.com/watch?v=a", [] { return false; });
    } catch (const mediatool::errors::MediaToolException& ex) {
        threw = true;
        EXPECT_EQ(ex.Info().code, "E_NOT_A_PLAYLIST");
    }
    EXPECT_TRUE(threw);
}

TEST(YtDlpProvider, InspectThrowsMediaToolExceptionOnErrorEvent) {
    FakeProcessRunner runner({
        R"({"event":"error","data":{"code":"E_PLAYLIST_NOT_SUPPORTED","category":"UNSUPPORTED_FORMAT",)"
        R"("message":"playlists not supported","details":"","recoverable":false}})",
    });
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    bool threw = false;
    try {
        provider.Inspect("https://example.com/playlist?list=abc", [] { return false; });
    } catch (const mediatool::errors::MediaToolException& ex) {
        threw = true;
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::UnsupportedFormat);
        EXPECT_EQ(ex.Info().code, "E_PLAYLIST_NOT_SUPPORTED");
    }
    EXPECT_TRUE(threw);
}

TEST(YtDlpProvider, InspectCancellationTerminatesProcessAndThrowsCancelled) {
    FakeProcessRunner runner({}, /*exitCode=*/0, /*staysRunning=*/true);
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    bool threw = false;
    try {
        provider.Inspect("https://example.com/watch?v=abc123", [] { return true; });
    } catch (const mediatool::errors::MediaToolException& ex) {
        threw = true;
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::Cancelled);
    }
    EXPECT_TRUE(threw);
    ASSERT_NE(runner.lastProcessState, nullptr);
    EXPECT_TRUE(runner.lastProcessState->terminated);
}

TEST(YtDlpProvider, InspectThrowsEngineFailureWhenProcessExitsWithoutMetadataOrError) {
    FakeProcessRunner runner({}, /*exitCode=*/1, /*staysRunning=*/false);
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    bool threw = false;
    try {
        provider.Inspect("https://example.com/watch?v=abc123", [] { return false; });
    } catch (const mediatool::errors::MediaToolException& ex) {
        threw = true;
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::EngineFailure);
    }
    EXPECT_TRUE(threw);
}

TEST(YtDlpProvider, InspectStopsAHungChildAtItsDeadlineAndFailsRecoverably) {
    // downloader.py already sets yt-dlp's socket_timeout, but that bounds ONE socket
    // operation, not the fetch: a child that stops making progress without any single
    // socket call timing out holds this thread forever. `staysRunning` is exactly that
    // child -- it never reports an exit on its own.
    FakeProcessRunner runner({}, 0, /*staysRunning=*/true);
    mediatool::downloader::DownloaderTimeouts timeouts;
    timeouts.inspect = std::chrono::milliseconds(1);
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py", "",
                                                   timeouts);

    try {
        provider.Inspect("https://example.com/watch?v=abc123", []() { return false; });
        FAIL() << "expected the deadline to fire";
    } catch (const mediatool::errors::MediaToolException& ex) {
        EXPECT_EQ(ex.Info().code, "E_INSPECT_TIMEOUT");
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::NetworkError);
        // Recoverable: a probe that wedged once is the archetypal case that succeeds on a
        // second attempt, and Phase C's retry policy keys off this flag.
        EXPECT_TRUE(ex.Info().recoverable);
    }

    // The point of the deadline is releasing the thread AND the process. Timing out while
    // leaving the child alive would just move the leak.
    ASSERT_NE(runner.lastProcessState, nullptr);
    EXPECT_TRUE(runner.lastProcessState->terminated);
    EXPECT_FALSE(runner.lastProcessState->running);
}

TEST(YtDlpProvider, CancellationStillWinsOverAnExpiredDeadline) {
    // Both conditions are true on the same poll iteration here (deadline 1ms, cancel
    // always true). The user asked to stop, so they get E_INSPECT_CANCELLED -- being told
    // "the site timed out" for a fetch you cancelled yourself is a lie about what happened.
    FakeProcessRunner runner({}, 0, /*staysRunning=*/true);
    mediatool::downloader::DownloaderTimeouts timeouts;
    timeouts.inspect = std::chrono::milliseconds(0);
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py", "",
                                                   timeouts);

    try {
        provider.Inspect("https://example.com/watch?v=abc123", []() { return true; });
        FAIL() << "expected cancellation";
    } catch (const mediatool::errors::MediaToolException& ex) {
        EXPECT_EQ(ex.Info().code, "E_INSPECT_CANCELLED");
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::Cancelled);
    }
}

TEST(YtDlpProvider, DownloadRefusesAnUnsafeFormatIdWithoutStartingAProcess) {
    // The frontend is gated too (see app/core/main.cpp), but this is the last point at
    // which the value is still ours: every path into yt-dlp's -f goes through here.
    FakeProcessRunner runner({});
    mediatool::downloader::YtDlpProvider provider(runner, "python.exe", "downloader.py");

    auto options = MakeOptions();
    options.formatId = "bestvideo[height<=1080]/best";

    try {
        provider.Download(
            options, [](const mediatool::downloads::DownloadMetadata&) {},
            [](const mediatool::jobs::Progress&) {}, [](const std::string&) {},
            []() { return false; });
        FAIL() << "expected the selector to be rejected";
    } catch (const mediatool::errors::MediaToolException& ex) {
        EXPECT_EQ(ex.Info().code, "E_INVALID_FORMAT_ID");
        EXPECT_EQ(ex.Info().category, mediatool::errors::ErrorCategory::UnsupportedFormat);
    }

    // Rejected before the spawn, not after: no process was ever started.
    EXPECT_TRUE(runner.lastExecutable.empty());
    EXPECT_EQ(runner.lastProcessState, nullptr);
}
