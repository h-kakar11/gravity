#pragma once

// Settings persistence abstraction (spec section 23). JsonFileSettingsStore is the
// Phase 1 implementation (a plain JSON file, no database). Load() must never throw for
// "file doesn't exist yet" -- it returns Settings::Defaults() in that case; it may throw
// errors::MediaToolException for a genuinely corrupt/unreadable file.

#include "core/settings/Settings.h"

namespace mediatool::settings {

class ISettingsStore {
public:
    virtual ~ISettingsStore() = default;
    virtual Settings Load() = 0;
    virtual void Save(const Settings& settings) = 0;
};

}  // namespace mediatool::settings
