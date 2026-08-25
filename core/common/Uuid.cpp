#include "core/common/Uuid.h"

#include <random>
#include <sstream>

namespace mediatool::common {

namespace {

// Thread-local so concurrent GenerateUuidV4() calls (e.g. JobManager creating jobs from
// multiple worker threads) never share a std::mt19937_64 state.
std::mt19937_64& RandomEngine() {
    thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

char HexDigit(unsigned value) { return "0123456789abcdef"[value & 0xF]; }

}  // namespace

std::string GenerateUuidV4() {
    std::uniform_int_distribution<unsigned> nibble(0, 15);
    auto& rng = RandomEngine();

    // xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx, y in {8,9,a,b}.
    std::ostringstream out;
    for (int i = 0; i < 8; ++i) out << HexDigit(nibble(rng));
    out << '-';
    for (int i = 0; i < 4; ++i) out << HexDigit(nibble(rng));
    out << "-4";
    for (int i = 0; i < 3; ++i) out << HexDigit(nibble(rng));
    out << '-';
    out << HexDigit(8 + (nibble(rng) & 0x3));
    for (int i = 0; i < 3; ++i) out << HexDigit(nibble(rng));
    out << '-';
    for (int i = 0; i < 12; ++i) out << HexDigit(nibble(rng));

    return out.str();
}

}  // namespace mediatool::common
