#pragma once

// Thin facade over spdlog (spec section 25) -- nothing else in the codebase should
// #include <spdlog/spdlog.h> directly; go through Log::Debug/Info/Warning/Error instead.
// Every call also forwards a LogEvent through an optional sink (see SetEventSink) so a
// later integration pass can pipe logs onto the same EventBus that carries job events,
// without this module depending on EventBus itself.

#include <functional>
#include <string>

#include "core/events/Event.h"

namespace mediatool::logging {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error,
};

// %LOCALAPPDATA%\Gravity\logs on Windows; falls back to "./logs" if LOCALAPPDATA is
// unset (e.g. a stripped-down test environment). Do not hardcode this path elsewhere.
std::string DefaultLogDirectory();

class Logger {
public:
    // Sets up a rotating file sink at logFilePath (parent directories created if
    // missing) plus a console sink, and sets the minimum level for both. Calling again
    // replaces the previous sinks -- safe to call once at process startup.
    static void Init(const std::string& logFilePath, LogLevel level);

    // Registered by a later integration pass to receive a LogEvent for every Log::* call
    // that passes the current level filter, in addition to the spdlog sinks. Pass an
    // empty std::function to detach. No-op (unset) by default.
    static void SetEventSink(std::function<void(events::Event)> sink);

    static void Write(LogLevel level, const std::string& subsystem, const std::string& message);
};

// Preferred call surface: mediatool::logging::Log::Info("JobManager", "...").
namespace Log {
void Debug(const std::string& subsystem, const std::string& message);
void Info(const std::string& subsystem, const std::string& message);
void Warning(const std::string& subsystem, const std::string& message);
void Error(const std::string& subsystem, const std::string& message);
}  // namespace Log

}  // namespace mediatool::logging
