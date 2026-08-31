#pragma once

// Phase 1 settings persistence: a single pretty-printed JSON file, no database. The path
// is injected via the constructor (not hardcoded) so tests can point this at a temp file
// instead of the real user profile -- see DefaultSettingsFilePath() below for the path
// production code should actually use.

#include <mutex>
#include <string>

#include "core/settings/ISettingsStore.h"
#include "core/settings/Settings.h"

namespace mediatool::settings {

// The real production settings path: "%LOCALAPPDATA%\Gravity\settings.json". Reads
// LOCALAPPDATA from the environment; never throws.
std::string DefaultSettingsFilePath();

// The pre-rebrand path ("%LOCALAPPDATA%\MediaTool\settings.json"), consulted once by
// Load() as a one-time migration source when the current path has no file yet -- an
// existing user upgrading past the MediaTool->Gravity rename should not silently lose
// their settings. Never throws.
std::string LegacySettingsFilePath();

class JsonFileSettingsStore final : public ISettingsStore {
public:
    explicit JsonFileSettingsStore(std::string filePath, std::string legacyFilePath = "");

    // Returns Settings::Defaults() if the file doesn't exist yet -- that is the expected
    // first-run state, not an error. Unless the file exists, first checks the
    // constructor's `legacyFilePath` (if non-empty) and migrates from there instead if it
    // has a valid, readable settings file -- a one-time upgrade path, not consulted again
    // once a file exists at the primary path. Also returns Settings::Defaults() (logging
    // a warning, never throwing) if a file that IS found (primary or legacy) can't be
    // read, isn't valid JSON, is missing fields, or fails Settings::Validate() -- a
    // corrupt or stale settings file must never prevent the app from starting (spec/audit
    // #5). A corrupt file itself is left untouched on disk (not overwritten) so it can
    // still be inspected; it will only be replaced the next time Save() is called with a
    // valid Settings object.
    Settings Load() override;

    // Creates parent directories if needed, then writes via the atomic write pattern
    // (spec section 13): serialize to "<filePath>.tmp", then rename over `filePath_` so a
    // crash mid-write can never leave a corrupt or half-written settings file behind.
    void Save(const Settings& settings) override;

private:
    Settings LoadFrom(const std::string& path, bool& outExists);

    // Load() and Save() are no longer confined to the single IPC thread: handlers that do
    // I/O (inspectFile, inspectDownloadUrl) run on the request executor (see
    // app/core/main.cpp), and each of them reads settings. Serializing here means a read
    // concurrent with a save observes the file either fully before or fully after that
    // save, never a partial rename.
    mutable std::mutex mutex_;
    std::string filePath_;
    std::string legacyFilePath_;
};

}  // namespace mediatool::settings
