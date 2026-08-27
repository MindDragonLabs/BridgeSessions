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
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto boundary = [&](size_t pos) {
        return pos == 0 || (!std::isalnum(static_cast<unsigned char>(lower[pos - 1])) &&
                            lower[pos - 1] != '_');
    };
    const auto value_end = [&](size_t begin) {
        if (begin < text.size() && (text[begin] == '"' || text[begin] == '\'')) {
            const char quote = text[begin++];
            size_t end = begin;
            while (end < text.size() && text[end] != quote) ++end;
            return std::pair{begin, end < text.size() ? end + 1 : end};
        }
        size_t end = begin;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
               text[end] != '&' && text[end] != ';' && text[end] != ',') ++end;
        return std::pair{begin, end};
    };

    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (lower.compare(i, 11, "-----begin ") == 0) {
            const size_t header_end = lower.find("-----", i + 11);
            if (header_end != std::string::npos &&
                lower.substr(i, header_end + 5 - i).find("private key-----") != std::string::npos) {
                const size_t end_marker = lower.find("-----end ", header_end + 5);
                const size_t end_dashes = end_marker == std::string::npos
                    ? std::string::npos : lower.find("-----", end_marker + 9);
                const size_t end = end_dashes == std::string::npos
                    ? text.size() : end_dashes + 5;
                out += "[REDACTED PRIVATE KEY]";
                i = end;
                continue;
            }
        }

        size_t prefix_end = std::string::npos;
        if (boundary(i) && lower.compare(i, 7, "bearer ") == 0) {
            prefix_end = i + 7;
        } else {
            size_t key_begin = i;
            bool quoted_key = false;
            if (text[key_begin] == '"' || text[key_begin] == '\'') {
                quoted_key = true;
                ++key_begin;
            }
            std::string_view key;
            for (std::string_view candidate : {"authorization", "password", "passwd",
                                               "api_key", "secret", "token",
                                               "proc-type", "dek-info"}) {
                if (lower.compare(key_begin, candidate.size(), candidate) == 0) {
                    key = candidate;
                    break;
                }
            }
            if (!key.empty() && boundary(i)) {
                size_t p = key_begin + key.size();
                if (quoted_key && p < text.size() && text[p] == text[i]) ++p;
                while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
                if (p < text.size() && (text[p] == '=' || text[p] == ':')) {
                    ++p;
                    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
                    if (key == "authorization" && lower.compare(p, 7, "bearer ") == 0)
                        p += 7;
                    prefix_end = p;
                }
            } else if (text[i] == '-' && i + 1 < text.size()) {
                size_t p = i + (text[i + 1] == '-' ? 2 : 1);
                const bool short_key = p < text.size() && lower[p] == 'k' &&
                    (p + 1 == text.size() || std::isspace(static_cast<unsigned char>(text[p + 1])));
                bool long_key = false;
                for (std::string_view candidate : {"authorization", "password", "passwd",
                                                   "api_key", "secret", "token"}) {
                    if (lower.compare(p, candidate.size(), candidate) == 0 &&
                        p + candidate.size() < text.size() &&
                        std::isspace(static_cast<unsigned char>(text[p + candidate.size()]))) {
                        p += candidate.size();
                        long_key = true;
                        break;
                    }
                }
                if (short_key) ++p;
                if (short_key || long_key) {
                    while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
                    prefix_end = p;
                }
            }
        }

        if (prefix_end != std::string::npos && prefix_end < text.size()) {
            out.append(text, i, prefix_end - i);
            const auto [value_begin, end] = value_end(prefix_end);
            if (value_begin > prefix_end) out.push_back(text[prefix_end]);
            out += "[REDACTED]";
            if (value_begin > prefix_end && end <= text.size() && end > value_begin &&
                text[end - 1] == text[prefix_end]) out.push_back(text[prefix_end]);
            i = end;
            continue;
        }
        out.push_back(text[i++]);
    }
    return out;
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
    auto stderr_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto fallback = std::make_shared<spdlog::logger>(
        name, std::make_shared<detail::RedactingSink>(stderr_sink));
    spdlog::register_logger(fallback);
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
