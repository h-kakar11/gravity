#include "core/jobs/JobArtifactCleanup.h"

#include "core/filesystem/FilenameSanitizer.h"
#include "core/filesystem/PathUtils.h"

namespace mediatool::jobs {

void CleanupJobArtifacts(filesystem::IFileSystem& fileSystem, const std::string& outputDirectory,
                          const std::string& filenameBase) {
    for (const auto& name : fileSystem.ListDirectory(outputDirectory)) {
        if (!filesystem::IsJobArtifactOf(filenameBase, name)) {
            continue;  // not this job's -- e.g. an unrelated file that merely shares a prefix
        }
        try {
            // DeleteFile(), never Delete(): even a correctly-scoped match must never be
            // allowed to recursively remove a directory tree.
            fileSystem.DeleteFile(filesystem::paths::Join(outputDirectory, name));
        } catch (...) {
            // Best-effort cleanup; a cleanup failure must never mask the real job error.
        }
    }
}

}  // namespace mediatool::jobs
