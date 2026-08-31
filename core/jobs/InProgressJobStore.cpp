#include "core/jobs/InProgressJobStore.h"

#include <algorithm>
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

std::string DefaultInProgressJobsFilePath() {
    const std::string localAppData = GetEnvOrEmpty("LOCALAPPDATA");
    if (localAppData.empty()) {
        return "Gravity\\jobs_in_progress.json";
    }
    return localAppData + "\\Gravity\\jobs_in_progress.json";
}

InProgressJobStore::InProgressJobStore(std::string filePath) : filePath_(std::move(filePath)) {}

std::vector<nlohmann::json> InProgressJobStore::Load() const {
    std::error_code existsError;
    if (!fs::exists(filePath_, existsError) || existsError) {
        return {};
    }

    std::ifstream input(filePath_, std::ios::binary);
    if (!input) {
        logging::Log::Warning(
            "InProgressJobs", "Could not open '" + filePath_ + "' for reading; returning empty list.");
        return {};
    }

    std::ostringstream contentStream;
    contentStream << input.rdbuf();

    try {
        const nlohmann::json parsed = nlohmann::json::parse(contentStream.str());
        if (!parsed.is_array()) {
            logging::Log::Warning("InProgressJobs",
                                   "'" + filePath_ + "' is not a JSON array; returning empty list.");
            return {};
        }
        return std::vector<nlohmann::json>(parsed.begin(), parsed.end());
    } catch (const nlohmann::json::exception& e) {
        logging::Log::Warning("InProgressJobs", "'" + filePath_ + "' is not valid JSON (" +
                                                      std::string(e.what()) + "); returning empty list.");
        return {};
    }
}

void InProgressJobStore::WriteAll(const std::vector<nlohmann::json>& entries) const {
    try {
        const fs::path targetPath(filePath_);
        const fs::path parentDir = targetPath.parent_path();
        if (!parentDir.empty()) {
            std::error_code createError;
            fs::create_directories(parentDir, createError);
            if (createError) {
                logging::Log::Warning("InProgressJobs", "Could not create '" + parentDir.string() +
                                                              "': " + createError.message());
                return;
            }
        }

        const fs::path tempPath = targetPath.string() + ".tmp";
        {
            std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
            if (!output) {
                logging::Log::Warning("InProgressJobs",
                                       "Could not open '" + tempPath.string() + "' for writing.");
                return;
            }
            // error_handler_t::replace: entries carry externally-influenced text (job
            // titles, error messages) this process doesn't control the encoding of --
            // default strict dump() would throw on invalid UTF-8, aborting this whole
            // write rather than persisting with the offending byte substituted. Same
            // reasoning as JobHistoryStore::Append and main.cpp's WriteLine.
            output << nlohmann::json(entries).dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        }

        std::error_code renameError;
        fs::rename(tempPath, targetPath, renameError);
        if (renameError) {
            logging::Log::Warning("InProgressJobs",
                                   "Could not finalize '" + filePath_ + "': " + renameError.message());
        }
    } catch (const std::exception& e) {
        // Best-effort: this must never disrupt the job-transition path that calls it.
        logging::Log::Warning("InProgressJobs", std::string("Failed to write: ") + e.what());
    }
}

void InProgressJobStore::Upsert(nlohmann::json snapshotJson) {
    if (!snapshotJson.contains("id") || !snapshotJson.at("id").is_string()) {
        logging::Log::Warning("InProgressJobs", "Refusing to upsert a snapshot with no string 'id'.");
        return;
    }
    const std::string id = snapshotJson.at("id").get<std::string>();

    std::vector<nlohmann::json> entries = Load();
    const auto it = std::find_if(entries.begin(), entries.end(), [&](const nlohmann::json& entry) {
        return entry.value("id", std::string()) == id;
    });
    if (it != entries.end()) {
        *it = std::move(snapshotJson);
    } else {
        entries.push_back(std::move(snapshotJson));
    }
    WriteAll(entries);
}

void InProgressJobStore::Remove(const std::string& jobId) {
    std::vector<nlohmann::json> entries = Load();
    const std::size_t before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                  [&](const nlohmann::json& entry) {
                                      return entry.value("id", std::string()) == jobId;
                                  }),
                  entries.end());
    if (entries.size() != before) {
        WriteAll(entries);
    }
}

}  // namespace mediatool::jobs
