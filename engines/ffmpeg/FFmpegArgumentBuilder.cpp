#include "engines/ffmpeg/FFmpegArgumentBuilder.h"

#include "core/errors/MediaToolException.h"

namespace mediatool::media {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

// Shared preamble for every invocation -- see the header for why each flag is here.
void AppendPreamble(std::vector<std::string>& args, const std::string& inputPath) {
    args.insert(args.end(), {"-hide_banner", "-nostdin", "-y", "-loglevel", "error",
                             "-progress", "pipe:1"});
    args.insert(args.end(), {"-i", inputPath});
}

// "scale=-2:720" rather than "-1": x264 requires even dimensions, and -2 rounds the
// computed width to the nearest even number instead of failing on an odd result.
std::string ScaleFilter(int maxHeight) {
    // min() keeps the filter a no-op for sources already shorter than maxHeight rather
    // than upscaling them, which would grow the file the user asked to shrink.
    return "scale=-2:'min(" + std::to_string(maxHeight) + ",ih)'";
}

int DefaultAudioBitrateKbps(TargetFormat format) {
    switch (format) {
        case TargetFormat::Opus: return 128;
        case TargetFormat::M4a:
        case TargetFormat::Mp3: return 192;
        default: return 192;
    }
}

void AppendAudioCodec(std::vector<std::string>& args, TargetFormat format,
                      const ConversionRequest& request) {
    const int bitrate = request.audioBitrateKbps.value_or(DefaultAudioBitrateKbps(format));
    switch (format) {
        case TargetFormat::Mp3:
            args.insert(args.end(), {"-c:a", "libmp3lame", "-b:a", std::to_string(bitrate) + "k"});
            return;
        case TargetFormat::M4a:
        case TargetFormat::Mp4:
        case TargetFormat::Mov:
        case TargetFormat::Mkv:
            args.insert(args.end(), {"-c:a", "aac", "-b:a", std::to_string(bitrate) + "k"});
            return;
        case TargetFormat::Opus:
        case TargetFormat::WebM:
            args.insert(args.end(), {"-c:a", "libopus", "-b:a", std::to_string(bitrate) + "k"});
            return;
        case TargetFormat::Wav:
            // Lossless PCM: a bitrate flag would be meaningless, so it is deliberately
            // not emitted even if the caller supplied one.
            args.insert(args.end(), {"-c:a", "pcm_s16le"});
            return;
        case TargetFormat::Flac:
            args.insert(args.end(), {"-c:a", "flac"});
            return;
        case TargetFormat::Gif:
            return;  // GIF carries no audio at all.
    }
}

// GIF needs a generated palette to look like anything; the split/palettegen/paletteuse
// graph does that in a single pass rather than the usual two-invocation recipe.
std::vector<std::string> BuildGifArgs(const std::string& inputPath,
                                      const std::string& outputPath,
                                      const ConversionRequest& request) {
    std::vector<std::string> args;
    AppendPreamble(args, inputPath);

    const int fps = request.gifFps.value_or(12);
    std::string chain = "fps=" + std::to_string(fps);
    if (request.maxHeight) chain += "," + ScaleFilter(*request.maxHeight) + ":flags=lanczos";

    args.insert(args.end(),
                {"-filter_complex",
                 "[0:v] " + chain + ",split [pal][use];[pal] palettegen [p];[use][p] paletteuse",
                 "-loop", "0", "-an", outputPath});
    return args;
}

}  // namespace

int CrfForPreset(CompressionPreset preset) {
    switch (preset) {
        case CompressionPreset::Low: return 30;     // smallest file
        case CompressionPreset::Medium: return 26;
        case CompressionPreset::High: return 21;    // near-transparent
    }
    return 26;
}

std::vector<std::string> BuildConversionArgs(const std::string& inputPath,
                                             const std::string& outputPath,
                                             const ConversionRequest& request) {
    if (inputPath.empty() || outputPath.empty()) {
        throw MediaToolException(ErrorInfo::Make(
            "E_INVALID_PROCESSING_OPTION", ErrorCategory::UnsupportedFormat,
            "Conversion needs both an input and an output path."));
    }

    const TargetFormat format = request.targetFormat;

    if (format == TargetFormat::Gif) return BuildGifArgs(inputPath, outputPath, request);

    if (IsAudioOnly(format)) {
        if (request.maxHeight || request.gifFps) {
            throw MediaToolException(ErrorInfo::Make(
                "E_INVALID_PROCESSING_OPTION", ErrorCategory::UnsupportedFormat,
                "Resolution and frame-rate options do not apply to an audio-only format.",
                "targetFormat=" + ToWireString(format)));
        }
        std::vector<std::string> args;
        AppendPreamble(args, inputPath);
        args.push_back("-vn");  // drop any video/cover-art stream
        AppendAudioCodec(args, format, request);
        args.push_back(outputPath);
        return args;
    }

    std::vector<std::string> args;
    AppendPreamble(args, inputPath);
    if (request.gifFps) {
        throw MediaToolException(ErrorInfo::Make(
            "E_INVALID_PROCESSING_OPTION", ErrorCategory::UnsupportedFormat,
            "A frame-rate option only applies when converting to GIF.",
            "targetFormat=" + ToWireString(format)));
    }
    if (request.maxHeight) args.insert(args.end(), {"-vf", ScaleFilter(*request.maxHeight)});

    switch (format) {
        case TargetFormat::WebM:
            // VP9's -b:v 0 selects true constant-quality mode; without it -crf is ignored.
            args.insert(args.end(), {"-c:v", "libvpx-vp9", "-crf", "32", "-b:v", "0"});
            break;
        case TargetFormat::Mp4:
        case TargetFormat::Mov:
        case TargetFormat::Mkv:
            args.insert(args.end(), {"-c:v", "libx264", "-preset", "medium", "-crf", "23",
                                     "-pix_fmt", "yuv420p"});
            break;
        default:
            break;
    }
    if (format == TargetFormat::Mp4 || format == TargetFormat::Mov) {
        // Move the index to the front so the result starts playing before it is fully
        // downloaded/copied -- standard for these containers.
        args.insert(args.end(), {"-movflags", "+faststart"});
    }
    AppendAudioCodec(args, format, request);
    args.push_back(outputPath);
    return args;
}

std::vector<std::string> BuildCompressionArgs(const std::string& inputPath,
                                              const std::string& outputPath,
                                              const CompressionRequest& request,
                                              bool hasVideoStream) {
    if (inputPath.empty() || outputPath.empty()) {
        throw MediaToolException(ErrorInfo::Make(
            "E_INVALID_PROCESSING_OPTION", ErrorCategory::UnsupportedFormat,
            "Compression needs both an input and an output path."));
    }

    std::vector<std::string> args;
    AppendPreamble(args, inputPath);

    const int audioBitrate = request.audioBitrateKbps.value_or(hasVideoStream ? 128 : 160);

    if (!hasVideoStream) {
        if (request.maxHeight) {
            throw MediaToolException(ErrorInfo::Make(
                "E_INVALID_PROCESSING_OPTION", ErrorCategory::UnsupportedFormat,
                "This file has no video track, so it cannot be resized.",
                "maxHeight=" + std::to_string(*request.maxHeight)));
        }
        // Audio-only input: shrink by lowering the audio bitrate. Running a video encoder
        // here would just fail with "no video stream".
        args.insert(args.end(),
                    {"-vn", "-c:a", "aac", "-b:a", std::to_string(audioBitrate) + "k", outputPath});
        return args;
    }

    if (request.maxHeight) args.insert(args.end(), {"-vf", ScaleFilter(*request.maxHeight)});
    args.insert(args.end(), {"-c:v", "libx264", "-preset", "medium", "-crf",
                             std::to_string(CrfForPreset(request.preset)), "-pix_fmt", "yuv420p",
                             "-c:a", "aac", "-b:a", std::to_string(audioBitrate) + "k",
                             "-movflags", "+faststart", outputPath});
    return args;
}

}  // namespace mediatool::media
