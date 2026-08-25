#pragma once

// Phase 1 settings persistence: a single pretty-printed JSON file, no database. The path
// is injected via the constructor (not hardcoded) so tests can point this at a temp file
// instead of the real user profile -- see DefaultSettingsFilePath() below for the path
// production code should actually use.

#include <string>

#include "core/settings/ISettingsStore.h"
#include "core/settings/Settings.h"

namespace mediatool::settings {

// The real production settings path: "%LOCALAPPDATA%\MediaTool\settings.json". Reads
// LOCALAPPDATA from the environment; never throws.
std::string DefaultSettingsFilePath();

class JsonFileSettingsStore final : public ISettingsStore {
public:
    explicit JsonFileSettingsStore(std::string filePath);

    // Returns Settings::Defaults() if the file doesn't exist yet -- that is the expected
    // first-run state, not an error. Also returns Settings::Defaults() (logging a
    // warning, never throwing) if the file exists but can't be read, isn't valid JSON, is
    // missing fields, or fails Settings::Validate() -- a corrupt or stale settings file
    // must never prevent the app from starting (spec/audit #5). The corrupt file itself
    // is left untouched on disk (not overwritten) so it can still be inspected; it will
    // only be replaced the next time Save() is called with a valid Settings object.
    Settings Load() override;

    // Creates parent directories if needed, then writes via the atomic write pattern
    // (spec section 13): serialize to "<filePath>.tmp", then rename over `filePath_` so a
    // crash mid-write can never leave a corrupt or half-written settings file behind.
    void Save(const Settings& settings) override;

private:
    std::string filePath_;
};

}  // namespace mediatool::settings
