#include "core/jobs/TestJob.h"

#include <chrono>
#include <sstream>
#include <thread>

#include "core/errors/MediaToolException.h"

namespace mediatool::jobs {

namespace {

[[noreturn]] void ThrowCancelled() {
    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_TEST_JOB_CANCELLED", errors::ErrorCategory::Cancelled, "Test job was cancelled"));
}

}  // namespace

TestJob::TestJob() : Job(JobType::Test) {}
TestJob::TestJob(common::IClock& clock) : Job(JobType::Test, clock) {}

void TestJob::Execute() {
    Progress starting;
    starting.percentage = 0.0;
    starting.statusMessage = "Test job starting";
    ReportProgress(starting);

    for (int step = 1; step <= kSteps; ++step) {
        if (IsCancellationRequested()) ThrowCancelled();
        if (!WaitWhilePaused()) ThrowCancelled();

        std::this_thread::sleep_for(std::chrono::milliseconds(kStepDelayMs));

        if (IsCancellationRequested()) ThrowCancelled();

        const double percentage = static_cast<double>(step) * (100.0 / kSteps);
        std::ostringstream status;
        status << "Test job step " << step << " of " << kSteps;

        Progress progress;
        progress.percentage = percentage;
        progress.currentItem = status.str();
        progress.statusMessage = status.str();
        ReportProgress(progress);
    }

    nlohmann::json result;
    result["message"] = "Test job completed successfully";
    result["steps"] = kSteps;
    SetResult(result);
}

}  // namespace mediatool::jobs
