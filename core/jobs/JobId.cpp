#include "core/jobs/JobTypes.h"

#include <random>
#include <sstream>

namespace mediatool::jobs {

namespace {

// Thread-local so concurrent GenerateJobId() calls (JobManager may create jobs from
// multiple threads) never share a std::mt19937 state.
std::mt19937_64& RandomEngine() {
    thread_local std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

char HexDigit(unsigned value) {
    return "0123456789abcdef"[value & 0xF];
}

}  // namespace

JobId GenerateJobId() {
    std::uniform_int_distribution<unsigned> nibble(0, 15);
    auto& rng = RandomEngine();

    // RFC 4122 version-4 layout: xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx, y in {8,9,a,b}.
    std::ostringstream out;
    out << "job-";
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

}  // namespace mediatool::jobs
