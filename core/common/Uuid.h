#pragma once

#include <string>

namespace mediatool::common {

// RFC 4122 version-4 UUID, lowercase hex, e.g. "3fa85f64-5717-4562-b3fc-2c963f66afa6".
// Shared by anything that needs a locally-unique id -- core/jobs/JobId.cpp prefixes it with
// "job-", core/settings presets prefix it with "preset-".
std::string GenerateUuidV4();

}  // namespace mediatool::common
