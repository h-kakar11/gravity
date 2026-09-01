#pragma once

// The single source of truth for operations this codebase *names* but does not run.
//
// Two vocabularies used to disagree about what the app can do. IMediaEngine
// (core/media/IMediaEngine.h) declares ExtractAudio/ExtractFrames and FFmpegEngine
// throws E_NOT_IMPLEMENTED from both; filesystem::CapabilitiesFor() independently
// advertised "extractAudio"/"extractFrames" to the frontend for every video file. The
// frontend therefore had no way to distinguish an operation it may offer from one that
// is guaranteed to fail, and the only way to find out was to run a job and read the
// error.
//
// So the deferral is declared once, here, and both vocabularies read it:
// filesystem::DeferredCapabilitiesFor() reports it up front (so a control can be
// disabled with a reason rather than offered and failed), and every engine that is asked
// to perform one throws MakeNotImplementedError() -- the same code, category and wording
// at both layers. The contract is exactly:
//
//   * An operation in CapabilitiesFor() will be attempted for real.
//   * An operation in DeferredCapabilitiesFor() fails with E_NOT_IMPLEMENTED
//     (UNSUPPORTED_FORMAT, recoverable=false) and never partially runs.
//
// Implementing one of these is deleting its entry here and watching the tests that
// assert the deferral fail -- deliberately, so the deferral cannot rot into a lie.

#include <string>
#include <vector>

#include "core/errors/ErrorInfo.h"

namespace mediatool::media {

// The one error code every layer uses for a deferred operation. Referenced by
// docs/ipc-contract.md.
inline constexpr const char* kNotImplementedErrorCode = "E_NOT_IMPLEMENTED";

// Operation tokens, spelled exactly as the capability vocabulary in
// core/filesystem/FileInfo.h spells them.
inline constexpr const char* kExtractAudioOperation = "extractAudio";
inline constexpr const char* kExtractFramesOperation = "extractFrames";

// Every deferred operation token, in a stable order.
const std::vector<std::string>& DeferredOperations();

// True if `operation` is declared somewhere in the codebase but deliberately not
// implemented. Case-sensitive; `operation` is a capability token, not a method name.
bool IsDeferredOperation(const std::string& operation);

// User-facing explanation of why `operation` is deferred -- what the frontend shows next
// to a disabled control, and the `message` of the error the engines throw. Empty for an
// operation that is not deferred.
std::string DeferralReason(const std::string& operation);

// The ErrorInfo every layer throws when asked to perform a deferred operation. Accepts a
// non-deferred name too (an engine method that simply is not written yet), in which case
// it produces the same shape with a generic reason -- a caller must never have to choose
// between two spellings of "not implemented".
errors::ErrorInfo MakeNotImplementedError(const std::string& operation);

}  // namespace mediatool::media
