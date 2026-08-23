#pragma once

// The one exception type used across core/ and engines/. Every operation that can fail
// in a way the user or the IPC layer needs to know about throws this (never a bare
// std::runtime_error, never an errno left unwrapped) so it always carries a structured
// ErrorInfo. Callers at a job/IPC boundary catch MediaToolException and surface its
// Info() directly -- they never need to re-derive an ErrorCategory from a message string.

#include <stdexcept>
#include <utility>

#include "core/errors/ErrorInfo.h"

namespace mediatool::errors {

class MediaToolException : public std::runtime_error {
public:
    explicit MediaToolException(ErrorInfo info)
        : std::runtime_error(info.message), info_(std::move(info)) {}

    const ErrorInfo& Info() const { return info_; }

private:
    ErrorInfo info_;
};

}  // namespace mediatool::errors
