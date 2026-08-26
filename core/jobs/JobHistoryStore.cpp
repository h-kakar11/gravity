#include "core/jobs/JobHistoryStore.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/logging/Logger.h"

namespace fs = std::filesystem;

namespace mediatool::jobs {

namespace {

std::string GetEnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : "";
}

}  // namespace

std::string DefaultJobHistoryFilePath() {
    const std::string localAppData = GetEnvOrEmpty("LOCALAPPDATA");
    if (localAppData.empty()) {
        return "Gravity\\job_history.json";
    }
    return localAppData + "\\Gravity\\job_history.json";
}

JobHistoryStore::JobHistoryStore(std::string filePath, std::size_t maxEntries)
    : filePath_(std::move(filePath)), maxEntries_(maxEntries) {}

std::vector<nlohmann::json> JobHistoryStore::Load() const {
    std::error_code existsError;
    if (!fs::exists(filePath_, existsError) || existsError) {
        return {};
    }

    std::ifstream input(filePath_, std::ios::binary);
    if (!input) {
        logging::Log::Warning("JobHistory",
                               "Could not open '" + filePath_ + "' for reading; returning empty history.");
        return {};
    }

    std::ostringstream contentStream;
    contentStream << input.rdbuf();

    try {
        const nlohmann::json parsed = nlohmann::json::parse(contentStream.str());
        if (!parsed.is_array()) {
            logging::Log::Warning("JobHistory", "'" + filePath_ + "' is not a JSON array; returning empty history.");
            return {};
        }
        return std::vector<nlohmann::json>(parsed.begin(), parsed.end());
    } catch (const nlohmann::json::exception& e) {
        logging::Log::Warning("JobHistory", "'" + filePath_ + "' is not valid JSON (" + e.what() +
                                                 "); returning empty history.");
        return {};
    }
}

void JobHistoryStore::Append(nlohmann::json snapshotJson) {
    std::vector<nlohmann::json> entries = Load();
    entries.push_back(std::move(snapshotJson));
    if (entries.size() > maxEntries_) {
        entries.erase(entries.begin(), entries.begin() + static_cast<long>(entries.size() - maxEntries_));
    }

    try {
        const fs::path targetPath(filePath_);
        const fs::path parentDir = targetPath.parent_path();
        if (!parentDir.empty()) {
            std::error_code createError;
            fs::create_directories(parentDir, createError);
            if (createError) {
                logging::Log::Warning("JobHistory", "Could not create '" + parentDir.string() +
                                                          "': " + createError.message());
                return;
            }
        }

        const fs::path tempPath = targetPath.string() + ".tmp";
        {
            std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
            if (!output) {
                logging::Log::Warning("JobHistory", "Could not open '" + tempPath.string() + "' for writing.");
                return;
            }
            // error_handler_t::replace: entries carry externally-influenced text (job
            // titles, paths) this process doesn't control the encoding of -- default
            // strict dump() would throw on invalid UTF-8, aborting this whole write (and,
            // uncaught further up the stack, crashing the process) rather than persisting
            // history with the offending byte substituted.
            output << nlohmann::json(entries).dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        }

        std::error_code renameError;
        fs::rename(tempPath, targetPath, renameError);
        if (renameError) {
            logging::Log::Warning("JobHistory", "Could not finalize '" + filePath_ + "': " + renameError.message());
        }
    } catch (const std::exception& e) {
        // Best-effort: history persistence must never disrupt the job-completion path
        // that calls this.
        logging::Log::Warning("JobHistory", std::string("Failed to append history entry: ") + e.what());
    }
}

}  // namespace mediatool::jobs
