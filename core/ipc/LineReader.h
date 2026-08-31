#pragma once

// Bounded NDJSON line reading for the request loop (audit #21).
//
// `std::getline(std::cin, line)` grows its string until it finds a newline or runs out of
// memory. The core process reads from a pipe fed by the Tauri shell, so in normal
// operation the lines are small -- but "the peer is well-behaved" is not a memory-safety
// argument, and the failure mode is bad: a single unterminated multi-megabyte line makes
// the process allocate without limit and eventually die with no diagnostic, taking every
// running job with it. Reading with an explicit ceiling turns that into a single rejected
// request the loop recovers from.

#include <cstddef>
#include <istream>
#include <string>

namespace mediatool::ipc {

enum class ReadLineStatus {
    // `line` holds a complete line (without its terminating newline).
    Ok,
    // The stream ended with no further data. `line` is empty.
    EndOfStream,
    // The line exceeded the byte limit. It has been consumed and discarded up to and
    // including its newline, so the next read resumes cleanly at the following line;
    // `line` is empty and `bytesDiscarded` says how much was thrown away.
    LineTooLong,
};

struct ReadLineResult {
    ReadLineStatus status = ReadLineStatus::EndOfStream;
    std::string line;
    std::size_t bytesDiscarded = 0;
};

// Reads one newline-terminated line from `in`, giving up on any line longer than
// `maxBytes` (which must be > 0). A final line with no trailing newline is returned as Ok.
// The oversized case still consumes the rest of that line so a caller looping on this
// makes progress rather than re-reading the same overlong line forever.
ReadLineResult ReadBoundedLine(std::istream& in, std::size_t maxBytes);

}  // namespace mediatool::ipc
