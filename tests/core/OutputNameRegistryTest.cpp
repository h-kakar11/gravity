// The reservation that stops two concurrent jobs picking the same output filename.
//
// The concurrency test here is the point of the whole class: deduplication alone passes a
// single-threaded test perfectly and still loses the user's files the moment two jobs run
// at once, which is exactly what Phase 5 introduced.

#include "core/filesystem/OutputNameRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/FileInfo.h"
#include "core/filesystem/MockFileSystem.h"

using mediatool::filesystem::FileInfo;
using mediatool::filesystem::MockFileSystem;
using mediatool::filesystem::OutputNameRegistry;

namespace {

void AddFile(MockFileSystem& fs, const std::string& path) {
    FileInfo info;
    info.path = path;
    info.filename = path.substr(path.find_last_of("/\\") + 1);
    info.sizeBytes = 1;
    fs.AddFile(info);
}

class OutputNameRegistryTest : public ::testing::Test {
protected:
    // Reservations outlive a single test unless released, and the registry is process-wide
    // by design -- so each test starts from a clean one.
    void SetUp() override { OutputNameRegistry::Instance().ClearForTesting(); }
    void TearDown() override { OutputNameRegistry::Instance().ClearForTesting(); }
};

}  // namespace

TEST_F(OutputNameRegistryTest, AFreeNameIsReturnedUnchanged) {
    MockFileSystem fs;
    fs.AddDirectory("/out");
    auto reservation = OutputNameRegistry::Instance().ReserveFilename("/out/clip.mp3", fs);
    EXPECT_EQ(reservation.Value(), "/out/clip.mp3");
    EXPECT_TRUE(reservation.IsHeld());
}

TEST_F(OutputNameRegistryTest, AnExistingFileOnDiskIsStillAvoided) {
    MockFileSystem fs;
    fs.AddDirectory("/out");
    AddFile(fs, "/out/clip.mp3");

    auto reservation = OutputNameRegistry::Instance().ReserveFilename("/out/clip.mp3", fs);
    EXPECT_EQ(reservation.Value(), "/out/clip (1).mp3");
}

TEST_F(OutputNameRegistryTest, AHeldReservationBlocksASecondCallerEvenWithNothingOnDisk) {
    // The case deduplication alone gets wrong: neither name exists yet.
    MockFileSystem fs;
    fs.AddDirectory("/out");
    auto& registry = OutputNameRegistry::Instance();

    auto first = registry.ReserveFilename("/out/clip.mp3", fs);
    auto second = registry.ReserveFilename("/out/clip.mp3", fs);
    auto third = registry.ReserveFilename("/out/clip.mp3", fs);

    EXPECT_EQ(first.Value(), "/out/clip.mp3");
    EXPECT_EQ(second.Value(), "/out/clip (1).mp3");
    EXPECT_EQ(third.Value(), "/out/clip (2).mp3");
}

TEST_F(OutputNameRegistryTest, ReleasingMakesTheNameAvailableAgain) {
    MockFileSystem fs;
    fs.AddDirectory("/out");
    auto& registry = OutputNameRegistry::Instance();

    {
        auto held = registry.ReserveFilename("/out/clip.mp3", fs);
        EXPECT_EQ(held.Value(), "/out/clip.mp3");
    }  // destructor releases

    auto again = registry.ReserveFilename("/out/clip.mp3", fs);
    EXPECT_EQ(again.Value(), "/out/clip.mp3");
}

TEST_F(OutputNameRegistryTest, ReleaseIsIdempotentAndSurvivesMoves) {
    MockFileSystem fs;
    fs.AddDirectory("/out");
    auto& registry = OutputNameRegistry::Instance();

    auto reservation = registry.ReserveFilename("/out/clip.mp3", fs);
    auto moved = std::move(reservation);
    EXPECT_TRUE(moved.IsHeld());
    EXPECT_FALSE(reservation.IsHeld());  // NOLINT(bugprone-use-after-move) -- that is the assertion

    moved.Release();
    moved.Release();  // must not double-release someone else's later claim
    EXPECT_FALSE(moved.IsHeld());

    auto again = registry.ReserveFilename("/out/clip.mp3", fs);
    EXPECT_EQ(again.Value(), "/out/clip.mp3");
}

TEST_F(OutputNameRegistryTest, BaseNamesAreReservedPerDirectory) {
    // The same stem in two folders is two unrelated files.
    MockFileSystem fs;
    fs.AddDirectory("/videos");
    fs.AddDirectory("/music");
    auto& registry = OutputNameRegistry::Instance();

    auto videos = registry.ReserveBaseName("/videos", "Holiday", fs);
    auto music = registry.ReserveBaseName("/music", "Holiday", fs);

    EXPECT_EQ(videos.Value(), "Holiday");
    EXPECT_EQ(music.Value(), "Holiday");
}

TEST_F(OutputNameRegistryTest, BaseNameReservationsCollideWithinOneDirectory) {
    MockFileSystem fs;
    fs.AddDirectory("/videos");
    auto& registry = OutputNameRegistry::Instance();

    auto first = registry.ReserveBaseName("/videos", "Holiday", fs);
    auto second = registry.ReserveBaseName("/videos", "Holiday", fs);

    EXPECT_EQ(first.Value(), "Holiday");
    EXPECT_EQ(second.Value(), "Holiday (1)");
}

TEST_F(OutputNameRegistryTest, BaseNameReservationAlsoAvoidsAnyExtensionOnDisk) {
    // A base name has no extension yet, so "taken" means "any file with this stem".
    MockFileSystem fs;
    fs.AddDirectory("/videos");
    AddFile(fs, "/videos/Holiday.webm");

    auto reservation = OutputNameRegistry::Instance().ReserveBaseName("/videos", "Holiday", fs);
    EXPECT_EQ(reservation.Value(), "Holiday (1)");
}

TEST_F(OutputNameRegistryTest, ConcurrentReservationsAreAllDistinct) {
    // The regression this class exists for. Sixteen threads race for the same desired name
    // with an empty directory; every one must come away with a different reservation.
    MockFileSystem fs;
    fs.AddDirectory("/out");
    auto& registry = OutputNameRegistry::Instance();

    constexpr int kThreads = 16;
    std::vector<OutputNameRegistry::Reservation> reservations(kThreads);
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            ++ready;
            while (!go) {
            }  // start together, to make the window as tight as possible
            reservations[i] = registry.ReserveFilename("/out/clip.mp4", fs);
        });
    }
    while (ready < kThreads) {
    }
    go = true;
    for (auto& thread : threads) thread.join();

    std::set<std::string> distinct;
    for (const auto& reservation : reservations) {
        EXPECT_TRUE(reservation.IsHeld());
        distinct.insert(reservation.Value());
    }
    EXPECT_EQ(distinct.size(), static_cast<std::size_t>(kThreads))
        << "two concurrent jobs were handed the same output name";
    EXPECT_EQ(distinct.count("/out/clip.mp4"), 1u);
}

TEST_F(OutputNameRegistryTest, ExhaustionThrowsRatherThanLoopingForever) {
    MockFileSystem fs;
    fs.AddDirectory("/out");
    // A filesystem that claims every path exists stands in for the pathological case; the
    // bounded attempt count must turn it into an error rather than a hang.
    class AlwaysExists final : public MockFileSystem {
    public:
        bool Exists(const std::string&) const override { return true; }
    };
    AlwaysExists always;
    EXPECT_THROW(OutputNameRegistry::Instance().ReserveFilename("/out/clip.mp3", always),
                 mediatool::errors::MediaToolException);
}
