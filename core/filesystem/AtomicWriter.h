#pragma once

// "Write to a partial file, rename over the final path only on confirmed success"
// pattern (spec section 13). Prevents a crashed/cancelled/failed job from leaving a
// half-written file sitting at the destination the user expects to find output at.

#include <string>

namespace mediatool::filesystem {

class AtomicWriter {
public:
    // `finalPath`'s directory must already exist; this class only manages the
    // temp-file/final-file pair inside it, it doesn't create directories.
    explicit AtomicWriter(std::string finalPath);
    ~AtomicWriter();

    AtomicWriter(const AtomicWriter&) = delete;
    AtomicWriter& operator=(const AtomicWriter&) = delete;
    AtomicWriter(AtomicWriter&& other) noexcept;
    AtomicWriter& operator=(AtomicWriter&& other) noexcept;

    // Where the caller should actually write output, e.g. "video.processing.mp4" for
    // a final path of "video.mp4".
    const std::string& TemporaryPath() const { return temporaryPath_; }
    const std::string& FinalPath() const { return finalPath_; }

    // Renames TemporaryPath() over FinalPath(). Call exactly once, only after the write
    // to TemporaryPath() is confirmed complete and correct. Throws
    // errors::MediaToolException (ErrorCategory::EngineFailure) if the temp file is
    // missing or the rename fails.
    void Commit();

    bool IsCommitted() const { return committed_; }

private:
    std::string finalPath_;
    std::string temporaryPath_;
    bool committed_ = false;

    void MoveFrom(AtomicWriter& other) noexcept;
};

}  // namespace mediatool::filesystem
