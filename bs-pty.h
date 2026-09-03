// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-pty.h — Session PTY, hosted workers, spawn/pump
// Extracted from bs-protocol.h (R6 structural refactor, 2026-09-02)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace — parent file provides it.
#pragma once

// ────────────────────────────────────────────────────────────────────
// 6. SESSION & PTY (ported from bs-server, namespace bs::mesh)
// ────────────────────────────────────────────────────────────────────

// ── PtyError ──────────────────────────────────────────────────────

struct PtyError {
    std::string message;
};

[[nodiscard]] std::string parent_session_id_from_environment() {
    const char* value = std::getenv("BS_PARENT_SESSION_ID");
    if (!value || !*value) return {};
    std::string parent(value);
    if (parent.size() > 256) return {};
    for (const unsigned char ch : parent) {
        if (ch <= 0x20 || ch == 0x7f) return {};
    }
    return parent;
}


// ── PTY functions ─────────────────────────────────────────────────

#ifdef _WIN32

[[nodiscard]] std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

[[nodiscard]] std::wstring first_windows_command_token(
    const std::wstring& command_line) {
    size_t begin = command_line.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) return {};
    if (command_line[begin] == L'\"') {
        const size_t end = command_line.find(L'\"', begin + 1);
        if (end == std::wstring::npos) return {};
        return command_line.substr(begin + 1, end - begin - 1);
    }
    const size_t end = command_line.find_first_of(L" \t", begin);
    return command_line.substr(begin, end == std::wstring::npos
                                      ? std::wstring::npos
                                      : end - begin);
}

[[nodiscard]] std::wstring resolve_windows_application(
    const std::wstring& command_line) {
    const std::wstring token = first_windows_command_token(command_line);
    if (token.empty()) return {};
    std::vector<wchar_t> resolved(32768, L'\0');
    const DWORD len = SearchPathW(nullptr, token.c_str(), nullptr,
                                  static_cast<DWORD>(resolved.size()),
                                  resolved.data(), nullptr);
    if (len == 0 || len >= resolved.size()) return {};
    return std::wstring(resolved.data(), len);
}

[[nodiscard]] std::vector<wchar_t> windows_child_environment(
        const std::string& session_id, const std::string& term) {
    std::vector<std::wstring> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block) {
        for (const wchar_t* cur = block; *cur; cur += std::wcslen(cur) + 1) {
            std::wstring entry(cur);
            const auto key_end = entry.find(L'=');
            const std::wstring key = key_end == std::wstring::npos
                ? entry : entry.substr(0, key_end);
            if (_wcsicmp(key.c_str(), L"BS_SESSION_ID") == 0 ||
                _wcsicmp(key.c_str(), L"TERM") == 0) {
                continue;
            }
            entries.push_back(std::move(entry));
        }
        FreeEnvironmentStringsW(block);
    }
    entries.push_back(L"BS_SESSION_ID=" + utf8_to_wide(session_id));
    entries.push_back(L"TERM=" + utf8_to_wide(term));
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

// Forward decls — full definitions live next to prepare_session_command.
[[nodiscard]] bool is_windows_cli_oneshot_command(const std::string& command);
[[nodiscard]] bool command_has_direct_windows_exe_token(const std::string& command);

// A3: create a Job Object whose KILL_ON_JOB_CLOSE limit kills the entire
// child process tree (ConPTY root + shell + grandchildren) when the daemon
// closes the handle. Replaces the leak-prone TerminateProcess-on-direct-child
// kill path for Windows sessions.
[[nodiscard]] HANDLE create_kill_job() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limit, sizeof(limit))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

// Best-effort: assign a freshly created child to the kill job. If the child is
// already nested in another job (some launchers pre-nest), log and continue —
// the direct TerminateProcess fallback still applies for that case.
inline void assign_to_kill_job(HANDLE job, HANDLE child) {
    if (!job || !child) return;
    if (!AssignProcessToJobObject(job, child)) {
        log_event("job_assign_failed", "err=" + std::to_string(GetLastError()));
    }
}

[[nodiscard]] std::expected<Session, PtyError> create_session(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    const std::string parent_id = parent_session_id_from_environment();
    auto child_environment = windows_child_environment(name, term);
    // v2.0.1: one-shot commands (cmd /c …) skip ConPTY. ConPTY/conhost often
    // only delivers mode CSI to the pipe while command text never arrives
    // before SessionDied (fleet RCA). Plain anonymous pipes capture stdout
    // reliably for health/shell --cmd.
    // v2.0.2: also treat direct powershell/pwsh -Command/-File one-shots as
    // pipe captures (they no longer go through cmd /c, so /c detection alone
    // is insufficient).
    const bool oneshot = is_windows_cli_oneshot_command(command);
    if (oneshot) {
        HANDLE out_r = nullptr, out_w = nullptr, in_r = nullptr, in_w = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!CreatePipe(&out_r, &out_w, &sa, 0))
            return std::unexpected(PtyError{"CreatePipe(out) failed"});
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) {
            CloseHandle(out_r); CloseHandle(out_w);
            return std::unexpected(PtyError{"CreatePipe(in) failed"});
        }
        // Parent keeps out_r / in_w non-inheritable; child gets out_w / in_r.
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out_w;
        si.hStdError = out_w;
        si.hStdInput = in_r;

        std::wstring cmdline = utf8_to_wide(command);
        std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
        mutable_cmdline.push_back(L'\0');
        const std::wstring application = resolve_windows_application(cmdline);

        PROCESS_INFORMATION pi{};
        BOOL created = CreateProcessW(
            application.empty() ? nullptr : application.c_str(),
            mutable_cmdline.data(),
            nullptr, nullptr,
            TRUE,  // inherit the pipe ends we left inheritable
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT |
                CREATE_SUSPENDED,   // A3: suspend until job assignment completes
            child_environment.data(), nullptr,
            &si, &pi);
        const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
        // Parent must close the ends it handed to the child.
        CloseHandle(out_w);
        CloseHandle(in_r);
        if (!created) {
            CloseHandle(out_r);
            CloseHandle(in_w);
            return std::unexpected(PtyError{
                "CreateProcessW(oneshot) failed: " + std::to_string(create_error)});
        }

        // A3: wrap the child in a KILL_ON_JOB_CLOSE job so the whole tree dies
        // with the session instead of leaking ConPTY/shell grandchildren. The
        // child is suspended until assignment, so it cannot spawn descendants
        // before being tracked.
        HANDLE job = create_kill_job();
        assign_to_kill_job(job, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);

        Session s;
        s.name = name;
        s.command = command;
        s.parent_id = parent_id;
        s.master_fd = out_r;
        s.write_handle = in_w;
        s.child_pid = pi.hProcess;
        s.hpcon = nullptr;
        s.job_handle = job;
        s.generation = ++g_session_generation;
        s.state = SessionState::Running;
        log_event("session_oneshot_pipes", name);
        (void)cols; (void)rows; (void)term;
        return s;
    }

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

    // Keep the ends passed to CreatePseudoConsole open until the child has
    // been attached with CreateProcessW. Closing them before attachment can
    // tear down the pseudoconsole and leave an HPCON that rejects resize.

    // The daemon-side pipe ends must never be inherited by the child. ConPTY
    // owns the opposite ends and brokers the child's standard streams itself.
    if (!SetHandleInformation(hPipeInWrite, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(hPipeOutRead, HANDLE_FLAG_INHERIT, 0)) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "SetHandleInformation failed: " + std::to_string(GetLastError())});
    }

    // Set up STARTUPINFOEX for the child process
    STARTUPINFOEXW siEx{};
    siEx.StartupInfo.cb = sizeof(siEx);

    // Add the ConPTY to the process attribute list.
    SIZE_T attrSize = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    if (attrSize == 0) {
        const DWORD error = GetLastError();
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "InitializeProcThreadAttributeList sizing failed: " +
            std::to_string(error)});
    }
    siEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!siEx.lpAttributeList) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{"HeapAlloc for process attributes failed"});
    }
    if (!InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0,
                                           &attrSize)) {
        const DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "InitializeProcThreadAttributeList failed: " +
            std::to_string(error)});
    }
    if (!UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(HPCON),
            nullptr, nullptr)) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(siEx.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "UpdateProcThreadAttribute failed: " + std::to_string(error)});
    }

    // Named/default shell profiles are complete command lines and become the
    // ConPTY root process directly. Client one-shot commands are wrapped once
    // by prepare_session_command before reaching this function.
    std::wstring cmdline = utf8_to_wide(command);
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');
    const std::wstring application = resolve_windows_application(cmdline);

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(
        application.empty() ? nullptr : application.c_str(),
        mutable_cmdline.data(),
        nullptr, nullptr,           // process/thread security
        FALSE,                      // ConPTY child inherits no daemon handles
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP |
            CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,  // A3: suspend until job assign
        child_environment.data(),
        nullptr,                    // current directory
        &siEx.StartupInfo,
        &pi);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();

    // The pseudoconsole duplicated these ends. Release our references only
    // after the child process has been attached.
    CloseHandle(hPipeInRead);
    CloseHandle(hPipeOutWrite);
    hPipeInRead = nullptr;
    hPipeOutWrite = nullptr;

    DeleteProcThreadAttributeList(siEx.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);

    if (!created) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "CreateProcessW failed: " + std::to_string(create_error)});
    }

    // A3: wrap the ConPTY child in a KILL_ON_JOB_CLOSE job, then resume.
    HANDLE job = create_kill_job();
    assign_to_kill_job(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    Session s;
    s.name = name;
    s.command = command;
    s.parent_id = parent_id;
    s.master_fd = hPipeOutRead;     // read: child stdout -> server
    s.write_handle = hPipeInWrite;  // write: server -> child stdin
    s.child_pid = pi.hProcess;      // process handle
    s.hpcon = hPC;                  // for ResizePseudoConsole
    s.job_handle = job;
    s.generation = ++g_session_generation;
    s.state = SessionState::Running;
    return s;
}

[[nodiscard]] std::expected<void, PtyError> resize_pty(intptr_t handle, uint16_t cols, uint16_t rows) {
    HPCON hPC = reinterpret_cast<HPCON>(handle);
    COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    const HRESULT result = ResizePseudoConsole(hPC, size);
    if (SUCCEEDED(result))
        return {};
    return std::unexpected(PtyError{
        "ResizePseudoConsole failed: HRESULT=" +
        std::to_string(static_cast<unsigned long>(result)) +
        " GetLastError=" + std::to_string(GetLastError())});
}

#else // POSIX create_session via fork+execpty

inline void close_nonstdio_fds_before_exec() {
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    ::closefrom(3);
    return;
#endif
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) max_fd = 1024;
    for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
}

[[nodiscard]] std::expected<Session, PtyError> create_session(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    const std::string parent_id = parent_session_id_from_environment();
    int master_fd = -1;
    struct winsize initial_ws {rows, cols, 0, 0};
    pid_t child = forkpty(&master_fd, nullptr, nullptr, &initial_ws);
    if (child < 0)
        return std::unexpected(PtyError{"forkpty failed"});
    if (child == 0) {
        // The relay ignores SIGPIPE so failed TLS writes become reconnectable
        // errors. Restore the normal disposition for the user's shell/process.
        ::signal(SIGPIPE, SIG_DFL);
        setenv("TERM", term.c_str(), 1);
        // Mark this as a BridgeSessions shell session so user profiles can
        // detect and skip interactive-only features (starship, tmux auto-attach,
        // zellij, mouse tracking, etc.) that produce escape-sequence garbage.
        setenv("BS_SESSION", "1", 1);
        setenv("BS_SESSION_ID", name.c_str(), 1);
        // Clear env vars that trigger interactive terminal features which
        // corrupt BS shell sessions with escape-sequence noise.
        unsetenv("FORCE_STARSHIP");
        unsetenv("ZELLIJ_AUTO_ATTACH");
        // Close inherited daemon FDs before exec.
        close_nonstdio_fds_before_exec();
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }
    const int master_flags = fcntl(master_fd, F_GETFL, 0);
    if (master_flags < 0 || fcntl(master_fd, F_SETFL, master_flags | O_NONBLOCK) < 0) {
        ::kill(child, SIGTERM);
        ::close(master_fd);
        return std::unexpected(PtyError{"failed to set PTY master nonblocking"});
    }

    Session s;
    s.name = name; s.command = command; s.parent_id = parent_id;
    s.master_fd = master_fd; s.child_pid = child;
    s.generation = ++g_session_generation;
    s.state = SessionState::Running;
    return s;
}

[[nodiscard]] std::expected<void, PtyError> resize_pty(intptr_t handle, uint16_t cols, uint16_t rows) {
    struct winsize ws = {rows, cols, 0, 0};
    if (ioctl(static_cast<int>(handle), TIOCSWINSZ, &ws) == 0) return {};
    return std::unexpected(PtyError{"TIOCSWINSZ failed"});
}

#endif // _WIN32

// ── Session-worker architecture (PTY hosting outside the daemon) ──────
// Included here — after create_session/resize_pty, before SessionRegistry —
// so the registry and MeshController can spawn/adopt/talk to workers.
#include "bs-session-worker.h"

// Path of the running executable (POSIX) — workers are this same binary
// re-exec'd with the hidden `session-worker` subcommand.
#ifndef _WIN32
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
[[nodiscard]] inline std::string current_exe_path() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(buf, ec);
    return ec ? buf : canon.string();
#else
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string{} : p.string();
#endif
}

// ── Hosted-session spawn + pump helpers ────────────────────────────────

// Create a session hosted in a detached worker process. The daemon talks to
// the worker over a Unix socket; the PTY/shell survive daemon restarts and
// upgrades. Caller falls back to create_session on failure.
#ifndef _WIN32
// Terminate an unregistered session worker and reap it. Failure-path and
// teardown only: a worker that never registered in the session table is
// invisible to reap_dead(), so a plain SIGTERM here leaked one zombie per
// failed spawn. A version-skewed worker binary that dies pre-bind on every
// incoming probe can then exhaust the process table within hours (fork()
// starts failing EAGAIN system-wide). Returns true when the worker is
// confirmed dead; exit_info (when non-null) receives the exit code, or
// 128+signal for signal death, or -1 when unknown (already reaped elsewhere).
inline bool terminate_worker_and_reap(pid_t wpid, int* exit_info = nullptr) {
    if (wpid <= 0) return true;
    if (exit_info) *exit_info = -1;
    if (::kill(wpid, SIGTERM) == 0 || errno != ESRCH) {
        int st = 0;
        for (int i = 0; i < 20; ++i) {            // ~1s graceful window
            if (::waitpid(wpid, &st, WNOHANG) == wpid) {
                if (exit_info)
                    *exit_info = WIFEXITED(st) ? WEXITSTATUS(st)
                                : (WIFSIGNALED(st) ? 128 + WTERMSIG(st) : -1);
                return true;
            }
            ::usleep(50 * 1000);
        }
        ::kill(wpid, SIGKILL);
    }
    int st = 0;
    while (::waitpid(wpid, &st, 0) < 0 && errno == EINTR) {}
    if (exit_info && WIFEXITED(st))
        *exit_info = WEXITSTATUS(st);
    else if (exit_info && WIFSIGNALED(st))
        *exit_info = 128 + WTERMSIG(st);
    return true;   // ECHILD (reaped elsewhere) also counts as dead
}

// Greptile P1 tail (26.08.31-release review): on the systemd-run spawn path
// spawn_session_worker() returns wpid==0 (the scope is owned by PID 1), so
// terminate_worker_and_reap(0) is a no-op. A fully-timed-out spawn — scope
// created but socket/READY never observed — then leaves a cold-starting
// systemd worker that nothing tracks or reaps. stop_session_worker_unit()
// closes that orphan tail: the caller records the deterministic unit name at
// spawn time (sanitize_systemd_unit_name lives above the worker-header
// include) and stops the scope on every failure path where the worker was
// never adopted into the session table (an adopted worker is owned by
// reap_dead()).
// Best-effort stop of a systemd-run --user --scope worker unit. Returns true
// when the unit is confirmed stopped (or was never running). The unit name is
// strictly validated before it ever reaches a shell: this helper must stay
// safe even if a future caller skips sanitize_systemd_unit_name(). Failure is
// deliberately non-fatal: worst case is the pre-existing orphan, never damage
// to a live session.
inline bool stop_session_worker_unit(const std::string& unit) {
    if (unit.empty()) return false;
    for (char c : unit) {
        unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == ':' || c == '.' || c == '_' || c == '-'))
            return false;   // refuse anything that could alter the command line
    }
    if (::system(("systemctl --user stop " + unit + " 2>/dev/null").c_str()) != 0)
        return false;   // no systemd user session (macOS/CI) or unit gone
    // "stop" on a scope returns once the stop job is queued; poll briefly so
    // callers that immediately re-spawn the same session name do not race the
    // dying worker for the socket path.
    for (int i = 0; i < 20; ++i) {                       // ~1s
        std::string probe =
            "systemctl --user is-active " + unit + " >/dev/null 2>&1";
        if (::system(probe.c_str()) != 0) return true;   // inactive/failed/gone
        ::usleep(50 * 1000);
    }
    int krc = ::system(("systemctl --user kill --signal=SIGKILL " + unit +
                        " 2>/dev/null").c_str());
    (void)krc;   // last-resort kill; best-effort by design
    return true;
}
#endif

[[nodiscard]] inline std::expected<Session, PtyError> create_session_hosted(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term,
    const std::string& app_home, const std::string& exe_path)
{
    if (app_home.empty() || exe_path.empty())
        return std::unexpected(PtyError{"hosted session needs app_home + exe_path"});

    worker::WorkerConfig wc;
    wc.session_name = name;
    wc.command = command;
    wc.cols = cols; wc.rows = rows; wc.term = term;
    wc.app_home = app_home;
    wc.socket_path = worker::worker_socket_path(app_home, name);
    // sun_path is 108 bytes (Linux) / 104 (macOS) — refuse early.
    if (wc.socket_path.size() >= 100)
        return std::unexpected(PtyError{"worker socket path too long"});

    // A live worker already serving this name (orphaned by a previous daemon
    // crash, never re-adopted) is reattached rather than replaced — the shell
    // inside it is the session the user expects.
    int fd = -1;
    std::string spawn_unit;   // systemd-run scope unit (empty on fork path)
    pid_t wpid = -1;
    if (worker::ping_worker(wc.socket_path)) {
        fd = worker::connect_to_worker(wc.socket_path, 2000);
        log_event("session_worker_reattach_orphan", name);
    } else {
        wpid = worker::spawn_session_worker(wc, exe_path, &spawn_unit);
        // 0 = spawned via systemd-run (worker pid arrives via the pid file);
        // >0 = direct-fork worker pid; -1 = failure.
        log_event("session_worker_spawn", name +
                  " via=" + std::string(wpid == 0 ? "systemd-run" :
                                        (wpid > 0 ? "fork" : "FAILED")));
        if (wpid < 0) return std::unexpected(PtyError{"worker spawn failed"});
        // Wait for the socket to come up (worker binds before the PTY child).
        // r3 fix (P3): the old fixed 60-iteration loop burned its whole budget in
        // ~3s on the ENOENT path (connect to a missing socket fails instantly) and
        // gave up while slow-but-healthy starts were still pending: systemd-run
        // scope cold-start (dbus + unit creation) and macOS per-exec binary
        // verification (AMFI page-in of an ad-hoc-signed binary) both exceed 3s
        // under load (2026-08-31 fleet logs: one linux node lost 1/6 spawns,
        // another peer lost every spawn during the upgrade wave, a mac peer
        // lost all). New policy:
        //   * 12s adaptive budget, 25ms->500ms exponential backoff;
        //   * early-death detection — if the worker pid is gone (fork path:
        //     waitpid WNOHANG; systemd-run path: pid file removed / pid reused as
        //     non-worker), stop waiting and fail fast with the real exit status.
        {
            long waited_us = 0;
            long backoff_us = 25 * 1000;
            constexpr long kBudgetUs = 12 * 1000 * 1000;
            bool worker_dead = false;
            int worker_exit_status = -1;
            while (fd < 0 && waited_us < kBudgetUs && !worker_dead) {
                fd = worker::connect_to_worker(wc.socket_path, 120);
                if (fd >= 0) break;
                ::usleep(backoff_us);
                waited_us += backoff_us;
                backoff_us = std::min(backoff_us * 2, 500L * 1000);
                if (wpid > 0) {
                    // Direct-fork path: reap without blocking.
                    int wstat = 0;
                    const pid_t r = ::waitpid(wpid, &wstat, WNOHANG);
                    if (r == wpid) {
                        worker_dead = true;
                        worker_exit_status = WIFEXITED(wstat)
                            ? WEXITSTATUS(wstat) : -1;
                    }
                } else if (wpid == 0) {
                    // systemd-run path: real pid lands in <socket>.pid once the
                    // scope is up. Scope creation itself can take seconds; only
                    // treat a STALE pid file as death (pid no longer alive).
                    std::ifstream pf(wc.socket_path + ".pid");
                    int wp = 0;
                    if (pf >> wp) {
                        if (::kill(static_cast<pid_t>(wp), 0) != 0 &&
                            errno == ESRCH) {
                            worker_dead = true;
                        }
                    }
                }
            }
            if (fd < 0 && worker_dead && worker_exit_status >= 0) {
                // Distinguish fast worker exits (bad args, exec failure) from
                // slow starts — the 2026-08-31 class of failures was latency,
                // not death; surfacing the exit code keeps both loud.
                return std::unexpected(PtyError{
                    "worker exited during startup (exit=" +
                    std::to_string(worker_exit_status) + ")"});
            }
        }
    }
    // Failure-path diagnostics: identify a version-skewed worker binary.
    // A daemon upgraded past its on-disk worker exe (or vice versa) fails
    // every spawn with the same generic error; surface what the worker
    // binary actually is so the skew is loud instead of a silent leak.
    auto worker_version_probe = [](const std::string& exe) {
        std::string probe = exe + " --version 2>/dev/null";
        FILE* pp = ::popen(probe.c_str(), "r");
        if (!pp) return std::string("unknown");
        char buf[96] = {};
        size_t n = ::fread(buf, 1, sizeof(buf) - 1, pp);
        int rc = ::pclose(pp);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) --n;
        buf[n] = '\0';
        std::string v(buf);
        if (v.empty()) v = "no output";
        return v + " (probe exit=" + std::to_string(rc) + ")";
    };
    if (fd < 0) {
        int wexit = -1;
        terminate_worker_and_reap(wpid, &wexit);
        if (wpid == 0 && !spawn_unit.empty())
            stop_session_worker_unit(spawn_unit);   // r4: systemd worker nobody tracks
        std::string msg = "worker socket never appeared";
        if (wpid > 0) {
            msg += " [worker exit=" + std::to_string(wexit) +
                   "; worker --version: " + worker_version_probe(exe_path) + "]";
            log_event("session_worker_spawn_failed", name + " " + msg);
        }
        return std::unexpected(PtyError{msg});
    }

    // READY carries name + child pid (last 4 bytes, u32be).
    worker::WorkerMessage ready;
    if (!worker::worker_recv(fd, ready, 3000) || ready.type != worker::WMSG_READY) {
        ::close(fd);
        int wexit = -1;
        terminate_worker_and_reap(wpid, &wexit);
        if (wpid == 0 && !spawn_unit.empty())
            stop_session_worker_unit(spawn_unit);   // r4: systemd worker nobody tracks
        return std::unexpected(PtyError{"worker READY handshake failed (worker exit=" +
                                        std::to_string(wexit) + ")"});
    }
    pid_t child = -1;
    if (ready.data.size() >= 4)
        child = static_cast<pid_t>(
            worker::read_u32be(ready.data.data() + ready.data.size() - 4));
    if (child <= 0) {
        ::close(fd);
        int wexit = -1;
        terminate_worker_and_reap(wpid, &wexit);
        if (wpid == 0 && !spawn_unit.empty())
            stop_session_worker_unit(spawn_unit);   // r4: systemd worker nobody tracks
        return std::unexpected(PtyError{"worker reported no child pid"});
    }

    // Worker pid file (written by the worker itself; may lag the socket by a
    // beat). Authoritative when spawned via systemd-run (wpid==0), best-effort
    // otherwise.
    for (int i = 0; i < 20; ++i) {   // up to ~1s
        std::ifstream pf(wc.socket_path + ".pid");
        long v = -1;
        if (pf >> v && v > 0) { wpid = static_cast<pid_t>(v); break; }
        if (wpid > 0) break;   // direct-fork pid known; file is best-effort
        ::usleep(50 * 1000);
    }

    // Switch the socket non-blocking for the daemon event loop.
    const int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        ::close(fd);
        if (wpid > 0) ::kill(wpid, SIGTERM);
        if (wpid == 0 && !spawn_unit.empty())
            stop_session_worker_unit(spawn_unit);   // r4: pid file never landed
        return std::unexpected(PtyError{"worker socket nonblock failed"});
    }

    Session s;
    s.name = name; s.command = command;
    s.parent_id = parent_session_id_from_environment();
    s.hosted = true;
    s.worker_pid = wpid;
    s.master_fd = fd;
    s.child_pid = child;   // informational only — NOT our child
    s.generation = ++g_session_generation;
    s.state = SessionState::Running;
    return s;
}

// Queue one framed worker message (partial-write safe) and best-effort flush.
inline void worker_queue_frame(Session& s, worker::WorkerMsgType t,
                               const void* data, size_t len) {
    if (len > 16u * 1024u * 1024u || s.master_fd < 0) return;
    uint8_t hdr[5];
    hdr[0] = static_cast<uint8_t>(t);
    worker::write_u32be(hdr + 1, static_cast<uint32_t>(len));
    s.worker_tx.append(reinterpret_cast<const char*>(hdr), 5);
    if (len > 0 && data)
        s.worker_tx.append(static_cast<const char*>(data), len);
    // Best-effort immediate flush; remainder stays queued for POLLOUT drain.
    while (!s.worker_tx.empty()) {
        const ssize_t n = ::write(s.master_fd, s.worker_tx.data(), s.worker_tx.size());
        if (n > 0) { s.worker_tx.erase(0, static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        s.worker_died = true;
        return;
    }
}

// Result of draining a hosted session's worker socket.
struct HostedPump {
    std::string output;      // WMSG_OUTPUT payloads → normal fanout path
    std::string scrollback;  // WMSG_SCROLLBACK payloads → scrollback only
};

// Drain available worker frames; reassembles partial frames across calls.
// Sets s.worker_died on WMSG_DIED, EOF, or stream desync. Never blocks
// (socket is O_NONBLOCK).
inline HostedPump pump_hosted_session(Session& s) {
    HostedPump out;
    if (!s.hosted || s.master_fd < 0 || s.worker_died) return out;

    std::array<char, 16384> buf{};
    for (;;) {
        const ssize_t n = ::read(s.master_fd, buf.data(), buf.size());
        if (n > 0) { s.worker_rx.append(buf.data(), static_cast<size_t>(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        s.worker_died = true;   // EOF (n==0) or hard error
        break;
    }

    // Reassemble frames: [type:1][len:4be][payload]
    size_t off = 0;
    while (s.worker_rx.size() - off >= 5) {
        const auto* p = reinterpret_cast<const uint8_t*>(s.worker_rx.data() + off);
        const uint8_t type = p[0];
        const uint32_t len = worker::read_u32be(p + 1);
        if (len > 16u * 1024u * 1024u) {      // desync guard — drop the worker
            s.worker_died = true;
            break;
        }
        if (s.worker_rx.size() - off < 5u + len) break;   // partial frame
        const char* payload = s.worker_rx.data() + off + 5;
        switch (static_cast<worker::WorkerMsgType>(type)) {
            case worker::WMSG_OUTPUT:
                out.output.append(payload, len);
                break;
            case worker::WMSG_SCROLLBACK:
                out.scrollback.append(payload, len);
                break;
            case worker::WMSG_DIED:
                if (len >= 8) {
                    s.worker_exit_code = static_cast<int32_t>(worker::read_u32be(
                        reinterpret_cast<const uint8_t*>(payload)));
                    s.worker_signal_num = static_cast<int32_t>(worker::read_u32be(
                        reinterpret_cast<const uint8_t*>(payload) + 4));
                }
                s.worker_died = true;
                break;
            default:
                break;   // READY/PONG/ERROR — nothing to do in the pump
        }
        off += 5u + len;
    }
    if (off > 0) s.worker_rx.erase(0, off);
    if (s.worker_rx.size() > 32u * 1024u * 1024u) {  // runaway guard
        s.worker_rx.clear();
        s.worker_died = true;
    }
    return out;
}
#endif // !_WIN32

