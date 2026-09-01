#pragma once

// Turns an ffmpeg/ffprobe failure into a specific, actionable ErrorInfo.
//
// This exists because ffmpeg's stderr was being thrown away. RunFfmpegJob passed an
// `ignoreStderr` callback and reported every failure as one code, E_FFMPEG_FAILED, with an
// exit code for detail -- so "the disk is full", "you do not have permission to write
// there", "this file is corrupt" and "that codec is not built into your ffmpeg" were
// indistinguishable to the frontend, to the retry policy, and to the user. The information
// needed to tell them apart was on the child's stderr the whole time.
//
// The mapping is a substring table over ffmpeg's own messages, for the same reason
// downloader.py's is: ffmpeg has no stable machine-readable error vocabulary, and the
// alternative -- parsing exit codes -- carries even less. It is a heuristic and says so;
// anything it does not recognize keeps the generic code rather than being guessed at.
//
// Every classification decides three things the caller cannot: which code, which category
// (which is what the retry policy vetoes on -- see core/jobs/RetryPolicy.h), and what the
// user should try instead.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/errors/ErrorInfo.h"

namespace mediatool::media {

// `stderrTail` is the last few lines of the failed process's stderr, `exitCode` its exit
// status. `availableBytes`, when known, is the free space at the output location -- it is
// only used to make a disk-full message concrete ("about 41 MB free"), never to decide the
// classification, because free space measured after the failure is not what the failure
// saw.
errors::ErrorInfo ClassifyFfmpegFailure(const std::string& stderrTail, int exitCode,
                                         std::optional<std::uint64_t> availableBytes);

// Same table, for a failed ffprobe. Separate entry point because a probe failure means
// something different: ffprobe not being able to read a file is the definition of "this is
// not a media file we can work with", so the default is E_INVALID_FILE rather than a
// generic engine failure.
errors::ErrorInfo ClassifyFfprobeFailure(const std::string& stderrTail, int exitCode);

// Human-readable size for an error message ("41.2 MB"). Exposed for testing.
std::string DescribeByteCount(std::uint64_t bytes);

}  // namespace mediatool::media
