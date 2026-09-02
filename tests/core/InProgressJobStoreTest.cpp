#include "core/jobs/InProgressJobStore.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tests/support/TempTestDirectory.h"

#include "core/jobs/JobSpec.h"

namespace mediatool::jobs {
namespace {

namespace fs = std::filesystem;

class InProgressJobStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = mediatool::testing::UniqueTempPath("mediatool_in_progress_job_store_test");
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
        fs::create_directories(tempDir_);
        storePath_ = (tempDir_ / "in_progress_jobs.json").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    static JobSpec MakeDownloadSpec(const std::string& id, const std::string& url) {
        JobSpec spec;
        spec.id = id;
        spec.type = JobType::Download;
        spec.params = {{"url", url}, {"outputDirectory", "C:\\out"}, {"quality", "1080P"}};
        spec.createdAt = "2026-09-01T00:00:00Z";
        return spec;
    }

    void WriteRaw(const std::string& contents) const {
        std::ofstream out(storePath_, std::ios::binary | std::ios::trunc);
        out << contents;
    }

    fs::path tempDir_;
    std::string storePath_;
};

TEST_F(InProgressJobStoreTest, LoadOnMissingFileReturnsEmpty) {
    InProgressJobStore store(storePath_);
    EXPECT_TRUE(store.Load().empty());
}

TEST_F(InProgressJobStoreTest, ASpecRoundTripsWithEverythingNeededToRebuildTheJob) {
    // This is the whole reason JobSpec exists rather than reusing JobSnapshot: a snapshot
    // says how a job is doing, and a recipe says what it was asked to do. Losing `params`
    // here would leave a recovered "download" with no URL.
    InProgressJobStore store(storePath_);
    store.Put(MakeDownloadSpec("job-1", "https://example.com/watch?v=abc"));

    const std::vector<JobSpec> loaded = InProgressJobStore(storePath_).Load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_EQ(loaded[0].id, "job-1");
    EXPECT_EQ(loaded[0].type, JobType::Download);
    EXPECT_EQ(loaded[0].params.at("url").get<std::string>(), "https://example.com/watch?v=abc");
    EXPECT_EQ(loaded[0].params.at("outputDirectory").get<std::string>(), "C:\\out");
    EXPECT_EQ(loaded[0].createdAt, "2026-09-01T00:00:00Z");
    EXPECT_EQ(loaded[0].recoveryCount, 0);
    EXPECT_FALSE(loaded[0].artifact.has_value());
}

TEST_F(InProgressJobStoreTest, PutOnAnExistingIdUpdatesItInPlaceKeepingSubmissionOrder) {
    // Submission order is not cosmetic: the recovery pass replays in this order so a
    // `dependsOn` edge still points at a job submitted before it. An update that moved an
    // entry to the back would silently invert a dependency.
    InProgressJobStore store(storePath_);
    store.Put(MakeDownloadSpec("job-1", "https://example.com/a"));
    store.Put(MakeDownloadSpec("job-2", "https://example.com/b"));
    store.Put(MakeDownloadSpec("job-3", "https://example.com/c"));

    JobSpec updated = MakeDownloadSpec("job-1", "https://example.com/a");
    updated.recoveryCount = 2;
    store.Put(updated);

    const std::vector<JobSpec> loaded = store.Load();
    ASSERT_EQ(loaded.size(), 3u);
    EXPECT_EQ(loaded[0].id, "job-1");
    EXPECT_EQ(loaded[0].recoveryCount, 2);
    EXPECT_EQ(loaded[1].id, "job-2");
    EXPECT_EQ(loaded[2].id, "job-3");
}

TEST_F(InProgressJobStoreTest, ArtifactLocationIsRecordedForALiveJobAndIgnoredForAnUnknownOne) {
    InProgressJobStore store(storePath_);
    store.Put(MakeDownloadSpec("job-1", "https://example.com/a"));

    store.SetArtifactLocation("job-1", {"C:\\out", "Vacation Clip"});
    // A job that already finished has no record; recording against it would resurrect one.
    store.SetArtifactLocation("job-gone", {"C:\\out", "Something Else"});

    const std::vector<JobSpec> loaded = store.Load();
    ASSERT_EQ(loaded.size(), 1u);
    ASSERT_TRUE(loaded[0].artifact.has_value());
    EXPECT_EQ(loaded[0].artifact->outputDirectory, "C:\\out");
    EXPECT_EQ(loaded[0].artifact->filenameBase, "Vacation Clip");
}

TEST_F(InProgressJobStoreTest, RemoveDropsOneEntryAndClearDropsThemAll) {
    InProgressJobStore store(storePath_);
    store.Put(MakeDownloadSpec("job-1", "https://example.com/a"));
    store.Put(MakeDownloadSpec("job-2", "https://example.com/b"));

    store.Remove("job-1");
    ASSERT_EQ(store.Load().size(), 1u);
    EXPECT_EQ(store.Load()[0].id, "job-2");

    store.Remove("job-does-not-exist");  // must not throw or disturb the rest
    EXPECT_EQ(store.Load().size(), 1u);

    store.Clear();
    EXPECT_TRUE(store.Load().empty());
}

TEST_F(InProgressJobStoreTest, ACorruptFileStartsEmptyRatherThanThrowing) {
    // This file is written by the process that is crashing, so a truncated or garbage file
    // is the expected state after the exact failure it exists to survive. Throwing here
    // would take down startup on the one launch that most needs to work.
    WriteRaw("{ this is not json");
    EXPECT_TRUE(InProgressJobStore(storePath_).Load().empty());

    WriteRaw("{\"not\":\"an array\"}");
    EXPECT_TRUE(InProgressJobStore(storePath_).Load().empty());
}

TEST_F(InProgressJobStoreTest, OneUnreadableEntryIsSkippedAndTheOthersSurvive) {
    // One record written by an older build (or torn by the crash) should not cost the user
    // the other nineteen.
    WriteRaw(R"([
        {"id":"job-1","type":"DOWNLOAD","params":{"url":"https://example.com/a"}},
        {"id":"job-2","type":"NOT_A_REAL_TYPE","params":{}},
        {"type":"DOWNLOAD","params":{}},
        {"id":"job-4","type":"CONVERSION","params":{"inputPath":"C:\\in\\a.mov"}}
    ])");

    const std::vector<JobSpec> loaded = InProgressJobStore(storePath_).Load();
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded[0].id, "job-1");
    EXPECT_EQ(loaded[1].id, "job-4");
    EXPECT_EQ(loaded[1].type, JobType::Conversion);
}

TEST_F(InProgressJobStoreTest, AHalfWrittenArtifactRecordIsTreatedAsAbsent) {
    // An artifact record with an empty filenameBase would scope a delete to "everything
    // IsJobArtifactOf accepts for the empty string". Absent is the only safe reading.
    WriteRaw(R"([
        {"id":"job-1","type":"DOWNLOAD","params":{},
         "artifact":{"outputDirectory":"C:\\out","filenameBase":""}}
    ])");
    const std::vector<JobSpec> loaded = InProgressJobStore(storePath_).Load();
    ASSERT_EQ(loaded.size(), 1u);
    EXPECT_FALSE(loaded[0].artifact.has_value());
}

TEST_F(InProgressJobStoreTest, ConcurrentPutsFromManyThreadsAllSurvive) {
    // Put() is a read-modify-write of one file called from JobManager worker threads via
    // the job state-changed callback. Unserialized, two jobs finishing at the same moment
    // each write the history they read, and the second rename discards the first.
    InProgressJobStore store(storePath_);
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, t] {
            for (int i = 0; i < kPerThread; ++i) {
                store.Put(MakeDownloadSpec("job-" + std::to_string(t) + "-" + std::to_string(i),
                                            "https://example.com/x"));
            }
        });
    }
    for (auto& thread : threads) thread.join();

    EXPECT_EQ(store.Load().size(), static_cast<std::size_t>(kThreads * kPerThread));
}

}  // namespace
}  // namespace mediatool::jobs
