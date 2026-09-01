#include "core/jobs/InProgressJobStore.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

#include "core/filesystem/PathUtils.h"
#include "core/logging/Logger.h"

namespace fs = std::filesystem;

namespace mediatool::jobs {

namespace {

constexpr const char* kLogTag = "InProgressJobs";

std::string GetEnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : "";
}

}  // namespace

std::string DefaultInProgressJobsFilePath() {
    const std::string localAppData = GetEnvOrEmpty("LOCALAPPDATA");
    if (localAppData.empty()) {
        return "Gravity\\in_progress_jobs.json";
    }
    return localAppData + "\\Gravity\\in_progress_jobs.json";
}

InProgressJobStore::InProgressJobStore(std::string filePath) : filePath_(std::move(filePath)) {}

std::vector<JobSpec> InProgressJobStore::Load() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return LoadLocked();
}

std::vector<JobSpec> InProgressJobStore::LoadLocked() const {
    std::error_code existsError;
    if (!fs::exists(filePath_, existsError) || existsError) {
        return {};
    }

    std::ifstream input(filePath_, std::ios::binary);
    if (!input) {
        logging::Log::Warning(kLogTag, "Could not open '" + filePath_ +
                                            "' for reading; starting with no recoverable jobs.");
        return {};
    }

    std::ostringstream contentStream;
    contentStream << input.rdbuf();

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(contentStream.str());
    } catch (const nlohmann::json::exception& e) {
        logging::Log::Warning(kLogTag, "'" + filePath_ + "' is not valid JSON (" + e.what() +
                                            "); starting with no recoverable jobs.");
        return {};
    }
    if (!parsed.is_array()) {
        logging::Log::Warning(kLogTag, "'" + filePath_ + "' is not a JSON array; starting with "
                                                          "no recoverable jobs.");
        return {};
    }

    std::vector<JobSpec> specs;
    specs.reserve(parsed.size());
    for (const nlohmann::json& entry : parsed) {
        try {
            JobSpec spec = JobSpec::FromJson(entry);
            if (spec.id.empty()) continue;
            specs.push_back(std::move(spec));
        } catch (const std::exception& e) {
            // One unreadable record -- written by an older build, or truncated by the
            // crash this file exists to survive -- must not cost the user the others.
            logging::Log::Warning(kLogTag,
                                   std::string("Skipping unreadable in-progress job entry: ") +
                                       e.what());
        }
    }
    return specs;
}

void InProgressJobStore::SaveLocked(const std::vector<JobSpec>& specs) const {
    try {
        const fs::path targetPath(filePath_);
        const fs::path parentDir = targetPath.parent_path();
        if (!parentDir.empty()) {
            std::error_code createError;
            fs::create_directories(parentDir, createError);
            if (createError) {
                logging::Log::Warning(kLogTag, "Could not create '" + parentDir.string() +
                                                    "': " + createError.message());
                return;
            }
        }

        nlohmann::json array = nlohmann::json::array();
        for (const JobSpec& spec : specs) {
            array.push_back(spec.ToJson());
        }

        const fs::path tempPath = filesystem::paths::UniqueTemporarySibling(targetPath.string());
        {
            std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
            if (!output) {
                logging::Log::Warning(kLogTag,
                                       "Could not open '" + tempPath.string() + "' for writing.");
                return;
            }
            // error_handler_t::replace for the same reason JobHistoryStore uses it: these
            // records carry paths and URLs this process does not control the encoding of,
            // and a strict dump() would throw mid-write rather than persisting the record
            // with the offending byte substituted.
            output << array.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        }

        std::error_code renameError;
        fs::rename(tempPath, targetPath, renameError);
        if (renameError) {
            logging::Log::Warning(kLogTag,
                                   "Could not finalize '" + filePath_ + "': " + renameError.message());
            std::error_code removeError;
            fs::remove(tempPath, removeError);
        }
    } catch (const std::exception& e) {
        // Best-effort: this runs on the job submission and job completion paths, neither
        // of which may be disrupted by a persistence failure.
        logging::Log::Warning(kLogTag, std::string("Failed to write in-progress jobs: ") + e.what());
    }
}

void InProgressJobStore::Put(const JobSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobSpec> specs = LoadLocked();
    auto existing = std::find_if(specs.begin(), specs.end(),
                                  [&](const JobSpec& s) { return s.id == spec.id; });
    if (existing != specs.end()) {
        *existing = spec;  // in place, so submission order survives an update
    } else {
        specs.push_back(spec);
    }
    SaveLocked(specs);
}

void InProgressJobStore::SetArtifactLocation(const JobId& id,
                                              const JobArtifactLocation& artifact) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobSpec> specs = LoadLocked();
    auto existing =
        std::find_if(specs.begin(), specs.end(), [&](const JobSpec& s) { return s.id == id; });
    if (existing == specs.end()) {
        return;
    }
    existing->artifact = artifact;
    SaveLocked(specs);
}

void InProgressJobStore::Remove(const JobId& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<JobSpec> specs = LoadLocked();
    const auto removed =
        std::remove_if(specs.begin(), specs.end(), [&](const JobSpec& s) { return s.id == id; });
    if (removed == specs.end()) {
        return;  // nothing to write: not ours, or already gone
    }
    specs.erase(removed, specs.end());
    SaveLocked(specs);
}

void InProgressJobStore::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    SaveLocked({});
}

}  // namespace mediatool::jobs
