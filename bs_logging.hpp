// bs_logging.hpp — Structured JSON logging (extracted from bs-protocol.h)
#pragma once
#include "bs_session_core.hpp"
namespace bs::mesh {
struct StructuredLoggerState {
    std::mutex mutex;
    std::string app_home;
    std::shared_ptr<spdlog::logger> logger;
};

inline StructuredLoggerState& structured_logger_state() {
    static StructuredLoggerState state;
    return state;
}

inline void configure_logger_home(const std::string& app_home) {
    if (app_home.empty()) return;
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.app_home == app_home) return;
    if (state.logger) state.logger->flush();
    spdlog::drop("bs-mesh");
    state.logger.reset();
    state.app_home = app_home;
}

#ifdef BS_TESTING
inline void reset_logger_for_test() {
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.logger) state.logger->flush();
    spdlog::drop("bs-mesh");
    state.logger.reset();
    state.app_home.clear();
}
#endif

// Thread-safe JSON logger
inline std::shared_ptr<spdlog::logger> get_logger() {
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.logger) return state.logger;

    if (state.app_home.empty()) {
        const char* home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        if (!home || !*home)
            throw std::runtime_error("cannot initialize logger: home directory unavailable");
        state.app_home = (std::filesystem::path(home) / ".bridgesessions").string();
    }
    if (!ensure_private_directory(state.app_home))
        throw std::runtime_error("cannot initialize logger directory " + state.app_home);

    const std::string path =
        (std::filesystem::path(state.app_home) / "bs-mesh.log").string();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        path, 1'048'576, 3);  // 1 MB, 3 rotated files
    file_sink->set_pattern("%v");  // raw JSON lines

    state.logger = std::make_shared<spdlog::logger>("bs-mesh", file_sink);
    state.logger->set_level(spdlog::level::info);
    state.logger->flush_on(spdlog::level::info);
    spdlog::register_logger(state.logger);
    return state.logger;
}

// Log a structured event as a single JSON line
inline void log_event(const std::string& event, const std::string& detail = "") {
    auto l = get_logger();
    nlohmann::json j;
    j["ts"] = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    j["event"] = event;
    if (!detail.empty()) j["detail"] = detail;
    l->info(j.dump());
}
} // namespace bs::mesh
