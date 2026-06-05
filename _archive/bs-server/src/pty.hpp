#pragma once

#ifdef _WIN32
#include <windows.h>
#include "session.hpp"
#include <cstdint>
#include <expected>
#include <string>
#else
#include "session.hpp"
#include <cstdint>
#include <string>
#include <expected>
#endif

namespace bs::server {

struct PtyError {
    std::string message;
};

// Open a PTY master. On Windows returns a ConPTY input pipe handle.
// On POSIX returns fd.
[[nodiscard]] std::expected<intptr_t, PtyError> open_pty();

// Resize an existing PTY.
[[nodiscard]] std::expected<void, PtyError> resize_pty(intptr_t handle, uint16_t cols, uint16_t rows);

// Fork + exec a child process attached to the PTY.
// On Windows: CreateProcess with ConPTY.
// Returns process handle (Windows) or pid (POSIX).
[[nodiscard]] std::expected<intptr_t, PtyError> spawn_child(
    intptr_t pty_handle,
    const std::string& command,
    const std::string& term_env = "xterm-256color");

// Create a new session: open PTY, spawn child.
[[nodiscard]] std::expected<Session, PtyError> create_session(
    const std::string& name,
    const std::string& command,
    uint16_t cols = 80,
    uint16_t rows = 24,
    const std::string& term = "xterm-256color");

} // namespace bs::server
