#pragma once

// How large a re-encode should aim to be -- the policy a compression job needs and a
// conversion job occasionally falls back on (issue #80).
//
// This lives in core/media/ rather than beside the ffmpeg argument builder because it is
// job-level policy, not argument construction: MediaProcessingJob (core/jobs) is the
// caller that probes the input and decides the target, and core must not depend on
// engines/ (see core/CMakeLists.txt -- the dependency runs engines -> core, never back).
// engines/ffmpeg then translates whatever target it is given into flags.

#include <optional>
#include <string>

namespace mediatool::media {

// The floor for any derived target. An absurdly small source must not produce an unusable
// bitrate just because the arithmetic said so.
inline constexpr int kMinTargetBitrateKbps = 64;

// True for an output format that carries no video stream at all (mp3/wav/flac/...), where
// a *video* bitrate target is meaningless and the audio bitrate is the only size lever.
bool IsAudioOnlyOutputFormat(const std::string& outputFormat);

// The rate-control target, in kbps, for re-encoding a source whose overall (container)
// bitrate is `sourceBitrateKbps`. std::nullopt means "impose no target" -- an unknown or
// nonsensical source bitrate, or a lossless request, where a bitrate cap would contradict
// what was asked for.
//
// This is what makes "compress" mean something. A compression job's target is always a
// FRACTION of the source (0.20x at "lowest" up to 0.75x at "ultra"), so the output is
// smaller than the input by construction rather than by hoping a quality-based CRF happens
// to land below it -- measured before this existed, a 1.19 MB H.264 clip re-encoded at the
// "medium" CRF came back at 1.03x its original size and at "high", 1.41x.
//
// A conversion job's target is only consulted when the resolved encoder ignores -crf (see
// EncoderSupportsCrf in engines/ffmpeg/FFmpegArgBuilder.cpp); its factors sit around 1.0x
// because a conversion is not asked to shrink anything, only to not balloon.
std::optional<int> TargetBitrateKbps(int sourceBitrateKbps, const std::string& quality,
                                      bool isCompression);

}  // namespace mediatool::media
