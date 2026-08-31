#include "core/ipc/LineReader.h"

namespace mediatool::ipc {

ReadLineResult ReadBoundedLine(std::istream& in, std::size_t maxBytes) {
    ReadLineResult result;
    if (maxBytes == 0) maxBytes = 1;

    std::streambuf* buffer = in.rdbuf();
    if (buffer == nullptr) return result;  // EndOfStream

    // Read through the streambuf rather than with std::getline: getline has no length
    // limit, and formatted extraction per character is far slower on the megabyte-scale
    // lines this function exists to survive.
    bool sawAnything = false;
    while (true) {
        const std::streambuf::int_type next = buffer->sbumpc();
        if (next == std::streambuf::traits_type::eof()) {
            in.setstate(std::ios::eofbit);
            if (!sawAnything) return result;  // EndOfStream, nothing read
            // A last line with no trailing newline is still a complete line.
            result.status = ReadLineStatus::Ok;
            return result;
        }

        sawAnything = true;
        const char character = std::streambuf::traits_type::to_char_type(next);
        if (character == '\n') {
            result.status = ReadLineStatus::Ok;
            return result;
        }

        if (result.line.size() >= maxBytes) {
            // Over the limit: stop accumulating (so the oversized content is never held in
            // memory in full) but keep draining to the newline so the stream stays aligned
            // with the NDJSON framing.
            result.bytesDiscarded = result.line.size() + 1;
            result.line.clear();
            result.line.shrink_to_fit();
            while (true) {
                const std::streambuf::int_type skipped = buffer->sbumpc();
                if (skipped == std::streambuf::traits_type::eof()) {
                    in.setstate(std::ios::eofbit);
                    break;
                }
                ++result.bytesDiscarded;
                if (std::streambuf::traits_type::to_char_type(skipped) == '\n') break;
            }
            result.status = ReadLineStatus::LineTooLong;
            return result;
        }

        result.line.push_back(character);
    }
}

}  // namespace mediatool::ipc
