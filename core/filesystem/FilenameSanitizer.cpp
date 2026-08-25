#include "core/filesystem/FilenameSanitizer.h"

#include <filesystem>
#include <iomanip>
#include <sstream>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"

namespace stdfs = std::filesystem;

namespace mediatool::filesystem {

namespace {

constexpr std::size_t kMaxFilenameCodepoints = 200;
// Sanity bound on how many "(N)" suffixes DeduplicateFilename will try before giving
// up -- guards against an IFileSystem implementation that always reports Exists()==true
// (a bug) turning into an infinite loop rather than a normal collision sequence.
constexpr int kMaxDeduplicationAttempts = 10000;

bool IsWindowsIllegalChar(unsigned char c) {
    switch (c) {
        case '<': case '>': case ':': case '"': case '/':
        case '\\': case '|': case '?': case '*':
            return true;
        default:
            return false;
    }
}

// Truncates to at most `maxCodepoints` UTF-8 codepoints without splitting a multi-byte
// sequence (a byte-length cap could sever an emoji/accented character mid-encoding and
// produce invalid UTF-8). Treats each Unicode scalar value as one "character"; a
// multi-codepoint grapheme cluster (e.g. an emoji ZWJ sequence) can in principle be cut
// between codepoints -- an acceptable simplification for a Phase 1 length guard.
std::string TruncateUtf8(const std::string& input, std::size_t maxCodepoints) {
    std::size_t codepoints = 0;
    std::size_t byteIndex = 0;
    while (byteIndex < input.size() && codepoints < maxCodepoints) {
        const unsigned char lead = static_cast<unsigned char>(input[byteIndex]);
        std::size_t seqLen = 1;
        if ((lead & 0x80) == 0x00) seqLen = 1;
        else if ((lead & 0xE0) == 0xC0) seqLen = 2;
        else if ((lead & 0xF0) == 0xE0) seqLen = 3;
        else if ((lead & 0xF8) == 0xF0) seqLen = 4;
        // Invalid lead byte: fall through with seqLen == 1 rather than looping forever.

        if (byteIndex + seqLen > input.size()) {
            break;  // incomplete trailing sequence -- stop before it
        }
        byteIndex += seqLen;
        ++codepoints;
    }
    return input.substr(0, byteIndex);
}

void TrimTrailingDotsAndSpaces(std::string& value) {
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) {
        value.pop_back();
    }
}

}  // namespace

std::string SanitizeWindowsFilename(const std::string& rawTitle) {
    std::string result;
    result.reserve(rawTitle.size());
    for (unsigned char c : rawTitle) {
        if (c < 0x20) {
            continue;  // control characters have no visual form; drop rather than replace
        }
        result.push_back(IsWindowsIllegalChar(c) ? '_' : static_cast<char>(c));
    }

    TrimTrailingDotsAndSpaces(result);
    result = TruncateUtf8(result, kMaxFilenameCodepoints);
    TrimTrailingDotsAndSpaces(result);  // truncation may expose a new trailing dot/space

    if (result.empty()) {
        result = "untitled";
    }
    return result;
}

std::string DeduplicateFilename(const std::string& desiredPath, const IFileSystem& fs) {
    if (!fs.Exists(desiredPath)) {
        return desiredPath;
    }

    const stdfs::path p(desiredPath);
    const stdfs::path parent = p.parent_path();
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();  // includes leading dot, or empty

    for (int i = 1; i <= kMaxDeduplicationAttempts; ++i) {
        const stdfs::path candidate = parent / (stem + " (" + std::to_string(i) + ")" + ext);
        std::string candidateStr = candidate.string();
        if (!fs.Exists(candidateStr)) {
            return candidateStr;
        }
    }

    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_DEDUP_EXHAUSTED", errors::ErrorCategory::Unknown,
        "Could not find a free filename for " + desiredPath,
        "Exceeded " + std::to_string(kMaxDeduplicationAttempts) + " numbered variants"));
}

namespace {

bool AnyFileHasBaseName(const std::vector<std::string>& names, const std::string& baseName) {
    for (const auto& name : names) {
        if (stdfs::path(name).stem().string() == baseName) return true;
    }
    return false;
}

}  // namespace

std::string DeduplicateBaseName(const std::string& directory, const std::string& desiredBaseName,
                                 const IFileSystem& fs) {
    const std::vector<std::string> existing = fs.ListDirectory(directory);
    if (!AnyFileHasBaseName(existing, desiredBaseName)) {
        return desiredBaseName;
    }

    for (int i = 1; i <= kMaxDeduplicationAttempts; ++i) {
        const std::string candidate = desiredBaseName + " (" + std::to_string(i) + ")";
        if (!AnyFileHasBaseName(existing, candidate)) {
            return candidate;
        }
    }

    throw errors::MediaToolException(errors::ErrorInfo::Make(
        "E_DEDUP_EXHAUSTED", errors::ErrorCategory::Unknown,
        "Could not find a free base filename for " + desiredBaseName,
        "Exceeded " + std::to_string(kMaxDeduplicationAttempts) + " numbered variants in " + directory));
}

std::string WithPlaylistIndex(const std::string& filename, int index, int totalCount) {
    const std::string totalDigits = std::to_string(totalCount > 0 ? totalCount : 1);
    std::ostringstream oss;
    oss << std::setw(static_cast<int>(totalDigits.size())) << std::setfill('0') << index;
    return oss.str() + " - " + filename;
}

}  // namespace mediatool::filesystem
