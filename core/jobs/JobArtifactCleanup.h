#pragma once

// Shared by every job type that allocates an output filename and must clean up after a
// failure (#3): DownloadJob and, from Phase 2 on, MediaProcessingJob. Extracted here so
// the fixed, safety-critical cleanup logic (core/filesystem/FilenameSanitizer.h's
// IsJobArtifactOf, plus IFileSystem::DeleteFile rather than the recursive Delete) exists
// in exactly one place rather than being duplicated per job type.

#include <string>

#include "core/filesystem/IFileSystem.h"

namespace mediatool::jobs {

// Best-effort: deletes every entry in `outputDirectory` that
// filesystem::IsJobArtifactOf recognizes as belonging to `filenameBase` -- never a bare
// prefix match, and never a recursive directory delete (uses IFileSystem::DeleteFile, not
// Delete). Never throws; a cleanup failure must never mask the real job error that
// triggered the cleanup in the first place.
void CleanupJobArtifacts(filesystem::IFileSystem& fileSystem, const std::string& outputDirectory,
                          const std::string& filenameBase);

}  // namespace mediatool::jobs
