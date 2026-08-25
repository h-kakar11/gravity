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
    // Progress::percentage stays unset.
    explicit FFmpegProgressParser(std::optional<double> totalDurationSeconds = std::nullopt);

    void FeedLine(const std::string& line);

    // Returns and clears the most recently completed block's progress, or nullopt if no
    // new block has completed since the last call.
    std::optional<jobs::Progress> TakeProgressIfReady();

private:
    std::optional<double> totalDurationSeconds_;
    std::unordered_map<std::string, std::string> fields_;
    std::optional<jobs::Progress> pending_;

    jobs::Progress BuildProgress(bool isEnd) const;
};

}  // namespace mediatool::media
