#pragma once

// In-memory ISettingsStore for tests -- no disk I/O. Starts pre-populated with
// Settings::Defaults() so callers that Load() before ever Save()-ing see sane values,
// matching JsonFileSettingsStore's "missing file -> Defaults()" behavior.

#include "core/settings/ISettingsStore.h"
#include "core/settings/Settings.h"

namespace mediatool::settings {

class MockSettingsStore final : public ISettingsStore {
public:
    MockSettingsStore();
    explicit MockSettingsStore(Settings initial);

    Settings Load() override;
    void Save(const Settings& settings) override;

private:
    Settings settings_;
};

}  // namespace mediatool::settings
