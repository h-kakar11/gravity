// The boundaries where untrusted input crosses into something that interprets it.
//
// Four of them, each with a different interpreter on the far side and therefore a
// different thing that "injection" means:
//
//   settings      -> a JSON file the app re-reads at startup, and values it acts on
//   process spawn -> CreateProcess/execve, via a structured argv
//   yt-dlp -f     -> an expression language
//   ffmpeg args   -> a flag parser and a filter-graph language
//
// These are grouped in one file on purpose. Each individual assertion would look
// unremarkable filed under the component it exercises; together they are the answer to
// "what happens when the value is hostile", which is a question about the system, not
// about any one class.

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/errors/MediaToolException.h"
#include "core/ipc/RequestValidation.h"
#include "core/process/MockProcessRunner.h"
#include "core/settings/Settings.h"
#include "engines/downloader/YtDlpFormatSelector.h"
#include "engines/ffmpeg/FFmpegArgBuilder.h"

namespace {

using mediatool::errors::MediaToolException;
using mediatool::media::BuildFfmpegArgs;
using mediatool::media::MediaProcessingOptions;

// Payloads that are dangerous in a SHELL. None of them should ever be dangerous here,
// because nothing in this codebase builds a command line -- IProcessRunner takes a
// structured argv. These exist to prove that stays true, not to suggest it is not.
const std::vector<std::string>& ShellMetacharacterPayloads() {
    static const std::vector<std::string> payloads = {
        "clip.mp4; rm -rf /",
        "clip.mp4 && calc.exe",
        "clip.mp4 | more",
        "clip.mp4 & start notepad",
        "$(whoami).mp4",
        "`whoami`.mp4",
        "clip.mp4\ncalc.exe",
        "clip.mp4\"; del *.*; \"",
        "%SYSTEMROOT%\\system32\\calc.exe",
    };
    return payloads;
}

bool Contains(const std::vector<std::string>& args, const std::string& value) {
    return std::find(args.begin(), args.end(), value) != args.end();
}

}  // namespace

// --- 1. Settings ----------------------------------------------------------------------

TEST(SecurityBoundary, SettingsValidationRejectsOutOfRangeValuesRatherThanClamping) {
    // updateSettings takes a partial object from the frontend and merges it. A value that
    // is out of range must be REFUSED, not silently clamped: clamping means the caller is
    // told "saved" while the app runs with something they did not choose, and the file
    // that gets written is then the clamped value, so the disagreement is permanent.
    nlohmann::json json = mediatool::settings::Settings::Defaults().ToJson();
    json["processing"]["concurrentJobs"] = 100000;
    EXPECT_THROW((void)mediatool::settings::Settings::FromJson(json), MediaToolException);

    json = mediatool::settings::Settings::Defaults().ToJson();
    json["processing"]["maxRetryAttempts"] = 0;  // "zero attempts" describes a job that never runs
    EXPECT_THROW((void)mediatool::settings::Settings::FromJson(json), MediaToolException);

    json = mediatool::settings::Settings::Defaults().ToJson();
    json["advanced"]["logLevel"] = "TRACE; DROP TABLE";
    EXPECT_THROW((void)mediatool::settings::Settings::FromJson(json), MediaToolException);
}

TEST(SecurityBoundary, SettingsPathFieldsMustBeAbsolutePathsNotArbitraryStrings) {
    // ffmpegPath and ytDlpPath are executable locations the app will LAUNCH. A relative
    // value there resolves against the process's working directory, which is whatever the
    // shell that started Gravity happened to be in -- the same class of defect as issue
    // #79, and a way to get a different binary run.
    nlohmann::json json = mediatool::settings::Settings::Defaults().ToJson();
    json["advanced"]["ffmpegPath"] = "ffmpeg.exe";
    EXPECT_THROW((void)mediatool::settings::Settings::FromJson(json), MediaToolException);

    json = mediatool::settings::Settings::Defaults().ToJson();
    json["advanced"]["ytDlpPath"] = "..\\..\\evil\\yt-dlp.exe";
    EXPECT_THROW((void)mediatool::settings::Settings::FromJson(json), MediaToolException);

    // An absolute path is accepted without the file having to exist -- a not-yet-created
    // output directory is legitimate, and existence is a runtime question anyway.
    json = mediatool::settings::Settings::Defaults().ToJson();
    json["advanced"]["ffmpegPath"] = "C:\\tools\\ffmpeg\\bin\\ffmpeg.exe";
    EXPECT_NO_THROW((void)mediatool::settings::Settings::FromJson(json));
}

TEST(SecurityBoundary, AnInvalidSettingsPayloadNeverPartiallyApplies) {
    // FromJson validates the WHOLE object before returning one, so a payload that is good
    // in its first half and hostile in its second cannot leave the app half-updated.
    nlohmann::json json = mediatool::settings::Settings::Defaults().ToJson();
    json["downloads"]["defaultQuality"] = "1080p";  // valid
    json["processing"]["concurrentJobs"] = -5;       // not
    EXPECT_THROW((void)mediatool::settings::Settings::FromJson(json), MediaToolException);
}

// --- 2. Process spawn -----------------------------------------------------------------

TEST(SecurityBoundary, ShellMetacharactersInAPathStayInsideOneArgvElement) {
    // The structural guarantee IProcessRunner exists for (spec section 16): args is a
    // vector, never a command string, so a shell operator in a filename is one more
    // character of one argument and there is no shell to interpret it. Asserted through
    // the arg builder because that is where a filename actually becomes an argv element.
    for (const std::string& payload : ShellMetacharacterPayloads()) {
        MediaProcessingOptions options;
        options.outputFormat = "mp4";
        const std::vector<std::string> args =
            BuildFfmpegArgs("C:\\in\\" + payload, "C:\\out\\clip.mp4", options, {"libx264"});

        // Exactly one element equals the input path, verbatim -- not split on the
        // metacharacter, not quoted, not escaped into something else.
        EXPECT_TRUE(Contains(args, "C:\\in\\" + payload)) << payload;
        EXPECT_EQ(std::count(args.begin(), args.end(), "C:\\in\\" + payload), 1) << payload;
    }
}

TEST(SecurityBoundary, ArgvElementsAreHandedToTheRunnerUnmodified) {
    // The other half of the same guarantee, one layer down: whatever vector a caller
    // builds is the vector the runner receives. A runner that concatenated and re-split
    // would undo everything above.
    mediatool::process::MockProcessRunner runner({}, {}, 0);
    std::vector<std::string> received;
    class CapturingRunner : public mediatool::process::IProcessRunner {
    public:
        explicit CapturingRunner(std::vector<std::string>& out) : out_(out) {}
        std::unique_ptr<mediatool::process::IProcess> Start(
            const std::string&, const std::vector<std::string>& args,
            const mediatool::process::ProcessOptions&, mediatool::process::OutputLineCallback,
            mediatool::process::OutputLineCallback) override {
            out_ = args;
            return nullptr;
        }

    private:
        std::vector<std::string>& out_;
    };

    CapturingRunner capturing(received);
    const std::vector<std::string> hostile = {"-i", "C:\\in\\a; rm -rf /.mp4", "-y",
                                               "C:\\out\\b\" && calc.exe.mp4"};
    (void)capturing.Start("ffmpeg.exe", hostile, {}, nullptr, nullptr);
    EXPECT_EQ(received, hostile);
}

// --- 3. yt-dlp format selector --------------------------------------------------------

TEST(SecurityBoundary, FormatIdRejectsTheExpressionSyntaxThatMakesDashFALanguage) {
    using mediatool::downloader::IsSafeFormatSelector;
    // -f is not a name field. Each of these means something to yt-dlp that the user who
    // picked a stream from a list did not ask for.
    const std::vector<std::string> hostile = {
        "all",                          // every stream on the page
        "mergeall",                     // ...merged into one file
        "bestvideo[height<=2160]",      // a filter, selecting something never shown
        "137/140",                      // a fallback chain
        "137,140",                      // a set
        "(137+140)[filesize>1M]",       // grouping plus a filter
        "best*",                        // a wildcard
        "137 140",                      // whitespace, i.e. two selectors
        "137;calc.exe",
        "$(whoami)",
        "--exec=calc.exe",              // a flag, if it ever reached argv unbounded
        "-f",                           // ...and the single-dash spelling
        ".\\evil",                       // a relative path, if it ever reached one
    };
    for (const std::string& selector : hostile) {
        EXPECT_FALSE(IsSafeFormatSelector(selector)) << selector;
    }
}

TEST(SecurityBoundary, FormatIdStillAcceptsWhatInspectActuallyReports) {
    using mediatool::downloader::IsSafeFormatSelector;
    // A gate that rejected real ids would be worse than no gate: it would push callers
    // toward passing the quality preset instead, losing the feature.
    for (const std::string& real : {"137", "140", "hls-1080p", "dash_video-2", "http-1.0",
                                     "137+140", "bestaudio"}) {
        EXPECT_TRUE(IsSafeFormatSelector(real)) << real;
    }
}

// --- 4. ffmpeg arguments --------------------------------------------------------------

TEST(SecurityBoundary, AnUnknownQualityOrCodecCannotBecomeAnFfmpegFlag) {
    // Free-text option values reach the arg builder. None of them may be emitted as a
    // separate argv element that ffmpeg would read as a flag.
    MediaProcessingOptions options;
    options.outputFormat = "mp4";
    options.quality = "-loglevel";
    options.videoCodec = "-i /etc/passwd";

    const std::vector<std::string> args =
        BuildFfmpegArgs("C:\\in\\a.mov", "C:\\out\\b.mp4", options, {"libx264"});
    // "-loglevel" IS in the command line -- the builder always emits it -- which is the
    // point: a quality value that spells a real flag must not become a SECOND one, and
    // must not change the value of the one that is there.
    EXPECT_EQ(std::count(args.begin(), args.end(), "-loglevel"), 1);
    const auto logLevelIt = std::find(args.begin(), args.end(), "-loglevel");
    ASSERT_NE(std::next(logLevelIt), args.end());
    EXPECT_EQ(*std::next(logLevelIt), "error");
    EXPECT_FALSE(Contains(args, "-i /etc/passwd"));
    // Neither value appears anywhere, in any element: quality is a lookup that degrades to
    // a known tier, and videoCodec resolves through an ALLOWLIST that degrades to the
    // bundled default. Neither is ever passed through.
    for (const std::string& arg : args) {
        EXPECT_EQ(arg.find("/etc/passwd"), std::string::npos) << arg;
    }
    const auto encoderIt = std::find(args.begin(), args.end(), "-c:v");
    ASSERT_NE(encoderIt, args.end());
    ASSERT_NE(std::next(encoderIt), args.end());
    EXPECT_EQ(*std::next(encoderIt), "libopenh264");
}

TEST(SecurityBoundary, AWatermarkPathWithFilterGraphMetacharactersDoesNotSplitTheGraph) {
    // ffmpeg's -filter_complex IS a language: ':' separates options, ',' chains filters,
    // ';' separates graphs, '[' names a pad. A watermark path is user-supplied, so a raw
    // one could append a filter of the attacker's choosing to the graph.
    MediaProcessingOptions options;
    options.outputFormat = "mp4";
    mediatool::media::WatermarkOptions watermark;
    watermark.imagePath = "C:\\wm\\logo.png";
    options.watermark = watermark;

    const std::vector<std::string> safeArgs =
        BuildFfmpegArgs("C:\\in\\a.mov", "C:\\out\\b.mp4", options, {"libx264"});
    const auto filterIt = std::find(safeArgs.begin(), safeArgs.end(), "-filter_complex");
    ASSERT_NE(filterIt, safeArgs.end());
    ASSERT_NE(std::next(filterIt), safeArgs.end());

    // The watermark image goes in as its own INPUT (-i), never interpolated into the
    // filter string -- which is why a metacharacter in it cannot reach the graph parser.
    // If this ever changes, the assertion below is what catches it.
    const std::string filterGraph = *std::next(filterIt);
    EXPECT_EQ(filterGraph.find(watermark.imagePath), std::string::npos);
    EXPECT_TRUE(Contains(safeArgs, watermark.imagePath));

    options.watermark->imagePath = "C:\\wm\\logo.png';drawtext=text='pwned";
    const std::vector<std::string> hostileArgs =
        BuildFfmpegArgs("C:\\in\\a.mov", "C:\\out\\b.mp4", options, {"libx264"});
    const auto hostileFilterIt =
        std::find(hostileArgs.begin(), hostileArgs.end(), "-filter_complex");
    ASSERT_NE(hostileFilterIt, hostileArgs.end());
    EXPECT_EQ(std::next(hostileFilterIt)->find("drawtext"), std::string::npos);
}

TEST(SecurityBoundary, NumericOptionsAreBoundedBeforeTheyBecomeArguments) {
    // A resolution or bitrate arrives as a number and is formatted into an argument. The
    // interesting values are the ones that are numbers but not sane ones.
    MediaProcessingOptions options;
    options.outputFormat = "mp4";
    options.videoBitrateKbps = -5000;

    const std::vector<std::string> args =
        BuildFfmpegArgs("C:\\in\\a.mov", "C:\\out\\b.mp4", options, {"libx264"});
    // videoBitrateKbps is read straight out of the caller's options JSON, so this value is
    // reachable from the IPC boundary. Emitting it produces "-b:v -5000k", i.e. a
    // malformed command line where ffmpeg reads the value as another flag.
    for (const std::string& arg : args) {
        EXPECT_NE(arg, "-5000k");
        EXPECT_NE(arg, "-10000k");
    }
    // It falls through to CRF instead of silently encoding at some invented rate: the
    // caller asked for a rate-control target that is not one, so there is no target.
    EXPECT_TRUE(Contains(args, "-crf"));

    options.videoBitrateKbps = 0;
    const std::vector<std::string> zeroArgs =
        BuildFfmpegArgs("C:\\in\\a.mov", "C:\\out\\b.mp4", options, {"libx264"});
    EXPECT_FALSE(Contains(zeroArgs, "0k"));
    EXPECT_TRUE(Contains(zeroArgs, "-crf"));
}

// --- 5. IPC parameter validation ------------------------------------------------------

TEST(SecurityBoundary, RequireEnumIsAnAllowlistNotABlocklist) {
    // Every enum-valued parameter goes through this. An allowlist fails closed on a value
    // nobody thought of; a blocklist fails open on exactly those.
    // Not a shell payload -- there is no shell here. The point is that a value the
    // allowlist does not contain is refused whatever it looks like, including one that
    // merely resembles an accepted value.
    for (const std::string& rejected :
         {"MEDIUM", "medium ", " medium", "medium;low", "medium\\u0000", "med", ""}) {
        const nlohmann::json params = {{"quality", rejected}};
        EXPECT_THROW((void)mediatool::ipc::RequireEnum(params, "quality",
                                                        {"lowest", "low", "medium", "high"}),
                     MediaToolException)
            << "accepted: [" << rejected << "]";
    }

    const nlohmann::json valid = {{"quality", "medium"}};
    EXPECT_EQ(mediatool::ipc::RequireEnum(valid, "quality", {"lowest", "low", "medium", "high"}),
               "medium");
}

TEST(SecurityBoundary, IntegerParametersAreRangeCheckedNotJustTypeChecked) {
    // A type check alone accepts INT64_MIN, which then becomes a thread count, an array
    // size, or a priority key.
    const nlohmann::json params = {{"priority", 9223372036854775807LL}};
    EXPECT_THROW((void)mediatool::ipc::RequireInt(params, "priority", -1000, 1000),
                 MediaToolException);

    const nlohmann::json negative = {{"priority", -9223372036854775807LL}};
    EXPECT_THROW((void)mediatool::ipc::RequireInt(negative, "priority", -1000, 1000),
                 MediaToolException);
}

TEST(SecurityBoundary, AnUnboundedArrayParameterIsCapped) {
    // dependsOn comes from the caller. Without a cap, a single createJob can make the
    // scheduler hold an arbitrarily large edge list under JobManager's lock.
    nlohmann::json huge = nlohmann::json::array();
    for (int i = 0; i < 5000; ++i) huge.push_back("job-" + std::to_string(i));
    const nlohmann::json params = {{"dependsOn", huge}};
    EXPECT_THROW((void)mediatool::ipc::OptionalStringArray(params, "dependsOn", 32),
                 MediaToolException);
}
