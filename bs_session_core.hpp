// SPDX-License-Identifier: BUSL-1.1
// bs_session_core.hpp — Session + PTY management (extracted from bs-protocol.h)
#pragma once

#include "bs_osc52.hpp"

namespace bs::mesh {

// ── PtyError ──────────────────────────────────────────────────────

struct PtyError {
    std::string message;
};


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

// Forward decls — full definitions live next to prepare_session_command.
[[nodiscard]] bool is_windows_cli_oneshot_command(const std::string& command);
[[nodiscard]] bool command_has_direct_windows_exe_token(const std::string& command);

[[nodiscard]] std::expected<Session, PtyError> create_session(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
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
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr, nullptr,
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
        CloseHandle(pi.hThread);

        Session s;
        s.name = name;
        s.command = command;
        s.master_fd = out_r;
        s.write_handle = in_w;
        s.child_pid = pi.hProcess;
        s.hpcon = nullptr;
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

    // Set TERM environment for terminal-aware children.
    const std::wstring wide_term = utf8_to_wide(term);
    SetEnvironmentVariableW(L"TERM", wide_term.c_str());

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(
        application.empty() ? nullptr : application.c_str(),
        mutable_cmdline.data(),
        nullptr, nullptr,           // process/thread security
        FALSE,                      // ConPTY child inherits no daemon handles
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP,
        nullptr,                    // environment (use parent's)
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

    CloseHandle(pi.hThread);

    Session s;
    s.name = name;
    s.command = command;
    s.master_fd = hPipeOutRead;     // read: child stdout -> server
    s.write_handle = hPipeInWrite;  // write: server -> child stdin
    s.child_pid = pi.hProcess;      // process handle
    s.hpcon = hPC;                  // for ResizePseudoConsole
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
    s.name = name; s.command = command;
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

// ── 2.0.8 P5: Cross-platform Computer Use dispatch ─────────────────────
// Dispatches CuaRequestMsg to the appropriate OS backend.

#ifndef _WIN32
[[nodiscard]] std::optional<std::string> find_binary(std::string_view name) {
    if (name.find('/') != std::string_view::npos) {
        std::filesystem::path p{name};
        if (::access(p.c_str(), X_OK) == 0) return p.string();
        return std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return std::nullopt;
    std::string path_str(path_env);
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(':', start);
        std::string_view part = end == std::string::npos
            ? std::string_view(path_str).substr(start)
            : std::string_view(path_str).substr(start, end - start);
        std::filesystem::path dir = part.empty() ? std::filesystem::path(".") : std::filesystem::path(part);
        std::filesystem::path candidate = dir / name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate.string();
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}
#endif // _WIN32

// CUA helper constants and helpers (used by bs-cua-helper.h)
inline constexpr uint16_t kCuaHelperPort = 19986;
[[nodiscard]] inline std::string cua_helper_token_path(const std::string& app_home) {
    return (std::filesystem::path(app_home) / "cua-helper-token").string();
}

// Cross-platform socket close helper for cua_helper_rpc
inline void close_socket(int fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

// CUA helper RPC: call the user-session helper at localhost:19986
// The helper runs in the interactive desktop session (where capture/input work).
// Returns {status=1, error} if helper not running — caller falls back.
[[nodiscard]] inline CuaResponseMsg cua_helper_rpc(const CuaRequestMsg& req, const std::string& app_home) {
    CuaResponseMsg resp;
    resp.status = 1;

    // Load auth token from app home
    std::string token_path = app_home + "/cua-helper-token";
    std::ifstream tf(token_path);
    if (!tf) { resp.error = "no cua-helper token"; return resp; }
    std::string token((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
    if (token.empty()) { resp.error = "empty cua-helper token"; return resp; }

    int sfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { resp.error = "socket failed"; return resp; }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(kCuaHelperPort);
    if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) < 0) {
        close_socket(sfd);
        resp.error = "cua-helper not running (connect failed)";
        return resp;
    }
    // P2: set 10s recv timeout so recv loop can't hang forever
#ifdef _WIN32
    DWORD rcv_timeout_ms = 10000;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_timeout_ms, sizeof(rcv_timeout_ms));
#else
    struct timeval rcv_tv { 10, 0 };
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
#endif

    // Send request as JSON line
    nlohmann::json j;
    j["action"] = req.action;
    j["x"] = req.x;
    j["y"] = req.y;
    j["button"] = req.button;
    j["hid_key"] = req.hid_key;
    j["modifiers"] = req.modifiers;
    j["text"] = req.text;
    j["token"] = token;
    std::string line = j.dump() + "\n";
    if (send(sfd, line.data(), (int)line.size(), 0) <= 0) {
        close_socket(sfd);
        resp.error = "cua-helper send failed";
        return resp;
    }

    // Read response
    std::string acc;
    char buf[65536];
    while (true) {
        int n = (int)recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        acc.append(buf, n);
        if (acc.find('\n') != std::string::npos) break;
    }
    close_socket(sfd);

    if (acc.empty()) { resp.error = "cua-helper no response"; return resp; }
    try {
        auto r = nlohmann::json::parse(acc);
        resp.status = r.value("status", 1);
        resp.error = r.value("error", "");
        resp.screen_w = r.value("screen_w", 0);
        resp.screen_h = r.value("screen_h", 0);
        resp.format = r.value("format", 0);
        if (r.contains("data") && r["data"].is_string()) {
            // Base64 decode inline (helper sends base64-encoded image data)
            std::string b64 = r["data"].get<std::string>();
            static const std::string b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            int val = 0, valb = -8;
            for (char c : b64) {
                if (c == '=') break;
                auto pos = b64chars.find(c);
                if (pos == std::string::npos) continue;
                val = (val << 6) | (int)pos;
                valb += 6;
                if (valb >= 0) {
                    resp.data.push_back((uint8_t)((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
        }
    } catch (const std::exception& e) {
        resp.error = std::string("cua-helper parse error: ") + e.what();
    }
    return resp;
}

[[nodiscard]] CuaResponseMsg cua_execute(const CuaRequestMsg& req, const std::string& app_home = "") {
    CuaResponseMsg resp;
    resp.status = 0;

#ifdef _WIN32
    // Windows: use PowerShell GDI screen capture (v2.0.11 P5c)
    if (req.action == 6) {
        std::string tmp_path;
        char tmpl[MAX_PATH];
        char tmpPathBuf[MAX_PATH];
        GetTempPathA(sizeof(tmpPathBuf), tmpPathBuf);
        GetTempFileNameA(tmpPathBuf, "bsc", 0, tmpl);
        tmp_path = std::string(tmpl) + ".png";
        ::u

... [OUTPUT TRUNCATED - 27,948 chars omitted out of 77,875 total] ...

ffix_len = 8;  // ".command"
            std::string name = key_str.substr(
                prefix_len, key_str.size() - prefix_len - suffix_len);
            if (!name.empty()) cfg.session_commands[std::move(name)] = std::string(val);
        }
        // ── seed <name> <addr> ───────────────────────────────
        else if (key_str == "seed") {
            // Parse: seed <name> <addr> [pubkey=<hex>]
            std::string v2(val);
            std::istringstream iss(v2);
            std::string seed_name, seed_addr;
            if (!(iss >> seed_name >> seed_addr)) continue;
            if (seed_name.empty() || seed_addr.empty()) continue;

            PeerEntry parsed;
            parsed.name = std::move(seed_name);
            parsed.addr = std::move(seed_addr);
            std::string extra;
            while (iss >> extra) {
                if (extra.starts_with("pubkey=")) {
                    parsed.pubkey_hex = extra.substr(7);
                }
            }

            // Deduplicate by name: if a seed with this name already exists, update fields.
            bool found = false;
            for (auto& s : cfg.seeds) {
                if (s.name == parsed.name) {
                    s.addr = parsed.addr;
                    s.pubkey_hex = parsed.pubkey_hex;
                    found = true;
                    break;
                }
            }
            if (!found) cfg.seeds.push_back(std::move(parsed));
        }
        // ── discovered <name> <addr> [pubkey=<hex>] [last_seen=<unix>] ──
        else if (key_str == "discovered") {
            // Parse: discovered <name> <addr> [pubkey=<hex>] [last_seen=<ts>]
            std::string v2(val);
            std::istringstream iss(v2);
            std::string d_name, d_addr;
            if (!(iss >> d_name >> d_addr)) continue; // need at least name and addr

            PeerEntry p;
            p.name = d_name;
            p.addr = d_addr;

            std::string extra;
            while (iss >> extra) {
                if (extra.starts_with("pubkey=")) {
                    p.pubkey_hex = extra.substr(7);
                } else if (extra.starts_with("last_seen=")) {
                    auto ts = parse_int(std::string_view(extra).substr(10));
                    if (ts.has_value()) p.last_seen = static_cast<uint64_t>(*ts);
                }
            }
            cfg.discovered.push_back(std::move(p));
        }
        // ── unknown keys silently ignored ───────────────────
    }

    return cfg;
}

[[nodiscard]] std::string parse_ssh_g_hostname(const std::string& expanded) {
    std::istringstream input(expanded);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key)) continue;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key != "hostname") continue;
        std::string hostname;
        if (fields >> hostname) return hostname;
    }
    return {};
}

// ── Peer name resolution: fuzzy matching helpers ──────────────
// NO-FALLBACK CONTRACT: bs resolves peer names, connects, and either
// succeeds or fails with diagnostics. It NEVER invokes ssh/winrm/telnet
// as a fallback transport. There is no code path that does so. This is
// a design invariant — see AUDIT-MOA-2026-08-06.md.

// Classic two-row Wagner-Fischer. Returns edit distance.
[[nodiscard]] inline size_t levenshtein(const std::string& a,
                                        const std::string& b) {
    const auto m = a.size(), n = b.size();
    if (m == 0) return n;
    if (n == 0) return m;
    std::vector<size_t> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = j;
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            size_t cost = std::tolower(static_cast<unsigned char>(a[i - 1])) !=
                          std::tolower(static_cast<unsigned char>(b[j - 1]));
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(curr);
    }
    return prev[n];
}

// Case-insensitive suffix/prefix check for tier-3 matching.
[[nodiscard]] inline bool name_has_segment(const std::string& name,
                                           const std::string& query) {
    if (query.empty() || name.size() < query.size()) return false;
    auto ic_eq = [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    };
    // Exact suffix: "shadow" matches "windows-peer"
    if (name.size() > query.size() &&
        name[name.size() - query.size() - 1] == '-' &&
        std::equal(query.rbegin(), query.rend(), name.rbegin(), ic_eq))
        return true;
    // Exact prefix: "shadow" matches "shadow-df8uluc8"
    if (name.size() > query.size() &&
        name[query.size()] == '-' &&
        std::equal(query.begin(), query.end(), name.begin(), ic_eq))
        return true;
    return false;
}

[[nodiscard]] bool config_peer_name_eq(const std::string& a,
                                       const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

[[nodiscard]] bool import_ssh_alias_peer(
    MeshConfig& cfg,
    const std::string& alias,
    const std::string& expanded_ssh_config) {
    std::string hostname = parse_ssh_g_hostname(expanded_ssh_config);
    std::string resolved_addr = hostname.empty() ? std::string{} : hostname + ":19949";

    auto refresh_existing = [&](std::vector<PeerEntry>& peers) {
        for (auto& peer : peers) {
            if (!config_peer_name_eq(peer.name, alias)) continue;
            if (!resolved_addr.empty()) peer.addr = resolved_addr;
            return true;
        }
        return false;
    };
    if (refresh_existing(cfg.seeds) || refresh_existing(cfg.discovered)) return true;
    if (resolved_addr.empty()) return false;

    // Only copy pubkey when exactly ONE peer has that addr (multi-peer shared
    // host must not blindly inherit another peer's key — name collision fix).
    auto copy_identity_for_addr = [&](const std::vector<PeerEntry>& peers) {
        const PeerEntry* match = nullptr;
        int count = 0;
        for (const auto& peer : peers) {
            if (peer.addr == resolved_addr && !peer.pubkey_hex.empty()) {
                match = &peer;
                ++count;
            }
        }
        return (count == 1) ? match->pubkey_hex : std::string{};
    };
    std::string pubkey = copy_identity_for_addr(cfg.seeds);
    if (pubkey.empty()) pubkey = copy_identity_for_addr(cfg.discovered);
    cfg.seeds.push_back(PeerEntry{alias, resolved_addr, std::move(pubkey), 0});
    return true;
}

[[nodiscard]] std::string trusted_peer_pubkey(const MeshConfig& cfg,
                                               const std::string& peer_name) {
    for (const auto& peer : cfg.seeds) {
        if (config_peer_name_eq(peer.name, peer_name)) return peer.pubkey_hex;
    }
    for (const auto& peer : cfg.discovered) {
        if (config_peer_name_eq(peer.name, peer_name)) return peer.pubkey_hex;
    }
    return {};
}

[[nodiscard]] bool peer_identity_matches(const std::string& expected,
                                         const std::string& actual) {
    return !expected.empty() && expected == actual;
}

// ── Outbound peer identity (mesh + direct) ─────────────────────────
// Independent review 2026-07-16 P0-1: mesh connector must not trust TLS alone.
struct OutboundPeerVerifyResult {
    bool ok = false;
    std::string reason;
};

[[nodiscard]] const PeerEntry* find_peer_entry_by_addr(const MeshConfig& cfg,
                                                       const std::string& addr) {
    for (const auto& peer : cfg.seeds) {
        if (peer.addr == addr) return &peer;
    }
    for (const auto& peer : cfg.discovered) {
        if (peer.addr == addr) return &peer;
    }
    return nullptr;
}

// Single verification routine for outbound links: pin ↔ cert ↔ Hello.
// require_pin: when true, empty expected_pubkey is a hard fail.
[[nodiscard]] OutboundPeerVerifyResult verify_outbound_peer_identity(
    const std::string& expected_pubkey,
    const std::string& cert_pubkey,
    const std::string& hello_pubkey,
    const std::string& expected_name,
    const std::string& hello_name,
    bool require_pin) {
    if (cert_pubkey.empty()) {
        return {false, "empty peer certificate public key"};
    }
    if (require_pin && expected_pubkey.empty()) {
        return {false, "no pinned public key (seed/discovered pubkey= required)"};
    }
    if (!expected_pubkey.empty() &&
        !peer_identity_matches(expected_pubkey, cert_pubkey)) {
        return {false, "certificate public key does not match pin"};
    }
    if (hello_pubkey.empty()) {
        return {false, "empty Hello pubkey"};
    }
    if (hello_pubkey != cert_pubkey) {
        return {false, "Hello pubkey does not match TLS certificate key"};
    }
    if (hello_name.empty()) {
        return {false, "empty Hello node name"};
    }
    // 2.0.8 MoA fix: reject control chars (log/IPC injection via node name).
    for (unsigned char ch : hello_name)
        if (ch < 0x20 || ch == 0x7f)
            return {false, "Hello node name contains control characters"};
    if (!expected_name.empty() &&
        !config_peer_name_eq(expected_name, hello_name)) {
        return {false, "Hello node name does not match expected peer name"};
    }
    return {true, {}};
}

// Inbound links are already authorized by the certificate callback, but the
// application Hello still has to identify the same key and must not claim a
// configured name belonging to another key. Otherwise an authorized peer can
// impersonate a different peer in name-based command routing.
[[nodiscard]] OutboundPeerVerifyResult verify_inbound_peer_identity(
    const MeshConfig& cfg,
    const std::string& cert_pubkey,
    const std::string& hello_pubkey,
    const std::string& hello_name) {
    if (cert_pubkey.empty()) return {false, "empty peer certificate public key"};
    if (hello_pubkey.empty()) return {false, "empty Hello pubkey"};
    if (hello_pubkey != cert_pubkey) {
        return {false, "Hello pubkey does not match TLS certificate key"};
    }
    if (hello_name.empty()) return {false, "empty Hello node name"};
    // 2.0.8 MoA fix: reject control chars in node names — a \n-bearing name
    // forges log lines (log injection) and breaks line-oriented IPC replies.
    for (unsigned char ch : hello_name)
        if (ch < 0x20 || ch == 0x7f)
            return {false, "Hello node name contains control characters"};

    auto check_peer = [&](const PeerEntry& peer) -> std::optional<std::string> {
        // Name collision fix: do NOT reject when cert_pubkey matches a trusted
        // key that belongs to a differently-named peer. Multiple peers may share
        // a host/key legitimately. The name→key binding check below still guards
        // against a different key claiming an existing name.
        if (!peer.pubkey_hex.empty() &&
            config_peer_name_eq(peer.name, hello_name) &&
            peer.pubkey_hex != cert_pubkey) {
            return "Hello node name is pinned to a different certificate key";
        }
        return std::nullopt;
    };
    bool matched_authoritative_seed = false;
    for (const auto& peer : cfg.seeds) {
        if (auto reason = check_peer(peer)) return {false, *reason};
        if (!peer.pubkey_hex.empty() && peer.pubkey_hex == cert_pubkey &&
            config_peer_name_eq(peer.name, hello_name)) {
            matched_authoritative_seed = true;
        }
    }
    // Explicit seed pins are operator-controlled and authoritative. Gossip may
    // retain a peer's previous key after a legitimate rotation; once the seed
    // name/key pair matches, stale discovered entries must not poison that
    // identity indefinitely.
    if (matched_authoritative_seed) return {true, {}};
    for (const auto& peer : cfg.discovered) {
        if (auto reason = check_peer(peer)) return {false, *reason};
    }
    return {true, {}};
}

// ── File transfer path containment (P0-3) ──────────────────────────
[[nodiscard]] std::optional<std::string> sanitize_transfer_filename(
    std::string_view name) {
    if (name.empty() || name.size() > 255) return std::nullopt;
    // Reject absolute paths / drive letters before basename.
    if (name[0] == '/' || name[0] == '\\') return std::nullopt;
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) &&
        name[1] == ':') {
        return std::nullopt;
    }
    std::string s(name);
    // Basename only (reject if empty after strip).
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (s.empty() || s == "." || s == "..") return std::nullopt;
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) {
        return std::nullopt;
    }
    for (unsigned char c : s) {
        if (c < 32 || c == 127) return std::nullopt;
    }
    // Windows reserved device names (case-insensitive).
    std::string upper = s;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    static constexpr const char* kReserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    std::string stem = upper;
    const auto dot = stem.find('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    for (const char* r : kReserved) {
        if (stem == r) return std::nullopt;
    }
    return s;
}

[[nodiscard]] bool path_is_inside_directory(const std::filesystem::path& candidate,
                                            const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root_abs = fs::weakly_canonical(root, ec);
    if (ec) root_abs = fs::absolute(root, ec);
    if (ec) return false;
    fs::path cand_abs = fs::weakly_canonical(candidate, ec);
    if (ec) cand_abs = fs::absolute(candidate, ec);
    if (ec) return false;
    auto root_s = root_abs.lexically_normal().string();
    auto cand_s = cand_abs.lexically_normal().string();
#ifdef _WIN32
    for (char& c : root_s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : cand_s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : root_s) if (c == '/') c = '\\';
    for (char& c : cand_s) if (c == '/') c = '\\';
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (root_s.empty()) return false;
    if (!root_s.empty() && root_s.back() == sep) {
        // ok
    } else {
        root_s.push_back(sep);
    }
    return cand_s == root_s.substr(0, root_s.size() - 1) ||
           cand_s.rfind(root_s, 0) == 0;
}

// ── App home isolation (--config-dir) ─────────────────────────────
// When --config-dir is set, ALL identity/config/receive/state live under that
// directory (not under $HOME/.bridgesessions). Audit residual R1.
struct AppPaths {
    std::string root;
    std::string config;
    std::string received;
    std::string authorized_keys;
    std::string sessions;
    std::string key_pem;
    std::string cert_pem;
    std::string pub;
    std::string logs;
    std::string state;
};

[[nodiscard]] inline AppPaths make_app_paths(std::string root) {
    if (root.empty()) root = expand_home("~/.bridgesessions");
    // Strip trailing slashes
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    const std::filesystem::path root_path(root);
    AppPaths p;
    p.root = root_path.string();
    p.config = (root_path / "config").string();
    p.received = (root_path / "received").string();
    p.authorized_keys = (root_path / "authorized_keys").string();
    p.sessions = (root_path / "sessions.json").string();
    p.key_pem = (root_path / "id_ed25519.pem").string();
    p.cert_pem = (root_path / "id_ed25519-cert.pem").string();
    p.pub = (root_path / "id_ed25519.pub").string();
    p.logs = (root_path / "logs").string();
    p.state = (root_path / "state").string();
    return p;
}

// Rewrite legacy ~/.bridgesessions/... defaults into an isolated app root.
[[nodiscard]] inline std::string resolve_under_app_home(const std::string& path,
                                                        const std::string& app_root) {
    if (path.empty()) return path;
    constexpr std::string_view kLegacy = "~/.bridgesessions";
    if (path == kLegacy || path.starts_with(std::string(kLegacy) + "/") ||
        path.starts_with(std::string(kLegacy) + "\\")) {
        std::string relative = path.substr(kLegacy.size());
        while (!relative.empty() &&
               (relative.front() == '/' || relative.front() == '\\')) {
            relative.erase(relative.begin());
        }
        if (relative.empty()) return std::filesystem::path(app_root).string();
        return (std::filesystem::path(app_root) / relative).string();
    }
    return expand_home(path);
}

inline void apply_app_home_defaults(MeshConfig& cfg, const std::string& app_root) {
    cfg.authorized_keys_path = resolve_under_app_home(cfg.authorized_keys_path, app_root);
    cfg.persistence_path = resolve_under_app_home(cfg.persistence_path, app_root);
}

// ── Per-daemon IPC authentication token ─────────────────────────────
// Each daemon instance generates a fresh CSPRNG token after binding its
// loopback IPC socket. The token is written owner-only under the app home;
// every CLI helper must read it and prepend it to each IPC request.
// There is no unauthenticated fallback.

[[nodiscard]] inline std::string ipc_token_path(const std::string& app_home) {
    return (std::filesystem::path(make_app_paths(app_home).root) /
            "ipc-token").string();
}

[[nodiscard]] inline std::string generate_ipc_token() {
    std::array<uint8_t, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed for IPC token");
    }
    static const char* d = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        token.push_back(d[b >> 4]);
        token.push_back(d[b & 0xF]);
    }
    return token;
}

[[nodiscard]] inline bool write_ipc_token_file(const std::string& app_home,
                                              const std::string& token) {
    return write_private_text_file(ipc_token_path(app_home), token);
}

[[nodiscard]] inline std::string load_ipc_token(const std::string& app_home) {
    std::ifstream f(ipc_token_path(app_home));
    if (!f.is_open()) return {};
    std::string token;
    if (std::getline(f, token)) {
        // Strip trailing CR in case the file was edited on Windows.
        if (!token.empty() && token.back() == '\r') token.pop_back();
    }
    return token;
}

[[nodiscard]] std::string expand_ssh_alias(const std::string& alias) {
    if (alias.empty() || !std::all_of(alias.begin(), alias.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '_' || c == '-';
        })) {
        return {};
    }
#ifdef _WIN32
    std::string command = "ssh -G " + alias + " 2>NUL";
    FILE* pipe = _popen(command.c_str(), "r");
#else
    std::string command = "ssh -G " + alias + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return {};
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

[[nodiscard]] bool import_ssh_alias_peer(MeshConfig& cfg,
                                         const std::string& alias) {
    return import_ssh_alias_peer(cfg, alias, expand_ssh_alias(alias));
}

enum class SessionCommandSource : uint8_t {
    ClientOverride,
    NamedProfile,
    ConfigDefault,
};

struct ResolvedSessionCommand {
    std::string command;
    SessionCommandSource source = SessionCommandSource::ConfigDefault;
};

// Escape a payload for cmd.exe `/S /C "..."`.
// Nested double-quotes must be doubled (`"` → `""`) or cmd terminates the
// outer `/C` string early. Prefer NOT wrapping PowerShell in cmd at all
// (see build_windows_command_line) — empirical: even with doubled quotes,
// `cmd /S /C "powershell -Command ""...| ForEach-Object { $_ }..."""` still
// breaks pipes so cmd tries to run ForEach-Object as its own command.
// `$` itself is not special to cmd; quote/pipe destruction makes `$_` look
// "mistreated". Callers still must protect `$` from *bash* expansion.
[[nodiscard]] std::string escape_for_cmd_slash_c(const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 8);
    for (unsigned char ch : payload) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

// First argv token of a Windows command line (quote-aware, best-effort).
[[nodiscard]] std::string first_windows_cli_token(const std::string& command) {
    size_t i = 0;
    while (i < command.size() && (command[i] == ' ' || command[i] == '\t')) ++i;
    if (i >= command.size()) return {};
    if (command[i] == '"') {
        const size_t end = command.find('"', i + 1);
        if (end == std::string::npos) return command.substr(i + 1);
        return command.substr(i + 1, end - (i + 1));
    }
    const size_t end = command.find_first_of(" \t", i);
    return command.substr(i, end == std::string::npos ? std::string::npos : end - i);
}

// Heuristic: command line starts with a real Windows application rather than a
// cmd builtin (`dir`, `echo`, …). Used to skip cmd /c wrapping so PowerShell
// scriptblocks with `$_` and pipes survive CreateProcess.
[[nodiscard]] bool command_has_direct_windows_exe_token(const std::string& command) {
    std::string token = first_windows_cli_token(command);
    if (token.empty()) return false;
    // basename lower
    size_t slash = token.find_last_of("\\/");
    std::string base = slash == std::string::npos ? token : token.substr(slash + 1);
    for (char& c : base) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".exe") == 0) {
        return true;
    }
    // bare names that SearchPath resolves with PATHEXT
    static constexpr const char* kKnown[] = {
        "powershell", "powershell.exe", "pwsh", "pwsh.exe",
        "cmd", "cmd.exe", "python", "python.exe", "python3", "python3.exe",
        "py", "py.exe",
    };
    for (const char* k : kKnown) {
        if (base == k) return true;
    }
    return false;
}

// True for non-interactive client-override launches that must use anonymous
// pipes (not ConPTY) so --cmd stdout is captured reliably.
[[nodiscard]] bool is_windows_cli_oneshot_command(const std::string& command) {
    if (command.find("/c ") != std::string::npos ||
        command.find("/C ") != std::string::npos ||
        command.find("/c\"") != std::string::npos ||
        command.find("/C\"") != std::string::npos) {
        return true;
    }
    std::string lower = command;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const bool is_ps =
        lower.find("powershell") != std::string::npos ||
        lower.find("pwsh") != std::string::npos;
    if (!is_ps) return false;
    return lower.find("-command") != std::string::npos ||
           lower.find("-encodedcommand") != std::string::npos ||
           lower.find("-file") != std::string::npos ||
           lower.find(" -c ") != std::string::npos ||
           lower.find(" -c\"") != std::string::npos;
}

[[nodiscard]] std::string build_windows_command_line(
    const ResolvedSessionCommand& resolved,
    const std::string& comspec,
    bool direct_executable_available = true) {
    // Named/default profile whose first token is a real application: direct.
    if (resolved.source != SessionCommandSource::ClientOverride &&
        direct_executable_available) {
        return resolved.command;
    }
    // ClientOverride that is already an executable cmdline (powershell …):
    // do NOT wrap in cmd /c. Wrapping destroys nested quotes, pipes, and $_.
    // Empirically even quote-doubling under cmd /S /C still breaks PS pipes.
    if (resolved.source == SessionCommandSource::ClientOverride &&
        direct_executable_available &&
        command_has_direct_windows_exe_token(resolved.command)) {
        return resolved.command;
    }
    // Builtins (`dir`, …) and non-resolvable commands: cmd /c + doubled quotes.
    const std::string shell = comspec.empty() ? "cmd.exe" : comspec;
    return "\"" + shell + "\" /d /s /c \"" +
           escape_for_cmd_slash_c(resolved.command) + "\"";
}

[[nodiscard]] std::string prepare_session_command(
    const ResolvedSessionCommand& resolved) {
#ifdef _WIN32
    const char* comspec = std::getenv("ComSpec");
    // Always SearchPath the first token — including ClientOverride — so
    // powershell.exe skips cmd wrapping while `dir` still gets cmd /c.
    const bool direct_executable_available =
        !resolve_windows_application(utf8_to_wide(resolved.command)).empty() ||
        command_has_direct_windows_exe_token(resolved.command);
    return build_windows_command_line(
        resolved, comspec ? comspec : "cmd.exe", direct_executable_available);
#else
    return resolved.command;
#endif
}

[[nodiscard]] ResolvedSessionCommand resolve_session_command(
    const MeshConfig& cfg,
    const std::string& session_name,
    const std::string& client_command) {
    if (!client_command.empty()) {
        return {client_command, SessionCommandSource::ClientOverride};
    }
    auto it = cfg.session_commands.find(session_name);
    if (it != cfg.session_commands.end() && !it->second.empty()) {
        return {it->second, SessionCommandSource::NamedProfile};
    }
    return {cfg.default_shell, SessionCommandSource::ConfigDefault};
}

// ── save_config — write MeshConfig back to file ──────────────────────

[[nodiscard]] bool save_config(const std::string& path, const MeshConfig& cfg) {
    std::string resolved = expand_home(path);

    std::ofstream f(resolved, std::ios::trunc);
    if (!f.is_open()) return false;

    f << "# bridgesessions mesh config\n";
    f << "# Generated — edit with care\n\n";

    // Node section
    f << "# ── Node identity ──────────────────────────────────\n";
    f << "node.name " << cfg.node_name << "\n";
    f << "node.listen " << cfg.listen_addr << ":" << cfg.listen_port << "\n";
    f << "\n";

    // Mesh section
    f << "# ── Mesh settings ──────────────────────────────────\n";
    f << "mesh.max_peers " << cfg.max_peers << "\n";
    f << "mesh.gossip_interval_secs " << cfg.gossip_interval_secs << "\n";
    f << "mesh.reconnect_backoff_max_secs " << cfg.reconnect_backoff_max_secs << "\n";
    f << "mesh.ping_interval_secs " << cfg.ping_interval_secs << "\n";
    f << "mesh.pong_timeout_secs " << cfg.pong_timeout_secs << "\n";
    f << "mesh.require_seed_pins " << (cfg.require_seed_pins ? "true" : "false") << "\n";
    f << "mesh.mdns_enabled " << (cfg.mdns_enabled ? "true" : "false") << "\n";
    f << "transfer.max_bytes " << cfg.transfer_max_bytes << "\n";
    f << "transport.webrtc_enabled " << (cfg.webrtc_enabled ? "true" : "false") << "\n";
    f << "dht.enabled " << (cfg.dht_enabled ? "true" : "false") << "\n";
    f << "upnp.enabled " << (cfg.upnp_enabled ? "true" : "false") << "\n";
    // Virtual folders
    for (auto& v : cfg.vfolders) {
        std::string prefix = "vfolder." + v.name + ".";
        f << prefix << "local " << v.local_path << "\n";
        f << prefix << "peer " << v.remote_peer << "\n";
        f << prefix << "remote " << v.remote_path << "\n";
        f << prefix << "direction " << v.direction << "\n";
        f << prefix << "interval " << v.sync_interval_secs << "\n";
    }
    f << "\n";

    // Seeds
    f << "# ── Bootstrap peers ────────────────────────────────\n";
    for (const auto& s : cfg.seeds) {
        write_peer_line(f, "seed", s);
    }
    f << "\n";

    // Discovered peers are runtime state learned via trusted mDNS/gossip.
    // They are intentionally NOT persisted so untrusted LAN announcements cannot
    // be written back to the operator's config file.
    (void)cfg.discovered;

    // Sessions
    f << "# ── Session defaults ───────────────────────────────\n";
    f << "sessions.scrollback_lines " << cfg.scrollback_lines << "\n";
    f << "sessions.idle_timeout_hours " << cfg.idle_timeout_hours << "\n";
    f << "sessions.default_shell " << cfg.default_shell << "\n";
    f << "sessions.terminal " << cfg.terminal << "\n";
    f << "sessions.persistence_path " << cfg.persistence_path << "\n";
    f << "sessions.authorized_keys_path " << cfg.authorized_keys_path << "\n";
    if (!cfg.session_commands.empty()) {
        f << "\n# ── Named persistent session commands ───────────────────\n";
        std::vector<std::pair<std::string, std::string>> profiles(
            cfg.session_commands.begin(), cfg.session_commands.end());
        std::sort(profiles.begin(), profiles.end());
        for (const auto& [name, command] : profiles) {
            f << "session." << name << ".command " << command << "\n";
        }
    }

    f.close();
    return true;
}
} // namespace bs::mesh
