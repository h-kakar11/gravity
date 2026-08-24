#include "core/jobs/JobStateMachine.h"

#include <gtest/gtest.h>

#include <set>
#include <utility>
#include <vector>

using mediatool::jobs::CanTransition;
using mediatool::jobs::IsActiveState;
using mediatool::jobs::IsExecutingState;
using mediatool::jobs::IsTerminalState;
using mediatool::jobs::JobState;

namespace {

const std::vector<JobState>& AllStates() {
    static const std::vector<JobState> states = {
        JobState::Queued,    JobState::Waiting,   JobState::Starting,  JobState::Running,
        JobState::Paused,    JobState::RetryWait, JobState::Completed, JobState::Failed,
        JobState::Cancelled, JobState::Skipped,   JobState::Retrying,
    };
    return states;
}

// The one authoritative copy of "which pairs are valid", checked below against
// CanTransition() for all 121 (from, to) combinations. Mirrors the table documented in
// core/jobs/JobStateMachine.h and docs/ipc-contract.md -- when the machine changes, this
// set and those docs change with it, deliberately and together.
const std::set<std::pair<JobState, JobState>>& ValidTransitions() {
    static const std::set<std::pair<JobState, JobState>> valid = {
        {JobState::Queued, JobState::Starting},
        {JobState::Queued, JobState::Cancelled},
        {JobState::Queued, JobState::Waiting},
        {JobState::Queued, JobState::Skipped},

        {JobState::Waiting, JobState::Queued},
        {JobState::Waiting, JobState::Cancelled},
        {JobState::Waiting, JobState::Skipped},

        {JobState::Starting, JobState::Running},
        {JobState::Starting, JobState::Failed},
        {JobState::Starting, JobState::Cancelled},

        {JobState::Running, JobState::Paused},
        {JobState::Running, JobState::Completed},
        {JobState::Running, JobState::Failed},
        {JobState::Running, JobState::Cancelled},

        {JobState::Paused, JobState::Running},
        {JobState::Paused, JobState::Cancelled},

        {JobState::RetryWait, JobState::Retrying},
        {JobState::RetryWait, JobState::Cancelled},

        {JobState::Failed, JobState::Retrying},
        {JobState::Failed, JobState::RetryWait},

        {JobState::Skipped, JobState::Waiting},

        {JobState::Retrying, JobState::Running},
        {JobState::Retrying, JobState::Failed},
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
                << "from=" << mediatool::jobs::ToWireString(from)
                << " to=" << mediatool::jobs::ToWireString(to);
        }
    }
}

TEST(JobStateMachine, EverySelfTransitionIsRejected) {
    for (JobState state : AllStates()) {
        EXPECT_FALSE(CanTransition(state, state))
            << "state=" << mediatool::jobs::ToWireString(state);
    }
}

TEST(JobStateMachine, CompletedAndCancelledHaveNoOutgoingTransitions) {
    for (JobState to : AllStates()) {
        EXPECT_FALSE(CanTransition(JobState::Completed, to));
        EXPECT_FALSE(CanTransition(JobState::Cancelled, to));
    }
}

TEST(JobStateMachine, FailedAndSkippedAreTerminalButRecoverable) {
    // Both count as terminal -- the scheduler will never pick them up on its own -- yet each
    // has exactly one escape hatch, which is what makes "Retry" work without inventing a
    // separate resurrection mechanism.
    EXPECT_TRUE(IsTerminalState(JobState::Failed));
    EXPECT_TRUE(IsTerminalState(JobState::Skipped));

    EXPECT_TRUE(CanTransition(JobState::Failed, JobState::Retrying));     // manual retry
    EXPECT_TRUE(CanTransition(JobState::Failed, JobState::RetryWait));    // automatic retry
    EXPECT_TRUE(CanTransition(JobState::Skipped, JobState::Waiting));     // re-evaluate deps

    // ...but not straight back to runnable without going through those.
    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Skipped, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::Skipped, JobState::Queued));
}

TEST(JobStateMachine, ARetryAlwaysPassesThroughRetryingBeforeRunning) {
    // There is exactly one door into Running from a retry, whether the retry was automatic
    // or manual. That single path is what makes "a job never executes twice" checkable.
    EXPECT_TRUE(CanTransition(JobState::RetryWait, JobState::Retrying));
    EXPECT_TRUE(CanTransition(JobState::Retrying, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::RetryWait, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::RetryWait, JobState::Starting));
}

TEST(JobStateMachine, EveryPreExecutionStateCanBeCancelled) {
    // Cancellation has to be reachable from every state where the user can still see the
    // job as "not finished", including the two added in Phase 5.
    for (JobState from : {JobState::Queued, JobState::Waiting, JobState::Starting,
                          JobState::Running, JobState::Paused, JobState::RetryWait,
                          JobState::Retrying}) {
        EXPECT_TRUE(CanTransition(from, JobState::Cancelled))
            << "state=" << mediatool::jobs::ToWireString(from);
    }
}

TEST(JobStateMachine, DependencyGatingMovesBothWays) {
    EXPECT_TRUE(CanTransition(JobState::Waiting, JobState::Queued));  // dependency completed
    EXPECT_TRUE(CanTransition(JobState::Queued, JobState::Waiting));  // dependency re-run
    EXPECT_TRUE(CanTransition(JobState::Waiting, JobState::Skipped)); // dependency failed
    EXPECT_TRUE(CanTransition(JobState::Queued, JobState::Skipped));
}

TEST(JobStateMachine, SampleOfInvalidTransitions) {
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Completed));
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Failed));
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Paused));
    EXPECT_FALSE(CanTransition(JobState::Queued, JobState::Retrying));

    EXPECT_FALSE(CanTransition(JobState::Waiting, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::Waiting, JobState::Starting));
    EXPECT_FALSE(CanTransition(JobState::Waiting, JobState::Completed));

    EXPECT_FALSE(CanTransition(JobState::Starting, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Starting, JobState::Paused));
    EXPECT_FALSE(CanTransition(JobState::Starting, JobState::Completed));
    EXPECT_FALSE(CanTransition(JobState::Starting, JobState::Retrying));

    EXPECT_FALSE(CanTransition(JobState::Running, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Running, JobState::Starting));
    EXPECT_FALSE(CanTransition(JobState::Running, JobState::Retrying));
    EXPECT_FALSE(CanTransition(JobState::Running, JobState::Skipped));

    EXPECT_FALSE(CanTransition(JobState::Paused, JobState::Starting));
    EXPECT_FALSE(CanTransition(JobState::Paused, JobState::Completed));
    EXPECT_FALSE(CanTransition(JobState::Paused, JobState::Failed));
    EXPECT_FALSE(CanTransition(JobState::Paused, JobState::Queued));

    EXPECT_FALSE(CanTransition(JobState::RetryWait, JobState::Completed));
    EXPECT_FALSE(CanTransition(JobState::RetryWait, JobState::Failed));

    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Running));
    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Cancelled));
    EXPECT_FALSE(CanTransition(JobState::Failed, JobState::Completed));

    EXPECT_FALSE(CanTransition(JobState::Retrying, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Retrying, JobState::Paused));
    EXPECT_FALSE(CanTransition(JobState::Retrying, JobState::Completed));

    EXPECT_FALSE(CanTransition(JobState::Skipped, JobState::Failed));
    EXPECT_FALSE(CanTransition(JobState::Skipped, JobState::Cancelled));

    EXPECT_FALSE(CanTransition(JobState::Completed, JobState::Queued));
    EXPECT_FALSE(CanTransition(JobState::Cancelled, JobState::Queued));
}

TEST(JobStateMachine, StatePredicatesAgreeWithEachOther) {
    for (JobState state : AllStates()) {
        // Active and terminal partition the state space exactly -- no state is both, none
        // is neither.
        EXPECT_NE(IsActiveState(state), IsTerminalState(state))
            << "state=" << mediatool::jobs::ToWireString(state);
        // Executing is a strict subset of active: a job holding a concurrency slot is by
        // definition still live.
        if (IsExecutingState(state)) EXPECT_TRUE(IsActiveState(state));
    }

    // Pending states are active but hold no slot -- that distinction is what lets the queue
    // hold a hundred jobs at concurrency 2.
    for (JobState state : {JobState::Queued, JobState::Waiting, JobState::RetryWait}) {
        EXPECT_TRUE(IsActiveState(state));
        EXPECT_FALSE(IsExecutingState(state));
    }
    for (JobState state : {JobState::Starting, JobState::Running, JobState::Retrying}) {
        EXPECT_TRUE(IsExecutingState(state));
    }
}
