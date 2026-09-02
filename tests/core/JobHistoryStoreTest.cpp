#include "core/jobs/JobHistoryStore.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "tests/support/TempTestDirectory.h"

namespace mediatool::jobs {
namespace {

namespace fs = std::filesystem;

class JobHistoryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = mediatool::testing::UniqueTempPath("mediatool_job_history_store_test");
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
        fs::create_directories(tempDir_);
        historyPath_ = (tempDir_ / "job_history.json").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    static nlohmann::json MakeSnapshot(const std::string& id) {
        return nlohmann::json{{"id", id}, {"type", "DOWNLOAD"}, {"state", "COMPLETED"}};
    }

    fs::path tempDir_;
    std::string historyPath_;
};

TEST_F(JobHistoryStoreTest, LoadOnMissingFileReturnsEmpty) {
    JobHistoryStore store(historyPath_);
    EXPECT_TRUE(store.Load().empty());
}

TEST_F(JobHistoryStoreTest, AppendThenLoadRoundTrips) {
    JobHistoryStore store(historyPath_);
    store.Append(MakeSnapshot("job-1"));
    store.Append(MakeSnapshot("job-2"));

    const auto entries = store.Load();
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].at("id"), "job-1");
    EXPECT_EQ(entries[1].at("id"), "job-2");
}

TEST_F(JobHistoryStoreTest, TrimsOldestEntriesPastMaxEntries) {
    JobHistoryStore store(historyPath_, /*maxEntries=*/3);
    for (int i = 0; i < 5; ++i) store.Append(MakeSnapshot("job-" + std::to_string(i)));

    const auto entries = store.Load();
    ASSERT_EQ(entries.size(), 3u);
    // Oldest (job-0, job-1) trimmed; the three most recent survive, in order.
    EXPECT_EQ(entries[0].at("id"), "job-2");
    EXPECT_EQ(entries[1].at("id"), "job-3");
    EXPECT_EQ(entries[2].at("id"), "job-4");
}

TEST_F(JobHistoryStoreTest, LoadOnCorruptFileReturnsEmptyWithoutThrowing) {
    {
        std::ofstream corrupt(historyPath_, std::ios::binary | std::ios::trunc);
        corrupt << "{ not an array";
    }
    JobHistoryStore store(historyPath_);
    EXPECT_NO_THROW({
        const auto entries = store.Load();
        EXPECT_TRUE(entries.empty());
    });
}

TEST_F(JobHistoryStoreTest, AppendCreatesParentDirectoriesIfMissing) {
    const fs::path nestedPath = tempDir_ / "nested" / "deeper" / "job_history.json";
    JobHistoryStore store(nestedPath.string());
    store.Append(MakeSnapshot("job-1"));
    EXPECT_TRUE(fs::exists(nestedPath));
}

// Regression test: a job title can carry text this process doesn't control the byte-level
// encoding of (an unusually-encoded video title, ffmpeg/yt-dlp error text). Default
// nlohmann::json::dump() throws on invalid UTF-8 -- see docs/pr43-findings.md. This must
// not throw, and the entry must still be persisted (with the offending byte substituted).
TEST_F(JobHistoryStoreTest, AppendSurvivesInvalidUtf8InTextFields) {
    JobHistoryStore store(historyPath_);
    nlohmann::json snapshot = MakeSnapshot("job-1");
    snapshot["title"] = "bad-title-\xC3\x28-end";  // 0xC3 with no valid continuation byte

    EXPECT_NO_THROW(store.Append(snapshot));

    const auto entries = store.Load();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].at("id"), "job-1");
}


// Every terminal job is supposed to end up in the session history. Before the store was
// serialized, Append() was a read-modify-write (load the file, push one entry, rename a
// new file over it) called from JobManager's worker threads, so at concurrentJobs > 1 two
// jobs completing together would both load the same history and the later rename would
// drop the earlier job's entry -- silent, and invisible in any single-threaded test.
TEST_F(JobHistoryStoreTest, ConcurrentAppendsFromManyThreadsLoseNoEntries) {
    constexpr int kThreads = 8;
    constexpr int kAppendsPerThread = 25;
    constexpr int kTotal = kThreads * kAppendsPerThread;

    JobHistoryStore store(historyPath_, /*maxEntries=*/kTotal);

    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&store, &go, t] {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < kAppendsPerThread; ++i) {
                store.Append(MakeSnapshot("job-" + std::to_string(t) + "-" + std::to_string(i)));
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();

    const std::vector<nlohmann::json> entries = store.Load();
    ASSERT_EQ(entries.size(), static_cast<size_t>(kTotal));

    // Not just the count: every distinct id must be present exactly once, which also
    // proves no entry was written twice or corrupted by an interleaved write.
    std::set<std::string> ids;
    for (const auto& entry : entries) ids.insert(entry.at("id").get<std::string>());
    EXPECT_EQ(ids.size(), static_cast<size_t>(kTotal));
}

}  // namespace
}  // namespace mediatool::jobs
