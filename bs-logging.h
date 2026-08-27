// bs-logging.h — Cross-platform structured operational logging for BridgeSessions
//
// Provides bs::log::init() / shutdown() / get() backed by spdlog with:
//   - Platform-aware log file paths (macOS, Linux, Windows)
//   - Daily rotation: max 5MB, keep 5 backups
//   - JSON file format (machine-parseable) + human-readable console (foreground only)
//   - Named loggers: "daemon", "mesh", "cua-helper", "shell", etc.
//
// This is SEPARATE from log_event() (bs-protocol.h), which writes JSON protocol
// events to bs-mesh.log. This harness is for OPERATIONAL logs: daemon lifecycle,
// config errors, CUA helper, peer connection state.

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async.h>
#include <spdlog/sinks/sink.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace bs::log {

inline std::string redact(std::string text) {
    auto redact_value = [&](std::string_view marker) {
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::string needle(marker);
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        size_t pos = 0;
        while ((pos = lower.find(needle, pos)) != std::string::npos) {
            const size_t begin = pos + marker.size();
            size_t end = begin;
            while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
                   text[end] != '&' && text[end] != ';' && text[end] != ',' &&
                   text[end] != '"' && text[end] != '\'') ++end;
            text.replace(begin, end - begin, "[REDACTED]");
            lower.replace(begin, end - begin, "[redacted]");
            pos = begin + 10;
        }
    };
    for (std::string_view marker : {"token=", "password=", "passwd=", "api_key=", "secret=",
                                    "authorization: bearer "}) redact_value(marker);
    const std::string begin_marker = "-----BEGIN PRIVATE KEY-----";
    const std::string end_marker = "-----END PRIVATE KEY-----";
    size_t pos = 0;
    while ((pos = text.find(begin_marker, pos)) != std::string::npos) {
        size_t end = text.find(end_marker, pos + begin_marker.size());
        end = end == std::string::npos ? text.size() : end + end_marker.size();
        text.replace(pos, end - pos, "[REDACTED PRIVATE KEY]");
        pos += 22;
    }
    return text;
}

// ── Constants ────────────────────────────────────────────────────
inline constexpr size_t kMaxFileSize   = 5 * 1024 * 1024;  // 5 MB
inline constexpr size_t kMaxFiles      = 5;                 // 5 rotated backups
inline constexpr size_t kQueueSize     = 8192;              // async queue items

// ── Internal state ───────────────────────────────────────────────
namespace detail {

class RedactingSink final : public spdlog::sinks::sink {
public:
    explicit RedactingSink(spdlog::sink_ptr target) : target_(std::move(target)) {}
    void log(const spdlog::details::log_msg& msg) override {
        std::string clean = redact(std::string(msg.payload.data(), msg.payload.size()));
        spdlog::details::log_msg safe(msg.logger_name, msg.level, clean);
        safe.time = msg.time;
        safe.thread_id = msg.thread_id;
        target_->log(safe);
    }
    void flush() override { target_->flush(); }
    void set_pattern(const std::string& pattern) override { target_->set_pattern(pattern); }
    void set_formatter(std::unique_ptr<spdlog::formatter> sink_formatter) override {
        target_->set_formatter(std::move(sink_formatter));
    }
private:
    spdlog::sink_ptr target_;
};

inline spdlog::level::level_enum configured_level() {
    const char* raw = std::getenv("BRIDGESESSIONS_LOG_LEVEL");
    if (!raw || !*raw) return spdlog::level::info;
    std::string level(raw);
    std::transform(level.begin(), level.end(), level.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (level == "trace") return spdlog::level::trace;
    if (level == "debug") return spdlog::level::debug;
    if (level == "warn" || level == "warning") return spdlog::level::warn;
    if (level == "error") return spdlog::level::err;
    if (level == "critical") return spdlog::level::critical;
    if (level == "off") return spdlog::level::off;
    return spdlog::level::info;
}

struct LogState {
    std::mutex mutex;
    std::shared_ptr<spdlog::logger> logger;
    std::vector<spdlog::sink_ptr> sinks;
    bool initialized = false;
    bool async_pool_ready = false;
};

inline LogState& state() {
    static LogState s;
    return s;
}

// Platform-aware log directory resolution
inline std::string default_log_dir() {
    namespace fs = std::filesystem;

#if defined(__APPLE__)
    // macOS: ~/Library/Logs/BridgeSessions/
    const char* home = std::getenv("HOME");
    if (!home || !*home) home = std::getenv("USERPROFILE");
    if (!home || !*home) return {};
    return (fs::path(home) / "Library" / "Logs" / "BridgeSessions").string();

#elif defined(_WIN32)
    // Windows: %LOCALAPPDATA%\bridgesessions\logs
    const char* local = std::getenv("LOCALAPPDATA");
    if (local && *local) {
        return (fs::path(local) / "bridgesessions" / "logs").string();
    }
    const char* home = std::getenv("USERPROFILE");
    if (!home || !*home) return {};
    return (fs::path(home) / "AppData" / "Local" / "bridgesessions" / "logs").string();

#else
    // Linux: $XDG_DATA_HOME/bridgesessions or ~/.local/share/bridgesessions
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        return (fs::path(xdg) / "bridgesessions").string();
    }
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return (fs::path(home) / ".local" / "share" / "bridgesessions").string();
#endif
}

// Ensure a directory exists with secure permissions (0700 on POSIX)
inline bool ensure_dir(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(path, ec);
    if (ec) return false;
#ifndef _WIN32
    fs::permissions(path, fs::perms::owner_all, ec);
#endif
    return fs::is_directory(path);
}

} // namespace detail

// ── Public API ───────────────────────────────────────────────────

// Initialize the operational logging system.
//   app_home   — BridgeSessions config dir (~/.bridgesessions); used as fallback
//                if platform log dir can't be resolved.
//   foreground — if true, also log to stderr (human-readable colored format).
//                When false (daemon mode), file-only.
inline void init(const std::string& app_home = {}, bool foreground = false) {
    auto& s = detail::state();
    std::lock_guard lock(s.mutex);

    // Don't double-init
    if (s.initialized) {
        s.logger->flush();
        spdlog::drop_all();
        s.logger.reset();
        s.sinks.clear();
        s.initialized = false;
    }

    // Resolve log directory
    std::string log_dir = detail::default_log_dir();
    if (log_dir.empty() && !app_home.empty()) {
        // Fallback: app_home/logs
        log_dir = (std::filesystem::path(app_home) / "logs").string();
    }
    if (log_dir.empty()) {
        // Last resort: /tmp/bridgesessions (or %TEMP%)
#ifdef _WIN32
        const char* tmp = std::getenv("TEMP");
#else
        const char* tmp = "/tmp";
#endif
        if (tmp) log_dir = (std::filesystem::path(tmp) / "bridgesessions").string();
    }
    if (log_dir.empty()) {
        spdlog::error("bs::log::init: cannot determine log directory");
        return;
    }

    if (!detail::ensure_dir(log_dir)) {
        spdlog::error("bs::log::init: cannot create log directory: {}", log_dir);
        return;
    }

    const std::string log_path =
        (std::filesystem::path(log_dir) / "bridgesessions.log").string();

    // File sink: rotating, 5MB x 5 files, JSON-like format
    // Use rotating sink for size-based rotation (5MB max, 5 backups).
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        log_path, kMaxFileSize, kMaxFiles);
    // Machine-parseable format: timestamp, level, logger name, message
    file_sink->set_pattern("{\"ts\":\"%Y-%m-%dT%H:%M:%S.%eZ\",\"level\":\"%l\","
                           "\"logger\":\"%n\",\"msg\":\"%v\"}");

    s.sinks.push_back(std::make_shared<detail::RedactingSink>(file_sink));

    // Console sink: human-readable, only in foreground
    if (foreground) {
        auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
        s.sinks.push_back(std::make_shared<detail::RedactingSink>(console_sink));
    }

    // Create async thread pool for non-blocking file writes
    if (!s.async_pool_ready) {
        spdlog::init_thread_pool(kQueueSize, 1);
        s.async_pool_ready = true;
    }

    // Create the root logger with all sinks
    s.logger = std::make_shared<spdlog::async_logger>(
        "bs", s.sinks.begin(), s.sinks.end(),
        spdlog::thread_pool(), spdlog::async_overflow_policy::block);
    s.logger->set_level(detail::configured_level());
    s.logger->flush_on(spdlog::level::warn);    // flush on WARN or higher
    spdlog::register_logger(s.logger);
    s.initialized = true;

    s.logger->info("=== BridgeSessions operational logging initialized ===");
    s.logger->info("Log file: {}", log_path);
    s.logger->info("Foreground console: {}", foreground ? "enabled" : "disabled");
}

// Flush and shut down the logging system
inline void shutdown() {
    auto& s = detail::state();
    std::lock_guard lock(s.mutex);
    if (s.logger) {
        s.logger->info("=== BridgeSessions operational logging shutting down ===");
        s.logger->flush();
    }
    spdlog::drop_all();
    s.logger.reset();
    s.sinks.clear();
    s.initialized = false;
}

// Get a named logger. If init() hasn't been called, returns spdlog default.
inline std::shared_ptr<spdlog::logger> get(const std::string& name) {
    auto& s = detail::state();
    std::lock_guard lock(s.mutex);

    // Return existing named logger if already created
    auto existing = spdlog::get(name);
    if (existing) return existing;

    // If system is initialized, create a child logger sharing the same sinks
    if (s.initialized && s.logger) {
        auto named = std::make_shared<spdlog::async_logger>(
            name, s.sinks.begin(), s.sinks.end(),
            spdlog::thread_pool(), spdlog::async_overflow_policy::block);
        named->set_level(detail::configured_level());
        spdlog::register_logger(named);
        return named;
    }

    // Not initialized — return a stderr-only default so calls don't crash
    auto fallback = spdlog::stderr_color_mt(name);
    fallback->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    fallback->set_level(detail::configured_level());
    return fallback;
}

// ── Convenience macros ───────────────────────────────────────────
// Usage: BS_LOG_INFO("daemon", "Listening on {}:{}", addr, port);
// Or:   auto log = bs::log::get("daemon"); log->info("...");

#define BS_LOG_TRACE(name, ...) bs::log::get(name)->trace(__VA_ARGS__)
#define BS_LOG_DEBUG(name, ...) bs::log::get(name)->debug(__VA_ARGS__)
#define BS_LOG_INFO(name, ...)  bs::log::get(name)->info(__VA_ARGS__)
#define BS_LOG_WARN(name, ...)  bs::log::get(name)->warn(__VA_ARGS__)
#define BS_LOG_ERROR(name, ...) bs::log::get(name)->error(__VA_ARGS__)
#define BS_LOG_CRITICAL(name, ...) bs::log::get(name)->critical(__VA_ARGS__)

} // namespace bs::log
