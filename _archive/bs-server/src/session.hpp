#pragma once

#include "ring_buffer.hpp"
#include <cstdint>
#include <chrono>
#include <string>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#endif

namespace bs::server {

// Default ring buffer: 1 MB (2^20) per ARCHITECTURE.md §3.2
constexpr size_t kDefaultRingBufferSize = 1'048'576;  // 1 MiB

enum class SessionState : uint8_t {
    Created,
    Running,
    Detached,
    Attached,
    Died,
    Exited,
    Killed,
    Recoverable,  // Phase 10: loaded from disk, not yet spawned
};

inline const char* session_state_str(SessionState s) {
    switch (s) {
        case SessionState::Created:  return "created";
        case SessionState::Running:  return "running";
        case SessionState::Detached: return "detached";
        case SessionState::Attached: return "attached";
        case SessionState::Died:     return "died";
        case SessionState::Exited:   return "exited";
        case SessionState::Killed:   return "killed";
        case SessionState::Recoverable: return "recoverable";
    }
    return "unknown";
}

struct Session {
    std::string name;
    std::string owner_id;
    std::string command;
#ifdef _WIN32
    HANDLE master_fd = nullptr;   // ConPTY output read handle (child stdout → server)
    HANDLE child_pid = nullptr;   // process handle
    HANDLE write_handle = nullptr; // ConPTY input write handle (server → child stdin)
    HPCON hpcon = nullptr;        // ConPTY handle — required for ResizePseudoConsole
#else
    int master_fd = -1;
    int child_pid = -1;
#endif
    SessionState state = SessionState::Created;

    // Scrollback ring buffer — 1 MB default
    RingBuffer<kDefaultRingBufferSize> scrollback;

    // Timestamps
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_output_at;
    std::chrono::steady_clock::time_point last_attach_at;

    // Auto-restart (Phase 6)
    bool auto_restart = false;
    int restart_failures = 0;
    std::chrono::steady_clock::time_point restart_window_start;

    Session();
    ~Session();

    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void touch_output();
    void reset_restart_failures();
#ifdef _WIN32
    bool is_valid() const { return master_fd != nullptr; }
#else
    bool is_valid() const { return master_fd >= 0; }
#endif
};

} // namespace bs::server
