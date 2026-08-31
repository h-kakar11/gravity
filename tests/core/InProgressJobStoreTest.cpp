#include "core/jobs/InProgressJobStore.h"

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

namespace mediatool::jobs {
namespace {

namespace fs = std::filesystem;

class InProgressJobStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = fs::temp_directory_path() / "mediatool_in_progress_job_store_test";
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
        fs::create_directories(tempDir_);
        storePath_ = (tempDir_ / "jobs_in_progress.json").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir_, ec);
    }

    static nlohmann::json MakeSnapshot(const std::string& id, const std::string& state = "RUNNING") {
        return nlohmann::json{{"id", id}, {"type", "DOWNLOAD"}, {"state", state}};
    }

    fs::path tempDir_;
    std::string storePath_;
};

TEST_F(InProgressJobStoreTest, LoadOnMissingFileReturnsEmpty) {
    InProgressJobStore store(storePath_);
    EXPECT_TRUE(store.Load().empty());
}

TEST_F(InProgressJobStoreTest, UpsertThenLoadRoundTrips) {
    InProgressJobStore store(storePath_);
    store.Upsert(MakeSnapshot("job-1"));
    store.Upsert(MakeSnapshot("job-2"));

    const auto entries = store.Load();
    ASSERT_EQ(entries.size(), 2u);
}

TEST_F(InProgressJobStoreTest, UpsertReplacesExistingEntryForSameId) {
    InProgressJobStore store(storePath_);
    store.Upsert(MakeSnapshot("job-1", "STARTING"));
    store.Upsert(MakeSnapshot("job-1", "RUNNING"));

    const auto entries = store.Load();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].at("state"), "RUNNING");
}

TEST_F(InProgressJobStoreTest, RemoveDropsOnlyTheMatchingEntry) {
    InProgressJobStore store(storePath_);
    store.Upsert(MakeSnapshot("job-1"));
    store.Upsert(MakeSnapshot("job-2"));

    store.Remove("job-1");

    const auto entries = store.Load();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].at("id"), "job-2");
}

TEST_F(InProgressJobStoreTest, RemoveOfUnknownIdIsANoOp) {
    InProgressJobStore store(storePath_);
    store.Upsert(MakeSnapshot("job-1"));

    EXPECT_NO_THROW(store.Remove("job-does-not-exist"));

    const auto entries = store.Load();
    ASSERT_EQ(entries.size(), 1u);
}

TEST_F(InProgressJobStoreTest, UpsertWithoutStringIdIsIgnored) {
    InProgressJobStore store(storePath_);
    nlohmann::json noId = {{"type", "DOWNLOAD"}, {"state", "RUNNING"}};

    EXPECT_NO_THROW(store.Upsert(noId));

    EXPECT_TRUE(store.Load().empty());
}

TEST_F(InProgressJobStoreTest, LoadOnCorruptFileReturnsEmptyWithoutThrowing) {
    {
        std::ofstream corrupt(storePath_, std::ios::binary | std::ios::trunc);
        corrupt << "{ not an array";
    }
    InProgressJobStore store(storePath_);
    EXPECT_NO_THROW({
        const auto entries = store.Load();
        EXPECT_TRUE(entries.empty());
    });
}

TEST_F(InProgressJobStoreTest, UpsertCreatesParentDirectoriesIfMissing) {
    const fs::path nestedPath = tempDir_ / "nested" / "deeper" / "jobs_in_progress.json";
    InProgressJobStore store(nestedPath.string());
    store.Upsert(MakeSnapshot("job-1"));
    EXPECT_TRUE(fs::exists(nestedPath));
}

// Same UTF-8 robustness requirement as JobHistoryStore -- see
// JobHistoryStoreTest.AppendSurvivesInvalidUtf8InTextFields and docs/pr43-findings.md.
TEST_F(InProgressJobStoreTest, UpsertSurvivesInvalidUtf8InTextFields) {
    InProgressJobStore store(storePath_);
    nlohmann::json snapshot = MakeSnapshot("job-1");
    snapshot["title"] = "bad-title-\xC3\x28-end";  // 0xC3 with no valid continuation byte

    EXPECT_NO_THROW(store.Upsert(snapshot));

    const auto entries = store.Load();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].at("id"), "job-1");
}

}  // namespace
}  // namespace mediatool::jobs
