#include "core/logging/Logger.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace mediatool::logging {

namespace {

constexpr std::size_t kMaxLogFileBytes = 5 * 1024 * 1024;
constexpr std::size_t kMaxRotatedFiles = 3;

std::mutex& SinkMutex() {
    static std::mutex mutex;
    return mutex;
}

std::function<void(events::Event)>& EventSink() {
    static std::function<void(events::Event)> sink;
    return sink;
}

// Guards currentLevel_ the same way SinkMutex guards the event sink, since Init() and
// Write() can race across threads.
LogLevel& CurrentLevelRef() {
    static LogLevel level = LogLevel::Info;
    return level;
}

spdlog::level::level_enum ToSpdlogLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return spdlog::level::debug;
        case LogLevel::Info:
            return spdlog::level::info;
        case LogLevel::Warning:
            return spdlog::level::warn;
        case LogLevel::Error:
            return spdlog::level::err;
    }
    return spdlog::level::info;
}

std::string ToWireLevel(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warning:
            return "WARNING";
        case LogLevel::Error:
            return "ERROR";
    }
    return "INFO";
}

}  // namespace

std::string DefaultLogDirectory() {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData != nullptr && *localAppData != '\0') {
        return (std::filesystem::path(localAppData) / "MediaTool" / "logs").string();
    }
    return "./logs";
}

void Logger::Init(const std::string& logFilePath, LogLevel level) {
    const std::filesystem::path path(logFilePath);
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }

    auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        logFilePath, kMaxLogFileBytes, kMaxRotatedFiles);
    // stderr, NOT stdout: mediatool-core's stdout is the NDJSON IPC channel
    // (docs/ipc-contract.md) and must never carry anything else -- a single Log::Info
    // call during RunIpcLoop() would otherwise interleave a plain-text line into the
    // protocol stream mid-session (found via manual Phase 2 integration testing: the
    // very first "IPC loop starting" info log corrupted the first stdout line every
    // time). Python's downloader.py already follows this same stdout-is-protocol-only
    // convention for the same reason -- see docs/protocols/downloader.md.
    auto consoleSink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

    auto logger = std::make_shared<spdlog::logger>(
        "mediatool", spdlog::sinks_init_list{fileSink, consoleSink});
    logger->set_pattern("%Y-%m-%d %H:%M:%S.%e [%l] %v");
    logger->set_level(ToSpdlogLevel(level));
    logger->flush_on(spdlog::level::warn);

    spdlog::set_default_logger(logger);

    std::lock_guard<std::mutex> lock(SinkMutex());
    CurrentLevelRef() = level;
}

void Logger::SetEventSink(std::function<void(events::Event)> sink) {
    std::lock_guard<std::mutex> lock(SinkMutex());
    EventSink() = std::move(sink);
}

void Logger::Write(LogLevel level, const std::string& subsystem, const std::string& message) {
    const std::string line = "[" + subsystem + "] " + message;
    // "{}" as the literal format string, `line` as its argument -- never pass a
    // caller-controlled string as the format string itself, since log messages
    // routinely contain '{'/'}' (JSON snippets, ffmpeg output) that fmt would otherwise
    // try to parse as format specifiers and throw on.
    spdlog::log(ToSpdlogLevel(level), "{}", line);

    std::function<void(events::Event)> sinkCopy;
    LogLevel currentLevel;
    {
        std::lock_guard<std::mutex> lock(SinkMutex());
        sinkCopy = EventSink();
        currentLevel = CurrentLevelRef();
    }
    // Mirror spdlog's own level filter so a Debug call doesn't reach the UI as a toast
    // when the configured level is, say, Info.
    if (!sinkCopy || level < currentLevel) {
        return;
    }

    nlohmann::json data{
        {"level", ToWireLevel(level)},
        {"message", message},
        {"subsystem", subsystem},
    };
    sinkCopy(events::MakeEvent(events::EventType::LogEvent, std::move(data)));
}

namespace Log {

void Debug(const std::string& subsystem, const std::string& message) {
    Logger::Write(LogLevel::Debug, subsystem, message);
}

void Info(const std::string& subsystem, const std::string& message) {
    Logger::Write(LogLevel::Info, subsystem, message);
}

void Warning(const std::string& subsystem, const std::string& message) {
    Logger::Write(LogLevel::Warning, subsystem, message);
}

void Error(const std::string& subsystem, const std::string& message) {
    Logger::Write(LogLevel::Error, subsystem, message);
}

}  // namespace Log

}  // namespace mediatool::logging
