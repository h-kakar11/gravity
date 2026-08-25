#pragma once

// Incremental parser for ffmpeg's `-progress pipe:1` output (spec section 16, spec
// section 36). ffmpeg periodically writes a block of `key=value` lines and terminates
// each block with `progress=continue` (more to come) or `progress=end` (final block).
// Feed lines one at a time as they arrive from IProcessRunner's stdout callback; a
// jobs::Progress becomes available via TakeProgressIfReady() once a block's terminating
// `progress=` line has been seen.
//
// Fully unit-testable by feeding canned text directly -- no real ffmpeg process needed.

#include <optional>
#include <string>
#include <unordered_map>

#include "core/jobs/Progress.h"

namespace mediatool::media {

class FFmpegProgressParser {
public:
    // `totalDurationSeconds` (typically from a prior Probe() call) lets the parser
    // compute a percentage from ffmpeg's out_time; leave empty if unknown, in which case
    // Progress::percentage stays unset. `inputBitrateBps` (also typically from Probe(),
    // in bits/second) lets it estimate Progress::speedBytesPerSecond as
    // (ffmpeg's speed multiplier) * inputBitrateBps / 8 -- an approximation (the true
    // instantaneous encode throughput isn't directly reported), but the same kind of
    // "close enough for a live estimate" approximation ffmpeg's own `speed=1.02x` already is.
    explicit FFmpegProgressParser(std::optional<double> totalDurationSeconds = std::nullopt,
                                  std::optional<double> inputBitrateBps = std::nullopt);

    void FeedLine(const std::string& line);

    // Returns and clears the most recently completed block's progress, or nullopt if no
    // new block has completed since the last call.
    std::optional<jobs::Progress> TakeProgressIfReady();

private:
    std::optional<double> totalDurationSeconds_;
    std::optional<double> inputBitrateBps_;
    std::unordered_map<std::string, std::string> fields_;
    std::optional<jobs::Progress> pending_;

    jobs::Progress BuildProgress(bool isEnd) const;
};

}  // namespace mediatool::media
