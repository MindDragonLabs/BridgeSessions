// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-mesh-support.h — Socket/poll helpers, terminal raw mode, WebRTC/DHT/NAT, worker pool
// Extracted from bs-mesh-controller.h (R6b structural refactor, 2026-09-03)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace or class — parent file provides it.
#pragma once

// ────────────────────────────────────────────────────────────────────
// 11. MESH CONTROLLER — connection manager, event loop, gossip
// ────────────────────────────────────────────────────────────────────

// Platform socket headers
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSESOCK close
#else
// windows.h / winsock2.h already included at top
#define CLOSESOCK closesocket
#endif

enum class ConnectFailReason { None, Refused, Timeout, TlsRejected, HelloRejected };

// R2: bound blocking connect/handshake/read time on a socket so a dead or
// silent peer can't hang the daemon or a CLI command indefinitely. Sets both
// SO_RCVTIMEO and SO_SNDTIMEO. Best-effort: failures are ignored (the prior
// behaviour was no timeout at all, so we never make things worse). Defined here,
// after the POSIX SOCKET/timeval definitions above, so it compiles on both
// platforms.
inline void set_socket_timeouts(SOCKET fd, int ms) {
    if (fd == INVALID_SOCKET) return;
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// Disable Nagle's algorithm on a socket — critical for interactive shells.
// Without this, the TCP stack batches small packets (individual keystrokes)
// for up to 40ms, making typing feel sluggish compared to SSH.
inline void set_tcp_nodelay(SOCKET fd) {
    if (fd == INVALID_SOCKET) return;
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
}

inline bool socket_selectable(SOCKET fd) {
    if (fd == INVALID_SOCKET) return false;
#ifdef _WIN32
    return true;
#else
    // Main event loop uses poll() (no FD_SETSIZE ceiling). Many helper paths
    // still use select(); reject fds >= FD_SETSIZE so FD_SET stays defined.
    return fd >= 0 && static_cast<int>(fd) < FD_SETSIZE;
#endif
}

// ── poll() abstraction (main mesh loop — no FD_SETSIZE ceiling) ────
#ifdef _WIN32
using bs_pollfd = WSAPOLLFD;
// Winsock2 uses POLLRDNORM/POLLWRNORM; some SDKs also define POLLIN/POLLOUT.
#ifndef POLLIN
#define POLLIN  POLLRDNORM
#endif
#ifndef POLLOUT
#define POLLOUT POLLWRNORM
#endif
[[nodiscard]] inline int bs_poll(bs_pollfd* fds, unsigned long nfds, int timeout_ms) {
    return WSAPoll(fds, nfds, timeout_ms);
}
#else
using bs_pollfd = pollfd;
[[nodiscard]] inline int bs_poll(bs_pollfd* fds, nfds_t nfds, int timeout_ms) {
    return poll(fds, nfds, timeout_ms);
}
#endif
[[nodiscard]] inline bool pollfd_readable(const bs_pollfd& p) {
    return (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}
[[nodiscard]] inline bool pollfd_writable(const bs_pollfd& p) {
    return (p.revents & POLLOUT) != 0;
}
[[nodiscard]] inline bool socket_pollable(SOCKET fd) {
    return fd != INVALID_SOCKET;
}
// Lookup helper: find poll result for an fd (linear scan; N is small).
[[nodiscard]] inline const bs_pollfd* find_pollfd(const std::vector<bs_pollfd>& pfds, SOCKET fd) {
    for (const auto& p : pfds) {
        if (static_cast<SOCKET>(p.fd) == fd) return &p;
    }
    return nullptr;
}
[[nodiscard]] inline bool poll_fd_readable(const std::vector<bs_pollfd>& pfds, SOCKET fd) {
    const auto* p = find_pollfd(pfds, fd);
    return p && pollfd_readable(*p);
}
[[nodiscard]] inline bool poll_fd_writable(const std::vector<bs_pollfd>& pfds, SOCKET fd) {
    const auto* p = find_pollfd(pfds, fd);
    return p && pollfd_writable(*p);
}

struct TimedConnectResult {
    bool connected = false;
    bool timed_out = false;
    int error = 0;
};

[[nodiscard]] inline TimedConnectResult connect_socket_with_timeout(
    SOCKET fd, const sockaddr* address, socklen_t address_len, int timeout_ms) {
    TimedConnectResult result;
    if (!socket_pollable(fd) || !address || timeout_ms < 0) {
        result.error = EINVAL;
        return result;
    }

#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
        result.error = WSAGetLastError();
        return result;
    }
    const auto restore_blocking = [&]() {
        u_long blocking = 0;
        ioctlsocket(fd, FIONBIO, &blocking);
    };
#else
    const int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
        result.error = errno;
        return result;
    }
    const auto restore_blocking = [&]() {
        fcntl(fd, F_SETFL, original_flags);
    };
#endif

    const int connect_result =
        connect(fd, address, static_cast<int>(address_len));
    if (connect_result == 0) {
        restore_blocking();
        result.connected = true;
        return result;
    }

#ifdef _WIN32
    const int pending_error = WSAGetLastError();
    if (pending_error != WSAEWOULDBLOCK && pending_error != WSAEINPROGRESS) {
        restore_blocking();
        result.error = pending_error;
        return result;
    }
#else
    if (errno != EINPROGRESS) {
        result.error = errno;
        restore_blocking();
        return result;
    }
#endif

    bs_pollfd write_fd{fd, POLLOUT, 0};
    const int selected = bs_poll(&write_fd, 1, timeout_ms);
    if (selected == 0) {
        result.timed_out = true;
        result.error = ETIMEDOUT;
        restore_blocking();
        return result;
    }
    if (selected < 0) {
#ifdef _WIN32
        result.error = WSAGetLastError();
#else
        result.error = errno;
#endif
        restore_blocking();
        return result;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&socket_error), &error_len) != 0) {
#ifdef _WIN32
        result.error = WSAGetLastError();
#else
        result.error = errno;
#endif
        restore_blocking();
        return result;
    }
    restore_blocking();
    result.error = socket_error;
    result.connected = socket_error == 0;
    return result;
}

[[nodiscard]] inline bool socket_peer_half_closed(SOCKET fd) {
    if (fd == INVALID_SOCKET) return true;
#ifndef _WIN32
#ifdef POLLRDHUP
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLRDHUP;
    if (poll(&pfd, 1, 0) <= 0) return false;
    return (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
#else
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) <= 0) return false;
    return (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
#endif
#else
    return false;
#endif
}

// Default handshake/connect timeout for outbound mesh + CLI paths (R2).
constexpr int kConnectTimeoutMs = 3000;      // bounded: a dead-addr dial must not starve the event loop
constexpr int kHealthConnectTimeoutMs = 5000;
constexpr int kAcceptHandshakeTimeoutMs = 2000;
// Steady-state recv timeout on established peer sockets. SSL_pending() only
// guarantees >=1 buffered byte, not a whole frame, so a frame split across TLS
// records (only the front half buffered) makes read_frame block in SSL_read_ex
// on the rest. With no timeout that stalls the single-threaded event loop until
// the peer sends more. A bounded timeout degrades that to "drop + reconnect"
// (check_conn_read's catch closes the conn; backoff redials) instead of a freeze.
// This applies only after select() reports frame data; idle healthy links do not
// enter the blocking read, so the bound can stay below the ping cadence.
constexpr int kPeerRecvTimeoutMs = 3000;
constexpr uint16_t kDefaultMeshCliPort = 19980;

[[nodiscard]] inline uint16_t resolve_mesh_cli_port(const char* value) {
    if (!value || !*value) return kDefaultMeshCliPort;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535)
        return kDefaultMeshCliPort;
    return static_cast<uint16_t>(parsed);
}

inline uint16_t mesh_cli_port() {
    static const uint16_t port =
        resolve_mesh_cli_port(std::getenv("BRIDGESESSIONS_IPC_PORT"));
    return port;
}

// ── Base64 helpers (RFC 4648, no padding) ──────────────────────
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline std::string b64enc(const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    std::string out; out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t(p[i]) << 16) | (i+1<len ? uint32_t(p[i+1]) << 8 : 0) | (i+2<len ? uint32_t(p[i+2]) : 0);
        out.push_back(kB64[(v>>18)&63]); out.push_back(kB64[(v>>12)&63]);
        out.push_back(i+1<len ? kB64[(v>>6)&63] : '='); out.push_back(i+2<len ? kB64[v&63] : '=');
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}
inline std::string b64enc(const std::string& s) { return b64enc(s.data(), s.size()); }
inline std::string b64dec(const std::string& s) {
    std::string out; out.reserve((s.size() * 3) / 4);
    int t[256] = {}; for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(kB64[i])] = i; t['='] = 0;
    uint32_t acc = 0; int bits = 0;
    for (char c : s) { acc = (acc << 6) | t[static_cast<uint8_t>(c)]; bits += 6; if (bits >= 8) { bits -= 8; out.push_back(static_cast<char>((acc >> bits) & 0xFF)); } }
    return out;
}

inline int tls_last_syscall_errno() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline void append_ssl_connect_error_detail(std::string& detail, int ssl_err) {
    if (ssl_err == SSL_ERROR_SYSCALL) {
        int se = tls_last_syscall_errno();
        if (se != 0)
            detail += " syscall_errno=" + std::to_string(se);
    }
}

inline ConnectFailReason classify_ssl_connect_fail(int ssl_err) {
    if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
        return ConnectFailReason::Timeout;
    if (ssl_err == SSL_ERROR_SYSCALL) {
        int se = tls_last_syscall_errno();
#ifdef _WIN32
        if (se == WSAETIMEDOUT || se == WSAECONNRESET || se == WSAECONNABORTED)
            return ConnectFailReason::Timeout;
#else
        if (se == ETIMEDOUT || se == ECONNRESET || se == ECONNABORTED)
            return ConnectFailReason::Timeout;
#endif
        if (se == 0)
            return ConnectFailReason::TlsRejected;  // clean EOF during handshake
    }
    return ConnectFailReason::TlsRejected;
}

inline bool wait_socket_ready(SOCKET fd, bool want_read, int timeout_ms) {
    if (!socket_pollable(fd)) return false;
    bs_pollfd pfd{fd, static_cast<short>(want_read ? POLLIN : POLLOUT), 0};
    int rc = bs_poll(&pfd, 1, timeout_ms);
    return rc > 0;
}

// Bounded blocking SSL handshake. Some platforms surface socket timeouts as
// SSL_ERROR_WANT_READ/WRITE even on blocking sockets. Retrying immediately can
// spin/hang. Wait for socket readiness until deadline instead.
inline int ssl_handshake_blocking(SSL* ssl, SOCKET fd, bool server_side, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int last_rc = -1;
    for (;;) {
        last_rc = server_side ? SSL_accept(ssl) : SSL_connect(ssl);
        if (last_rc > 0) return last_rc;
        int err = SSL_get_error(ssl, last_rc);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return last_rc;
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return last_rc;
        int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (!wait_socket_ready(fd, err == SSL_ERROR_WANT_READ, std::max(1, remain))) return last_rc;
    }
}

inline int ssl_connect_blocking(SSL* ssl, SOCKET fd, int timeout_ms) {
    return ssl_handshake_blocking(ssl, fd, false, timeout_ms);
}

inline int ssl_accept_blocking(SSL* ssl, SOCKET fd, int timeout_ms) {
    return ssl_handshake_blocking(ssl, fd, true, timeout_ms);
}

#ifndef _WIN32
static std::atomic<bool> g_config_reload_requested{false};
static void sighup_reload_handler(int) { g_config_reload_requested.store(true); }
#endif


// ────────────────────────────────────────────────────────────────────
// 9. TERMINAL RAW MODE (Windows + POSIX) — moved before MeshController for shell_peer
// ────────────────────────────────────────────────────────────────────

struct SavedConsole {
#ifdef _WIN32
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    CONSOLE_SCREEN_BUFFER_INFO buffer_info{};
#else
    struct termios saved_termios {};
#endif
};

struct TermError { std::string msg; };

inline void configure_sigpipe_handling() noexcept {
#ifndef _WIN32
    ::signal(SIGPIPE, SIG_IGN);
#endif
}

inline bool local_input_requests_disconnect(std::string_view input) {
    return input.find('\x03') != std::string_view::npos;
}

inline bool queue_disconnected_input(std::string& pending, std::string_view input) {
    if (local_input_requests_disconnect(input)) return true;
    if (local_input_requests_detach(input)) return true;  // Ctrl-D quits reconnect-wait
    constexpr size_t kMaxPendingInput = 64 * 1024;
    if (pending.size() < kMaxPendingInput) {
        const size_t room = kMaxPendingInput - pending.size();
        pending.append(input.substr(0, room));
    }
    return false;
}

inline std::string terminal_cleanup_sequence() {
    // A remote TUI can leave these modes enabled when its transport disappears.
    // Reset every common mouse protocol plus focus/bracketed-paste, restore the
    // cursor, and leave the alternate screen only when the local client exits.
    return "\x1b[?9l"
           "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1004l"
           "\x1b[?1005l\x1b[?1006l\x1b[?1015l\x1b[?1016l"
           "\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l";
}

inline void cleanup_terminal_modes() {
    std::cout << terminal_cleanup_sequence() << std::flush;
}

#ifdef _WIN32

inline SavedConsole enable_raw_mode(bool forward_ctrl_c = true) {
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SavedConsole saved{};
    GetConsoleMode(hIn, &saved.input_mode);
    GetConsoleMode(hOut, &saved.output_mode);
    GetConsoleScreenBufferInfo(hOut, &saved.buffer_info);
    // Baseline: strip line-editing/echo, enable VT input passthrough.
    DWORD newIn = saved.input_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)
                | ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!forward_ctrl_c) {
        // When signal forwarding is OFF, keep ENABLE_PROCESSED_INPUT so the
        // local console raises a Ctrl-C control event for the CLI to catch
        // (matching POSIX behavior where ISIG is preserved in raw mode).
        newIn |= ENABLE_PROCESSED_INPUT;
    }
    SetConsoleMode(hIn, newIn);
    DWORD newOut = saved.output_mode
                 | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    SetConsoleMode(hOut, newOut);
    return saved;
}

inline void restore_terminal(const SavedConsole& saved) {
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved.input_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved.output_mode);
}

inline std::pair<uint16_t, uint16_t> get_winsize() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    return {static_cast<uint16_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1),
            static_cast<uint16_t>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1)};
}

#else

#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

inline SavedConsole enable_raw_mode(bool forward_ctrl_c = true) {
    SavedConsole saved{};
    if (::tcgetattr(STDIN_FILENO, &saved.saved_termios) < 0) {
        throw std::runtime_error("tcgetattr failed: " + std::string(std::strerror(errno)));
    }
    struct termios raw = saved.saved_termios;
    ::cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    // When --signal-forward is off, keep ISIG so the local terminal
    // delivers SIGINT on Ctrl-C instead of sending byte 0x03 to the
    // remote PTY.  The CLI then catches SIGINT and sends SignalMsg.
    if (forward_ctrl_c) {
        raw.c_lflag &= ~(tcflag_t)ISIG;
    }
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        throw std::runtime_error("tcsetattr failed: " + std::string(std::strerror(errno)));
    }
    return saved;
}

inline void restore_terminal(const SavedConsole& saved) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved.saved_termios);
}

inline std::pair<uint16_t, uint16_t> get_winsize() {
    struct winsize ws {};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0) return {80, 24};
    return {ws.ws_col, ws.ws_row};
}

#endif

class InteractiveTerminalGuard {
    SavedConsole saved_;
    bool active_ = true;
public:
    InteractiveTerminalGuard(bool forward_ctrl_c = true) : saved_(enable_raw_mode(forward_ctrl_c)) {}
    InteractiveTerminalGuard(const InteractiveTerminalGuard&) = delete;
    InteractiveTerminalGuard& operator=(const InteractiveTerminalGuard&) = delete;
    ~InteractiveTerminalGuard() { restore(); }

    const SavedConsole& saved() const noexcept { return saved_; }

    void restore() noexcept {
        if (!active_) return;
        // Cleanup must be interpreted while Windows VT output processing is on.
        try { cleanup_terminal_modes(); } catch (...) {}
        try { restore_terminal(saved_); } catch (...) {}
        active_ = false;
    }
};

inline bool stdin_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

[[nodiscard]] inline bool shell_command_uses_interactive_mode(
    std::string_view command, bool /*stdin_tty*/) {
    // An explicit command is always finite, even when invoked from a PTY.
    // Only an empty command requests an attachable interactive shell.
    return command.empty();
}

// One-shot --cmd shells must not collide on the shared "default" session name.
// Hermes/agents always use -n default (CLI default); unique names isolate each
// invocation even before server-side force-respawn.
[[nodiscard]] inline std::string make_ephemeral_session_name(std::string_view prefix) {
    static std::atomic<uint32_t> seq{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const unsigned pid = static_cast<unsigned>(GetCurrentProcessId());
#else
    const unsigned pid = static_cast<unsigned>(::getpid());
#endif
    return std::string(prefix) + std::to_string(pid) + "-"
        + std::to_string(now & 0xffffff) + "-"
        + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] inline std::string make_ephemeral_cmd_session_name() {
    return make_ephemeral_session_name("cmd-");
}

// `bs <peer>` with no session argument: unique interactive PTY each time.
[[nodiscard]] inline std::string make_ephemeral_shell_session_name() {
    return make_ephemeral_session_name("tty-");
}

// Empty/omitted name → new tty-* session. An explicit name reattaches.
[[nodiscard]] inline std::string resolve_quick_connect_session_name(
        std::string_view requested) {
    if (requested.empty()) return make_ephemeral_shell_session_name();
    return std::string(requested);
}

// Bound noninteractive shell waits so agents fail loud instead of spinning
// until their outer timeout (Hermes 20s/60s/180s). Override with BS_SHELL_TIMEOUT_SEC.
[[nodiscard]] inline int noninteractive_shell_timeout_sec() {
    const char* env = std::getenv("BS_SHELL_TIMEOUT_SEC");
    if (env && *env) {
        try {
            int v = std::stoi(env);
            if (v >= 5 && v <= 7200) return v;
        } catch (...) {}
    }
    return 120;  // default 2 minutes
}

// Health commands can report process exit before their final OutputMsg reaches
// the stream. A successful probe therefore drains until the nonce is observed
// or the transport reaches EOF; failed probes can complete immediately.
[[nodiscard]] inline bool health_probe_drain_complete(
        int32_t exit_code, std::string_view output,
        std::string_view nonce, bool transport_eof) {
    if (exit_code != 0) return true;
    return transport_eof || output.find(nonce) != std::string_view::npos;
}

// ── TLS close_notify helper — clean TLS shutdown before closing socket ──
inline void ssl_close(SSL* ssl, SOCKET sfd) {
    if (ssl && socket_pollable(sfd)) {
        SSL_shutdown(ssl);
        // drain pending data for 1s
        bs_pollfd pfd{sfd, POLLIN, 0};
        (void)bs_poll(&pfd, 1, 1000);
    }
    if (sfd != INVALID_SOCKET) CLOSESOCK(sfd);
}

// ────────────────────────────────────────────────────────────────────
// D15: WebRTC DataChannel wrapper (behind #ifndef BS_NO_WEBRTC)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_NO_WEBRTC
struct WebRtcChannel {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    bool dc_open = false;
    std::mutex dc_mutex;
    std::vector<uint8_t> recv_buf;

    // Create a PeerConnection offering client
    static std::shared_ptr<rtc::PeerConnection> create_offerer(
        const std::string& sdp, std::string& out_local_sdp)
    {
        rtc::Configuration config;
        auto pc = std::make_shared<rtc::PeerConnection>(config);
        pc->setRemoteDescription(rtc::Description(sdp, "offer"));
        auto desc = pc->createAnswer();
        pc->setLocalDescription(desc.type());
        out_local_sdp = std::string(desc);
        return pc;
    }

    // Create a PeerConnection answering client
    static std::shared_ptr<rtc::PeerConnection> create_answerer(
        const std::string& sdp, std::string& out_local_sdp)
    {
        rtc::Configuration config;
        auto pc = std::make_shared<rtc::PeerConnection>(config);
        pc->setRemoteDescription(rtc::Description(sdp, "answer"));
        out_local_sdp = std::string(*pc->localDescription());
        return pc;
    }
};
#endif // BS_NO_WEBRTC

// ────────────────────────────────────────────────────────────────────
// D16: Kademlia-style DHT (behind #ifndef BS_NO_DHT)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_NO_DHT

using NodeId = std::array<uint8_t, 32>;

inline NodeId pubkey_to_node_id(const std::string& pubkey_hex) {
    std::string sha = bs::mesh::sha256_hex(pubkey_hex);
    NodeId id{};
    for (size_t i = 0; i < 32 && i * 2 + 1 < sha.size(); ++i) {
        char buf[3] = {sha[i*2], sha[i*2+1], 0};
        id[i] = static_cast<uint8_t>(std::strtoul(buf, nullptr, 16));
    }
    return id;
}

inline unsigned xor_leading_zeros(const NodeId& a, const NodeId& b) {
    unsigned leading = 0;
    for (size_t i = 0; i < 32; ++i) {
        uint8_t diff = a[i] ^ b[i];
        if (diff == 0) {
            leading += 8;
            continue;
        }
        // Count leading zeros of this byte
        for (int j = 7; j >= 0; --j) {
            if ((diff >> j) & 1) break;
            ++leading;
        }
        break;
    }
    return leading;
}

struct DhtPeer {
    std::string name;
    std::string addr;
    NodeId node_id{};
    uint64_t last_seen = 0;
};

class DhtNode {
    NodeId our_id_;
    std::string our_name_;
    std::string our_addr_;
    static constexpr size_t kBucketSize = 20;
    static constexpr size_t kNumBuckets = 256;
    std::vector<std::vector<DhtPeer>> buckets_{kNumBuckets};

    mutable std::shared_mutex mutex_;

    int bucket_index(const NodeId& id) const {
        unsigned z = xor_leading_zeros(our_id_, id);
        return std::min<int>(static_cast<int>(z), static_cast<int>(kNumBuckets - 1));
    }

public:
    DhtNode() = default;

    void init(const std::string& our_pubkey, const std::string& our_name, const std::string& our_addr) {
        our_id_ = pubkey_to_node_id(our_pubkey);
        our_name_ = our_name;
        our_addr_ = our_addr;
    }

    void bootstrap(const std::vector<DhtPeer>& seeds) {
        std::unique_lock lock(mutex_);
        for (auto& s : seeds) {
            int idx = bucket_index(s.node_id);
            auto& bucket = buckets_[static_cast<size_t>(idx)];
            bool exists = false;
            for (auto& b : bucket) {
                if (b.node_id == s.node_id) { exists = true; break; }
            }
            if (!exists) {
                if (bucket.size() >= kBucketSize) bucket.erase(bucket.begin());
                bucket.push_back(s);
            }
        }
    }

    std::vector<DhtPeer> find_closest(const NodeId& target, int k = 20) const {
        std::shared_lock lock(mutex_);
        std::vector<DhtPeer> all;
        for (auto& bucket : buckets_) {
            for (auto& p : bucket) {
                all.push_back(p);
            }
        }
        std::sort(all.begin(), all.end(), [&](const DhtPeer& a, const DhtPeer& b) {
            // Sort by XOR distance from target.
            // P2 audit fix: iterate i=0..31 (byte 0 = MSB, matches pubkey_to_node_id
            // and xor_leading_zeros). Was iterating 31..0, inverting distance order.
            for (int i = 0; i < 32; ++i) {
                uint8_t da = a.node_id[i] ^ target[i];
                uint8_t db = b.node_id[i] ^ target[i];
                if (da != db) return da < db;
            }
            return false;
        });
        if (all.size() > static_cast<size_t>(k)) all.resize(static_cast<size_t>(k));
        return all;
    }

    void add_peer(const DhtPeer& peer) {
        std::unique_lock lock(mutex_);
        int idx = bucket_index(peer.node_id);
        auto& bucket = buckets_[static_cast<size_t>(idx)];
        for (auto& existing : bucket) {
            if (existing.node_id == peer.node_id) {
                existing.last_seen = peer.last_seen;
                existing.addr = peer.addr;
                return;
            }
        }
        if (bucket.size() >= kBucketSize) {
            bucket.erase(bucket.begin());
        }
        bucket.push_back(peer);
    }

    const NodeId& our_id() const { return our_id_; }
};

#endif // BS_NO_DHT

// ────────────────────────────────────────────────────────────────────
// D17: NAT traversal via UPnP (behind #ifndef BS_NO_NAT)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_NO_NAT

class UpnpNat {
    bool initialized_ = false;
    std::string external_ip_;
    std::string lan_addr_;
    struct UPNPUrls urls_;
    struct IGDdatas data_;
    std::vector<char> devlist_buf_;
    uint16_t mapped_port_ = 0;  // P2 audit fix: track for cleanup

public:
    UpnpNat() {
        std::memset(&urls_, 0, sizeof(urls_));
        std::memset(&data_, 0, sizeof(data_));
    }

    ~UpnpNat() { cleanup(); }

    bool init() {
        char lan_addr[64] = {};
        char wan_addr[64] = {};
        int error = 0;

        // Discover UPnP devices (timeout 2000ms)
        struct UPNPDev* devlist = upnpDiscover(
            2000, nullptr, nullptr, 0, 0, 2, &error);

        if (!devlist) {
            return false;
        }

        // Copy devlist for later cleanup
        struct UPNPDev* cur = devlist;
        while (cur) {
            size_t off = devlist_buf_.size();
            devlist_buf_.resize(off + sizeof(UPNPDev));
            cur = cur->pNext;
        }

        // Get valid IGD
        int ret = UPNP_GetValidIGD(devlist, &urls_, &data_,
                                    lan_addr, sizeof(lan_addr),
                                    wan_addr, sizeof(wan_addr));
        if (ret != 1) {
            freeUPNPDevlist(devlist);
            return false;
        }

        lan_addr_ = lan_addr;
        external_ip_ = wan_addr;
        initialized_ = true;

        freeUPNPDevlist(devlist);
        return true;
    }

    bool setup_port_mapping(uint16_t port) {
        if (!initialized_) return false;

        std::string port_str = std::to_string(port);
        int ret = UPNP_AddPortMapping(
            urls_.controlURL, data_.first.servicetype,
            port_str.c_str(), port_str.c_str(),
            lan_addr_.c_str(),
            "bridgesessions", "TCP", nullptr, "0");

        if (ret == UPNPCOMMAND_SUCCESS) mapped_port_ = port;
        return ret == UPNPCOMMAND_SUCCESS;
    }

    const std::string& external_ip() const { return external_ip_; }
    bool is_initialized() const { return initialized_; }

    void cleanup() {
        if (urls_.controlURL) {
            // P2 audit fix: delete the port mapping we set up (was leaked —
            // mappings accumulated on the router across daemon restarts).
            if (mapped_port_ != 0) {
                std::string port_str = std::to_string(mapped_port_);
                UPNP_DeletePortMapping(
                    urls_.controlURL, data_.first.servicetype,
                    port_str.c_str(), "TCP", nullptr);
                mapped_port_ = 0;
            }
            FreeUPNPUrls(&urls_);
        }
        initialized_ = false;
    }
};

#endif // BS_NO_NAT

// ── Long-operation worker pool (v2.0.6) ─────────────────────────────
// Moves FILE_SEND/RECV wait, EDIT_DL/UP, VFOLDER_SYNC, and remote FileRequest
// work off the MeshController event loop. Each task runs on a fixed-size pool
// of joinable worker threads. The handler receives the IPC socket for wait-style
// operations so progress/final responses can stream without blocking the daemon.

struct LongOperationTask {
    enum class Type {
        FileSendWait,
        FileRecvWait,
        RemoteFileRequest,
        AutoUpgrade,
    };
    Type type;
    std::string peer_name;
    std::string path1;  // local path / remote path / vfolder name
    std::string path2;  // local dest / local path
    SSL* ssl = nullptr;
    SOCKET sock_fd = INVALID_SOCKET;
    std::shared_ptr<std::atomic<bool>> exec_busy;
    std::shared_ptr<std::atomic<bool>> exec_completed;
    SOCKET ipc_fd = INVALID_SOCKET;  // owned by worker for wait-style ops
    std::shared_ptr<std::atomic<bool>> cancelled;
    // Shared "last progress" timestamp. The worker refreshes it on each
    // transfer progress tick; the exec watchdog uses it so a *healthy,
    // progressing* long transfer (file send / vfolder sync) is never killed
    // at the 90s deadline, while a *stalled* transfer (no progress for 90s)
    // still trips it. Set at enqueue time to now.
    std::shared_ptr<std::atomic<std::chrono::steady_clock::time_point::rep>>
        last_progress_at;
};

class LongOperationWorkerPool {
public:
    using Handler = std::function<void(const LongOperationTask&)>;

    explicit LongOperationWorkerPool(size_t thread_count, Handler handler)
        : handler_(std::move(handler)) {
        for (size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~LongOperationWorkerPool() { shutdown(); }

    void enqueue(LongOperationTask task) {
        {
            std::lock_guard lock(mutex_);
            constexpr size_t kMaxQueuedTasks = 64;
            if (shutdown_ || queue_.size() >= kMaxQueuedTasks) {
                if (task.exec_completed) task.exec_completed->store(true);
                if (task.exec_busy) task.exec_busy->store(false);
                if (task.ipc_fd != INVALID_SOCKET) CLOSESOCK(task.ipc_fd);
                return;
            }
            queue_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    size_t pending_count() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    void worker_loop() {
        while (true) {
            LongOperationTask task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
                if (shutdown_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            if (!task.cancelled || !task.cancelled->load()) {
                try {
                    handler_(task);
                } catch (...) {
                    // Worker errors are returned to the IPC client or logged by handler.
                }
            }
            if (task.exec_completed) task.exec_completed->store(true);
            if (task.exec_busy) task.exec_busy->store(false);
            if (task.ipc_fd != INVALID_SOCKET) {
                CLOSESOCK(task.ipc_fd);
            }
        }
    }

    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<LongOperationTask> queue_;
    bool shutdown_ = false;
    Handler handler_;
};

