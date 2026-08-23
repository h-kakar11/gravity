#include "core/settings/MockSettingsStore.h"

#include <utility>

namespace mediatool::settings {

MockSettingsStore::MockSettingsStore() : settings_(Settings::Defaults()) {}

MockSettingsStore::MockSettingsStore(Settings initial) : settings_(std::move(initial)) {}

Settings MockSettingsStore::Load() { return settings_; }

void MockSettingsStore::Save(const Settings& settings) { settings_ = settings; }

}  // namespace mediatool::settings
