#pragma once

// Builds the argv vector for a conversion/compression ffmpeg invocation. Pure: no process
// launch, no filesystem access, no clock -- feed it a request, get a vector<string> back.
// That makes every codec/container/filter decision assertable in a unit test without a
// real ffmpeg binary, which is the point (spec section 16: argv vectors, never
// concatenated shell strings).
//
// Every produced argv starts with the same safety preamble:
//   -hide_banner -nostdin -y -loglevel error -progress pipe:1
// `-nostdin` matters: ffmpeg inheriting our stdin would fight the NDJSON protocol reader
// for input. `-progress pipe:1` is what FFmpegProgressParser consumes.
//
// The output path passed here is expected to be the AtomicWriter temporary path, not the
// user's final destination -- see FFmpegEngine.

#include <string>
#include <vector>

#include "core/media/ProcessingOptions.h"

namespace mediatool::media {

// The CRF (constant rate factor) an x264/VP9 encode uses for `preset`. Exposed so tests
// and docs can assert the mapping rather than re-deriving it from argv.
int CrfForPreset(CompressionPreset preset);

// Arguments for converting `inputPath` into `outputPath` per `request`.
// Throws errors::MediaToolException (ErrorCategory::UnsupportedFormat) if the request
// cannot be expressed -- e.g. an audio-only target combined with a video-only option.
std::vector<std::string> BuildConversionArgs(const std::string& inputPath,
                                             const std::string& outputPath,
                                             const ConversionRequest& request);

// Arguments for re-encoding `inputPath` smaller into `outputPath` per `request`.
// `hasVideoStream` comes from a prior Probe(): an audio-only input is compressed by
// re-encoding its audio track rather than by running a video encoder that would fail.
std::vector<std::string> BuildCompressionArgs(const std::string& inputPath,
                                              const std::string& outputPath,
                                              const CompressionRequest& request,
                                              bool hasVideoStream);

}  // namespace mediatool::media
