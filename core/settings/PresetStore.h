#pragma once

// Persists named option presets ("save this Convert configuration as 'Gaming'") --
// Phase 3 builds the schema/store only; Phase 4.6 wires the listPresets/savePreset/
// deletePreset IPC commands and the frontend UI on top of it. Follows the same atomic
// -write-then-rename, fallback-to-empty-on-corrupt pattern as
// core/settings/JsonFileSettingsStore.h and core/jobs/JobHistoryStore.h.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace mediatool::settings {

struct Preset {
    std::string id;
    std::string name;
    std::string kind;  // "DOWNLOAD" | "CONVERSION" | "COMPRESSION"
    // The full options JSON for that kind -- a DownloadJobParams-shaped or
    // MediaProcessingOptions-shaped object, opaque to this store.
    nlohmann::json options;

    nlohmann::json ToJson() const;
    static Preset FromJson(const nlohmann::json& json);
};

// "%LOCALAPPDATA%\Gravity\presets.json". Reads LOCALAPPDATA from the environment; never
// throws.
std::string DefaultPresetsFilePath();

class PresetStore {
public:
    explicit PresetStore(std::string filePath);

    // Returns an empty vector (never throws, just logs a warning) if the file doesn't
    // exist yet or is corrupt/unreadable -- presets are a convenience feature, never
    // allowed to affect whether the app starts. A preset entry that individually fails to
    // parse is skipped rather than invalidating the whole file.
    std::vector<Preset> Load() const;

    // Overwrites the whole preset list atomically (mirrors JsonFileSettingsStore::Save's
    // temp-file-then-rename pattern).
    void Save(const std::vector<Preset>& presets);

private:
    std::string filePath_;
};

}  // namespace mediatool::settings
