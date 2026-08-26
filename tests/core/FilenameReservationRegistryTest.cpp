#include "core/filesystem/FilenameReservationRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "core/filesystem/MockFileSystem.h"
#include "core/filesystem/PathUtils.h"

using mediatool::filesystem::FilenameReservationRegistry;
using mediatool::filesystem::MockFileSystem;

TEST(FilenameReservationRegistryTest, ReturnsDesiredNameWhenFree) {
    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    FilenameReservationRegistry registry;

    auto reservation = registry.Reserve("C:\\out", "video", fs);
    EXPECT_EQ(reservation.BaseName(), "video");
}

TEST(FilenameReservationRegistryTest, SecondReservationForSameNameGetsANumberedVariant) {
    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    FilenameReservationRegistry registry;

    auto first = registry.Reserve("C:\\out", "video", fs);
    auto second = registry.Reserve("C:\\out", "video", fs);

    EXPECT_EQ(first.BaseName(), "video");
    EXPECT_EQ(second.BaseName(), "video (1)");
}

TEST(FilenameReservationRegistryTest, ReleasingFreesTheNameForReuse) {
    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    FilenameReservationRegistry registry;

    {
        auto reservation = registry.Reserve("C:\\out", "video", fs);
        EXPECT_EQ(reservation.BaseName(), "video");
    }  // released here

    auto again = registry.Reserve("C:\\out", "video", fs);
    EXPECT_EQ(again.BaseName(), "video");
}

TEST(FilenameReservationRegistryTest, StillAvoidsNamesAlreadyOnDisk) {
    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    mediatool::filesystem::FileInfo existing;
    // Built via paths::Join (not a hardcoded backslash literal) so MockFileSystem's
    // parent-directory bookkeeping -- and therefore ListDirectory("C:\\out") -- actually
    // sees this file regardless of which platform's std::filesystem separator the test
    // happens to run under.
    existing.path = mediatool::filesystem::paths::Join("C:\\out", "video.mp4");
    existing.filename = "video.mp4";
    fs.AddFile(existing);

    FilenameReservationRegistry registry;
    auto reservation = registry.Reserve("C:\\out", "video", fs);
    EXPECT_EQ(reservation.BaseName(), "video (1)");
}

// Regression test for #12: N threads racing to reserve the same desired base name in the
// same directory must never collide -- every thread gets a distinct name.
TEST(FilenameReservationRegistryTest, ConcurrentReservationsNeverCollide) {
    MockFileSystem fs;
    fs.AddDirectory("C:\\out");
    FilenameReservationRegistry registry;

    constexpr int kThreads = 32;
    std::mutex resultsMutex;
    std::vector<std::string> claimedNames;
    std::vector<FilenameReservationRegistry::Reservation> reservations;
    std::mutex reservationsMutex;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&] {
            auto reservation = registry.Reserve("C:\\out", "video", fs);
            std::lock_guard<std::mutex> resultsLock(resultsMutex);
            claimedNames.push_back(reservation.BaseName());
            std::lock_guard<std::mutex> reservationsLock(reservationsMutex);
            reservations.push_back(std::move(reservation));
        });
    }
    for (auto& t : threads) t.join();

    ASSERT_EQ(claimedNames.size(), static_cast<size_t>(kThreads));
    const std::set<std::string> uniqueNames(claimedNames.begin(), claimedNames.end());
    EXPECT_EQ(uniqueNames.size(), static_cast<size_t>(kThreads))
        << "two or more threads claimed the same base name";
}
