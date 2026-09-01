#include "core/filesystem/ToolPathResolver.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

using mediatool::filesystem::BuildToolCandidates;
using mediatool::filesystem::CleanEnvPathValue;
using mediatool::filesystem::ExecutableDirectory;
using mediatool::filesystem::FirstExisting;

namespace stdfs = std::filesystem;

namespace {

// Candidate lists are built with make_preferred(), so an expectation written with '/'
// has to be normalized the same way before it can be compared on either host.
std::string Native(const std::string& path) {
    return stdfs::path(path).make_preferred().string();
}

bool Contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), Native(value)) != list.end();
}

std::size_t IndexOf(const std::vector<std::string>& list, const std::string& value) {
    const auto it = std::find(list.begin(), list.end(), Native(value));
    return it == list.end() ? list.size() : static_cast<std::size_t>(it - list.begin());
}

}  // namespace

TEST(ToolPathResolverTest, CleanEnvPathValueStripsSurroundingQuotesAndWhitespace) {
    // `set MEDIATOOL_PYTHON_PATH="C:\Py\python.exe"` in a batch file keeps the quotes in
    // the value. They then reach CreateProcess as part of the filename, which cannot
    // resolve -- while `echo %MEDIATOOL_PYTHON_PATH%` still looks correct to whoever set
    // it. Issue #79's "the path exists and is readable" report is consistent with this.
    EXPECT_EQ(CleanEnvPathValue("\"C:\\Py\\python.exe\""), "C:\\Py\\python.exe");
    EXPECT_EQ(CleanEnvPathValue("  C:\\Py\\python.exe \n"), "C:\\Py\\python.exe");
    EXPECT_EQ(CleanEnvPathValue(" \"C:\\Py\\python.exe\" "), "C:\\Py\\python.exe");
    // A lone quote is not a matched pair and must be left alone rather than half-stripped.
    EXPECT_EQ(CleanEnvPathValue("\"C:\\Py\\python.exe"), "\"C:\\Py\\python.exe");
    EXPECT_EQ(CleanEnvPathValue(""), "");
}

TEST(ToolPathResolverTest, AnExplicitOverrideIsTriedFirst) {
    const auto candidates =
        BuildToolCandidates("C:\\custom\\python.exe", "C:\\app", {"python/python.exe"});
    ASSERT_FALSE(candidates.empty());
    EXPECT_EQ(candidates.front(), Native("C:\\custom\\python.exe"));
}

// These two build their expectations through stdfs::path rather than as literals: the
// separator a join produces is a property of the HOST's path grammar (see PathUtils.h on
// why that distinction matters here), and what is under test is which directory a
// candidate is anchored to, not how the separator is spelled.
TEST(ToolPathResolverTest, CandidatesAreAnchoredToTheExecutableDirectory) {
    // The bug (issue #79): the only non-env candidate used to be a CWD-relative literal,
    // and the core's working directory is whatever the Tauri shell inherited -- under
    // `tauri dev` that's app/desktop/src-tauri, never the repository root the literal was
    // written against.
    const stdfs::path exeDir = stdfs::path("install") / "Gravity";
    const auto candidates = BuildToolCandidates("", exeDir.string(), {"python/python.exe"});
    EXPECT_TRUE(Contains(candidates, (exeDir / "python/python.exe").string()));
}

TEST(ToolPathResolverTest, AncestorsOfTheExecutableDirectoryAreSearchedForTheDevLayout) {
    // A dev build's core sits at build/<preset>/app/core/, four levels below the repo root
    // where python/downloader/.venv actually lives -- so anchoring to the executable's own
    // directory is not enough on its own; its ancestors have to be searched too.
    const stdfs::path repoRoot = stdfs::path("repo");
    const stdfs::path exeDir = repoRoot / "build" / "windows-mingw-debug" / "app" / "core";
    const std::string relative = "python/downloader/.venv/Scripts/python.exe";
    const auto candidates = BuildToolCandidates("", exeDir.string(), {relative});
    EXPECT_TRUE(Contains(candidates, (repoRoot / relative).string()));
}

TEST(ToolPathResolverTest, TheLegacyCwdRelativeCandidateIsKeptButRankedLast) {
    // Anything that resolved before #79 must keep resolving; it just stops being the only
    // thing tried.
    const auto candidates = BuildToolCandidates("", "C:\\app", {"python/python.exe"});
    ASSERT_TRUE(Contains(candidates, "python/python.exe"));
    EXPECT_EQ(IndexOf(candidates, "python/python.exe"), candidates.size() - 1);
}

TEST(ToolPathResolverTest, CandidatesAreDeduplicatedSoDiagnosticsDoNotRepeat) {
    const auto candidates =
        BuildToolCandidates("python/python.exe", "C:\\app", {"python/python.exe"});
    const std::set<std::string> unique(candidates.begin(), candidates.end());
    EXPECT_EQ(unique.size(), candidates.size());
}

TEST(ToolPathResolverTest, AncestorWalkTerminatesAtTheFilesystemRoot) {
    // parent_path() of a root is the root itself; without the guard this loops forever.
    const auto candidates = BuildToolCandidates("", stdfs::path("/").string(), {"tool"});
    EXPECT_LT(candidates.size(), 10u);
}

TEST(ToolPathResolverTest, FirstExistingReturnsTheFirstAcceptedCandidate) {
    const std::vector<std::string> candidates = {"a", "b", "c"};
    const auto found =
        FirstExisting(candidates, [](const std::string& p) { return p == "b" || p == "c"; });
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, "b");
}

TEST(ToolPathResolverTest, FirstExistingReportsNothingRatherThanGuessing) {
    // The whole point of checking: "none of these exist" has to be distinguishable from
    // "here, launch this", so the caller can name what it tried instead of handing
    // CreateProcess a path it already knows is wrong.
    const auto found = FirstExisting({"a", "b"}, [](const std::string&) { return false; });
    EXPECT_FALSE(found.has_value());
}

TEST(ToolPathResolverTest, ExecutableDirectoryIsAnExistingDirectoryContainingThisTestBinary) {
    // Deliberately not derived from argv[0], which is whatever the parent chose to pass.
    const std::string dir = ExecutableDirectory();
    ASSERT_FALSE(dir.empty());
    std::error_code ec;
    EXPECT_TRUE(stdfs::is_directory(dir, ec));
}
