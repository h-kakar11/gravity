#include "core/settings/PresetStore.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/errors/ErrorInfo.h"
#include "core/errors/MediaToolException.h"
#include "core/logging/Logger.h"

namespace fs = std::filesystem;

namespace mediatool::settings {

namespace {

std::string GetEnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : "";
}

}  // namespace

nlohmann::json Preset::ToJson() const {
    return nlohmann::json{{"id", id}, {"name", name}, {"kind", kind}, {"options", options}};
}

Preset Preset::FromJson(const nlohmann::json& json) {
    Preset preset;
    preset.id = json.at("id").get<std::string>();
    preset.name = json.at("name").get<std::string>();
    preset.kind = json.at("kind").get<std::string>();
    preset.options = json.value("options", nlohmann::json::object());
    return preset;
}

std::string DefaultPresetsFilePath() {
    const std::string localAppData = GetEnvOrEmpty("LOCALAPPDATA");
    if (localAppData.empty()) {
        return "Gravity\\presets.json";
    }
    return localAppData + "\\Gravity\\presets.json";
}

PresetStore::PresetStore(std::string filePath) : filePath_(std::move(filePath)) {}

std::vector<Preset> PresetStore::Load() const {
    std::error_code existsError;
    if (!fs::exists(filePath_, existsError) || existsError) {
        return {};
    }

    std::ifstream input(filePath_, std::ios::binary);
    if (!input) {
        logging::Log::Warning("PresetStore",
                               "Could not open '" + filePath_ + "' for reading; returning no presets.");
        return {};
    }

    std::ostringstream contentStream;
    contentStream << input.rdbuf();

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(contentStream.str());
    } catch (const nlohmann::json::exception& e) {
        logging::Log::Warning("PresetStore",
                               "'" + filePath_ + "' is not valid JSON (" + e.what() + "); returning no presets.");
        return {};
    }
    if (!parsed.is_array()) {
        logging::Log::Warning("PresetStore", "'" + filePath_ + "' is not a JSON array; returning no presets.");
        return {};
    }

    std::vector<Preset> presets;
    presets.reserve(parsed.size());
    for (const auto& entry : parsed) {
        try {
            presets.push_back(Preset::FromJson(entry));
        } catch (const std::exception& e) {
            // One malformed entry (e.g. from a future/older schema version) shouldn't
            // discard every other valid preset in the file.
            logging::Log::Warning("PresetStore", std::string("Skipping unreadable preset entry: ") + e.what());
        }
    }
    return presets;
}

void PresetStore::Save(const std::vector<Preset>& presets) {
    nlohmann::json array = nlohmann::json::array();
    for (const auto& preset : presets) array.push_back(preset.ToJson());

    const fs::path targetPath(filePath_);
    const fs::path parentDir = targetPath.parent_path();
    if (!parentDir.empty()) {
        std::error_code createError;
        fs::create_directories(parentDir, createError);
        if (createError) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_PRESETS_DIR_CREATE_FAILED", errors::ErrorCategory::PermissionError,
                "Could not create the presets directory.",
                "create_directories('" + parentDir.string() + "') failed: " + createError.message()));
        }
    }

    const fs::path tempPath = targetPath.string() + ".tmp";
    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_PRESETS_WRITE_FAILED", errors::ErrorCategory::PermissionError,
                "Could not write the presets file.", "Failed to open '" + tempPath.string() + "' for writing."));
        }
        output << array.dump(2);
    }

    std::error_code renameError;
    fs::rename(tempPath, targetPath, renameError);
    if (renameError) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_PRESETS_RENAME_FAILED", errors::ErrorCategory::PermissionError,
            "Could not finalize saving the presets file.",
            "rename('" + tempPath.string() + "' -> '" + targetPath.string() + "') failed: " + renameError.message()));
    }
}

}  // namespace mediatool::settings
