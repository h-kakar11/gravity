#include "core/queue/QueuePersistence.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "core/errors/MediaToolException.h"
#include "core/filesystem/AtomicWriter.h"

namespace stdfs = std::filesystem;

namespace mediatool::queue {

namespace {

using errors::ErrorCategory;
using errors::ErrorInfo;
using errors::MediaToolException;

std::string GetEnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

std::int64_t NowMsSinceEpoch() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

nlohmann::json PersistedQueue::ToJson() const {
    nlohmann::json json;
    json["schemaVersion"] = schemaVersion;
    json["runState"] = ToWireString(runState);
    json["maxConcurrency"] = maxConcurrency;
    json["pendingOrder"] = pendingOrder;
    nlohmann::json array = nlohmann::json::array();
    for (const auto& record : records) array.push_back(record.ToJson());
    json["records"] = array;
    return json;
}

QueuePersistence::QueuePersistence(std::string stateFilePath)
    : stateFilePath_(std::move(stateFilePath)) {}

std::string QueuePersistence::DefaultStateFilePath() {
    // Built through std::filesystem rather than by concatenating "\\": the product ships on
    // Windows, where this yields the same backslash path it always did, but the core is also
    // built and run on Linux for development and CI, and a literal backslash there produces
    // one absurdly-named file instead of a directory (spec section 11: never hand-assemble
    // path separators).
    const std::string localAppData = GetEnvOrEmpty("LOCALAPPDATA");
    const stdfs::path base = localAppData.empty()
                                 // Mirrors DefaultSettingsFilePath()'s handling of a missing
                                 // LOCALAPPDATA: stay relative rather than throw, since this
                                 // may be called just to display a path.
                                 ? stdfs::path()
                                 : stdfs::path(localAppData);
    return (base / "MediaTool" / "queue.json").make_preferred().string();
}

void QueuePersistence::Save(const PersistedQueue& queue) const {
    const stdfs::path path(stateFilePath_);
    const stdfs::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        stdfs::create_directories(parent, ec);
        if (ec && !stdfs::exists(parent)) {
            throw MediaToolException(ErrorInfo::Make(
                "E_QUEUE_STATE_DIR_UNUSABLE", ErrorCategory::PermissionError,
                "The queue state folder could not be created.",
                "path=" + parent.string() + " error=" + ec.message()));
        }
    }

    // Write-temp / rename-over. The temp file is removed by AtomicWriter's destructor on
    // any path that does not reach Commit(), so a failure here cannot leave debris behind.
    filesystem::AtomicWriter writer(stateFilePath_);
    {
        std::ofstream output(writer.TemporaryPath(), std::ios::binary | std::ios::trunc);
        if (!output) {
            throw MediaToolException(ErrorInfo::Make(
                "E_QUEUE_STATE_WRITE_FAILED", ErrorCategory::PermissionError,
                "The queue state file could not be written.",
                "path=" + writer.TemporaryPath()));
        }
        output << queue.ToJson().dump(2) << "\n";
        output.flush();
        if (!output) {
            throw MediaToolException(ErrorInfo::Make(
                "E_QUEUE_STATE_WRITE_FAILED", ErrorCategory::DiskSpaceError,
                "The queue state file could not be written completely.",
                "path=" + writer.TemporaryPath()));
        }
    }  // closing the stream flushes to the OS before the rename below
    writer.Commit();
}

std::optional<std::string> QueuePersistence::Quarantine() const {
    const std::string target =
        stateFilePath_ + ".corrupt-" + std::to_string(NowMsSinceEpoch());
    std::error_code ec;
    stdfs::rename(stateFilePath_, target, ec);
    if (ec) return std::nullopt;
    return target;
}

LoadOutcome QueuePersistence::Load() const {
    LoadOutcome outcome;

    std::error_code ec;
    if (!stdfs::exists(stateFilePath_, ec) || ec) {
        outcome.status = LoadOutcome::Status::NotPresent;
        return outcome;
    }

    // Every failure below funnels into the same recovery: quarantine the file, report why,
    // start empty. Never throw -- a bad queue file must not stop the app from launching.
    const auto recover = [&outcome, this](const std::string& diagnostic) {
        outcome.status = LoadOutcome::Status::Recovered;
        outcome.queue = PersistedQueue{};
        outcome.diagnostic = diagnostic;
        outcome.quarantinedPath = Quarantine();
        return outcome;
    };

    std::string contents;
    try {
        std::ifstream input(stateFilePath_, std::ios::binary);
        if (!input) return recover("the queue state file could not be opened for reading");
        contents.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    } catch (const std::exception& ex) {
        return recover(std::string("reading the queue state file failed: ") + ex.what());
    }

    if (contents.find_first_not_of(" \t\r\n") == std::string::npos) {
        return recover("the queue state file is empty");
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(contents);
    } catch (const std::exception& ex) {
        // Covers both outright garbage and a file truncated mid-write by a crash.
        return recover(std::string("the queue state file is not valid JSON: ") + ex.what());
    }

    if (!parsed.is_object()) return recover("the queue state file is not a JSON object");

    int schemaVersion = 0;
    if (parsed.contains("schemaVersion") && parsed["schemaVersion"].is_number_integer())
        schemaVersion = parsed["schemaVersion"].get<int>();
    if (schemaVersion <= 0) return recover("the queue state file has no usable schemaVersion");
    if (schemaVersion > kQueueSchemaVersion) {
        // Written by a newer build. Reading it would mean guessing at fields we do not
        // understand, so keep it intact and start clean rather than corrupt the user's real
        // state by writing a downgraded version over it.
        return recover("the queue state file was written by a newer version (schemaVersion " +
                       std::to_string(schemaVersion) + ", this build understands " +
                       std::to_string(kQueueSchemaVersion) + ")");
    }
    // schemaVersion < kQueueSchemaVersion would be migrated here. Version 1 is the first
    // format, so there is nothing to migrate from yet; JobRecord::FromJson already tolerates
    // missing fields, which is what a v1 -> v2 migration would mostly consist of.

    PersistedQueue queue;
    queue.schemaVersion = schemaVersion;

    if (parsed.contains("runState") && parsed["runState"].is_string()) {
        try {
            queue.runState = QueueRunStateFromWireString(parsed["runState"].get<std::string>());
        } catch (const MediaToolException&) {
            queue.runState = QueueRunState::Running;  // unknown enum -> safe default
        }
    }
    if (parsed.contains("maxConcurrency") && parsed["maxConcurrency"].is_number_unsigned()) {
        const auto value = parsed["maxConcurrency"].get<std::uint64_t>();
        // Clamped rather than trusted: this value comes back from a file a user can edit.
        queue.maxConcurrency = static_cast<std::size_t>(value == 0 ? 1 : std::min<std::uint64_t>(value, 32));
    }

    if (parsed.contains("records") && parsed["records"].is_array()) {
        for (const auto& entry : parsed["records"]) {
            try {
                queue.records.push_back(JobRecord::FromJson(entry));
            } catch (const MediaToolException&) {
                // One unreadable entry must not cost the user the whole queue -- skip it and
                // keep the rest. Counted in the diagnostic below.
                continue;
            }
        }
        const auto skipped = parsed["records"].size() - queue.records.size();
        if (skipped > 0) {
            outcome.diagnostic = std::to_string(skipped) +
                                 " queue entries were unreadable and were dropped";
        }
    }

    if (parsed.contains("pendingOrder") && parsed["pendingOrder"].is_array()) {
        for (const auto& id : parsed["pendingOrder"]) {
            if (id.is_string() && !id.get<std::string>().empty())
                queue.pendingOrder.push_back(id.get<std::string>());
        }
    }

    outcome.status = LoadOutcome::Status::Loaded;
    outcome.queue = std::move(queue);
    return outcome;
}

std::vector<jobs::JobId> ApplyRestartRecovery(std::vector<JobRecord>& records) {
    std::vector<jobs::JobId> recovered;
    for (JobRecord& record : records) {
        if (!jobs::IsExecutingState(record.state)) continue;

        record.state = jobs::JobState::Failed;
        record.finishedAtMs.reset();
        record.lastRetryReason = "interrupted by an unexpected shutdown";
        ++record.revision;
        recovered.push_back(record.id);
    }
    return recovered;
}

}  // namespace mediatool::queue
