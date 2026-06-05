// logging.hpp — Phase 12: structured JSON logging via spdlog
// Wraps spdlog for file-based JSON logging. All iostream output
// remains for terminal use; this is for audit and monitoring.

#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <string>

namespace bs::server {

// Thread-safe JSON logger
inline std::shared_ptr<spdlog::logger> get_logger() {
    static auto logger = []() {
        // Create a rotating file sink
        const char* home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        std::string path = home ? std::string(home) + "/.bridgesessions/bs-server.log"
                                : "/tmp/bs-server.log";
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path, 1'048'576, 3);  // 1 MB, 3 rotated files
        file_sink->set_pattern("%v");  // raw JSON lines

        auto l = std::make_shared<spdlog::logger>("bs-server", file_sink);
        l->set_level(spdlog::level::info);
        l->flush_on(spdlog::level::info);
        spdlog::register_logger(l);
        return l;
    }();
    return logger;
}

// Log a structured event as a single JSON line
inline void log_event(const std::string& event, const std::string& detail = "") {
    auto* l = get_logger().get();
    if (!detail.empty()) {
        l->info(R"({{"ts":"{}","event":"{}","detail":"{}"}})",
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()),
                event, detail);
    } else {
        l->info(R"({{"ts":"{}","event":"{}"}})",
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()),
                event);
    }
}

} // namespace bs::server
