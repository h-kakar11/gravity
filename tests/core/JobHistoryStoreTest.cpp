#include "core/jobs/JobHistoryStore.h"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace mediatool::jobs {
namespace {

namespace fs = std::filesystem;

class JobHistoryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() / "mediatool_job_history_store_test";
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

}  // namespace
}  // namespace mediatool::jobs
