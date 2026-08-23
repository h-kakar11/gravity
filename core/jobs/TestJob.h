#pragma once

// The Phase-1 "minimal developer/test interface proving the architecture works" (spec
// sections 33/42). TestJob touches neither the network nor the filesystem: it just
// sleeps in small increments while reporting progress, so it can exercise
// Job/JobManager/JobStateMachine/IPC end-to-end without any real engine or downloader
// being wired up yet.

#include "core/jobs/Job.h"

namespace mediatool::jobs {

class TestJob final : public Job {
public:
    TestJob();
    explicit TestJob(common::IClock& clock);

    bool SupportsPause() const override { return true; }

    void Execute() override;

private:
    static constexpr int kSteps = 10;
    static constexpr int kStepDelayMs = 100;
};

}  // namespace mediatool::jobs
