#include "core/settings/JsonFileSettingsStore.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

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

std::string DefaultSettingsFilePath() {
    const std::string localAppData = GetEnvOrEmpty("LOCALAPPDATA");
    if (localAppData.empty()) {
        // No sensible absolute fallback -- keep this relative rather than throwing, since
        // this function itself must never throw (callers may invoke it just to display a
        // path in a settings UI).
        return "Gravity\\settings.json";
    }
    return localAppData + "\\Gravity\\settings.json";
}

std::string LegacySettingsFilePath() {
    const std::string localAppData = GetEnvOrEmpty("LOCALAPPDATA");
    if (localAppData.empty()) {
        return "MediaTool\\settings.json";
    }
    return localAppData + "\\MediaTool\\settings.json";
}

JsonFileSettingsStore::JsonFileSettingsStore(std::string filePath, std::string legacyFilePath)
    : filePath_(std::move(filePath)), legacyFilePath_(std::move(legacyFilePath)) {}

Settings JsonFileSettingsStore::LoadFrom(const std::string& path, bool& outExists) {
    std::error_code existsError;
    outExists = fs::exists(path, existsError) && !existsError;
    if (!outExists) {
        return Settings::Defaults();
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        logging::Log::Warning("Settings", "Could not open '" + path + "' for reading; using defaults.");
        return Settings::Defaults();
    }

    std::ostringstream contentStream;
    contentStream << input.rdbuf();
    const std::string content = contentStream.str();

    // A settings file that is unreadable JSON, missing fields, or fails Settings::Validate()
    // (e.g. a hand-edited or out-of-range value, or a file written by a future/older
    // version) must never stop the app from starting -- fall back to defaults rather than
    // throwing. The file itself is left alone on disk, not overwritten, so it remains
    // available to inspect; only a subsequent successful Save() replaces it.
    try {
        const nlohmann::json parsed = nlohmann::json::parse(content);
        return Settings::FromJson(parsed);
    } catch (const nlohmann::json::exception& e) {
        logging::Log::Warning("Settings",
                               "Settings file '" + path + "' is not valid JSON (" + e.what() +
                                   "); using defaults.");
        return Settings::Defaults();
    } catch (const errors::MediaToolException& e) {
        logging::Log::Warning("Settings", "Settings file '" + path + "' failed validation (" +
                                               e.Info().details + "); using defaults.");
        return Settings::Defaults();
    }
}

Settings JsonFileSettingsStore::Load() {
    bool exists = false;
    Settings settings = LoadFrom(filePath_, exists);
    if (exists) {
        return settings;
    }

    if (!legacyFilePath_.empty()) {
        bool legacyExists = false;
        Settings legacy = LoadFrom(legacyFilePath_, legacyExists);
        if (legacyExists) {
            logging::Log::Info("Settings", "Migrated settings from legacy path '" + legacyFilePath_ +
                                                "' (no file yet at '" + filePath_ + "').");
            return legacy;
        }
    }

    return settings;  // Settings::Defaults(), from the primary-path lookup above
}

void JsonFileSettingsStore::Save(const Settings& settings) {
    const fs::path targetPath(filePath_);
    const fs::path parentDir = targetPath.parent_path();

    if (!parentDir.empty()) {
        std::error_code createError;
        fs::create_directories(parentDir, createError);
        if (createError) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_SETTINGS_DIR_CREATE_FAILED", errors::ErrorCategory::PermissionError,
                "Could not create the settings directory.",
                "create_directories('" + parentDir.string() + "') failed: " + createError.message()));
        }
    }

    const fs::path tempPath = targetPath.string() + ".tmp";

    {
        std::ofstream output(tempPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_SETTINGS_WRITE_FAILED", errors::ErrorCategory::PermissionError,
                "Could not write the settings file.",
                "Failed to open '" + tempPath.string() + "' for writing."));
        }
        // error_handler_t::replace: settings paths are user-supplied and this process
        // doesn't control their encoding -- default strict dump() would throw on invalid
        // UTF-8, uncaught up to the settings-load/save boundary.
        output << settings.ToJson().dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
        output.flush();
        if (!output) {
            throw errors::MediaToolException(errors::ErrorInfo::Make(
                "E_SETTINGS_WRITE_FAILED", errors::ErrorCategory::PermissionError,
                "Could not write the settings file.",
                "Write to '" + tempPath.string() + "' failed mid-stream."));
        }
    }

    std::error_code renameError;
    fs::rename(tempPath, targetPath, renameError);
    if (renameError) {
        throw errors::MediaToolException(errors::ErrorInfo::Make(
            "E_SETTINGS_RENAME_FAILED", errors::ErrorCategory::PermissionError,
            "Could not finalize saving the settings file.",
            "rename('" + tempPath.string() + "' -> '" + targetPath.string() + "') failed: " +
                renameError.message()));
    }
}

}  // namespace mediatool::settings
