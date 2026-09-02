#pragma once

// A temp path that belongs to exactly one test.
//
// The fixtures that touch the filesystem each used a FIXED path under
// std::filesystem::temp_directory_path() -- "mediatool_in_progress_job_store_test" and
// friends -- and began by `remove_all`-ing it. That is safe only while nothing else is
// using it, which stops being true the moment ctest is given `-j`: every test in a fixture
// shares one directory, so one test's SetUp deletes the files another test is midway
// through asserting on. `ctest -j8 -R InProgressJobStoreTest` failed 3, then 2, then 5 of
// its 9 tests on three consecutive runs; serially it passes 9/9. Nothing was wrong with
// the code under test, which is what makes this the worst kind of test failure.
//
// Uniqueness comes from the process id plus a per-process counter, which covers both ways
// these tests can run concurrently: gtest_discover_tests gives each test case its own
// process (different pids), and a single binary invoked directly runs them all in one
// process (different counter values). tests/integration/CoreProcessFixture.cpp already
// took this approach for the same reason; this is that idea shared.
//
// Header-only and reached as "tests/support/TempTestDirectory.h" -- mtcore already exports
// the repo root as a public include directory, so this needs no CMake change.

#include <atomic>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace mediatool::testing {

inline unsigned long CurrentProcessId() {
#ifdef _WIN32
    return static_cast<unsigned long>(_getpid());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

// <temp>/<prefix>-<pid>-<n>: a path no other concurrently-running test will pick. Creating
// and removing it stays the caller's business, so existing SetUp/TearDown pairs keep
// working unchanged -- their `remove_all` simply stops being able to hit anyone else.
inline std::filesystem::path UniqueTempPath(const std::string& prefix) {
    static std::atomic<unsigned long> counter{0};
    return std::filesystem::temp_directory_path() /
           (prefix + "-" + std::to_string(CurrentProcessId()) + "-" +
            std::to_string(counter.fetch_add(1)));
}

}  // namespace mediatool::testing
