// Durability, corruption recovery, schema versioning and restart recovery
// (spec sections 22-26, 46). These use a real temporary directory rather than a mock
// filesystem: the whole point of this layer is that the bytes on disk survive a process
// dying, and a mock cannot demonstrate that.

#include "core/queue/QueuePersistence.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/errors/MediaToolException.h"

namespace stdfs = std::filesystem;

using mediatool::errors::MediaToolException;
using mediatool::jobs::JobState;
using mediatool::jobs::JobType;
using mediatool::queue::ApplyRestartRecovery;
using mediatool::queue::JobRecord;
using mediatool::queue::kQueueSchemaVersion;
using mediatool::queue::LoadOutcome;
using mediatool::queue::PersistedQueue;
using mediatool::queue::QueuePersistence;
using mediatool::queue::QueueRunState;

namespace {

JobRecord MakeRecord(const std::string& id, JobState state = JobState::Queued) {
    JobRecord record;
    record.id = id;
    record.spec.type = JobType::Download;
    record.spec.params = {{"url", "https://example.com/" + id}};
    record.state = state;
    record.sequence = 1;
    record.createdAtMs = 1'700'000'000'000;
    return record;
}

class QueuePersistenceTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = stdfs::temp_directory_path() /
               ("gravity_persist_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        stdfs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ec;
        stdfs::remove_all(dir_, ec);
    }

    std::string Path() const { return (dir_ / "queue.json").string(); }

    void WriteRaw(const std::string& contents) const {
        std::ofstream output(Path(), std::ios::binary | std::ios::trunc);
        output << contents;
    }

    std::string ReadRaw() const {
        std::ifstream input(Path(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    stdfs::path dir_;
};

}  // namespace

// --- round trip -----------------------------------------------------------------------------

TEST_F(QueuePersistenceTest, SavesAndLoadsAQueue) {
    QueuePersistence persistence(Path());

    PersistedQueue queue;
    queue.runState = QueueRunState::Paused;
    queue.maxConcurrency = 4;
    JobRecord record = MakeRecord("job-a");
    record.priority = mediatool::queue::JobPriority::High;
    record.attempt = 2;
    record.retryPolicy.maxRetries = 5;
    record.dependencies = {"job-b"};
    record.metadata = {{"title", "Example"}};
    queue.records.push_back(record);
    queue.records.push_back(MakeRecord("job-b", JobState::Completed));
    queue.pendingOrder = {"job-a"};

    persistence.Save(queue);
    const auto outcome = persistence.Load();

    ASSERT_EQ(outcome.status, LoadOutcome::Status::Loaded);
    EXPECT_EQ(outcome.queue.schemaVersion, kQueueSchemaVersion);
    EXPECT_EQ(outcome.queue.runState, QueueRunState::Paused);
    EXPECT_EQ(outcome.queue.maxConcurrency, 4u);
    ASSERT_EQ(outcome.queue.records.size(), 2u);
    EXPECT_EQ(outcome.queue.pendingOrder, std::vector<std::string>{"job-a"});

    const auto& loaded = outcome.queue.records[0];
    EXPECT_EQ(loaded.id, "job-a");
    EXPECT_EQ(loaded.priority, mediatool::queue::JobPriority::High);
    EXPECT_EQ(loaded.attempt, 2);
    EXPECT_EQ(loaded.retryPolicy.maxRetries, 5);
    EXPECT_EQ(loaded.dependencies, std::vector<std::string>{"job-b"});
    EXPECT_EQ(loaded.metadata["title"], "Example");
    EXPECT_EQ(loaded.spec.params["url"], "https://example.com/job-a");
}

TEST_F(QueuePersistenceTest, SavingCreatesMissingDirectories) {
    const std::string nested = (dir_ / "a" / "b" / "queue.json").string();
    QueuePersistence persistence(nested);
    EXPECT_NO_THROW(persistence.Save(PersistedQueue{}));
    EXPECT_TRUE(stdfs::exists(nested));
}

TEST_F(QueuePersistenceTest, SaveIsAtomicAndLeavesNoTemporaryFile) {
    QueuePersistence persistence(Path());
    PersistedQueue queue;
    queue.records.push_back(MakeRecord("job-a"));
    persistence.Save(queue);

    // The whole point of AtomicWriter here: after a successful save there is exactly one
    // file, and no ".processing" sibling that a later run could mistake for real state.
    int fileCount = 0;
    for (const auto& entry : stdfs::directory_iterator(dir_)) {
        ++fileCount;
        EXPECT_EQ(entry.path().filename().string(), "queue.json");
    }
    EXPECT_EQ(fileCount, 1);
}

TEST_F(QueuePersistenceTest, SavingTwiceReplacesRatherThanAppends) {
    QueuePersistence persistence(Path());
    PersistedQueue first;
    first.records.push_back(MakeRecord("job-a"));
    persistence.Save(first);

    PersistedQueue second;
    second.records.push_back(MakeRecord("job-b"));
    persistence.Save(second);

    const auto outcome = persistence.Load();
    ASSERT_EQ(outcome.queue.records.size(), 1u);
    EXPECT_EQ(outcome.queue.records[0].id, "job-b");
}

// --- corruption -------------------------------------------------------------------------------

TEST_F(QueuePersistenceTest, MissingFileIsANormalFirstRun) {
    const auto outcome = QueuePersistence(Path()).Load();
    EXPECT_EQ(outcome.status, LoadOutcome::Status::NotPresent);
    EXPECT_TRUE(outcome.queue.records.empty());
    EXPECT_FALSE(outcome.quarantinedPath.has_value());
}

TEST_F(QueuePersistenceTest, EmptyFileIsRecoveredNotCrashed) {
    WriteRaw("");
    const auto outcome = QueuePersistence(Path()).Load();
    EXPECT_EQ(outcome.status, LoadOutcome::Status::Recovered);
    EXPECT_FALSE(outcome.diagnostic.empty());
}

TEST_F(QueuePersistenceTest, WhitespaceOnlyFileIsRecovered) {
    WriteRaw("   \n\t  \n");
    EXPECT_EQ(QueuePersistence(Path()).Load().status, LoadOutcome::Status::Recovered);
}

TEST_F(QueuePersistenceTest, MalformedJsonIsRecoveredAndQuarantined) {
    WriteRaw(R"({"schemaVersion": 1, "records": [{"id": "a", )");  // truncated mid-write
    const auto outcome = QueuePersistence(Path()).Load();

    EXPECT_EQ(outcome.status, LoadOutcome::Status::Recovered);
    ASSERT_TRUE(outcome.quarantinedPath.has_value());
    // Preserved, not destroyed -- the evidence is worth more than the tidiness.
    EXPECT_TRUE(stdfs::exists(*outcome.quarantinedPath));
    EXPECT_FALSE(stdfs::exists(Path()));
}

TEST_F(QueuePersistenceTest, NonObjectDocumentIsRecovered) {
    WriteRaw("[1, 2, 3]");
    EXPECT_EQ(QueuePersistence(Path()).Load().status, LoadOutcome::Status::Recovered);
}

TEST_F(QueuePersistenceTest, MissingSchemaVersionIsRecovered) {
    WriteRaw(R"({"records": []})");
    const auto outcome = QueuePersistence(Path()).Load();
    EXPECT_EQ(outcome.status, LoadOutcome::Status::Recovered);
    EXPECT_NE(outcome.diagnostic.find("schemaVersion"), std::string::npos);
}

TEST_F(QueuePersistenceTest, FutureSchemaVersionIsRefusedWithoutOverwriting) {
    WriteRaw(R"({"schemaVersion": 999, "records": []})");
    const auto outcome = QueuePersistence(Path()).Load();

    EXPECT_EQ(outcome.status, LoadOutcome::Status::Recovered);
    EXPECT_NE(outcome.diagnostic.find("newer version"), std::string::npos);
    // Reading it would mean guessing at fields we do not understand; keeping it intact
    // means a downgrade cannot silently destroy the user's real state.
    ASSERT_TRUE(outcome.quarantinedPath.has_value());
    EXPECT_TRUE(stdfs::exists(*outcome.quarantinedPath));
}

TEST_F(QueuePersistenceTest, CurrentSchemaVersionIsAccepted) {
    WriteRaw(R"({"schemaVersion": 1, "records": [], "pendingOrder": []})");
    EXPECT_EQ(QueuePersistence(Path()).Load().status, LoadOutcome::Status::Loaded);
}

TEST_F(QueuePersistenceTest, OneUnreadableEntryDoesNotCostTheWholeQueue) {
    WriteRaw(R"({"schemaVersion": 1, "records": [
        {"id": "good-1", "state": "QUEUED"},
        {"noIdAtAll": true},
        {"id": "good-2", "state": "QUEUED"}
    ]})");
    const auto outcome = QueuePersistence(Path()).Load();

    EXPECT_EQ(outcome.status, LoadOutcome::Status::Loaded);
    EXPECT_EQ(outcome.queue.records.size(), 2u);
    EXPECT_NE(outcome.diagnostic.find("unreadable"), std::string::npos);
}

TEST_F(QueuePersistenceTest, UnknownEnumValuesFallBackInsteadOfThrowing) {
    WriteRaw(R"({"schemaVersion": 1, "runState": "SIDEWAYS", "records": [
        {"id": "a", "state": "TELEPORTING", "priority": "URGENT"}
    ]})");
    const auto outcome = QueuePersistence(Path()).Load();

    ASSERT_EQ(outcome.status, LoadOutcome::Status::Loaded);
    EXPECT_EQ(outcome.queue.runState, QueueRunState::Running);
    ASSERT_EQ(outcome.queue.records.size(), 1u);
    // An unrecognized state becomes Failed rather than something that might get scheduled.
    EXPECT_EQ(outcome.queue.records[0].state, JobState::Failed);
    EXPECT_EQ(outcome.queue.records[0].priority, mediatool::queue::JobPriority::Normal);
}

TEST_F(QueuePersistenceTest, MissingFieldsFallBackToDefaults) {
    WriteRaw(R"({"schemaVersion": 1, "records": [{"id": "sparse"}]})");
    const auto outcome = QueuePersistence(Path()).Load();

    ASSERT_EQ(outcome.queue.records.size(), 1u);
    const auto& record = outcome.queue.records[0];
    EXPECT_EQ(record.id, "sparse");
    EXPECT_EQ(record.attempt, 0);
    EXPECT_TRUE(record.dependencies.empty());
    EXPECT_FALSE(record.nextRetryAtMs.has_value());
}

TEST_F(QueuePersistenceTest, AbsurdConcurrencyFromAnEditedFileIsClamped) {
    // The state file lives in the user's profile and can be hand-edited.
    WriteRaw(R"({"schemaVersion": 1, "maxConcurrency": 100000, "records": []})");
    const auto outcome = QueuePersistence(Path()).Load();
    EXPECT_LE(outcome.queue.maxConcurrency, 32u);
    EXPECT_GE(outcome.queue.maxConcurrency, 1u);
}

TEST_F(QueuePersistenceTest, ADamagedFileDoesNotBlockLaterSaves) {
    WriteRaw("garbage");
    QueuePersistence persistence(Path());
    ASSERT_EQ(persistence.Load().status, LoadOutcome::Status::Recovered);

    PersistedQueue queue;
    queue.records.push_back(MakeRecord("fresh"));
    EXPECT_NO_THROW(persistence.Save(queue));
    EXPECT_EQ(persistence.Load().queue.records.size(), 1u);
}

// --- restart recovery ---------------------------------------------------------------------------

TEST(RestartRecovery, ExecutingJobsBecomeRetryableFailures) {
    std::vector<JobRecord> records{
        MakeRecord("was-running", JobState::Running),
        MakeRecord("was-starting", JobState::Starting),
        MakeRecord("was-retrying", JobState::Retrying),
    };

    const auto recovered = ApplyRestartRecovery(records);

    EXPECT_EQ(recovered.size(), 3u);
    for (const auto& record : records) {
        // Never reported as completed -- we cannot know what state its output is in.
        EXPECT_EQ(record.state, JobState::Failed) << record.id;
        EXPECT_FALSE(record.finishedAtMs.has_value());
        EXPECT_FALSE(record.lastRetryReason.empty());
    }
}

TEST(RestartRecovery, PendingAndFinishedJobsAreLeftAlone) {
    std::vector<JobRecord> records{
        MakeRecord("queued", JobState::Queued),
        MakeRecord("waiting", JobState::Waiting),
        MakeRecord("retry-wait", JobState::RetryWait),
        MakeRecord("done", JobState::Completed),
        MakeRecord("failed", JobState::Failed),
        MakeRecord("cancelled", JobState::Cancelled),
    };
    const auto before = records;

    const auto recovered = ApplyRestartRecovery(records);

    EXPECT_TRUE(recovered.empty());
    for (std::size_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(records[i].state, before[i].state) << records[i].id;
    }
}

TEST(RestartRecovery, BumpsTheRevisionSoTheFrontendNoticesTheChange) {
    std::vector<JobRecord> records{MakeRecord("was-running", JobState::Running)};
    records[0].revision = 7;

    ApplyRestartRecovery(records);

    EXPECT_EQ(records[0].revision, 8);
}

TEST(RestartRecovery, IsIdempotent) {
    // Recovering an already-recovered set must not keep changing it.
    std::vector<JobRecord> records{MakeRecord("was-running", JobState::Running)};
    ApplyRestartRecovery(records);
    const auto secondPass = ApplyRestartRecovery(records);

    EXPECT_TRUE(secondPass.empty());
    EXPECT_EQ(records[0].state, JobState::Failed);
}
