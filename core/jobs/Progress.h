#pragma once

// Universal progress model (docs/ipc-contract.md, spec section 7). Works for
// byte-oriented operations (downloads), frame/time-oriented operations (encoding), and
// count-oriented operations (batch file processing) by making everything except
// statusMessage optional -- callers fill in only what applies to their operation.

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace mediatool::jobs {

struct Progress {
    std::optional<double> percentage;              // 0-100
    std::optional<std::uint64_t> processedBytes;
    std::optional<std::uint64_t> totalBytes;
    std::optional<double> speedBytesPerSecond;
    std::optional<double> etaSeconds;
    std::optional<std::string> currentItem;         // e.g. "file 43 of 100"
    std::string statusMessage;                      // always present, human-readable

    nlohmann::json ToJson() const;
    static Progress FromJson(const nlohmann::json& json);
};

}  // namespace mediatool::jobs
