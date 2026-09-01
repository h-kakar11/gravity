#include "core/jobs/JobStateMachine.h"

#include <gtest/gtest.h>

#include <set>
#include <utility>
#include <vector>

using mediatool::jobs::CanTransition;
using mediatool::jobs::JobState;

namespace {

const std::vector<JobState>& AllStates() {
    static const std::vector<JobState> states = {
        JobState::Queued,  JobState::Starting, JobState::Running,  JobState::Paused,
        JobState::Completed, JobState::Failed, JobState::Cancelled, JobState::Retrying,
    };
    return states;
}

// The exact table from docs/ipc-contract.md / spec sections 4-6. This is the one
// authoritative copy of "which pairs are valid" that the exhaustive test below checks
// CanTransition() against for all 64 (from, to) combinations.
const std::set<std::pair<JobState, JobState>>& ValidTransitions() {
    static const std::set<std::pair<JobState, JobState>> valid = {
        {JobState::Queued, JobState::Starting},
        {JobState::Queued, JobState::Cancelled},
        {JobState::Starting, JobState::Running},
        {JobState::Starting, JobState::Failed},
        {JobState::Starting, JobState::Cancelled},
        {JobState::Running, JobState::Paused},
        {JobState::Running, JobState::Completed},
        {JobState::Running, JobState::Failed},
        {JobState::Running, JobState::Cancelled},
        {JobState::Paused, JobState::Running},
        {JobState::Paused, JobState::Cancelled},
        // RUNNING -> RETRYING is the AUTOMATIC retry path (core/jobs/RetryPolicy.h): an
        // attempt failed and the next one is already scheduled, so the job never becomes
        // terminal. Routing it through FAILED instead would cancel every job depending on
        // it and write a failure into Session History for an attempt about to be repeated.
        {JobState::Running, JobState::Retrying},
        // FAILED -> RETRYING remains the MANUAL path: a user pressing Retry on a job that
        // already gave up.
        {JobState::Failed, JobState::Retrying},
        {JobState::Retrying, JobState::Running},
        {JobState::Retrying, JobState::Failed},
        // A job sitting out a backoff is exactly when a user gives up on it; it must not
        // have to wait out the timer to be cancellable.
        {JobState::Retrying, JobState::Cancelled},
    };
    return valid;
}

}  // namespace

TEST(JobStateMachine, ExhaustiveTransitionTable) {
    const auto& valid = ValidTransitions();
    for (JobState from : AllStates()) {
        for (JobState to : AllStates()) {
            const bool expected = valid.count({from, to}) > 0;
            EXPECT_EQ(CanTransition(from, to), expected)
                << "from=" << static_cast<int>(from) << " to=" << static_cast<int>(to);
        }
    }
}

TEST(JobStateMachine, EverySelfTransitionIsRejected) {
    for (JobState state : AllStates()) {
        EXPECT_FALSE(CanTransition(state, state)) << "state=" << static_cast<int>(state);
    }
}

TEST(JobStateMachine, TerminalStatesHaveNoOutgoingTransitions) {
    for (JobState to : AllStates()) {
        EXPECT_FALSE(CanTransition(JobState::Completed, to));
        EXPECT_FALSE(CanTransition(JobState::Cancelled, to));
    }
}

TEST(JobStateMachine, EachValidTransitionIndividually) {
    EXPECT_TRUE(CanTransition(JobState::Queued, JobState::Starting));
    EXPECT_TRUE(CanTransition(JobState::Queued, JobState::Cancelled));

    EXPECT_TRUE(CanTransition(JobState::Starting, JobState::Running));
    EXPECT_TRUE(CanTransition(JobState::Starting, JobState::Failed));
    EXPECT_TRUE(CanTransition(JobState::Starting, JobState::Cancelled));

    EXPECT_TRUE(CanTransition(JobState::Running, JobState::Paused));
    EXPECT_TRUE(CanTransition(JobState::Running, JobState::Completed));
    EXPECT_TRUE(CanTransition(JobState::Running, JobState::Failed));
    EXPECT_TRUE(CanTransition(JobState::Running, JobState::Cancelled));
    EXPECT_TRUE(CanTransition(JobState::Running, JobState::Retrying));

    EXPECT_TRUE(CanTransition(JobState::Paused, JobState::Running));
    EXPECT_TRUE(CanTransition(JobState::Paused, JobState::Cancelled));

    EXPECT_TRUE(CanTransition(JobState::Failed, JobState::Retrying));

    EXPECT_TRUE(CanTransition(JobState::Retrying, JobState::Running));
    EXPECT_TRUE(CanTransition(JobState::Retrying, JobState::Failed));
    EXPECT_TRUE(CanTransition(JobState::Retrying, JobState::Cancelled));
}

TEST(JobStateMachine, SampleOfInvalidTransitions) {
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Completed));
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Failed));
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Paused));
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Retrying));

    EXPECT_FALSE(CanTransition(JobState::Starting, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Starting, JobState::Paused));
    EXPECT_FALSE(CanTransition(JobState::Starting, JobState::Completed));
    EXPECT_FALSE(CanTransition(JobState::Starting, JobState::Retrying));

    EXPECT_FALSE(CanTransition(JobState::Running, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Running, JobState::Starting));

    EXPECT_FALSE(CanTransition(JobState::Paused, JobState::Starting));
    EXPECT_FALSE(CanTransition(JobState::Paused, JobState::Completed));
    EXPECT_FALSE(CanTransition(JobState::Paused, JobState::Failed));
    EXPECT_FALSE(CanTransition(JobState::Paused, JobState::Queued));

    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Cancelled));
    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Completed));

    EXPECT_FALSE(CanTransition(JobState::Retrying, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Retrying, JobState::Paused));
    EXPECT_FALSE(CanTransition(JobState::Retrying, JobState::Completed));

    EXPECT_FALSE(CanTransition(JobState::Completed, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Cancelled, JobState::Queued));
}
