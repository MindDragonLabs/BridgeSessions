// bs-server: bridgesessions session daemon — single-threaded event loop
// Windows: Winsock2 + WSAPoll + ConPTY support

#include <bstransport/tls.hpp>
#include <bstransport/frame_io.hpp>
#include <bsprotocol/codec.hpp>

#include "session.hpp"
#include "session_manager.hpp"
#include "pty.hpp"
#include "osc52_capture.hpp"
#include "persistence.hpp"
#include "logging.hpp"

#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#ifndef _WIN32
#include <unistd.h>   // getpid() for banner
#endif
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
using ssize_t = long long;
#undef min
#undef max
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include <CLI/CLI.hpp>
#include <filesystem>

using namespace bs::protocol;
using namespace bs::transport;
using namespace bs::server;

extern int cmd_keygen();
extern int cmd_authorize(const char* hex_pubkey);

namespace {

constexpr size_t kMaxConnections = 64;
constexpr size_t kScrollbackChunkLines = 500;

#ifdef _WIN32
static std::atomic<bool> g_running{true};
BOOL WINAPI ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}
#define SOCKET_CLOSE closesocket
#define SOCKET_ERR INVALID_SOCKET
#define socklen_t int
#else
volatile sig_atomic_t g_running = 1;
void sig_handler(int) { g_running = 0; }
#define SOCKET_CLOSE close
#define SOCKET_ERR -1
#endif

[[nodiscard]] bool write_all(intptr_t fd, const char* data, size_t size) {
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(fd);
    DWORD written;
    return WriteFile(h, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
#else
    const char* p = data;
    while (size > 0) {
        ssize_t n = ::write(static_cast<int>(fd), p, size);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        p += n; size -= static_cast<size_t>(n);
    }
    return true;
#endif
}

[[nodiscard]] ssize_t read_pty(intptr_t fd, char* buf, size_t bufsize) {
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(fd);
    DWORD n;
    if (!ReadFile(h, buf, static_cast<DWORD>(bufsize), &n, nullptr)) return -1;
    return static_cast<ssize_t>(n);
#else
    return ::read(static_cast<int>(fd), buf, bufsize);
#endif
}

bool set_cloexec(intptr_t) { return true; }

// ── send_scrollback ───────────────────────────────────────────
constexpr const char* kAppName    = "BridgeSpace Sessions Server";
constexpr const char* kAppVersion = "26.05.31";
void send_scrollback(SSL* ssl, Session& session) {
    std::string content = session.scrollback.read_last_lines(2000);
    if (content.empty()) return;
    size_t pos = 0;
    uint32_t line_count = 0;
    uint32_t total_lines = (uint32_t)std::count(content.begin(), content.end(), '\n');
    while (pos < content.size()) {
        size_t end = pos;
        uint32_t chunk_lines = 0;
        while (end < content.size() && chunk_lines < kScrollbackChunkLines) {
            if (content[end] == '\n') ++chunk_lines;
            ++end;
        }
        ScrollbackMsg chunk;
        chunk.data = content.substr(pos, end - pos);
        chunk.total_lines = total_lines;
        chunk.chunk_index = line_count;
        line_count += chunk_lines;
        try { write_frame(ssl, chunk); } catch (...) { break; }
        try {
            Message ack = read_frame(ssl);
            if (!std::get_if<ScrollbackAckMsg>(&ack)) break;
        } catch (...) { break; }
        pos = end;
    }
}

// ── Per-connection state ──────────────────────────────────────
struct Conn {
    SslPtr ssl;
    Session* session = nullptr;
    std::chrono::steady_clock::time_point last_pong = std::chrono::steady_clock::now();
    std::string owner_id;
#ifdef _WIN32
    SOCKET client_fd = INVALID_SOCKET;
#else
    int client_fd = -1;
#endif
    bool has_acked_scrollback = false;
};

bool conn_valid(const Conn& c) {
#ifdef _WIN32
    return c.client_fd != INVALID_SOCKET;
#else
    return c.client_fd >= 0;
#endif
}

// ── Server main loop ──────────────────────────────────────────
int run_server(const std::string& listen_host, uint16_t listen_port,
               const std::string& cert_file,
               const std::string& key_file, const std::string& auth_file,
               const std::string& persist_path)
{
    std::ios::sync_with_stdio(false);
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2,2), &wsaData);
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler); signal(SIGPIPE, SIG_IGN);
#endif

    bool is_wildcard = listen_host.empty() || listen_host == "*" || listen_host == "0.0.0.0"
                    || listen_host == "[::]" || listen_host == "::";

    // Create listen socket — try dual-stack IPv6 first
    int family = AF_INET6;
    auto listen_fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_fd != SOCKET_ERR) {
        int off = 0;
        ::setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    }
    // If IPv6 socket failed or address is IPv4 literal, use IPv4
    bool is_v4_literal = !is_wildcard && listen_host.find(':') == std::string::npos;
    if (listen_fd == SOCKET_ERR || is_v4_literal) {
        if (listen_fd != SOCKET_ERR) SOCKET_CLOSE(listen_fd);
        family = AF_INET;
        listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    }
    if (listen_fd == SOCKET_ERR)
        throw std::runtime_error("socket() failed");

    int opt = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    if (family == AF_INET6) {
        struct sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(listen_port);
        if (is_wildcard) {
            addr6.sin6_addr = in6addr_any;
        } else {
            std::string v6host = listen_host;
            if (v6host.front() == '[' && v6host.back() == ']')
                v6host = v6host.substr(1, v6host.size() - 2);
            if (::inet_pton(AF_INET6, v6host.c_str(), &addr6.sin6_addr) != 1)
                throw std::runtime_error("--listen: cannot parse address: " + listen_host);
        }
        if (::bind(listen_fd, (struct sockaddr*)&addr6, sizeof(addr6)) != 0)
            throw std::runtime_error("bind failed");
    } else {
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(listen_port);
        if (is_wildcard) addr.sin_addr.s_addr = htonl(INADDR_ANY);
        else if (::inet_pton(AF_INET, listen_host.c_str(), &addr.sin_addr) != 1)
            throw std::runtime_error("--listen: cannot parse IPv4 address: " + listen_host);
        if (::bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0)
            throw std::runtime_error("bind failed");
    }
    if (::listen(listen_fd, 16) != 0)
        throw std::runtime_error("listen() failed");

    ServerConfig cfg;
    cfg.cert_file = cert_file.c_str();
    cfg.key_file = key_file.c_str();
    if (!auth_file.empty()) cfg.authorized_keys_file = auth_file.c_str();
    auto tls_ctx = create_server_context(cfg);
    SessionManager mgr;

    if (!persist_path.empty()) {
        mgr.set_persistence_path(persist_path);
        mgr.load_persisted_sessions();
    }

    std::string display_host = listen_host.empty() ? "0.0.0.0" : listen_host;
    std::cout
        << kAppName << " v" << kAppVersion << "\n"
        << "  listen    : " << display_host << ":" << listen_port << "\n"
        << "  cert      : " << cert_file << "\n"
        << "  key       : " << key_file << "\n"
        << "  auth_keys : " << (auth_file.empty() ? std::string{"<none>"} : auth_file) << "\n"
        << "  max_conns : " << kMaxConnections
#ifdef _WIN32
        << "  (windows: select+ConPTY)"
#else
        << "  (posix: poll+PTY)"
#endif
        << "\n"
        << "  max_frame : " << bs::protocol::MAX_FRAME_SIZE << " bytes\n"
        << "  pid       : "
#ifdef _WIN32
        << ::GetCurrentProcessId()
#else
        << ::getpid()
#endif
        << "\n"
        << "ready.\n" << std::flush;
    log_event("startup",
        "v" + std::string(kAppVersion)
        + " listen=" + display_host + ":" + std::to_string(listen_port)
        + " max_conns=" + std::to_string(kMaxConnections)
        + " cert=" + cert_file
        + " auth=" + (auth_file.empty() ? std::string{"<none>"} : auth_file));

    Conn conns[kMaxConnections]{};
#ifdef _WIN32
    // Windows: select() for sockets, PeekNamedPipe for ConPTY pipes.
    // WSAPoll cannot monitor pipe handles, so we split the concern.
    fd_set read_fds;
#else
    struct pollfd fds[1 + kMaxConnections * 2]{};
#endif
    size_t nfds = 1;
#ifndef _WIN32
    fds[0].fd = listen_fd; fds[0].events = POLLIN;
#else
    (void)nfds; // unused on Windows (select() path)
#endif

    auto last_keepalive = std::chrono::steady_clock::now();

    while (g_running) {
#ifdef _WIN32
        // Build fd_set for select()
        FD_ZERO(&read_fds);
        FD_SET(listen_fd, &read_fds);
        int max_fd = static_cast<int>(listen_fd);
        for (size_t ci = 0; ci < kMaxConnections; ++ci) {
            if (conn_valid(conns[ci])) {
                int fd = static_cast<int>(conns[ci].client_fd);
                FD_SET(fd, &read_fds);
                if (fd > max_fd) max_fd = fd;
            }
        }
        struct timeval tv = {1, 0}; // 1 second timeout
        int ret = ::select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (WSAGetLastError() == WSAEINTR) continue;
            break;
        }
#else
        int ret = ::poll(fds, static_cast<nfds_t>(nfds), 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
#endif

        // Accept new connections
#ifdef _WIN32
        if (FD_ISSET(listen_fd, &read_fds)) {
#else
        if (fds[0].revents & POLLIN) {
#endif
            auto cfd = ::accept(listen_fd, nullptr, nullptr);
            if (cfd != SOCKET_ERR) {
                SslPtr ssl(SSL_new(tls_ctx.get()));
                SSL_set_fd(ssl.get(), static_cast<int>(cfd));
                if (SSL_accept(ssl.get()) > 0) {
                    std::string owner_id = peer_public_key_hex(ssl.get());
                    if (owner_id.empty()) {
                        log_event("close", "no peer pubkey");
                        SOCKET_CLOSE(cfd);
                    } else {
                        for (size_t ci = 0; ci < kMaxConnections; ++ci) {
                            if (!conn_valid(conns[ci])) {
                                conns[ci] = Conn{};
                                conns[ci].owner_id = owner_id;
                                conns[ci].ssl = std::move(ssl);
                                conns[ci].client_fd = cfd;
                                conns[ci].last_pong = std::chrono::steady_clock::now();
                                log_event("accept", "owner=" + owner_id.substr(0, 12) + "… slot=" + std::to_string(ci));
#ifndef _WIN32
                                size_t base = 1 + ci * 2;
                                fds[base].fd = cfd; fds[base].events = POLLIN;
                                fds[base+1].fd = SOCKET_ERR; fds[base+1].events = 0;
                                if (base + 2 > nfds) nfds = base + 2;
#endif
                                goto accepted;
                            }
                        }
                        log_event("close", "no free slot");
                        SOCKET_CLOSE(cfd);
                    }
                } else {
                    int ssl_err = SSL_get_error(ssl.get(), -1);
                    char err_buf[256] = {0};
                    ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
                    log_event("close", std::string("ssl_accept failed: err=") + std::to_string(ssl_err) + " msg=" + err_buf);
                    SOCKET_CLOSE(cfd);
                }
            }
            accepted:;
        }

        // Keepalive — send Ping every 5 seconds
        auto now = std::chrono::steady_clock::now();
        if (now - last_keepalive > std::chrono::seconds(5)) {
            for (size_t ki = 0; ki < kMaxConnections; ++ki) {
                if (conn_valid(conns[ki])) {
                    try { write_frame(conns[ki].ssl.get(), PingMsg{}); } catch (...) {}
                }
            }
            last_keepalive = now;
        }

        // Handle connections
        for (size_t ci = 0; ci < kMaxConnections; ++ci) {
            if (!conn_valid(conns[ci])) continue;

            auto& conn = conns[ci];
#ifdef _WIN32
            int sock_fd = static_cast<int>(conn.client_fd);
#else
            size_t base = 1 + ci * 2;
#endif

            // Pong timeout
            if (now - conn.last_pong > std::chrono::seconds(30)) {
                goto close_conn;
            }

#ifdef _WIN32
            if (FD_ISSET(sock_fd, &read_fds)) {
#else
            if (fds[base].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (fds[base].revents & (POLLHUP | POLLERR)) goto close_conn;
#endif

                Message msg;
                try { msg = read_frame(conn.ssl.get()); }
                catch (...) { goto close_conn; }
                conn.last_pong = std::chrono::steady_clock::now();

                if (auto* at = std::get_if<AttachMsg>(&msg)) {
                    log_event("attach", "name=" + at->session_name + " cols=" + std::to_string(at->cols) + " rows=" + std::to_string(at->rows));
                    std::string sname = at->session_name, cmd;
                    auto colon = sname.find(':');
                    if (colon != sname.npos) { cmd = sname.substr(colon+1); sname = sname.substr(0,colon); }
                    uint16_t c = at->cols ? at->cols : 80, r = at->rows ? at->rows : 24;

                    auto* existing = mgr.get_for(conn.owner_id, sname);
                    bool reattach = (existing && existing->state != SessionState::Recoverable);
                    if (existing && existing->state == SessionState::Recoverable) {
                        auto result = mgr.resurrect_for(conn.owner_id, sname, c, r, at->term);
                        if (!result) { log_event("attach_fail", "resurrect: " + result.error().message); goto close_conn; }
                        conn.session = *result;
                    } else {
                        auto result = mgr.attach_for(conn.owner_id, sname, cmd, c, r, at->term);
                        if (!result) { log_event("attach_fail", "attach_for: " + result.error().message); goto close_conn; }
                        conn.session = *result;
                        if (reattach) send_scrollback(conn.ssl.get(), *conn.session);
                    }
                    log_event("attach_ok", "name=" + sname);
                    conn.has_acked_scrollback = true;
#ifndef _WIN32
                    fds[base+1].fd = conn.session->master_fd;
                    fds[base+1].events = POLLIN;
#endif
                }
                else if (std::get_if<SessionListMsg>(&msg)) {
                    try { write_frame(conn.ssl.get(), SessionListMsg{mgr.list_for(conn.owner_id)}); }
                    catch (...) { goto close_conn; }
                }
                else if (auto* ks = std::get_if<KeystrokeMsg>(&msg)) {
                    if (conn.session) {
#ifdef _WIN32
                        if (!write_all(reinterpret_cast<intptr_t>(conn.session->write_handle),
                                       ks->data.data(), ks->data.size())) goto close_conn;
#else
                        if (!write_all(conn.session->master_fd, ks->data.data(), ks->data.size())) goto close_conn;
#endif
                    }
                }
                else if (auto* rs = std::get_if<ResizeMsg>(&msg)) {
                    if (conn.session) (void)resize_pty(reinterpret_cast<intptr_t>(conn.session->hpcon), rs->cols, rs->rows);
                }
                else if (std::get_if<DetachMsg>(&msg)) {
                    mgr.detach_for(conn.owner_id, conn.session ? conn.session->name : "");
                    try { write_frame(conn.ssl.get(), ExitCodeMsg{}); } catch (...) {}
                    goto close_conn;
                }
                else if (std::get_if<PingMsg>(&msg)) {
                    try { write_frame(conn.ssl.get(), PongMsg{}); } catch (...) { goto close_conn; }
                }
                else if (std::get_if<PongMsg>(&msg)) {
                    conn.last_pong = std::chrono::steady_clock::now();
                }
                else if (auto* sig = std::get_if<SignalMsg>(&msg)) {
                    if (conn.session
#ifdef _WIN32
                        && conn.session->child_pid
#else
                        && conn.session->child_pid > 0
#endif
                    ) {
#ifdef _WIN32
                        DWORD ctrl = CTRL_C_EVENT;
                        switch (sig->signal) {
                            case SignalMsg::SignalType::CtrlC: ctrl = CTRL_C_EVENT; break;
                            case SignalMsg::SignalType::CtrlZ: ctrl = CTRL_BREAK_EVENT; break;
                            case SignalMsg::SignalType::CtrlBackslash: ctrl = CTRL_BREAK_EVENT; break;
                        }
                        GenerateConsoleCtrlEvent(ctrl, 0);
#else
                        int signum = 0;
                        switch (sig->signal) {
                            case SignalMsg::SignalType::CtrlC: signum = SIGINT; break;
                            case SignalMsg::SignalType::CtrlZ: signum = SIGTSTP; break;
                            case SignalMsg::SignalType::CtrlBackslash: signum = SIGQUIT; break;
                        }
                        if (signum) ::kill(-conn.session->child_pid, signum);
#endif
                    }
                }
                else if (auto* cp = std::get_if<ClipboardMsg>(&msg)) {
                    if (conn.session) {
                        std::string paste = "\x1b[200~" + cp->text + "\x1b[201~";
#ifdef _WIN32
                        if (!write_all(reinterpret_cast<intptr_t>(conn.session->write_handle),
                                       paste.data(), paste.size())) goto close_conn;
#else
                        if (!write_all(conn.session->master_fd, paste.data(), paste.size())) goto close_conn;
#endif
                    }
                    ClipboardEchoMsg echo; echo.hash = cp->hash;
                    try { write_frame(conn.ssl.get(), echo); } catch (...) {}
                }
            }

            // PTY output → TLS socket
#ifdef _WIN32
            // Windows: poll ConPTY pipe with PeekNamedPipe
            if (conn.session) {
                DWORD available = 0;
                if (PeekNamedPipe(conn.session->master_fd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                    char buf[65536];
                    DWORD n = 0;
                    if (ReadFile(conn.session->master_fd, buf, sizeof(buf), &n, nullptr) && n > 0) {
                        conn.session->scrollback.write(std::span<const char>(buf, n));
                        conn.session->touch_output();

                        std::string_view raw(buf, n);
                        auto osc = scan_osc52(raw);

                        if (osc.clipboard_text) {
                            ClipboardMsg clip;
                            clip.text = *osc.clipboard_text;
                            clip.hash = bs::protocol::sha256_hex(clip.text);
                            try { write_frame(conn.ssl.get(), clip); } catch (...) {}
                        }

                        OutputMsg out;
                        out.data = std::move(osc.cleaned_text);
                        try { write_frame(conn.ssl.get(), out); } catch (...) { goto close_conn; }
                    } else {
                        // ReadFile failed — pipe broken (child exited)
                        goto close_conn;
                    }
                }
                // Check if child process exited
                if (conn.session->child_pid &&
                    WaitForSingleObject(conn.session->child_pid, 0) == WAIT_OBJECT_0) {
                    // Child exited — drain remaining pipe data, then close
                    goto close_conn;
                }
            }
#else
            if (fds[base+1].revents & (POLLIN | POLLHUP)) {
                if (!conn.session) goto close_conn;
                bool saw_hup = (fds[base+1].revents & POLLHUP) != 0;
                if (fds[base+1].revents & POLLIN) {
                    char buf[65536];
                    ssize_t n = ::read(conn.session->master_fd, buf, sizeof(buf));
                    if (n <= 0) {
                        goto close_conn;
                    }
                    size_t bytes_read = static_cast<size_t>(n);
                    conn.session->scrollback.write(std::span<const char>(buf, bytes_read));
                    conn.session->touch_output();

                    std::string_view raw(buf, bytes_read);
                    auto osc = scan_osc52(raw);

                    if (osc.clipboard_text) {
                        ClipboardMsg clip;
                        clip.text = *osc.clipboard_text;
                        clip.hash = bs::protocol::sha256_hex(clip.text);
                        try { write_frame(conn.ssl.get(), clip); } catch (...) {}
                    }

                    OutputMsg out;
                    out.data = std::move(osc.cleaned_text);
                    try { write_frame(conn.ssl.get(), out); } catch (...) { goto close_conn; }
                }
                if (saw_hup) goto close_conn;
            }
#endif
            continue;

        close_conn:
            log_event("close_conn", "owner=" + conn.owner_id.substr(0, 12) + " has_session=" + std::to_string(conn.session != nullptr));
            if (conn.ssl) {
                try { write_frame(conn.ssl.get(), ExitCodeMsg{}); } catch (...) {}
            }
            if (conn_valid(conn)) SOCKET_CLOSE(conn.client_fd);
#ifndef _WIN32
            fds[base].fd = SOCKET_ERR; fds[base].events = 0;
            fds[base+1].fd = SOCKET_ERR; fds[base+1].events = 0;
#endif
            conn.ssl.reset();
            conn.session = nullptr;
            conn.owner_id.clear();
#ifdef _WIN32
            conn.client_fd = INVALID_SOCKET;
#else
            conn.client_fd = -1;
#endif
            conn.has_acked_scrollback = false;
        }
    }

    mgr.reap_dead();

    if (!persist_path.empty()) {
        if (mgr.save_persisted_sessions()) {
            std::cout << "saved " << mgr.count() << " sessions to " << persist_path << std::endl;
        }
    }

    std::cout << "shutdown (" << mgr.count() << " sessions active) — bye." << std::endl;
    log_event("shutdown", "sessions=" + std::to_string(mgr.count()));
    SOCKET_CLOSE(listen_fd);
    return 0;
}

} // anonymous namespace

// ── main ─────────────────────────────────────────────────────
int main(int argc, char** argv) {
    CLI::App app{"bs-server — bridgesessions session daemon"};
#ifndef __clang_analyzer__
    app.set_version_flag("--version,-V", "0.5.1");
#endif

    std::string listen_arg = "9948";
    std::string cert_file, key_file, auth_file, persist_path, hex_pubkey;

    app.add_option("-l,--listen", listen_arg, "Listen address (port, :port, IPv4:port, or *:port)")->default_val("9948");
    app.add_option("-c,--cert", cert_file, "TLS certificate file (PEM)");
    app.add_option("-k,--key", key_file, "TLS private key file (PEM)");
    app.add_option("--auth", auth_file, "authorized_keys file");
    app.add_option("-p,--persist", persist_path, "Session persistence JSON path");

    auto* auth_cmd = app.add_subcommand("authorize", "Authorize a client public key");
    auth_cmd->add_option("pubkey", hex_pubkey, "Hex-encoded ed25519 public key")->required();
    int auth_result = 0;
    auth_cmd->callback([&]() { auth_result = cmd_authorize(hex_pubkey.c_str()); });

    auto* kg_cmd = app.add_subcommand("keygen", "Generate ed25519 keypair");
    int keygen_result = 0;
    kg_cmd->callback([&]() { keygen_result = cmd_keygen(); });

    try { app.parse(argc, argv); }
    catch (const CLI::ParseError& e) { return app.exit(e); }

    if (app.got_subcommand(auth_cmd)) return auth_result;
    if (app.got_subcommand(kg_cmd)) return keygen_result;

    if (cert_file.empty() || key_file.empty()) {
        auto [c,k] = generate_cert_key_pair("bs-server");
#ifdef _WIN32
        const char* home = getenv("USERPROFILE");
        if (!home) home = getenv("HOMEDRIVE") ? (std::string(getenv("HOMEDRIVE")) + getenv("HOMEPATH")).c_str() : nullptr;
#else
        const char* home = getenv("HOME");
#endif
        std::string dir = home ? std::string(home) + "/.bridgesessions" : "/tmp";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto write_pem = [&](const std::string& name, const std::string& content) {
            std::string path = dir + "/" + name;
            std::ofstream out(path, std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + path);
            out << content; out.close();
            return path;
        };
        cert_file = write_pem("_bs_autocert.pem", c);
        key_file  = write_pem("_bs_autokey.pem", k);
    }

    std::string listen_host;
    uint16_t listen_port = 9948;
    auto colon = listen_arg.rfind(':');
    if (colon == std::string::npos) {
        listen_port = static_cast<uint16_t>(std::stoul(listen_arg));
    } else {
        listen_host = listen_arg.substr(0, colon);
        listen_port = static_cast<uint16_t>(std::stoul(listen_arg.substr(colon + 1)));
    }

    try {
        return run_server(listen_host, listen_port, cert_file, key_file, auth_file, persist_path);
    } catch (const std::exception& e) {
        std::cerr << "bs-server: " << e.what() << std::endl;
        return 1;
    }
}
