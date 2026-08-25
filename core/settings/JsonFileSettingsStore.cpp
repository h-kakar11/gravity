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
        return "MediaTool\\settings.json";
    }
    return localAppData + "\\MediaTool\\settings.json";
}

JsonFileSettingsStore::JsonFileSettingsStore(std::string filePath) : filePath_(std::move(filePath)) {}

Settings JsonFileSettingsStore::Load() {
    std::error_code existsError;
    const bool exists = fs::exists(filePath_, existsError);
    if (existsError || !exists) {
        return Settings::Defaults();
    }

    std::ifstream input(filePath_, std::ios::binary);
    if (!input) {
        logging::Log::Warning("Settings",
                               "Could not open '" + filePath_ + "' for reading; using defaults.");
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
        logging::Log::Warning("Settings", "Settings file '" + filePath_ +
                                               "' is not valid JSON (" + e.what() +
                                               "); using defaults.");
        return Settings::Defaults();
    } catch (const errors::MediaToolException& e) {
        logging::Log::Warning("Settings", "Settings file '" + filePath_ +
                                               "' failed validation (" + e.Info().details +
                                               "); using defaults.");
        return Settings::Defaults();
    }
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
        output << settings.ToJson().dump(2);
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
