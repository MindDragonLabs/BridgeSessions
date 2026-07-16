// SPDX-License-Identifier: BSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-client: bridgesessions terminal relay agent
// Phase 12 — CLI11 argparse

#include <bstransport/tls.hpp>
#include <bstransport/frame_io.hpp>
#include <bsprotocol/codec.hpp>

#include "terminal_raw.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <random>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <CLI/CLI.hpp>

using namespace bs::protocol;
using namespace bs::transport;
using namespace bs::client;
using namespace std::chrono_literals;

// Phase 9: keygen subcommand (defined in keygen.cpp)
extern int cmd_keygen();

// Clipboard bridge header (ObjC++, included in main.mm wrapper)
// Forward-declare if not using ObjC++:
#if defined(__OBJC__) || defined(__OBJC2__)
#include "clipboard_bridge.mm"
#else
namespace bs::client {
class ClipboardBridge {
public:
    using ClipboardCallback = std::function<void(std::string, std::string)>;
    explicit ClipboardBridge(ClipboardCallback) {}
    bool poll() { return false; }
    void write_to_clipboard(std::string_view) {}
    void ack_hash(std::string_view) {}
    void stop() {}
};
} // namespace bs::client
#endif

namespace {

volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_winch = 0;

void sig_handler(int sig) {
    if (sig == SIGWINCH) g_winch = 1;
    else g_running = 0;
}

// ── Exponential backoff with jitter ──────────────────────────
struct Backoff {
    std::chrono::milliseconds current = 100ms;
    std::chrono::milliseconds max = 5000ms;
    std::minstd_rand rng{std::random_device{}()};

    void wait() {
        if (!g_running) return;
        auto jitter = std::uniform_int_distribution<int>(
            -(int)(current.count() / 4), (int)(current.count() / 4))(rng);
        auto delay = std::chrono::milliseconds(current.count() + jitter);
        if (delay < 50ms) delay = 50ms;
        std::cerr << "reconnecting in " << (delay.count() / 1000.0) << "s..." << std::endl;
        std::this_thread::sleep_for(delay);
        current = std::min(current * 2, max);
    }

    void reset() { current = 100ms; }
};

// ── Signal character detection ────────────────────────────────
bool is_signal_char(const char* data, ssize_t n, SignalMsg& out) {
    if (n != 1) return false;
    switch (data[0]) {
        case 0x03: out.signal = SignalMsg::SignalType::CtrlC; return true;
        case 0x1A: out.signal = SignalMsg::SignalType::CtrlZ; return true;
        case 0x1C: out.signal = SignalMsg::SignalType::CtrlBackslash; return true;
    }
    return false;
}

// ── TLS connect helper ────────────────────────────────────────
struct TlsConnection {
    SslCtxPtr ctx;
    SslPtr ssl;
    int sock_fd = -1;
    bool ok = false;
};

TlsConnection tls_connect(const std::string& server_addr, uint16_t server_port,
                          const std::string& cert_file, const std::string& key_file,
                          bool quiet)
{
    TlsConnection c;
    std::string port_str = std::to_string(server_port);
    struct addrinfo hints{}; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* ai = nullptr;
    if (getaddrinfo(server_addr.c_str(), port_str.c_str(), &hints, &ai) != 0 || !ai) {
        if (!quiet) std::cerr << "resolve " << server_addr << ": " << gai_strerror(errno) << std::endl;
        return c;
    }
    c.sock_fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (c.sock_fd < 0) { freeaddrinfo(ai); return c; }
    if (::connect(c.sock_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        if (!quiet) std::cerr << "connect: " << strerror(errno) << std::endl;
        close(c.sock_fd); c.sock_fd = -1; freeaddrinfo(ai); return c;
    }
    freeaddrinfo(ai);

    ClientConfig cfg; cfg.cert_file = cert_file.c_str(); cfg.key_file = key_file.c_str();
    cfg.known_servers_file = "/tmp/bs-known-servers.json";
    try {
        c.ctx = create_client_context(cfg, [](const std::string& fp) {
            std::cerr << "TOFU: " << fp << " (auto-accepted)" << std::endl; return true;
        });
        c.ssl = SslPtr(SSL_new(c.ctx.get()));
        SSL_set_fd(c.ssl.get(), c.sock_fd);
        if (SSL_connect(c.ssl.get()) <= 0) {
            if (!quiet) std::cerr << "SSL_connect failed" << std::endl;
            close(c.sock_fd); c.sock_fd = -1; return c;
        }
    } catch (...) { close(c.sock_fd); c.sock_fd = -1; return c; }
    c.ok = true;
    return c;
}

// ── Core relay loop (shared by stdin/stdout and bridge modes) ──
// io_fd: read keystrokes from, write output to (same fd for bridge, STDIN/STDOUT for terminal)
bool run_relay(TlsConnection& conn, int io_fd,
               const std::string& attach_name, uint16_t cols, uint16_t rows,
               const std::string& term_env, bool quiet, bool is_bridge)
{
    // Attach
    AttachMsg at;
    at.session_name = attach_name; at.cols = cols; at.rows = rows; at.term = term_env;
    write_frame(conn.ssl.get(), at);
    if (!quiet) std::cerr << "attached to '" << attach_name << "'" << std::endl;

    // Clipboard bridge (skip in bridge mode — BridgeSpace handles clipboard)
    std::string last_clipboard_hash;
    ClipboardBridge clipboard([&](std::string text, std::string hash) {
        if (is_bridge || hash == last_clipboard_hash) return;
        ClipboardMsg c; c.text = std::move(text); c.hash = hash;
        last_clipboard_hash = hash;
        try { write_frame(conn.ssl.get(), c); } catch(...) {}
    });

    signal(SIGWINCH, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    struct pollfd fds[2];
    fds[0].fd = io_fd; fds[0].events = POLLIN;
    fds[1].fd = conn.sock_fd; fds[1].events = POLLIN;
    char buf[65536];

    while (g_running) {
        if (g_winch && !is_bridge) {
            g_winch = 0;
            auto ws = get_winsize();
            if (ws && ws->first > 0 && ws->second > 0) {
                ResizeMsg rs; rs.cols = ws->first; rs.rows = ws->second;
                try { write_frame(conn.ssl.get(), rs); } catch(...) { break; }
            }
        }

        int ret = ::poll(fds, 2, 100);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        if (!is_bridge) clipboard.poll();

        // Local I/O (stdin or bridge socket)
        if (fds[0].revents & POLLIN) {
            ssize_t n = ::read(io_fd, buf, sizeof(buf));
            if (n <= 0) break;
            if (!is_bridge && n == 1 && buf[0] == 0x04) {
                try { write_frame(conn.ssl.get(), DetachMsg{}); } catch(...) {}
                return false;
            }
            // In bridge mode, pass control chars as keystrokes; BridgeSpace sends Signal frames separately
            if (!is_bridge) {
                SignalMsg sig;
                if (is_signal_char(buf, n, sig)) {
                    try { write_frame(conn.ssl.get(), sig); } catch(...) { break; }
                    continue;
                }
            }
            KeystrokeMsg ks; ks.data.assign(buf, n);
            try { write_frame(conn.ssl.get(), ks); } catch(...) { break; }
        }

        // Server → local
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            if (fds[1].revents & POLLHUP) { if (!quiet) std::cerr << "server disconnected" << std::endl; break; }
            try {
                auto msg = read_frame(conn.ssl.get());
                if (auto* out = std::get_if<OutputMsg>(&msg)) {
                    ::write(io_fd, out->data.data(), out->data.size());
                } else if (std::get_if<ExitCodeMsg>(&msg)) {
                    return false;
                } else if (std::get_if<SessionDiedMsg>(&msg)) {
                    return false;
                } else if (std::get_if<PingMsg>(&msg)) {
                    try { write_frame(conn.ssl.get(), PongMsg{}); } catch(...) { break; }
                } else if (auto* sb = std::get_if<ScrollbackMsg>(&msg)) {
                    ::write(io_fd, sb->data.data(), sb->data.size());
                    write_frame(conn.ssl.get(), ScrollbackAckMsg{});
                } else if (auto* cg = std::get_if<ClipboardMsg>(&msg)) {
                    if (!is_bridge) { clipboard.write_to_clipboard(cg->text); last_clipboard_hash = cg->hash; }
                } else if (auto* ce = std::get_if<ClipboardEchoMsg>(&msg)) {
                    if (!is_bridge) last_clipboard_hash = ce->hash;
                }
            } catch (...) { break; }
        }
    }

    SSL_shutdown(conn.ssl.get());
    close(conn.sock_fd);
    return true;
}

// ── connect_and_relay ─────────────────────────────────────────
bool connect_and_relay(const std::string& server_addr, uint16_t server_port,
                       const std::string& session_name, const std::string& command_override,
                       const std::string& cert_file, const std::string& key_file,
                       uint16_t cols, uint16_t rows, const std::string& term_env,
                       bool quiet)
{
    std::string attach_name = session_name;
    if (!command_override.empty()) attach_name += ":" + command_override;
    auto conn = tls_connect(server_addr, server_port, cert_file, key_file, quiet);
    if (!conn.ok) return true;
    if (!quiet) std::cerr << "connected to " << server_addr << ":" << server_port
                          << " (TLS handshake complete)" << std::endl;
    return run_relay(conn, STDOUT_FILENO /* write to stdout, read from stdin */,
                     attach_name, cols, rows, term_env, quiet, false);
}

// ── bridge_relay — Unix socket mode (Phase 11) ────────────────
bool bridge_relay(const std::string& bridge_path,
                  const std::string& server_addr, uint16_t server_port,
                  const std::string& session_name, const std::string& command_override,
                  const std::string& cert_file, const std::string& key_file,
                  uint16_t cols, uint16_t rows, const std::string& term_env,
                  bool quiet)
{
    std::string attach_name = session_name;
    if (!command_override.empty()) attach_name += ":" + command_override;

    // Connect to BridgeSpace via Unix socket
    int bridge_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (bridge_fd < 0) {
        if (!quiet) std::cerr << "bridge socket: " << strerror(errno) << std::endl;
        return false;
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, bridge_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(bridge_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (!quiet) std::cerr << "bridge connect " << bridge_path << ": " << strerror(errno) << std::endl;
        close(bridge_fd);
        return false;
    }

    auto conn = tls_connect(server_addr, server_port, cert_file, key_file, quiet);
    if (!conn.ok) { close(bridge_fd); return true; }
    if (!quiet) std::cerr << "bridged to " << server_addr << ":" << server_port
                          << " via " << bridge_path << std::endl;

    bool result = run_relay(conn, bridge_fd, attach_name, cols, rows, term_env, quiet, true);
    close(bridge_fd);
    return result;
}

} // anonymous namespace

// ── main ─────────────────────────────────────────────────────
int main(int argc, char** argv) {
    CLI::App app{"bs-client — bridgesessions terminal relay agent"};
    app.set_version_flag("--version,-V", "0.5.0");

    std::string server_addr = "127.0.0.1";
    uint16_t server_port = 9943;
    std::string session_name = "default";
    std::string command_override;
    std::string cert_file, key_file;
    std::string term_env;
    std::string bridge_path;  // Phase 11: Unix socket path for BridgeSpace
    bool quiet = false;

    app.add_option("-s,--server", server_addr, "Server host:port")->default_val("127.0.0.1:9943");
    app.add_option("-n,--name", session_name, "Session name")->default_val("default");
    app.add_option("-x,--cmd", command_override, "Command override (name:cmd shorthand still works)");
    app.add_option("-c,--cert", cert_file, "TLS certificate file (PEM)");
    app.add_option("-k,--key", key_file, "TLS private key file (PEM)");
    app.add_option("-t,--term", term_env, "TERM environment override");
    app.add_flag("-q,--quiet", quiet, "Suppress status messages");
    app.add_option("-b,--bridge", bridge_path, "BridgeSpace Unix socket path (enables bridge mode)");

    // Parse server:port from the --server option
    app.callback([&]() {
        auto colon = server_addr.find(':');
        if (colon != std::string::npos) {
            server_port = (uint16_t)std::stoul(server_addr.substr(colon+1));
            server_addr = server_addr.substr(0, colon);
        }
    });

    // Subcommand: keygen
    int keygen_result = 0;
    auto* kg_cmd = app.add_subcommand("keygen", "Generate ed25519 keypair");
    kg_cmd->callback([&]() { keygen_result = cmd_keygen(); });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    if (app.got_subcommand(kg_cmd)) return keygen_result;

    // Auto-generate cert if not provided
    if (cert_file.empty() || key_file.empty()) {
        auto [c,k] = generate_cert_key_pair("bs-client");
        cert_file = c; key_file = k;
    }

    // Terminal raw mode (skip in bridge mode)
    struct termios saved_term{};
    if (bridge_path.empty()) {
        auto saved = enable_raw_mode();
        if (!saved) { std::cerr << "raw mode: " << saved.error().message << std::endl; return 1; }
        saved_term = *saved;
    }

    uint16_t cols = 80, rows = 24;
    if (!bridge_path.empty()) {
        cols = 120; rows = 40;  // Bridge mode: fixed default
    } else {
        auto ws = get_winsize();
        if (ws && ws->first > 0) { cols = ws->first; rows = ws->second; }
    }
    const char* term = term_env.empty() ? getenv("TERM") : term_env.c_str();
    std::string term_str = (term && *term) ? term : "xterm-256color";

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // Phase 11: Bridge mode — single-shot, no backoff
    if (!bridge_path.empty()) {
        bridge_relay(bridge_path, server_addr, server_port, session_name,
                     command_override, cert_file, key_file, cols, rows, term_str, quiet);
        return 0;
    }

    Backoff backoff;
    auto deadline = std::chrono::steady_clock::now() + 30s;

    while (g_running && std::chrono::steady_clock::now() < deadline) {
        bool reconnect = connect_and_relay(
            server_addr, server_port, session_name, command_override,
            cert_file, key_file, cols, rows, term_str, quiet);
        if (!reconnect || !g_running) break;
        backoff.wait();
    }

    restore_terminal(saved_term);
    if (g_running && !quiet) {
        std::cerr << "session '" << session_name
                  << "' survived — reattach with: bs-client --server="
                  << server_addr << ":" << server_port
                  << " --name=" << session_name << std::endl;
    }
    return 0;
}
