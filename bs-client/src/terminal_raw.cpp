#include "terminal_raw.hpp"
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace bs::client {

#ifdef _WIN32

std::expected<SavedConsole, TermError> enable_raw_mode() {
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SavedConsole saved{};

    if (!GetConsoleMode(hIn, &saved.input_mode))
        return std::unexpected(TermError{"GetConsoleMode(in) failed"});
    if (!GetConsoleMode(hOut, &saved.output_mode))
        return std::unexpected(TermError{"GetConsoleMode(out) failed"});
    GetConsoleScreenBufferInfo(hOut, &saved.buffer_info);

    // Raw mode: disable line input and echo, keep virtual terminal input for ANSI escape sequences
    DWORD newIn = saved.input_mode
                & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT)
                | ENABLE_VIRTUAL_TERMINAL_INPUT
                | ENABLE_PROCESSED_INPUT;

    if (!SetConsoleMode(hIn, newIn))
        return std::unexpected(TermError{"SetConsoleMode(in) failed"});

    DWORD newOut = saved.output_mode
                 | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                 | DISABLE_NEWLINE_AUTO_RETURN;

    if (!SetConsoleMode(hOut, newOut))
        return std::unexpected(TermError{"SetConsoleMode(out) failed"});

    return saved;
}

void restore_terminal(const SavedConsole& saved) {
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved.input_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved.output_mode);
}

std::expected<std::pair<uint16_t, uint16_t>, TermError> get_winsize() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return std::unexpected(TermError{"GetConsoleScreenBufferInfo failed"});
    return std::pair{
        static_cast<uint16_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1),
        static_cast<uint16_t>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1)
    };
}

#else

std::expected<struct termios, TermError> enable_raw_mode() {
    struct termios saved {};
    struct termios raw {};

    if (::tcgetattr(STDIN_FILENO, &saved) < 0) {
        return std::unexpected(TermError{
            "tcgetattr failed: " + std::string(strerror(errno))});
    }

    raw = saved;
    ::cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        return std::unexpected(TermError{
            "tcsetattr failed: " + std::string(strerror(errno))});
    }

    return saved;
}

void restore_terminal(const struct termios& saved) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
}

std::expected<std::pair<uint16_t, uint16_t>, TermError> get_winsize() {
    struct winsize ws {};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0) {
        return std::unexpected(TermError{
            "TIOCGWINSZ failed: " + std::string(strerror(errno))});
    }
    return std::pair{ws.ws_col, ws.ws_row};
}

#endif

} // namespace bs::client
