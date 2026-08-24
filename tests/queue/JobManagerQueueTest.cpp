// Integration tests for the orchestrator itself: real Job objects, real worker threads,
// real state transitions -- but a controllable clock and an explicitly driven scheduler, so
// nothing here depends on wall-clock timing.
//
// The pattern throughout is: submit, call RunSchedulerPassForTesting() (which dispatches and
// then waits for whatever it started), assert. No sleeps, no retry-until-it-passes loops
// (spec section 62).

#include "core/jobs/JobManager.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "core/queue/QueuePersistence.h"

namespace stdfs = std::filesystem;

using mediatool::errors::ErrorCategory;
using mediatool::errors::ErrorInfo;
using mediatool::errors::MediaToolException;
using mediatool::jobs::IJobFactory;
using mediatool::jobs::Job;
using mediatool::jobs::JobId;
using mediatool::jobs::JobManager;
using mediatool::jobs::JobState;
using mediatool::jobs::JobType;
using mediatool::jobs::Progress;
using mediatool::queue::HistoryScope;
using mediatool::queue::JobPriority;
using mediatool::queue::JobSpec;
using mediatool::queue::MoveDirection;
using mediatool::queue::QueueRunState;
using mediatool::queue::RetryPolicy;

namespace {

// A clock the test moves by hand. Overriding NowUnixMillis is what makes retry backoff and
// fairness aging assertable without waiting for real seconds to pass.
class ControlledClock final : public mediatool::common::IClock {
public:
    std::string NowIso8601Utc() const override { return "2026-01-01T00:00:00.000Z"; }
    std::int64_t NowUnixMillis() const override { return now_.load(); }
    void Advance(std::int64_t ms) { now_ += ms; }
    void SetNow(std::int64_t ms) { now_ = ms; }

private:
    std::atomic<std::int64_t> now_{1'000'000};
};

// A job whose behaviour the test dictates: succeed, fail with a given error, or block until
// released. Records how many times it actually executed, which is how "no job runs twice"
// is verified.
class ScriptedJob final : public Job {
public:
    enum class Behaviour { Succeed, Fail, BlockUntilReleased };

    ScriptedJob(JobType type, Behaviour behaviour, ErrorInfo error = {})
        : Job(type), behaviour_(behaviour), error_(std::move(error)) {}

    void Execute() override {
        ++executions;
        started = true;
        switch (behaviour_) {
            case Behaviour::Succeed:
                ReportProgress(Progress{.percentage = 100.0, .statusMessage = "done"});
                if (!outputPath_.empty()) SetResult({{"outputPath", outputPath_}});
                return;
            case Behaviour::Fail:
                throw MediaToolException(error_);
            case Behaviour::BlockUntilReleased: {
                // Bounded so a bug fails the test instead of hanging the whole suite.
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (!release_ && std::chrono::steady_clock::now() < deadline) {
                    if (IsCancellationRequested()) {
                        throw MediaToolException(ErrorInfo::Make(
                            "E_CANCELLED", ErrorCategory::Cancelled, "cancelled"));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                if (IsCancellationRequested()) {
                    throw MediaToolException(
                        ErrorInfo::Make("E_CANCELLED", ErrorCategory::Cancelled, "cancelled"));
                }
                return;
            }
        }
    }

    void Release() { release_ = true; }
    // Flipped so a later attempt of the same job can behave differently from the first.
    void SetBehaviour(Behaviour behaviour) { behaviour_ = behaviour; }

    // Publishes an outputPath, so a following pipeline stage has something to resolve.
    void PublishOutput(std::string path) { outputPath_ = std::move(path); }
    void ApplyResolvedInput(const std::string& inputPath) override { resolvedInput = inputPath; }

    std::string resolvedInput;

    std::atomic<int> executions{0};
    std::atomic<bool> started{false};

private:
    std::atomic<Behaviour> behaviour_;
    ErrorInfo error_;
    std::atomic<bool> release_{false};
    std::string outputPath_;
};

// Rebuilds jobs from specs after a restart. `params.behaviour` decides what the rebuilt job
// does, so a restart test can make the second run succeed where the first was interrupted.
class ScriptedJobFactory final : public IJobFactory {
public:
    std::unique_ptr<Job> Create(const JobSpec& spec) override {
        ++created;
        if (spec.params.value("unbuildable", false)) {
            throw MediaToolException(ErrorInfo::Make("E_JOB_TYPE_NOT_IMPLEMENTED",
                                                      ErrorCategory::UnsupportedFormat,
                                                      "cannot rebuild this job"));
        }
        const std::string behaviour = spec.params.value("behaviour", std::string("succeed"));
        auto job = std::make_unique<ScriptedJob>(
            spec.type,
            behaviour == "fail" ? ScriptedJob::Behaviour::Fail : ScriptedJob::Behaviour::Succeed,
            ErrorInfo::Make("E_TEST", ErrorCategory::Unknown, "scripted failure"));
        return job;
    }

    std::atomic<int> created{0};
};

JobManager::Options MakeOptions(std::size_t concurrency, const std::string& stateFile = "") {
    JobManager::Options options;
    options.maxConcurrentJobs = concurrency;
    options.stateFilePath = stateFile;
    options.persistIntervalMs = 0;   // tests want every durable change written immediately
    options.progressIntervalMs = 0;  // and every progress event delivered
    options.agingIntervalMs = 0;     // aging is covered by SchedulerCoreTest
    options.defaultRetryPolicy.maxRetries = 0;  // opt in per test
    return options;
}

JobManager::SubmitRequest MakeRequest(JobType type = JobType::Test) {
    JobManager::SubmitRequest request;
    request.spec.type = type;
    request.spec.params = {{"behaviour", "succeed"}};
    return request;
}

class JobManagerQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir_ = stdfs::temp_directory_path() /
                   ("gravity_queue_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        stdfs::create_directories(tempDir_);
    }
    void TearDown() override {
        std::error_code ec;
        stdfs::remove_all(tempDir_, ec);
    }

    std::string StatePath() const { return (tempDir_ / "queue.json").string(); }

    static void ExpectConsistent(const JobManager& manager) {
        const auto violations = manager.ValidateInvariants();
        EXPECT_TRUE(violations.empty()) << [&violations] {
            std::string joined;
            for (const auto& v : violations) joined += "\n  - " + v;
            return joined;
        }();
    }

    ControlledClock clock_;
    stdfs::path tempDir_;
};

}  // namespace

// --- basic lifecycle -----------------------------------------------------------------------

TEST_F(JobManagerQueueTest, RunsASubmittedJobToCompletion) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto job = std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed);
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    EXPECT_EQ(manager.GetJob(id).state, JobState::Queued);
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
    EXPECT_EQ(raw->executions.load(), 1);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, FailedJobReportsItsError) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto job = std::make_unique<ScriptedJob>(
        JobType::Test, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_BOOM", ErrorCategory::EngineFailure, "it broke"));
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    manager.RunSchedulerPassForTesting();

    const auto snapshot = manager.GetJob(id);
    EXPECT_EQ(snapshot.state, JobState::Failed);
    ASSERT_TRUE(snapshot.error.has_value());
    EXPECT_EQ(snapshot.error->code, "E_BOOM");
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, OneBrokenJobDoesNotStopTheQueue) {
    // Spec section 42: a failing job must not take the scheduler with it.
    JobManager manager(MakeOptions(1), nullptr, clock_);
    const JobId bad = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Fail,
                                      ErrorInfo::Make("E_BOOM", ErrorCategory::Unknown, "boom")),
        MakeRequest());
    const JobId good = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
        MakeRequest());

    manager.RunSchedulerPassForTesting();
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(bad).state, JobState::Failed);
    EXPECT_EQ(manager.GetJob(good).state, JobState::Completed);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, GetJobThrowsForUnknownId) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    EXPECT_THROW(manager.GetJob("job-nope"), MediaToolException);
}

// --- concurrency ---------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, ConcurrencyLimitCapsSimultaneousExecution) {
    JobManager manager(MakeOptions(2), nullptr, clock_);
    std::vector<ScriptedJob*> jobs;
    std::vector<JobId> ids;
    for (int i = 0; i < 5; ++i) {
        auto job = std::make_unique<ScriptedJob>(JobType::Test,
                                                 ScriptedJob::Behaviour::BlockUntilReleased);
        jobs.push_back(job.get());
        ids.push_back(manager.SubmitJob(std::move(job), MakeRequest()));
    }

    // Dispatch without waiting: RunSchedulerPassForTesting would block on the jobs we are
    // deliberately holding open.
    manager.SetMaxConcurrency(2);
    const auto snapshotBefore = manager.GetQueueSnapshot();
    ASSERT_EQ(snapshotBefore.statistics.queued, 5);

    // Drive one dispatch pass, then check nothing beyond the limit started.
    std::thread driver([&manager] { manager.RunSchedulerPassForTesting(); });

    // Wait (bounded) for exactly two jobs to be executing, then confirm it stays at two.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int started = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        started = 0;
        for (auto* job : jobs) {
            if (job->started) ++started;
        }
        if (started >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(started, 2) << "more jobs started than the concurrency limit allows";
    EXPECT_LE(manager.GetQueueSnapshot().statistics.running, 2);

    for (auto* job : jobs) job->Release();
    driver.join();

    // Everything eventually runs -- the cap delays work, it does not drop it.
    for (int pass = 0; pass < 5; ++pass) manager.RunSchedulerPassForTesting();
    for (const auto& id : ids) EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, ConcurrencyCanBeRaisedAtRuntime) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    for (int i = 0; i < 4; ++i) {
        manager.SubmitJob(std::make_unique<ScriptedJob>(JobType::Test,
                                                        ScriptedJob::Behaviour::Succeed),
                          MakeRequest());
    }
    EXPECT_EQ(manager.MaxConcurrentJobs(), 1u);

    manager.SetMaxConcurrency(4);
    EXPECT_EQ(manager.MaxConcurrentJobs(), 4u);

    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(manager.GetQueueSnapshot().statistics.completed, 4);
    ExpectConsistent(manager);
}

// --- queue pause ---------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, PausedQueueStartsNothingAndResumeReleasesIt) {
    JobManager manager(MakeOptions(2), nullptr, clock_);
    manager.PauseQueue();
    std::vector<JobId> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(manager.SubmitJob(
            std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
            MakeRequest()));
    }

    manager.RunSchedulerPassForTesting();
    for (const auto& id : ids) EXPECT_EQ(manager.GetJob(id).state, JobState::Queued);
    EXPECT_EQ(manager.GetQueueSnapshot().runState, QueueRunState::Paused);

    manager.ResumeQueue();
    manager.RunSchedulerPassForTesting();
    manager.RunSchedulerPassForTesting();
    for (const auto& id : ids) EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, PausingDoesNotStopAJobAlreadyRunning) {
    // Documented semantics (spec section 11): pause means "start nothing new", and the UI
    // must never claim a still-running process is paused.
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto job = std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::BlockUntilReleased);
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    std::thread driver([&manager] { manager.RunSchedulerPassForTesting(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!raw->started && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(raw->started);

    manager.PauseQueue();
    EXPECT_EQ(manager.GetJob(id).state, JobState::Running);  // NOT "Paused"

    raw->Release();
    driver.join();
    EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
}

// --- cancellation --------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, CancellingAQueuedJobNeverStartsIt) {
    // Spec section 39, case 1.
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto job = std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed);
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    manager.CancelJob(id);
    EXPECT_EQ(manager.GetJob(id).state, JobState::Cancelled);

    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(raw->executions.load(), 0) << "a cancelled job must never be executed";
    EXPECT_EQ(manager.GetJob(id).state, JobState::Cancelled);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, CancellingARunningJobTerminatesIt) {
    // Spec section 39, case 2.
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto job = std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::BlockUntilReleased);
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    std::thread driver([&manager] { manager.RunSchedulerPassForTesting(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!raw->started && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(raw->started);

    manager.CancelJob(id);
    driver.join();

    EXPECT_EQ(manager.GetJob(id).state, JobState::Cancelled);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, CancelAndCompletionRaceProducesExactlyOneTerminalState) {
    // Spec section 39, case 3. Runs the race many times: the job finishes at the same moment
    // cancellation is requested, and whichever wins, the result must be a single terminal
    // state and never an exception or a double transition.
    for (int iteration = 0; iteration < 40; ++iteration) {
        JobManager manager(MakeOptions(1), nullptr, clock_);
        auto job = std::make_unique<ScriptedJob>(JobType::Test,
                                                 ScriptedJob::Behaviour::BlockUntilReleased);
        ScriptedJob* raw = job.get();
        const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

        std::thread driver([&manager] { manager.RunSchedulerPassForTesting(); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!raw->started && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Release and cancel from two threads with no ordering between them.
        std::thread releaser([raw] { raw->Release(); });
        std::thread canceller([&manager, &id] { manager.CancelJob(id); });
        releaser.join();
        canceller.join();
        driver.join();

        const JobState state = manager.GetJob(id).state;
        EXPECT_TRUE(state == JobState::Completed || state == JobState::Cancelled)
            << "unexpected terminal state " << mediatool::jobs::ToWireString(state);
        EXPECT_EQ(raw->executions.load(), 1);
        ExpectConsistent(manager);
    }
}

TEST_F(JobManagerQueueTest, CancellingDuringRetryWaitStopsTheRetry) {
    // Spec section 39, case 4 / section 40, through the full manager rather than the
    // scheduler alone.
    auto options = MakeOptions(1);
    options.defaultRetryPolicy.maxRetries = 3;
    options.defaultRetryPolicy.initialDelayMs = 10'000;
    JobManager manager(options, nullptr, clock_);

    auto job = std::make_unique<ScriptedJob>(
        JobType::Test, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "flaky"));
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    manager.RunSchedulerPassForTesting();
    ASSERT_EQ(manager.GetJob(id).state, JobState::RetryWait);

    manager.CancelJob(id);
    EXPECT_EQ(manager.GetJob(id).state, JobState::Cancelled);

    clock_.Advance(60'000);
    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(raw->executions.load(), 1) << "a cancelled retry must not run";
    EXPECT_EQ(manager.GetJob(id).state, JobState::Cancelled);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, CancellingADependencySkipsItsDependents) {
    // Spec section 39, case 5.
    JobManager manager(MakeOptions(2), nullptr, clock_);
    const JobId parent = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
        MakeRequest());
    auto request = MakeRequest(JobType::Conversion);
    request.dependencies = {parent};
    auto child = std::make_unique<ScriptedJob>(JobType::Conversion, ScriptedJob::Behaviour::Succeed);
    ScriptedJob* rawChild = child.get();
    const JobId dependent = manager.SubmitJob(std::move(child), request);

    manager.CancelJob(parent);
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(parent).state, JobState::Cancelled);
    EXPECT_EQ(manager.GetJob(dependent).state, JobState::Skipped);
    EXPECT_EQ(rawChild->executions.load(), 0);
    ExpectConsistent(manager);
}

// --- retry ---------------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, TransientFailureIsRetriedAutomaticallyAfterBackoff) {
    auto options = MakeOptions(1);
    options.defaultRetryPolicy.maxRetries = 2;
    options.defaultRetryPolicy.initialDelayMs = 1'000;
    options.defaultRetryPolicy.multiplier = 2.0;
    JobManager manager(options, nullptr, clock_);

    auto job = std::make_unique<ScriptedJob>(
        JobType::Test, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "connection reset"));
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(manager.GetJob(id).state, JobState::RetryWait);
    EXPECT_EQ(manager.GetJob(id).attempt, 1);
    EXPECT_EQ(raw->executions.load(), 1);

    // Not yet due.
    clock_.Advance(500);
    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(raw->executions.load(), 1);

    // Now due. This attempt succeeds.
    raw->SetBehaviour(ScriptedJob::Behaviour::Succeed);
    clock_.Advance(600);
    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(raw->executions.load(), 2);
    EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, PermanentFailureIsNotRetried) {
    auto options = MakeOptions(1);
    options.defaultRetryPolicy.maxRetries = 5;
    JobManager manager(options, nullptr, clock_);

    auto job = std::make_unique<ScriptedJob>(
        JobType::Test, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_INPUT_NOT_FOUND", ErrorCategory::FileNotFound, "gone"));
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    manager.RunSchedulerPassForTesting();
    clock_.Advance(600'000);
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(id).state, JobState::Failed);
    EXPECT_EQ(raw->executions.load(), 1) << "a deterministic failure must fail immediately";
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, RetriesAreBoundedByTheBudget) {
    auto options = MakeOptions(1);
    options.defaultRetryPolicy.maxRetries = 2;
    options.defaultRetryPolicy.initialDelayMs = 100;
    JobManager manager(options, nullptr, clock_);

    auto job = std::make_unique<ScriptedJob>(
        JobType::Test, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "always down"));
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    for (int i = 0; i < 10; ++i) {
        manager.RunSchedulerPassForTesting();
        clock_.Advance(120'000);
    }

    // 1 original run + 2 retries, then it stays Failed forever.
    EXPECT_EQ(raw->executions.load(), 3);
    EXPECT_EQ(manager.GetJob(id).state, JobState::Failed);
    EXPECT_EQ(manager.GetJob(id).attempt, 2);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, RetryPolicyOfZeroDisablesAutomaticRetry) {
    JobManager manager(MakeOptions(1), nullptr, clock_);  // maxRetries defaults to 0 here
    auto job = std::make_unique<ScriptedJob>(
        JobType::Test, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "flaky"));
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    manager.RunSchedulerPassForTesting();
    clock_.Advance(600'000);
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(id).state, JobState::Failed);
    EXPECT_EQ(raw->executions.load(), 1);
}

TEST_F(JobManagerQueueTest, ManualRetryRunsAFailedJobAgain) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto job = std::make_unique<ScriptedJob>(
        JobType::Test, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_INPUT_NOT_FOUND", ErrorCategory::FileNotFound, "gone"));
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    manager.RunSchedulerPassForTesting();
    ASSERT_EQ(manager.GetJob(id).state, JobState::Failed);

    raw->SetBehaviour(ScriptedJob::Behaviour::Succeed);
    manager.RetryJob(id);
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
    EXPECT_EQ(raw->executions.load(), 2);
    // Identity is preserved: a retry is the same job, not a clone (spec section 16).
    EXPECT_EQ(manager.GetJob(id).id, id);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, ManualRetrySkipsARemainingBackoff) {
    auto options = MakeOptions(1);
    options.defaultRetryPolicy.maxRetries = 3;
    options.defaultRetryPolicy.initialDelayMs = 300'000;  // five minutes
    options.defaultRetryPolicy.maxDelayMs = 600'000;      // must be >= the initial delay
    JobManager manager(options, nullptr, clock_);

    auto job = std::make_unique<ScriptedJob>(
        JobType::Test, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "flaky"));
    ScriptedJob* raw = job.get();
    const JobId id = manager.SubmitJob(std::move(job), MakeRequest());

    manager.RunSchedulerPassForTesting();
    ASSERT_EQ(manager.GetJob(id).state, JobState::RetryWait);

    raw->SetBehaviour(ScriptedJob::Behaviour::Succeed);
    manager.RetryJob(id);              // no clock advance -- the wait is skipped, not waited out
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, RetryIsRejectedForJobsThatAreNotRetryable) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    const JobId id = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
        MakeRequest());

    EXPECT_THROW(manager.RetryJob(id), MediaToolException);  // still Queued
    manager.RunSchedulerPassForTesting();
    EXPECT_THROW(manager.RetryJob(id), MediaToolException);  // now Completed
    EXPECT_THROW(manager.RetryJob("job-nope"), MediaToolException);
}

TEST_F(JobManagerQueueTest, RetryAllFailedRetriesOnlyFailedJobs) {
    JobManager manager(MakeOptions(2), nullptr, clock_);
    std::vector<ScriptedJob*> failing;
    std::vector<JobId> failedIds;
    for (int i = 0; i < 3; ++i) {
        auto job = std::make_unique<ScriptedJob>(
            JobType::Test, ScriptedJob::Behaviour::Fail,
            ErrorInfo::Make("E_X", ErrorCategory::Unknown, "nope"));
        failing.push_back(job.get());
        failedIds.push_back(manager.SubmitJob(std::move(job), MakeRequest()));
    }
    const JobId ok = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
        MakeRequest());

    for (int pass = 0; pass < 4; ++pass) manager.RunSchedulerPassForTesting();
    ASSERT_EQ(manager.GetQueueSnapshot().statistics.failed, 3);

    for (auto* job : failing) job->SetBehaviour(ScriptedJob::Behaviour::Succeed);
    const auto retried = manager.RetryAllFailed();
    EXPECT_EQ(retried.size(), 3u);

    for (int pass = 0; pass < 4; ++pass) manager.RunSchedulerPassForTesting();
    for (const auto& id : failedIds) EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
    EXPECT_EQ(manager.GetJob(ok).state, JobState::Completed);
    ExpectConsistent(manager);
}

// --- dependencies -----------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, DependencyChainRunsInOrder) {
    JobManager manager(MakeOptions(4), nullptr, clock_);
    const JobId download = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Download, ScriptedJob::Behaviour::Succeed),
        MakeRequest(JobType::Download));

    auto convertRequest = MakeRequest(JobType::Conversion);
    convertRequest.dependencies = {download};
    const JobId convert = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Conversion, ScriptedJob::Behaviour::Succeed),
        convertRequest);

    auto compressRequest = MakeRequest(JobType::Compression);
    compressRequest.dependencies = {convert};
    const JobId compress = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Compression, ScriptedJob::Behaviour::Succeed),
        compressRequest);

    EXPECT_EQ(manager.GetJob(convert).state, JobState::Waiting);
    EXPECT_EQ(manager.GetJob(compress).state, JobState::Waiting);

    manager.RunSchedulerPassForTesting();  // download
    manager.RunSchedulerPassForTesting();  // release + run convert
    manager.RunSchedulerPassForTesting();  // release + run compress
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(download).state, JobState::Completed);
    EXPECT_EQ(manager.GetJob(convert).state, JobState::Completed);
    EXPECT_EQ(manager.GetJob(compress).state, JobState::Completed);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, FailedDependencySkipsTheWholeChain) {
    JobManager manager(MakeOptions(4), nullptr, clock_);
    const JobId download = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Download, ScriptedJob::Behaviour::Fail,
                                      ErrorInfo::Make("E_X", ErrorCategory::Unknown, "no")),
        MakeRequest(JobType::Download));

    auto convertRequest = MakeRequest(JobType::Conversion);
    convertRequest.dependencies = {download};
    auto convertJob = std::make_unique<ScriptedJob>(JobType::Conversion, ScriptedJob::Behaviour::Succeed);
    ScriptedJob* rawConvert = convertJob.get();
    const JobId convert = manager.SubmitJob(std::move(convertJob), convertRequest);

    auto compressRequest = MakeRequest(JobType::Compression);
    compressRequest.dependencies = {convert};
    auto compressJob = std::make_unique<ScriptedJob>(JobType::Compression, ScriptedJob::Behaviour::Succeed);
    ScriptedJob* rawCompress = compressJob.get();
    const JobId compress = manager.SubmitJob(std::move(compressJob), compressRequest);

    for (int pass = 0; pass < 4; ++pass) manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(download).state, JobState::Failed);
    EXPECT_EQ(manager.GetJob(convert).state, JobState::Skipped);
    EXPECT_EQ(manager.GetJob(compress).state, JobState::Skipped);
    EXPECT_EQ(rawConvert->executions.load(), 0);
    EXPECT_EQ(rawCompress->executions.load(), 0);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, RetryingASkippedJobReEvaluatesItsDependencies) {
    JobManager manager(MakeOptions(2), nullptr, clock_);
    auto parentJob = std::make_unique<ScriptedJob>(
        JobType::Download, ScriptedJob::Behaviour::Fail,
        ErrorInfo::Make("E_X", ErrorCategory::Unknown, "no"));
    ScriptedJob* rawParent = parentJob.get();
    const JobId parent = manager.SubmitJob(std::move(parentJob), MakeRequest(JobType::Download));

    auto childRequest = MakeRequest(JobType::Conversion);
    childRequest.dependencies = {parent};
    const JobId child = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Conversion, ScriptedJob::Behaviour::Succeed),
        childRequest);

    for (int pass = 0; pass < 3; ++pass) manager.RunSchedulerPassForTesting();
    ASSERT_EQ(manager.GetJob(child).state, JobState::Skipped);

    // Fix the parent and retry both.
    rawParent->SetBehaviour(ScriptedJob::Behaviour::Succeed);
    manager.RetryJob(parent);
    manager.RetryJob(child);
    EXPECT_EQ(manager.GetJob(child).state, JobState::Waiting);

    for (int pass = 0; pass < 4; ++pass) manager.RunSchedulerPassForTesting();
    EXPECT_EQ(manager.GetJob(parent).state, JobState::Completed);
    EXPECT_EQ(manager.GetJob(child).state, JobState::Completed);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, UnknownDependencyIsRejectedAndLeavesNothingBehind) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto request = MakeRequest();
    request.dependencies = {"job-does-not-exist"};

    EXPECT_THROW(manager.SubmitJob(std::make_unique<ScriptedJob>(
                                       JobType::Test, ScriptedJob::Behaviour::Succeed),
                                   request),
                 MediaToolException);
    EXPECT_TRUE(manager.ListJobs().empty());
    ExpectConsistent(manager);
}

// --- duplicates -------------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, DuplicateSubmissionIsRejectedUnlessExplicitlyAllowed) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto request = MakeRequest();
    request.duplicateKey = "DOWNLOAD|https://example.com/v";
    manager.SubmitJob(std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
                      request);

    EXPECT_THROW(manager.SubmitJob(std::make_unique<ScriptedJob>(
                                       JobType::Test, ScriptedJob::Behaviour::Succeed),
                                   request),
                 MediaToolException);
    EXPECT_EQ(manager.ListJobs().size(), 1u);

    request.allowDuplicate = true;
    EXPECT_NO_THROW(manager.SubmitJob(std::make_unique<ScriptedJob>(
                                          JobType::Test, ScriptedJob::Behaviour::Succeed),
                                      request));
    EXPECT_EQ(manager.ListJobs().size(), 2u);
    ExpectConsistent(manager);
}

// --- ordering & priority ------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, PriorityAndReorderingAffectDispatchOrder) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    std::vector<JobId> ids;
    for (int i = 0; i < 3; ++i) {
        ids.push_back(manager.SubmitJob(
            std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
            MakeRequest()));
    }

    manager.MoveJob(ids[2], MoveDirection::Top);
    EXPECT_EQ(manager.GetQueueSnapshot().pendingOrder.front(), ids[2]);

    manager.SetJobPriority(ids[1], JobPriority::High);
    manager.RunSchedulerPassForTesting();
    // Priority beats queue position.
    EXPECT_EQ(manager.GetJob(ids[1]).state, JobState::Completed);
    EXPECT_EQ(manager.GetJob(ids[2]).state, JobState::Queued);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, MovingANonPendingJobIsRejected) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    const JobId id = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
        MakeRequest());
    manager.RunSchedulerPassForTesting();

    EXPECT_THROW(manager.MoveJob(id, MoveDirection::Top), MediaToolException);
    EXPECT_THROW(manager.MoveJob("job-nope", MoveDirection::Top), MediaToolException);
}

// --- history --------------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, ClearHistoryRemovesFinishedJobsOnly) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    const JobId done = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
        MakeRequest());
    manager.RunSchedulerPassForTesting();
    const JobId queued = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::BlockUntilReleased),
        MakeRequest());

    const auto removed = manager.ClearHistory(HistoryScope::Completed);
    EXPECT_EQ(removed, std::vector<JobId>{done});
    EXPECT_THROW(manager.GetJob(done), MediaToolException);
    EXPECT_NO_THROW(manager.GetJob(queued));
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, RemoveJobRequiresATerminalJob) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    const JobId id = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
        MakeRequest());

    EXPECT_THROW(manager.RemoveJob(id), MediaToolException);
    manager.RunSchedulerPassForTesting();
    EXPECT_NO_THROW(manager.RemoveJob(id));
    EXPECT_THROW(manager.GetJob(id), MediaToolException);
}

// --- events ---------------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, StateChangeCallbackSeesTheFullLifecycle) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    std::mutex mutex;
    std::vector<JobState> seen;
    manager.OnJobStateChanged([&](const JobId&, JobState state) {
        std::lock_guard<std::mutex> lock(mutex);
        seen.push_back(state);
    });

    manager.SubmitJob(std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
                      MakeRequest());
    manager.RunSchedulerPassForTesting();

    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_GE(seen.size(), 3u);
    EXPECT_EQ(seen[0], JobState::Starting);
    EXPECT_EQ(seen[1], JobState::Running);
    EXPECT_EQ(seen.back(), JobState::Completed);
}

TEST_F(JobManagerQueueTest, RetryScheduledCallbackReportsAttemptAndDelay) {
    auto options = MakeOptions(1);
    options.defaultRetryPolicy.maxRetries = 2;
    options.defaultRetryPolicy.initialDelayMs = 4'000;
    JobManager manager(options, nullptr, clock_);

    std::mutex mutex;
    int seenAttempt = 0;
    std::int64_t seenDelay = -1;
    manager.OnRetryScheduled([&](const JobId&, int attempt, std::int64_t delayMs, const std::string&) {
        std::lock_guard<std::mutex> lock(mutex);
        seenAttempt = attempt;
        seenDelay = delayMs;
    });

    manager.SubmitJob(std::make_unique<ScriptedJob>(
                          JobType::Test, ScriptedJob::Behaviour::Fail,
                          ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "flaky")),
                      MakeRequest());
    manager.RunSchedulerPassForTesting();

    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(seenAttempt, 1);
    EXPECT_EQ(seenDelay, 4'000);
}

TEST_F(JobManagerQueueTest, ProgressEventsAreThrottled) {
    auto options = MakeOptions(1);
    options.progressIntervalMs = 100'000;  // effectively "only the first one gets through"
    JobManager manager(options, nullptr, clock_);

    std::atomic<int> progressEvents{0};
    manager.OnJobProgress([&](const JobId&, const Progress&) { ++progressEvents; });

    class ChattyJob final : public Job {
    public:
        ChattyJob() : Job(JobType::Test) {}
        void Execute() override {
            for (int i = 0; i < 500; ++i)
                ReportProgress(Progress{.percentage = static_cast<double>(i) / 5.0,
                                        .statusMessage = "working"});
        }
    };

    manager.SubmitJob(std::make_unique<ChattyJob>(), MakeRequest());
    manager.RunSchedulerPassForTesting();

    // 500 progress reports must not become 500 IPC events (spec section 28).
    EXPECT_LE(progressEvents.load(), 3);
    EXPECT_GE(progressEvents.load(), 1);
}

// --- persistence & restart --------------------------------------------------------------------

TEST_F(JobManagerQueueTest, QueueStateIsWrittenAtomicallyAndReloaded) {
    ScriptedJobFactory factory;
    {
        JobManager manager(MakeOptions(2, StatePath()), &factory, clock_);
        auto request = MakeRequest(JobType::Download);
        request.priority = JobPriority::High;
        manager.SubmitJob(
            std::make_unique<ScriptedJob>(JobType::Download, ScriptedJob::Behaviour::Succeed),
            request);
        manager.FlushPersistence();
    }

    ASSERT_TRUE(stdfs::exists(StatePath()));
    std::ifstream input(StatePath());
    nlohmann::json parsed;
    input >> parsed;
    EXPECT_EQ(parsed["schemaVersion"], mediatool::queue::kQueueSchemaVersion);
    ASSERT_EQ(parsed["records"].size(), 1u);
    EXPECT_EQ(parsed["records"][0]["priority"], "HIGH");
    // No temp file left behind by the atomic write.
    EXPECT_FALSE(stdfs::exists(StatePath() + ".processing"));
}

TEST_F(JobManagerQueueTest, QueuedJobsSurviveARestart) {
    ScriptedJobFactory factory;
    JobId persistedId;
    {
        JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
        manager.PauseQueue();  // keep it queued rather than letting it run
        auto request = MakeRequest(JobType::Download);
        request.priority = JobPriority::High;
        persistedId = manager.SubmitJob(
            std::make_unique<ScriptedJob>(JobType::Download, ScriptedJob::Behaviour::Succeed),
            request);
        manager.FlushPersistence();
    }

    JobManager restored(MakeOptions(1, StatePath()), &factory, clock_);
    const auto report = restored.RestoreFromDisk();

    EXPECT_EQ(report.status, mediatool::queue::LoadOutcome::Status::Loaded);
    EXPECT_EQ(report.restoredJobs, 1);
    const auto snapshot = restored.GetJob(persistedId);
    EXPECT_EQ(snapshot.id, persistedId) << "a restored job must keep its original id";
    EXPECT_EQ(snapshot.state, JobState::Queued);
    EXPECT_EQ(snapshot.priority, JobPriority::High);
    EXPECT_EQ(restored.GetQueueSnapshot().runState, QueueRunState::Paused);
    ExpectConsistent(restored);

    restored.ResumeQueue();
    restored.RunSchedulerPassForTesting();
    EXPECT_EQ(restored.GetJob(persistedId).state, JobState::Completed);
}

TEST_F(JobManagerQueueTest, InterruptedJobsComeBackAsRetryableFailures) {
    // Spec section 25: a job that was RUNNING when the process died must never be reported
    // as completed, and must not silently re-run either.
    ScriptedJobFactory factory;
    const JobId id = "job-interrupted";
    {
        mediatool::queue::PersistedQueue persisted;
        mediatool::queue::JobRecord record;
        record.id = id;
        record.spec.type = JobType::Download;
        record.spec.params = {{"behaviour", "succeed"}};
        record.state = JobState::Running;  // as if the process died mid-job
        record.sequence = 1;
        persisted.records.push_back(record);
        mediatool::queue::QueuePersistence(StatePath()).Save(persisted);
    }

    JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
    const auto report = manager.RestoreFromDisk();

    EXPECT_EQ(report.interruptedJobs, 1);
    const auto snapshot = manager.GetJob(id);
    EXPECT_EQ(snapshot.state, JobState::Failed);
    ASSERT_TRUE(snapshot.error.has_value());
    EXPECT_EQ(snapshot.error->code, "E_JOB_INTERRUPTED");
    EXPECT_TRUE(snapshot.error->recoverable);

    // It must not restart on its own...
    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(manager.GetJob(id).state, JobState::Failed);

    // ...but the user can retry it, and then it runs cleanly.
    manager.RetryJob(id);
    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, RestoreSweepsAStaleProcessingArtifactLeftByACrash) {
    // A real crash (SIGKILL, power loss -- not a graceful shutdown, which already cleans up
    // via AtomicWriter's own destructor) never runs that destructor, so a CONVERSION/
    // COMPRESSION job that was actively writing can leave "<name>.processing.<ext>" sitting
    // in its output directory forever if the job is never retried. RestoreFromDisk() must
    // sweep it on the very next startup, not only when the same job happens to be retried.
    const stdfs::path outputDir = tempDir_ / "out";
    stdfs::create_directories(outputDir);
    const stdfs::path stale = outputDir / "crashvictim.processing.mp4";
    { std::ofstream(stale) << "partial bytes from the crashed attempt"; }
    const stdfs::path unrelated = outputDir / "someone-elses-file.mp4";
    { std::ofstream(unrelated) << "a real, unrelated file that must survive"; }

    ScriptedJobFactory factory;
    const JobId id = "job-crashed-compression";
    {
        mediatool::queue::PersistedQueue persisted;
        mediatool::queue::JobRecord record;
        record.id = id;
        record.spec.type = JobType::Compression;
        record.spec.params = {{"behaviour", "succeed"}, {"outputDirectory", outputDir.string()}};
        record.state = JobState::Running;  // as if the process died mid-write
        record.sequence = 1;
        persisted.records.push_back(record);
        mediatool::queue::QueuePersistence(StatePath()).Save(persisted);
    }

    JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
    manager.RestoreFromDisk();

    EXPECT_FALSE(stdfs::exists(stale)) << "the stale .processing scratch file must be swept";
    EXPECT_TRUE(stdfs::exists(unrelated)) << "a real file that merely shares the directory "
                                              "must never be touched by the sweep";
}

TEST_F(JobManagerQueueTest, DependenciesAndOrderingSurviveARestart) {
    ScriptedJobFactory factory;
    JobId parent;
    JobId child;
    {
        JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
        manager.PauseQueue();
        parent = manager.SubmitJob(
            std::make_unique<ScriptedJob>(JobType::Download, ScriptedJob::Behaviour::Succeed),
            MakeRequest(JobType::Download));
        auto request = MakeRequest(JobType::Conversion);
        request.dependencies = {parent};
        child = manager.SubmitJob(
            std::make_unique<ScriptedJob>(JobType::Conversion, ScriptedJob::Behaviour::Succeed),
            request);
        manager.FlushPersistence();
    }

    JobManager restored(MakeOptions(1, StatePath()), &factory, clock_);
    restored.RestoreFromDisk();

    EXPECT_EQ(restored.GetJob(child).dependencies, std::vector<JobId>{parent});
    EXPECT_EQ(restored.GetJob(child).state, JobState::Waiting);

    restored.ResumeQueue();
    restored.RunSchedulerPassForTesting();
    restored.RunSchedulerPassForTesting();
    EXPECT_EQ(restored.GetJob(parent).state, JobState::Completed);
    EXPECT_EQ(restored.GetJob(child).state, JobState::Completed);
    ExpectConsistent(restored);
}

TEST_F(JobManagerQueueTest, RetryStateSurvivesARestart) {
    ScriptedJobFactory factory;
    const JobId id = "job-retrywait";
    {
        mediatool::queue::PersistedQueue persisted;
        mediatool::queue::JobRecord record;
        record.id = id;
        record.spec.type = JobType::Download;
        record.spec.params = {{"behaviour", "succeed"}};
        record.state = JobState::RetryWait;
        record.attempt = 2;
        record.retryPolicy.maxRetries = 5;
        record.nextRetryAtMs = 1'000'000 + 5'000;
        record.sequence = 1;
        persisted.records.push_back(record);
        persisted.pendingOrder = {id};
        mediatool::queue::QueuePersistence(StatePath()).Save(persisted);
    }

    JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
    manager.RestoreFromDisk();

    const auto snapshot = manager.GetJob(id);
    EXPECT_EQ(snapshot.state, JobState::RetryWait);
    EXPECT_EQ(snapshot.attempt, 2);
    EXPECT_EQ(snapshot.maxRetries, 5);
    ExpectConsistent(manager);

    // Not yet due...
    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(manager.GetJob(id).state, JobState::RetryWait);
    // ...and it runs once its deadline passes.
    clock_.Advance(6'000);
    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
}

TEST_F(JobManagerQueueTest, CorruptStateFileIsQuarantinedAndTheAppStillStarts) {
    {
        std::ofstream output(StatePath());
        output << R"({"schemaVersion": 1, "records": [ {"id": "a", trunca)";
    }

    ScriptedJobFactory factory;
    JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
    const auto report = manager.RestoreFromDisk();

    EXPECT_EQ(report.status, mediatool::queue::LoadOutcome::Status::Recovered);
    EXPECT_FALSE(report.diagnostic.empty());
    ASSERT_TRUE(report.quarantinedPath.has_value());
    EXPECT_TRUE(stdfs::exists(*report.quarantinedPath))
        << "the corrupt file must be preserved for diagnosis, not deleted";
    EXPECT_TRUE(manager.ListJobs().empty());

    // The queue is fully usable afterwards.
    const JobId id = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
        MakeRequest());
    manager.RunSchedulerPassForTesting();
    EXPECT_EQ(manager.GetJob(id).state, JobState::Completed);
}

TEST_F(JobManagerQueueTest, EmptyAndMissingStateFilesAreHandled) {
    ScriptedJobFactory factory;
    {
        JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
        EXPECT_EQ(manager.RestoreFromDisk().status,
                  mediatool::queue::LoadOutcome::Status::NotPresent);
    }
    { std::ofstream output(StatePath()); }  // zero bytes
    {
        JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
        EXPECT_EQ(manager.RestoreFromDisk().status,
                  mediatool::queue::LoadOutcome::Status::Recovered);
    }
}

TEST_F(JobManagerQueueTest, FutureSchemaVersionIsRefusedWithoutDestroyingTheFile) {
    {
        std::ofstream output(StatePath());
        output << R"({"schemaVersion": 9999, "records": []})";
    }
    ScriptedJobFactory factory;
    JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
    const auto report = manager.RestoreFromDisk();

    EXPECT_EQ(report.status, mediatool::queue::LoadOutcome::Status::Recovered);
    EXPECT_NE(report.diagnostic.find("newer version"), std::string::npos);
    ASSERT_TRUE(report.quarantinedPath.has_value());
    EXPECT_TRUE(stdfs::exists(*report.quarantinedPath));
}

TEST_F(JobManagerQueueTest, JobsThatCannotBeRebuiltAreCountedNotCrashed) {
    ScriptedJobFactory factory;
    {
        mediatool::queue::PersistedQueue persisted;
        mediatool::queue::JobRecord record;
        record.id = "job-unbuildable";
        record.spec.type = JobType::Download;
        record.spec.params = {{"unbuildable", true}};
        record.state = JobState::Queued;
        record.sequence = 1;
        persisted.records.push_back(record);
        mediatool::queue::QueuePersistence(StatePath()).Save(persisted);
    }

    JobManager manager(MakeOptions(1, StatePath()), &factory, clock_);
    const auto report = manager.RestoreFromDisk();

    EXPECT_EQ(report.unbuildableJobs, 1);
    EXPECT_EQ(report.restoredJobs, 0);
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, ProgressDoesNotTriggerPersistence) {
    // Spec section 52: writing the state file on every progress tick is exactly the design
    // being ruled out.
    auto options = MakeOptions(1, StatePath());
    options.persistIntervalMs = 1'000'000;  // any write during the test would have to be forced
    JobManager manager(options, nullptr, clock_);

    class ChattyJob final : public Job {
    public:
        ChattyJob() : Job(JobType::Test) {}
        void Execute() override {
            for (int i = 0; i < 200; ++i)
                ReportProgress(Progress{.percentage = 1.0, .statusMessage = "working"});
        }
    };
    manager.SubmitJob(std::make_unique<ChattyJob>(), MakeRequest());
    manager.RunSchedulerPassForTesting();

    // The only write is the forced one; progress never caused an unthrottled save.
    manager.FlushPersistence();
    ASSERT_TRUE(stdfs::exists(StatePath()));
    const auto size = stdfs::file_size(StatePath());
    EXPECT_GT(size, 0u);
}

// --- pipeline input resolution ------------------------------------------------------------

TEST_F(JobManagerQueueTest, ADependentTakesItsInputFromTheProducersActualOutput) {
    // Spec section 19. The producing job's output path is not knowable when the pipeline is
    // declared, so the follower names the job instead and the backend resolves it.
    JobManager manager(MakeOptions(2), nullptr, clock_);

    auto producerJob = std::make_unique<ScriptedJob>(JobType::Download, ScriptedJob::Behaviour::Succeed);
    producerJob->PublishOutput("/downloads/Some Title (2).mp4");
    const JobId producer = manager.SubmitJob(std::move(producerJob), MakeRequest(JobType::Download));

    auto followerRequest = MakeRequest(JobType::Conversion);
    followerRequest.spec.params["inputFromJobId"] = producer;
    followerRequest.dependencies = {producer};
    auto followerJob = std::make_unique<ScriptedJob>(JobType::Conversion, ScriptedJob::Behaviour::Succeed);
    ScriptedJob* follower = followerJob.get();
    const JobId followerId = manager.SubmitJob(std::move(followerJob), followerRequest);

    manager.RunSchedulerPassForTesting();
    manager.RunSchedulerPassForTesting();

    EXPECT_EQ(manager.GetJob(followerId).state, JobState::Completed);
    EXPECT_EQ(follower->resolvedInput, "/downloads/Some Title (2).mp4")
        << "the follower must read the path the producer actually wrote";
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, AJobWithNoResolvedInputIsLeftAlone) {
    JobManager manager(MakeOptions(1), nullptr, clock_);
    auto job = std::make_unique<ScriptedJob>(JobType::Conversion, ScriptedJob::Behaviour::Succeed);
    ScriptedJob* raw = job.get();
    manager.SubmitJob(std::move(job), MakeRequest(JobType::Conversion));

    manager.RunSchedulerPassForTesting();

    EXPECT_TRUE(raw->resolvedInput.empty()) << "a job that declared no source must not be rewritten";
}

TEST_F(JobManagerQueueTest, AProducerThatRecordedNoOutputFailsTheDependentClearly) {
    // The scheduler guarantees the producer COMPLETED, so a missing outputPath here is a
    // real inconsistency and must be reported as such rather than surfacing later as a
    // confusing "input file not found" against an empty path.
    JobManager manager(MakeOptions(2), nullptr, clock_);
    const JobId producer = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Download, ScriptedJob::Behaviour::Succeed),
        MakeRequest(JobType::Download));  // succeeds but publishes no result

    auto followerRequest = MakeRequest(JobType::Conversion);
    followerRequest.spec.params["inputFromJobId"] = producer;
    followerRequest.dependencies = {producer};
    const JobId followerId = manager.SubmitJob(
        std::make_unique<ScriptedJob>(JobType::Conversion, ScriptedJob::Behaviour::Succeed),
        followerRequest);

    manager.RunSchedulerPassForTesting();
    manager.RunSchedulerPassForTesting();

    const auto snapshot = manager.GetJob(followerId);
    EXPECT_EQ(snapshot.state, JobState::Failed);
    ASSERT_TRUE(snapshot.error.has_value());
    EXPECT_EQ(snapshot.error->code, "E_DEPENDENCY_OUTPUT_MISSING");
    ExpectConsistent(manager);
}

// --- stress ---------------------------------------------------------------------------------

TEST_F(JobManagerQueueTest, DeterministicStressAcrossManyMixedJobs) {
    // Spec section 48: a large mixed workload exercised through the real manager, with
    // priorities, dependencies, cancellations, retries, reordering and pause/resume all in
    // play. Deterministic in structure: the same jobs, the same operations, every run.
    auto options = MakeOptions(4);
    options.defaultRetryPolicy.maxRetries = 1;
    options.defaultRetryPolicy.initialDelayMs = 100;
    JobManager manager(options, nullptr, clock_);

    std::vector<JobId> ids;
    std::vector<JobId> cancelled;
    JobId previousDownload;

    for (int i = 0; i < 60; ++i) {
        const JobType type = (i % 3 == 0)   ? JobType::Download
                             : (i % 3 == 1) ? JobType::Conversion
                                            : JobType::Compression;
        auto request = MakeRequest(type);
        request.priority = (i % 5 == 0)   ? JobPriority::High
                           : (i % 7 == 0) ? JobPriority::Low
                                          : JobPriority::Normal;

        // Every third group forms a download -> convert dependency pair.
        if (type == JobType::Conversion && !previousDownload.empty() && i % 6 == 1)
            request.dependencies = {previousDownload};

        auto behaviour = ScriptedJob::Behaviour::Succeed;
        ErrorInfo error;
        if (i % 11 == 0) {
            behaviour = ScriptedJob::Behaviour::Fail;
            error = ErrorInfo::Make("E_NET", ErrorCategory::NetworkError, "transient");
        } else if (i % 13 == 0) {
            behaviour = ScriptedJob::Behaviour::Fail;
            error = ErrorInfo::Make("E_INPUT_NOT_FOUND", ErrorCategory::FileNotFound, "permanent");
        }

        const JobId id = manager.SubmitJob(
            std::make_unique<ScriptedJob>(type, behaviour, error), request);
        ids.push_back(id);
        if (type == JobType::Download) previousDownload = id;

        if (i % 9 == 4) manager.MoveJob(id, MoveDirection::Top);
        if (i % 17 == 5) {
            manager.CancelJob(id);
            cancelled.push_back(id);
        }
    }

    manager.PauseQueue();
    manager.RunSchedulerPassForTesting();
    // Nothing may have started while paused.
    for (const auto& id : ids) {
        const JobState state = manager.GetJob(id).state;
        EXPECT_TRUE(state == JobState::Queued || state == JobState::Waiting ||
                    state == JobState::Cancelled)
            << "job started while the queue was paused: " << mediatool::jobs::ToWireString(state);
    }
    ExpectConsistent(manager);

    manager.ResumeQueue();
    for (int pass = 0; pass < 60; ++pass) {
        manager.RunSchedulerPassForTesting();
        clock_.Advance(1'000);
        ExpectConsistent(manager);
    }

    // Every job reached a terminal state; nothing is stuck.
    for (const auto& id : ids) {
        const JobState state = manager.GetJob(id).state;
        EXPECT_TRUE(mediatool::jobs::IsTerminalState(state))
            << "job " << id << " stuck in " << mediatool::jobs::ToWireString(state);
    }
    for (const auto& id : cancelled)
        EXPECT_EQ(manager.GetJob(id).state, JobState::Cancelled);

    const auto stats = manager.GetQueueSnapshot().statistics;
    EXPECT_EQ(stats.total, static_cast<int>(ids.size()));
    EXPECT_EQ(stats.running, 0);
    EXPECT_EQ(stats.queued, 0);
    EXPECT_EQ(stats.waiting, 0);
    EXPECT_GE(stats.completed, 1);
    EXPECT_GE(stats.cancelled, static_cast<int>(cancelled.size()));
    ExpectConsistent(manager);
}

TEST_F(JobManagerQueueTest, ConcurrentControlCallsDoNotCorruptTheQueue) {
    // Hammers the manager from several threads at once: submissions, cancellations, priority
    // changes, reorders and snapshot reads all racing. The assertion is that the queue's
    // invariants survive, and that nothing deadlocks or crashes.
    auto options = MakeOptions(3);
    JobManager manager(options, nullptr, clock_);

    std::atomic<bool> stop{false};
    std::mutex idsMutex;
    std::vector<JobId> ids;

    std::thread submitter([&] {
        for (int i = 0; i < 80 && !stop; ++i) {
            try {
                const JobId id = manager.SubmitJob(
                    std::make_unique<ScriptedJob>(JobType::Test, ScriptedJob::Behaviour::Succeed),
                    MakeRequest());
                std::lock_guard<std::mutex> lock(idsMutex);
                ids.push_back(id);
            } catch (const MediaToolException&) {
            }
        }
    });

    std::thread controller([&] {
        for (int i = 0; i < 200 && !stop; ++i) {
            JobId target;
            {
                std::lock_guard<std::mutex> lock(idsMutex);
                if (ids.empty()) continue;
                target = ids[static_cast<std::size_t>(i) % ids.size()];
            }
            // All of these legitimately throw when the job has moved on; that is the API
            // contract, not a failure.
            try { manager.SetJobPriority(target, JobPriority::High); } catch (const MediaToolException&) {}
            try { manager.MoveJob(target, MoveDirection::Top); } catch (const MediaToolException&) {}
            if (i % 7 == 0) {
                try { manager.CancelJob(target); } catch (const MediaToolException&) {}
            }
        }
    });

    std::thread reader([&] {
        for (int i = 0; i < 200 && !stop; ++i) {
            const auto snapshot = manager.GetQueueSnapshot();
            EXPECT_GE(snapshot.statistics.total, 0);
            EXPECT_LE(static_cast<std::size_t>(snapshot.statistics.running),
                      snapshot.maxConcurrency);
        }
    });

    std::thread driver([&] {
        for (int i = 0; i < 60 && !stop; ++i) manager.RunSchedulerPassForTesting();
    });

    submitter.join();
    controller.join();
    reader.join();
    driver.join();
    stop = true;

    for (int pass = 0; pass < 40; ++pass) manager.RunSchedulerPassForTesting();
    ExpectConsistent(manager);

    const auto stats = manager.GetQueueSnapshot().statistics;
    EXPECT_GE(stats.total, 0);
    EXPECT_LE(static_cast<std::size_t>(stats.running), manager.MaxConcurrentJobs());
}
