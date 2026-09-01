#include "core/media/DeferredOperations.h"

#include <algorithm>

namespace mediatool::media {

namespace {

// Kept beside the token so a reason can never go missing for a deferred operation: the
// table IS the deferral list, and DeferredOperations() is derived from it.
struct Deferral {
    const char* operation;
    const char* reason;
};

// Wording is user-facing (it reaches a disabled control's tooltip and an ErrorInfo
// message), so it says what the user cannot do and why, not which C++ method is missing.
constexpr Deferral kDeferrals[] = {
    {kExtractAudioOperation,
     "Extracting a standalone audio track is not available in this build yet. Converting "
     "the file to an audio format (mp3, m4a, wav, ...) does the same thing today."},
    {kExtractFramesOperation,
     "Extracting still frames is not available in this build yet."},
};

const Deferral* Find(const std::string& operation) {
    for (const Deferral& deferral : kDeferrals) {
        if (operation == deferral.operation) {
            return &deferral;
        }
    }
    return nullptr;
}

}  // namespace

const std::vector<std::string>& DeferredOperations() {
    static const std::vector<std::string> operations = [] {
        std::vector<std::string> result;
        for (const Deferral& deferral : kDeferrals) {
            result.emplace_back(deferral.operation);
        }
        return result;
    }();
    return operations;
}

bool IsDeferredOperation(const std::string& operation) { return Find(operation) != nullptr; }

std::string DeferralReason(const std::string& operation) {
    const Deferral* deferral = Find(operation);
    return deferral ? deferral->reason : std::string();
}

errors::ErrorInfo MakeNotImplementedError(const std::string& operation) {
    const Deferral* deferral = Find(operation);
    const std::string message =
        deferral ? deferral->reason : operation + " is not implemented in this build.";
    return errors::ErrorInfo::Make(kNotImplementedErrorCode,
                                    errors::ErrorCategory::UnsupportedFormat, message,
                                    "operation=" + operation +
                                        " (see core/media/DeferredOperations.h for the "
                                        "deferral contract)",
                                    /*recoverable=*/false);
}

}  // namespace mediatool::media
