#pragma once

#include <cstdint>
#include <expected>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

namespace bs::client {

struct TermError {
    std::string message;
};

#ifdef _WIN32
struct SavedConsole {
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    CONSOLE_SCREEN_BUFFER_INFO buffer_info{};
};

// Enable virtual terminal raw input mode
[[nodiscard]] std::expected<SavedConsole, TermError> enable_raw_mode();

// Restore console mode
void restore_terminal(const SavedConsole& saved);

// Get current console window size (cols, rows)
[[nodiscard]] std::expected<std::pair<uint16_t, uint16_t>, TermError> get_winsize();

#else
// Enable raw mode on stdin. Returns the original termios for restore.
[[nodiscard]] std::expected<struct termios, TermError> enable_raw_mode();

// Restore terminal settings from saved state.
void restore_terminal(const struct termios& saved);

// Get current terminal window size.
[[nodiscard]] std::expected<std::pair<uint16_t, uint16_t>, TermError> get_winsize();
#endif

} // namespace bs::client
