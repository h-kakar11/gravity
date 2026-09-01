// Adversarial coverage of the ONE gate every user-supplied path passes through before it
// reaches the filesystem: paths::IsSafeUserSuppliedPath (core/filesystem/PathUtils.h),
// which createJob, inspectFile and inspectDownloadUrl all call.
//
// These are deliberately not "does Join insert a separator" tests -- PathUtilsTest already
// covers the helpers' happy paths. What is exercised here is the shape of input an IPC
// caller can actually send, including the shapes that exist specifically to get past a
// naive check: mixed separators, a traversal hidden mid-path, a drive-relative path that
// looks absolute, Unicode, and a path long enough to matter on Windows.
//
// Every check in this file is LEXICAL, and that is the design, not an omission: the gate
// runs before the path is handed to the filesystem, on a path that may not exist yet (an
// output directory is routinely created afterwards). Where a lexical check cannot decide
// something -- a symlink or junction pointing outside the tree -- the test says so
// explicitly rather than pretending coverage.

#include <gtest/gtest.h>

#include <string>

#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/PathUtils.h"

namespace paths = mediatool::filesystem::paths;

namespace {

// The production call site always passes the setting; naming the two cases makes each
// assertion below say which policy it is asserting under.
constexpr bool kNetworkPathsAllowed = true;
constexpr bool kNetworkPathsDenied = false;

}  // namespace

TEST(WindowsPathValidation, AcceptsTheOrdinaryAbsolutePathsTheAppActuallyUses) {
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\Users\\hamim\\Videos", kNetworkPathsDenied));
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\Users\\hamim\\clip.mp4", kNetworkPathsDenied));
    // Forward slashes are legal on Windows and arrive from anything web-flavored.
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:/Users/hamim/Videos", kNetworkPathsDenied));
    // Lowercase drive letters are equally valid; Windows paths are case-insensitive and
    // the gate must not disagree with the OS about that.
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("c:\\users\\HAMIM\\Videos", kNetworkPathsDenied));
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("Z:\\", kNetworkPathsDenied));
}

TEST(WindowsPathValidation, RejectsEveryShapeOfRelativePath) {
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("", kNetworkPathsDenied));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("Videos", kNetworkPathsDenied));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath(".\\Videos", kNetworkPathsDenied));
    // Rooted on the current drive but not on a drive -- "relative to whichever drive the
    // process happens to be on" is exactly the CWD assumption this gate exists to refuse.
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("\\Users\\hamim", kNetworkPathsDenied));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("/Users/hamim", kNetworkPathsDenied));
    // "C:foo" is DRIVE-RELATIVE on Windows: it resolves against the process's current
    // directory on drive C, not against C:\. It looks absolute and is not.
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:Videos", kNetworkPathsDenied));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:", kNetworkPathsDenied));
}

TEST(WindowsPathValidation, RejectsTraversalWhereverItAppearsAndWhicheverSeparatorItUses) {
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:\\Users\\..\\Windows", kNetworkPathsDenied));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:/Users/../Windows", kNetworkPathsDenied));
    // Mixed separators in one path, which is legal on Windows and is the obvious way to
    // slip past a check that splits on only one of them.
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:\\Users/../Windows", kNetworkPathsDenied));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:/Users\\..\\Windows", kNetworkPathsDenied));
    // Buried deep, and trailing.
    EXPECT_FALSE(
        paths::IsSafeUserSuppliedPath("C:\\a\\b\\c\\d\\..\\..\\..\\e", kNetworkPathsDenied));
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:\\Users\\hamim\\..", kNetworkPathsDenied));
}

TEST(WindowsPathValidation, ADotDotInsideANameIsNotTraversal) {
    // The check is on path SEGMENTS, not on the substring "..". A file legitimately named
    // "report..final.txt" or a directory "my..stuff" must not be rejected -- a substring
    // check would reject both, and users do create such names.
    EXPECT_TRUE(
        paths::IsSafeUserSuppliedPath("C:\\Users\\hamim\\report..final.txt", kNetworkPathsDenied));
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\my..stuff\\clip.mp4", kNetworkPathsDenied));
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\Users\\..hidden", kNetworkPathsDenied));
}

TEST(WindowsPathValidation, UncPathsAreGatedByTheSettingAndNothingElse) {
    const std::string unc = "\\\\fileserver\\share\\Videos";
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath(unc, kNetworkPathsDenied));
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath(unc, kNetworkPathsAllowed));

    // The forward-slash spelling of a UNC path is equally valid to Windows, so the gate
    // must recognize it as one rather than treating it as an ordinary path.
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("//fileserver/share/Videos", kNetworkPathsDenied));

    // Traversal still loses even when UNC is permitted: the two checks are independent.
    EXPECT_FALSE(
        paths::IsSafeUserSuppliedPath("\\\\fileserver\\share\\..\\other", kNetworkPathsAllowed));
}

TEST(WindowsPathValidation, UnicodePathsArePassedThroughUntouched) {
    // The gate is about shape, not about character set. A user with a non-ASCII profile
    // name or a video titled in Japanese must not be told their path is unsafe.
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\Users\\Ünter\\Videos", kNetworkPathsDenied));
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\動画\\clip.mp4", kNetworkPathsDenied));
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\Users\\hamim\\🎬", kNetworkPathsDenied));
    // ...and traversal is still traversal in a Unicode path.
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath("C:\\動画\\..\\Windows", kNetworkPathsDenied));
}

TEST(WindowsPathValidation, AVeryLongPathIsNotRejectedByThisGate) {
    // Deliberately documenting a NON-guarantee. MAX_PATH is a filesystem-API limit, not a
    // path-shape rule, and rejecting a long path here would break users who have long-path
    // support enabled or who use the \\?\ prefix. The place that actually deals with the
    // limit is FilenameSanitizer::TruncateBaseNameForMaxPath, exercised below.
    const std::string longPath = "C:\\Users\\hamim\\" + std::string(400, 'a') + "\\clip.mp4";
    EXPECT_GT(longPath.size(), 260u);
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath(longPath, kNetworkPathsDenied));
}

TEST(WindowsPathValidation, TheMaxPathBudgetIsEnforcedWhereTheFilenameIsChosen) {
    // The real defense: a base name is shortened to fit under the limit for the directory
    // it is going into, so a 300-character video title cannot produce an unwritable path.
    const std::string deepDirectory = "C:\\Users\\hamim\\Videos\\" + std::string(180, 'd');
    const std::string absurdBaseName(300, 'n');

    const std::string truncated =
        mediatool::filesystem::TruncateBaseNameForMaxPath(deepDirectory, absurdBaseName);
    EXPECT_LT(truncated.size(), absurdBaseName.size());
    // Directory + separator + base + a plausible extension still fits.
    EXPECT_LE(deepDirectory.size() + 1 + truncated.size() + 5, 260u);
    // And it never truncates to nothing -- an empty base name would collide with every
    // other over-long name in the same directory.
    EXPECT_FALSE(truncated.empty());
}

TEST(WindowsPathValidation, ASymlinkedPathIsNotDetectedHereAndIsNotMeantToBe) {
    // Recorded so the gap is a decision rather than an assumption. This gate is lexical and
    // runs before the path exists, so a junction or symlink at C:\Users\hamim\Videos
    // pointing at C:\Windows passes it -- there is nothing in the string to see. Closing
    // that would mean resolving the path against the live filesystem, which cannot work for
    // an output directory that has not been created yet and would introduce a TOCTOU window
    // of its own (resolve, then the link is repointed, then write).
    //
    // What actually bounds the damage is downstream and unconditional: every write goes to
    // a name reserved through FilenameReservationRegistry inside the directory the user
    // chose, and cleanup only ever deletes entries IsJobArtifactOf accepts -- never a
    // recursive delete, never a bare prefix match.
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath("C:\\Users\\hamim\\Videos", kNetworkPathsDenied));
}

TEST(WindowsPathValidation, NormalizeIsALexicalOperationAndNotTheGate) {
    // Normalize() collapses "..", so calling it BEFORE the gate would launder a traversal
    // into an acceptable path. Order matters, and this records why: the production call
    // sites validate first and normalize later, never the reverse.
    //
    // Spelled with forward slashes on purpose. Normalize() is std::filesystem's
    // lexically_normal(), whose separator grammar is the HOST's -- on a POSIX build
    // "C:\a\..\b" is one filename component and nothing is collapsed at all. That is
    // harmless in production (this app only runs on Windows) but it makes a backslash
    // spelling of this test assert different things on different build hosts, which is
    // exactly the host-dependence LooksAbsoluteWindowsPath was written to avoid. Forward
    // slashes are separators on both, so this asserts the same thing everywhere.
    const std::string traversal = "C:/Users/hamim/../../Windows/System32";
    EXPECT_FALSE(paths::IsSafeUserSuppliedPath(traversal, kNetworkPathsDenied));
    const std::string normalized = paths::Normalize(traversal);
    EXPECT_EQ(normalized.find(".."), std::string::npos);
    EXPECT_TRUE(paths::IsSafeUserSuppliedPath(normalized, kNetworkPathsDenied));
}
