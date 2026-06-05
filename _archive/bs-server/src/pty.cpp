#include "pty.hpp"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#else
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#ifdef __APPLE__
#include <sys/ttycom.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace bs::server {

#ifdef _WIN32

// ── Windows: ConPTY implementation ──────────────────────────────────

std::expected<intptr_t, PtyError> open_pty() {
    return reinterpret_cast<intptr_t>(INVALID_HANDLE_VALUE); // placeholder — real work done in create_session
}

std::expected<void, PtyError> resize_pty(intptr_t handle, uint16_t cols, uint16_t rows) {
    HPCON hPC = reinterpret_cast<HPCON>(handle);
    COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    if (SUCCEEDED(ResizePseudoConsole(hPC, size)))
        return {};
    return std::unexpected(PtyError{"ResizePseudoConsole failed"});
}

std::expected<intptr_t, PtyError> spawn_child(
    intptr_t pty_handle, const std::string& command, const std::string& term_env)
{
    // pty_handle is not used directly — create_session handles the full ConPTY setup
    return reinterpret_cast<intptr_t>(INVALID_HANDLE_VALUE);
}

std::expected<Session, PtyError> create_session(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    HANDLE hPipeInRead = nullptr, hPipeInWrite = nullptr;
    HANDLE hPipeOutRead = nullptr, hPipeOutWrite = nullptr;

    // Create pipes for ConPTY
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; // inheritable
    if (!CreatePipe(&hPipeInRead, &hPipeInWrite, &sa, 0))
        return std::unexpected(PtyError{"CreatePipe(in) failed"});
    if (!CreatePipe(&hPipeOutRead, &hPipeOutWrite, &sa, 0)) {
        CloseHandle(hPipeInRead); CloseHandle(hPipeInWrite);
        return std::unexpected(PtyError{"CreatePipe(out) failed"});
    }

    // Create pseudo console
    COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    HPCON hPC = nullptr;
    HRESULT hr = CreatePseudoConsole(size, hPipeInRead, hPipeOutWrite, 0, &hPC);
    if (FAILED(hr)) {
        CloseHandle(hPipeInRead); CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead); CloseHandle(hPipeOutWrite);
        return std::unexpected(PtyError{"CreatePseudoConsole failed: " + std::to_string(hr)});
    }

    // Per MSDN: CreatePseudoConsole takes ownership of the handles passed
    // to it. After a successful call, the caller MUST close the inbound read
    // and outbound write ends so they don't leak and so ConPTY's pipe
    // completion gets signaled correctly.
    CloseHandle(hPipeInRead);
    CloseHandle(hPipeOutWrite);
    hPipeInRead = nullptr;
    hPipeOutWrite = nullptr;

    // Set up STARTUPINFOEX for the child process
    STARTUPINFOEXA siEx{};
    siEx.StartupInfo.cb = sizeof(siEx);
    siEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    // Child's stdio goes through the ConPTY
    siEx.StartupInfo.hStdInput = nullptr;  // ConPTY handles stdio internally
    siEx.StartupInfo.hStdOutput = nullptr;
    siEx.StartupInfo.hStdError = nullptr;

    // Add the ConPTY to the process attribute list
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    siEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!siEx.lpAttributeList || !InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize)) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead); CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead); CloseHandle(hPipeOutWrite);
        return std::unexpected(PtyError{"InitializeProcThreadAttributeList failed"});
    }
    UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(HPCON), nullptr, nullptr);

    // Build command line: cmd.exe /c <command>
    std::string cmdline = "cmd.exe /c \"" + command + "\"";

    // Set TERM environment
    SetEnvironmentVariableA("TERM", term.c_str());

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessA(
        nullptr,                    // app name
        const_cast<LPSTR>(cmdline.c_str()),
        nullptr, nullptr,           // process/thread security
        TRUE,                       // inherit handles
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,                    // environment (use parent's)
        nullptr,                    // current directory
        &siEx.StartupInfo,
        &pi);

    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);

    if (!created) {
        ClosePseudoConsole(hPC);
        // Only the writer/reader we kept are open here; the other two
        // were closed right after CreatePseudoConsole succeeded.
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{"CreateProcess failed: " + std::to_string(GetLastError())});
    }

    CloseHandle(pi.hThread);

    Session s;
    s.name = name;
    s.master_fd = hPipeOutRead;   // read: child stdout → server
    s.write_handle = hPipeInWrite; // write: server → child stdin
    s.child_pid = pi.hProcess;     // process handle
#ifdef _WIN32
    s.hpcon = hPC;                 // for ResizePseudoConsole
#endif
    s.state = SessionState::Running;
    return s;
}

#else

// ── POSIX implementation ────────────────────────────────────────────

namespace {
bool set_cloexec(int fd) {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) return false;
    return ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}
}

std::expected<intptr_t, PtyError> open_pty() {
    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return std::unexpected(PtyError{"posix_openpt failed: " + std::string(strerror(errno))});
    if (!set_cloexec(master)) { close(master);
        return std::unexpected(PtyError{"fcntl FD_CLOEXEC failed: " + std::string(strerror(errno))}); }
    if (::grantpt(master) < 0) { close(master);
        return std::unexpected(PtyError{"grantpt failed: " + std::string(strerror(errno))}); }
    if (::unlockpt(master) < 0) { close(master);
        return std::unexpected(PtyError{"unlockpt failed: " + std::string(strerror(errno))}); }
    return master;
}

std::expected<void, PtyError> resize_pty(intptr_t handle, uint16_t cols, uint16_t rows) {
    int master_fd = static_cast<int>(handle);
    struct winsize ws {};
    ws.ws_col = cols; ws.ws_row = rows;
    if (::ioctl(master_fd, TIOCSWINSZ, &ws) < 0)
        return std::unexpected(PtyError{"TIOCSWINSZ failed: " + std::string(strerror(errno))});
    ::ioctl(master_fd, TIOCSIG, SIGWINCH);
    return {};
}

std::expected<intptr_t, PtyError> spawn_child(
    intptr_t pty_handle, const std::string& command, const std::string& term_env)
{
    int master_fd = static_cast<int>(pty_handle);
    (void)master_fd;
    const char* slave_name = ::ptsname(master_fd);
    if (!slave_name) return std::unexpected(PtyError{"ptsname failed"});

    pid_t pid = ::fork();
    if (pid < 0)
        return std::unexpected(PtyError{"fork failed: " + std::string(strerror(errno))});

    if (pid == 0) {
        ::setsid();
        int slave = ::open(slave_name, O_RDWR);
        if (slave < 0) _exit(1);
        ::dup2(slave, STDIN_FILENO);
        ::dup2(slave, STDOUT_FILENO);
        ::dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO) ::close(slave);
        ::setenv("TERM", term_env.c_str(), 1);
        const char* argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
        ::execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }
    return pid;
}

std::expected<Session, PtyError> create_session(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    auto fd = open_pty(); if (!fd) return std::unexpected(fd.error());
    auto pid = spawn_child(*fd, command, term);
    if (!pid) { close(static_cast<int>(*fd)); return std::unexpected(pid.error()); }
    (void)resize_pty(*fd, cols, rows);
    Session s; s.name = name; s.master_fd = static_cast<int>(*fd); s.child_pid = static_cast<int>(*pid);
    s.state = SessionState::Running;
    return s;
}

#endif

} // namespace bs::server
