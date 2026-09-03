#include "core/filesystem/FilenameSanitizer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <unordered_set>

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

// Replaces any byte sequence that is not well-formed UTF-8 with '_', one bad sequence at
// a time, leaving everything else untouched.
//
// A video title arrives from a source this process doesn't control the encoding of, and
// yt-dlp/YouTube metadata has been observed to contain unpaired UTF-16 surrogates. Python's
// json.dumps() and nlohmann::json's parser both pass these through -- as a 3-byte sequence
// in the D800-DFFF range that is syntactically shaped like UTF-8 but which the Unicode
// standard explicitly forbids UTF-8 from encoding. std::filesystem::path's Windows
// implementation enforces that prohibition strictly: constructing a path from such a string
// throws std::filesystem_error ("Cannot convert character sequence: Illegal byte
// sequence"), which was crashing the whole job (E_JOB_UNHANDLED_EXCEPTION) the moment a
// title like this reached SanitizeWindowsFilename's own stdfs::path construction below --
// the one place in this codebase that handles externally-influenced text without the
// defensive treatment used everywhere else (main.cpp's WriteLine, JobHistoryStore,
// InProgressJobStore all reach for json::error_handler_t::replace for exactly this class of
// problem; std::filesystem::path has no equivalent "replace and continue" option, so this
// function is that option, applied before the title ever reaches a path).
std::string RepairUtf8(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    std::size_t i = 0;
    while (i < input.size()) {
        const unsigned char lead = static_cast<unsigned char>(input[i]);
        std::size_t seqLen;
        char32_t codepoint;
        char32_t minCodepoint;
        if ((lead & 0x80) == 0x00) {
            result.push_back(static_cast<char>(lead));
            ++i;
            continue;
        } else if ((lead & 0xE0) == 0xC0) {
            seqLen = 2; codepoint = lead & 0x1F; minCodepoint = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            seqLen = 3; codepoint = lead & 0x0F; minCodepoint = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            seqLen = 4; codepoint = lead & 0x07; minCodepoint = 0x10000;
        } else {
            result.push_back('_');  // stray continuation byte or an invalid lead byte
            ++i;
            continue;
        }

        bool structurallyValid = i + seqLen <= input.size();
        for (std::size_t j = 1; structurallyValid && j < seqLen; ++j) {
            const unsigned char cont = static_cast<unsigned char>(input[i + j]);
            if ((cont & 0xC0) != 0x80) {
                structurallyValid = false;
                break;
            }
            codepoint = (codepoint << 6) | (cont & 0x3F);
        }
        if (!structurallyValid) {
            // Truncated or malformed sequence -- only the lead byte is known to be bad, so
            // skip just that one byte and let the next byte be re-examined on its own.
            result.push_back('_');
            ++i;
            continue;
        }

        // Rejects overlong encodings, values past the Unicode range, and -- the case this
        // function exists for -- an encoded UTF-16 surrogate (D800-DFFF). The sequence is
        // structurally well-formed UTF-8 shape, so the whole seqLen bytes are consumed as one
        // bad sequence rather than leaving its continuation bytes to be reprocessed as stray
        // lead bytes (which would emit one '_' per byte instead of one '_' for the sequence).
        if (codepoint < minCodepoint || codepoint > 0x10FFFF ||
            (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            result.push_back('_');
            i += seqLen;
            continue;
        }
        result.append(input, i, seqLen);
        i += seqLen;
    }
    return result;
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

// Case-insensitive Windows reserved device names -- reserved as a bare name AND with any
// extension (e.g. "NUL", "NUL.txt", "nul.tar.gz" are all invalid).
bool IsReservedWindowsDeviceName(const std::string& stem) {
    static const std::unordered_set<std::string> kReserved = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5",
        "LPT6", "LPT7", "LPT8", "LPT9",
    };
    std::string upper = stem;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                    [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return kReserved.count(upper) > 0;
}

// Like TruncateUtf8 above, but bounded by byte count rather than codepoint count -- used
// for the MAX_PATH budget below, which is itself a byte/char-count budget, not a
// codepoint-count one.
std::string TruncateUtf8Bytes(const std::string& input, std::size_t maxBytes) {
    if (input.size() <= maxBytes) return input;
    std::size_t byteIndex = 0;
    while (byteIndex < maxBytes) {
        const unsigned char lead = static_cast<unsigned char>(input[byteIndex]);
        std::size_t seqLen = 1;
        if ((lead & 0x80) == 0x00) seqLen = 1;
        else if ((lead & 0xE0) == 0xC0) seqLen = 2;
        else if ((lead & 0xF0) == 0xE0) seqLen = 3;
        else if ((lead & 0xF8) == 0xF0) seqLen = 4;
        if (byteIndex + seqLen > maxBytes) break;  // would split a sequence -- stop before it
        byteIndex += seqLen;
    }
    return input.substr(0, byteIndex);
}

constexpr std::size_t kLegacyMaxPath = 259;  // MAX_PATH (260) minus the terminating null
// Headroom left in the budget for a numbered dedup suffix (" (9999)" is 7 chars) plus a
// reasonably long extension (e.g. ".webm" is 5) plus the path separator itself.
constexpr std::size_t kPathReserveForSuffixAndExtension = 20;

}  // namespace

std::string SanitizeWindowsFilename(const std::string& rawTitle) {
    const std::string repaired = RepairUtf8(rawTitle);
    std::string result;
    result.reserve(repaired.size());
    for (unsigned char c : repaired) {
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

    const stdfs::path asPath(result);
    const std::string stem = asPath.stem().string();
    if (IsReservedWindowsDeviceName(stem)) {
        result = stem + "_file" + asPath.extension().string();
    }

    return result;
}

std::string TruncateBaseNameForMaxPath(const std::string& directory, const std::string& baseName) {
    const std::size_t overhead = directory.size() + 1 + kPathReserveForSuffixAndExtension;
    if (overhead >= kLegacyMaxPath) {
        return baseName;  // directory alone leaves no usable budget -- nothing to do here
    }
    const std::size_t allowed = kLegacyMaxPath - overhead;
    if (baseName.size() <= allowed) {
        return baseName;
    }
    std::string truncated = TruncateUtf8Bytes(baseName, allowed);
    TrimTrailingDotsAndSpaces(truncated);
    return truncated.empty() ? "untitled" : truncated;
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

bool IsJobArtifactOf(const std::string& filenameBase, const std::string& candidateName) {
    if (candidateName == filenameBase) return true;
    if (candidateName.size() <= filenameBase.size()) return false;
    if (candidateName.compare(0, filenameBase.size(), filenameBase) != 0) return false;
    return candidateName[filenameBase.size()] == '.';
}

std::string WithPlaylistIndex(const std::string& filename, int index, int totalCount) {
    const std::string totalDigits = std::to_string(totalCount > 0 ? totalCount : 1);
    std::ostringstream oss;
    oss << std::setw(static_cast<int>(totalDigits.size())) << std::setfill('0') << index;
    return oss.str() + " - " + filename;
}

}  // namespace mediatool::filesystem
