// Isolated acceptance: real CLI daemon processes, TLS pins, loopback IPC, PTYs.
// Usage: panel_session_acceptance /absolute/path/to/bridgesessions
// Exit 77 is an explicit socket/platform blocker; zero requires every proof.
#include "../../bs-protocol.h"
#include <filesystem>
#include <iostream>
#include <set>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace bs::mesh;
using namespace std::chrono_literals;

#ifndef _WIN32
namespace {
void need(bool ok, const std::string& detail) {
    if (!ok) throw std::runtime_error(detail);
}
// Poll step. The iteration count is harness_wait_ms(), not a private timeout.
constexpr int kHarnessPollMs = 10;
// The larger pubkey will not dial until tie_break_outbound_quiet_ms() elapses.
// That accessor sums the initial window plus EVERY exponential extension
// (kTieBreakMaxExtends = 5 today), which is 756s. The harness does not need
// all of it: one accept window is what must elapse before the first probe
// dial. Budget for the first window plus one more for the dial, TLS
// handshake, and route, and leave room to report. Bounded well under the
// 120s ctest timeout so a failure is reported rather than killed.
[[nodiscard]] constexpr int harness_wait_ms() noexcept {
    return 2 * MeshController::tie_break_accept_window_ms() + 6000;
}
[[nodiscard]] constexpr int harness_wait_polls() noexcept {
    return (harness_wait_ms() + kHarnessPollMs - 1) / kHarnessPollMs;
}
void sleep_poll() {
    std::this_thread::sleep_for(std::chrono::milliseconds(kHarnessPollMs));
}
struct Fd {
    int value = -1;
    explicit Fd(int fd) : value(fd) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { if (value >= 0) ::close(value); }
};
struct Root {
    std::string path;
    Root() {
        char pattern[] = "/tmp/bs-panel-acceptance-XXXXXX";
        auto* result = ::mkdtemp(pattern);
        need(result != nullptr, "mkdtemp failed");
        path = result;
    }
    ~Root() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
struct Identity {
    std::string root, pubkey;
    explicit Identity(const std::string& directory) : root(directory) {
        std::filesystem::create_directories(root);
        ::chmod(root.c_str(), 0700);
        auto paths = make_app_paths(root);
        auto pair = generate_cert_key_pair("panel-test");
        pubkey = pubkey_hex_from_pem(pair.second);
        need(write_private_text_file(paths.cert_pem, pair.first), "write test cert");
        need(write_private_text_file(paths.key_pem, pair.second), "write test key");
        need(write_private_text_file(paths.pub, pubkey + "\n"), "write test pubkey");
    }
};
uint16_t free_port() {
    Fd fd(::socket(AF_INET, SOCK_STREAM, 0));
    need(fd.value >= 0, "socket failed: " + std::to_string(errno));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    need(::bind(fd.value, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "ephemeral bind");
    socklen_t size = sizeof(addr);
    need(::getsockname(fd.value, reinterpret_cast<sockaddr*>(&addr), &size) == 0, "getsockname");
    return ntohs(addr.sin_port);
}
int tcp(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd); return -1;
    }
    set_socket_timeouts(fd, 3000);
    return fd;
}
std::string ipc(uint16_t port, const std::string& token, const std::string& command) {
    Fd fd(tcp(port));
    need(fd.value >= 0, "IPC connect failed on " + std::to_string(port));
    const std::string request = token + " " + command + "\n";
    // Fragment the auth prefix and command; the daemon must frame the whole line.
    size_t sent = 0;
    while (sent < request.size()) {
        size_t amount = sent == 0 ? std::min(size_t{9}, request.size()) : request.size() - sent;
        int n = ::send(fd.value, request.data() + sent, static_cast<int>(amount), 0);
        need(n > 0, "IPC send"); sent += static_cast<size_t>(n);
        if (sent == 9) std::this_thread::sleep_for(2ms);
    }
    std::string response;
    char buffer[4096];
    while (response.find('\n') == std::string::npos && response.size() <= 128 * 1024) {
        int n = ::recv(fd.value, buffer, sizeof(buffer), 0);
        need(n > 0, "IPC EOF/timeout for " + command.substr(0, 100));
        response.append(buffer, static_cast<size_t>(n));
    }
    need(response.back() == '\n' && response.find('\n') == response.size() - 1,
         "IPC returned multiple or partial lines");
    return response;
}
struct Daemon {
    pid_t pid = -1;
    uint16_t port;
    std::string root, token;
    Daemon(const std::string& binary, const Identity& id, MeshConfig config, uint16_t ipc_port)
        : port(ipc_port), root(id.root) {
        need(save_config(root + "/config", config), "save isolated config");
        pid = ::fork();
        need(pid >= 0, "fork daemon");
        if (pid == 0) {
            ::setenv("BRIDGESESSIONS_IPC_PORT", std::to_string(port).c_str(), 1);
            // Test child shells belong to these daemons and cannot survive cleanup.
            ::setenv("BS_SESSION_WORKER", "0", 1);
            int input = ::open("/dev/null", O_RDONLY);
            int log = ::open((root + "/stdout.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (input < 0 || log < 0) ::_exit(126);
            ::dup2(input, STDIN_FILENO); ::dup2(log, STDOUT_FILENO); ::dup2(log, STDERR_FILENO);
            ::close(input); ::close(log);
            ::execl(binary.c_str(), binary.c_str(), "--config-dir", root.c_str(),
                    "--config", (root + "/config").c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
    }
    void ready() {
        const int polls = harness_wait_polls();
        std::string last = "no probe";
        for (int i = 0; i < polls; ++i) {
            int status;
            const pid_t exited = ::waitpid(pid, &status, WNOHANG);
            if (exited == pid) pid = -1;
            need(exited == 0, "daemon exited during startup: " + root);
            token = load_ipc_token(root);
            if (!token.empty()) {
                try {
                    last = ipc(port, token, "DAEMON_PROBE");
                    if (last == "OK bridgesessions\n") return;
                } catch (const std::exception& ex) {
                    last = ex.what();
                } catch (...) {
                    last = "probe failed";
                }
            }
            sleep_poll();
        }
        throw std::runtime_error(
            "daemon readiness timeout after " + std::to_string(harness_wait_ms())
            + "ms root=" + root + " last=" + last);
    }
    std::string call(const std::string& command) const { return ipc(port, token, command); }
    ~Daemon() {
        if (pid < 0) return;
        // Best-effort only in cleanup; proof calls above never swallow a failure.
        for (const auto& name : {"one", "two", "flood", "dead"}) {
            try { (void)call("SESSION_KILL . " + std::string(name)); } catch (...) {}
        }
        ::kill(pid, SIGTERM);
        for (int i = 0; i < 100; ++i) {
            if (::waitpid(pid, nullptr, WNOHANG) == pid) return;
            std::this_thread::sleep_for(10ms);
        }
        ::kill(pid, SIGKILL); (void)::waitpid(pid, nullptr, 0);
    }
};
struct PausedDaemon {
    pid_t pid;
    explicit PausedDaemon(pid_t child) : pid(child) {
        need(::kill(pid, SIGSTOP) == 0, "pause isolated daemon");
    }
    ~PausedDaemon() { (void)::kill(pid, SIGCONT); }
};

std::string receive_pending(int fd) {
    std::string reply;
    char buffer[1024];
    while (reply.find('\n') == std::string::npos) {
        int n = ::recv(fd, buffer, sizeof(buffer), 0);
        need(n > 0, "pending request timed out without an explicit error");
        reply.append(buffer, static_cast<size_t>(n));
    }
    return reply;
}
void send_pending(int fd, const Daemon& daemon) {
    need(fd >= 0, "pending IPC connect");
    const std::string request = daemon.token + " SESSION_SCROLLBACK panel-b two 0 64\n";
    size_t sent = 0;
    while (sent < request.size()) {
        int n = ::send(fd, request.data() + sent, static_cast<int>(request.size() - sent), 0);
        need(n > 0, "pending IPC send"); sent += static_cast<size_t>(n);
    }
}
struct Peer {
    Fd fd;
    SslCtxPtr ctx;
    SslPtr ssl;
    Peer(uint16_t port, const Identity& client, const std::string& expected_key,
         const std::string& name, const std::string& version = version_string_with_local_caps())
        : fd(tcp(port)) {
        need(fd.value >= 0, "TLS TCP connect");
        auto paths = make_app_paths(client.root);
        NodeTlsConfig config;
        config.cert_file = paths.cert_pem; config.key_file = paths.key_pem;
        // Authentication below verifies both TLS certificate and Hello against a pin.
        config.tofu_cb = [](const std::string&) { return true; };
        ctx = create_node_tls(config, TlsMode::Connect);
        ssl.reset(SSL_new(ctx.get()));
        need(ssl != nullptr, "SSL_new"); SSL_set_fd(ssl.get(), fd.value);
        need(set_expected_peer_pubkey(ssl.get(), expected_key), "set TLS peer pin");
        need(SSL_connect(ssl.get()) > 0, "TLS handshake rejected");
        HelloMsg hello; hello.node_name = name; hello.pubkey_hex = client.pubkey; hello.version = version;
        write_frame(ssl.get(), hello, CONTROL_STREAM_ID);
        auto message = read_frame(ssl.get());
        need(std::holds_alternative<HelloMsg>(message), "missing peer Hello");
        auto remote = std::get<HelloMsg>(message);
        need(remote.pubkey_hex == expected_key, "Hello pin mismatch");
        need(peer_public_key_hex(ssl.get()) == expected_key, "TLS certificate pin mismatch");
    }
    Message until(MessageType type) {
        for (int i = 0; i < 100; ++i) {
            auto message = read_frame(ssl.get());
            if (message_type(message) == type) return message;
            need(!std::holds_alternative<SessionDiedMsg>(message), "session creation failed");
        }
        throw std::runtime_error("expected TLS reply absent");
    }
    void spawn(const std::string& name, const std::string& command) {
        AttachMsg attach; attach.session_name = name; attach.command = command;
        attach.cols = 80; attach.rows = 24; attach.term = "xterm";
        write_frame(ssl.get(), attach, CONTROL_STREAM_ID);
        auto ack = std::get<AttachAckMsg>(until(MessageType::AttachAck));
        need(ack.session_name == name && ack.attach_id != 0, "wrong AttachAck session");
        write_frame(ssl.get(), DetachMsg{}, CONTROL_STREAM_ID);
    }
};
struct Read {
    uint64_t next = 0;
    std::string data;
    bool reset = false;
};
Read read(const Daemon& daemon, const std::string& machine, const std::string& session,
          uint64_t offset = 0, uint32_t limit = 65536) {
    std::string reply = daemon.call("SESSION_SCROLLBACK " + machine + " " + session + " " +
                                  std::to_string(offset) + " " + std::to_string(limit));
    std::istringstream fields(reply);
    std::string ok, encoded, reset, extra; Read result;
    need(bool(fields >> ok >> result.next >> encoded) && ok == "OK", "read failed: " + reply);
    if (fields >> reset) { need(reset == "RESET", "unknown read suffix"); result.reset = true; }
    need(!(fields >> extra), "extra read fields");
    if (encoded != "-") {
        auto data = b64dec_strict(encoded, limit);
        need(data.has_value(), "invalid or oversized read base64"); result.data = std::move(*data);
    }
    need(result.next >= offset && result.next >= result.data.size(), "invalid byte cursor");
    return result;
}
void marker(const Daemon& daemon, const std::string& machine, const std::string& session,
            const std::string& expected) {
    uint64_t offset = 0;
    std::string carry;
    for (int i = 0; i < 200; ++i) {
        auto part = read(daemon, machine, session, offset);
        if (part.reset) carry.clear();
        carry += part.data;
        if (carry.find(expected) != std::string::npos) return;
        if (carry.size() > expected.size()) carry.erase(0, carry.size() - expected.size());
        offset = part.next;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("actual PTY marker absent: " + expected);
}
void error(const Daemon& daemon, const std::string& command) {
    need(daemon.call(command).rfind("ERROR ", 0) == 0, "unexpected success: " + command.substr(0, 100));
}
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    std::cerr << "BLOCKED: run this POSIX two-daemon harness on Linux/macOS\n"; return 77;
#else
    if (argc != 2) { std::cerr << "Usage: panel_session_acceptance /absolute/path/to/bridgesessions\n"; return 2; }
    Fd probe(::socket(AF_INET, SOCK_STREAM, 0));
    if (probe.value < 0) {
        std::cerr << "BLOCKED: loopback socket creation failed: " << std::strerror(errno) << "\n";
        return 77;
    }
    configure_sigpipe_handling();
    try {
        const std::string binary = std::filesystem::absolute(argv[1]).string();
        need(::access(binary.c_str(), X_OK) == 0, "daemon binary not executable");
        Root root;
        Identity a(root.path + "/a"), b(root.path + "/b"), client(root.path + "/client"),
                 legacy(root.path + "/legacy"), stranger(root.path + "/stranger");
        const uint16_t mesh_a = free_port(), mesh_b = free_port(), ipc_a = free_port(), ipc_b = free_port();
        need(std::set<uint16_t>{mesh_a, mesh_b, ipc_a, ipc_b}.size() == 4, "ephemeral port collision; rerun");
        MeshConfig ca, cb;
        ca.node_name = "panel-a"; cb.node_name = "panel-b";
        ca.listen_addr = cb.listen_addr = "127.0.0.1";
        ca.listen_port = mesh_a; cb.listen_port = mesh_b;
        ca.auto_upgrade = cb.auto_upgrade = false;
        ca.mdns_enabled = cb.mdns_enabled = false;
        ca.gossip_interval_secs = cb.gossip_interval_secs = 1;
        ca.ping_interval_secs = cb.ping_interval_secs = 300;
        ca.authorized_keys_path = a.root + "/authorized_keys";
        cb.authorized_keys_path = b.root + "/authorized_keys";
        need(write_private_text_file(ca.authorized_keys_path, b.pubkey + "\n" + client.pubkey + "\n" + legacy.pubkey + "\n"), "a auth keys");
        need(write_private_text_file(cb.authorized_keys_path, a.pubkey + "\n" + client.pubkey + "\n"), "b auth keys");
        ca.seeds.push_back(PeerEntry{.name="panel-b", .addr="127.0.0.1:" + std::to_string(mesh_b), .pubkey_hex=b.pubkey});
        Daemon db(binary, b, cb, ipc_b); db.ready();
        Daemon da(binary, a, ca, ipc_a); da.ready();
        // Prove local token auth; the stranger identity must fail before Attach.
        need(ipc(ipc_a, "incorrect-token", "SESSION_KILL . one") == "ERROR unauthorized\n", "IPC auth bypass");
        bool rejected = false;
        try { Peer unauthorized(mesh_b, stranger, b.pubkey, "stranger"); } catch (...) { rejected = true; }
        need(rejected, "untrusted TLS certificate accepted");
        {
            Peer local(mesh_a, client, a.pubkey, "local-creator");
            local.spawn("one", "stty raw -echo; printf READY_LOCAL; exec cat");
        }
        {
            Peer remote(mesh_b, client, b.pubkey, "remote-creator");
            remote.spawn("one", "stty raw -echo; printf READY_REMOTE; exec cat");
            remote.spawn("two", "stty raw -echo; printf READY_TWO; exec cat");
            remote.spawn("dead", "printf DEAD_REMOTE; exit 0");
            // Keep the flood PTY live after producing more than the actual ring capacity.
            remote.spawn("flood", "stty raw -echo; head -c " + std::to_string(kDefaultRingBufferSize + 4096) +
                         " /dev/zero | tr '\\000' F; printf FLOOD_DONE; exec cat");
        }
        marker(da, ".", "one", "READY_LOCAL");
        // Wait for the real pinned mesh route, then require actual remote bytes.
        // Budget tracks the product tie-break quiet period, not a fixed 3s.
        {
            const int polls = harness_wait_polls();
            std::string last;
            for (int i = 0; i < polls; ++i) {
                last = da.call("SESSION_SCROLLBACK panel-b one 0 65536");
                if (last.rfind("OK ", 0) == 0) break;
                if (i + 1 >= polls) {
                    throw std::runtime_error(
                        "pinned mesh route never became ready within "
                        + std::to_string(harness_wait_ms())
                        + "ms; last reply: " + last
                        + "\n  accept window is "
                        + std::to_string(MeshController::tie_break_accept_window_ms())
                        + "ms, and the defer EXTENDS exponentially on expiry"
                        + " (12s -> 24s -> 48s -> 96s -> 192s)."
                        + "\n  The first outbound probe is not allowed until all"
                        + " extensions are exhausted, so a larger test budget"
                        + " cannot fix this."
                        + "\n  This is a product-side tie-break race, not a test"
                        + " budget problem. See TODO-2026-10-04.md B4.");
                }
                sleep_poll();
            }
        }
        marker(da, "panel-b", "one", "READY_REMOTE");
        marker(da, "panel-b", "two", "READY_TWO");
        marker(da, "panel-b", "dead", "DEAD_REMOTE");
        std::this_thread::sleep_for(100ms);
        error(da, "SESSION_INPUT panel-b dead eA");
        auto local_start = read(da, ".", "one").next;
        auto remote_start = read(da, "panel-b", "one").next;
        need(da.call("SESSION_INPUT . one " + b64enc("LOCAL_BYTES\n")) == "OK\n", "local input");
        need(da.call("SESSION_INPUT panel-b one " + b64enc("REMOTE_BYTES\n")) == "OK\n", "remote input");
        need(da.call("SESSION_INPUT panel-b two " + b64enc("TWO_BYTES\n")) == "OK\n", "isolated remote input");
        marker(da, ".", "one", "LOCAL_BYTES"); marker(da, "panel-b", "one", "REMOTE_BYTES");
        marker(da, "panel-b", "two", "TWO_BYTES");
        need(read(da, ".", "one", local_start).data == "LOCAL_BYTES\n", "local byte readback mismatch");
        need(read(da, "panel-b", "one", remote_start).data == "REMOTE_BYTES\n", "remote byte readback mismatch");
        need(read(da, ".", "one").data.find("REMOTE_BYTES") == std::string::npos, "local/remote same-name cross-talk");
        need(read(da, "panel-b", "two").data.find("REMOTE_BYTES") == std::string::npos, "remote session cross-talk");
        // Exact 64KiB request and arbitrary bytes, returned through the actual remote ring.
        std::string payload; for (int i = 0; i < 65536; ++i) payload.push_back(static_cast<char>(i & 255));
        auto cursor = read(da, "panel-b", "two").next;
        need(da.call("SESSION_INPUT panel-b two " + b64enc(payload)) == "OK\n", "64KiB remote input rejected");
        std::string echoed;
        for (int i = 0; i < 400 && echoed.size() < payload.size(); ++i) {
            auto part = read(da, "panel-b", "two", cursor, 4096);
            need(!part.reset, "unexpected gap in 64KiB echo"); cursor = part.next; echoed += part.data;
            std::this_thread::sleep_for(5ms);
        }
        need(echoed == payload, "arbitrary-byte 64KiB echo mismatch");
        for (const auto& machine : {std::string("."), std::string("panel-b")}) {
            error(da, "SESSION_INPUT " + machine + " one " + b64enc(std::string(65537, 'x')));
            error(da, "SESSION_SCROLLBACK " + machine + " one 0 65537");
            error(da, "SESSION_SCROLLBACK " + machine + " one 18446744073709551616 1");
            error(da, "SESSION_SCROLLBACK " + machine + " one -1 1");
            error(da, "SESSION_SCROLLBACK " + machine + " one 18446744073709551615 1");
            error(da, "SESSION_INPUT " + machine + " ../one eA");
            error(da, "SESSION_INPUT " + machine + " missing eA");
            error(da, "SESSION_SCROLLBACK " + machine + " missing 0 1");
            error(da, "SESSION_KILL " + machine + " missing");
            for (const auto& bad : {"A", "AB", "aGk=", "aGk!", "aG k"})
                error(da, "SESSION_INPUT " + machine + " one " + bad);
        }
        error(da, "SESSION_INPUT bad/machine one eA");
        error(da, std::string("SESSION_KILL . one") + std::string(1, '\0') + "ignored");
        error(da, "SESSION_KILL . one\nSESSION_KILL . one");
        marker(da, "panel-b", "flood", "FLOOD_DONE");
        auto reset = read(da, "panel-b", "flood", 0, 17);
        need(reset.reset && reset.data == std::string(17, 'F'), "stale cursor lacks true ring RESET");
        auto following = read(da, "panel-b", "flood", reset.next, 17);
        need(!following.reset && following.next == reset.next + following.data.size() &&
             following.data == std::string(17, 'F'), "limited RESET skipped unread ring bytes");
        need(read(da, "panel-b", "flood", following.next, 0).data.empty(), "zero-limit read");
        {
            Peer old(mesh_a, legacy, a.pubkey, "legacy-peer", "26.09.01+frm2");
            write_frame(old.ssl.get(), PingMsg{}, CONTROL_STREAM_ID); (void)old.until(MessageType::Pong);
            need(da.call("SESSION_KILL legacy-peer one") == "ERROR peer does not support session control\n",
                 "legacy peer was sent an unsupported control frame");
            marker(da, ".", "one", "LOCAL_BYTES");
        }
        // A real stalled daemon must yield a bounded error, and its late reply
        // must not complete the next request. No control operation is fabricated.
        {
            PausedDaemon paused(db.pid);
            auto started = std::chrono::steady_clock::now();
            need(da.call("SESSION_SCROLLBACK panel-b two 0 64") == "ERROR session control timeout\n",
                 "stalled remote read did not fail explicitly");
            need(std::chrono::steady_clock::now() - started < 3500ms, "unbounded remote request timeout");
            Fd abandoned(tcp(ipc_a)); send_pending(abandoned.value, da);
            need(da.call("DAEMON_PROBE") == "OK bridgesessions\n", "pending IPC blocked daemon");
        }
        need(da.call("SESSION_INPUT panel-b two " + b64enc("AFTER_TIMEOUT\n")) == "OK\n",
             "late reply or abandoned IPC broke subsequent input");
        marker(da, "panel-b", "two", "AFTER_TIMEOUT");
        {
            PausedDaemon paused(db.pid);
            Fd pending(tcp(ipc_a)); send_pending(pending.value, da);
            need(da.call("DAEMON_PROBE") == "OK bridgesessions\n", "pending read blocked IPC");
            need(da.call("RECONNECT panel-b").rfind("OK ", 0) == 0, "isolated mesh reconnect failed");
            need(receive_pending(pending.value) == "ERROR mesh connection lost\n",
                 "replaced transport completed an old pending request");
        }
        {
            const int polls = harness_wait_polls();
            std::string last;
            for (int i = 0; i < polls; ++i) {
                last = da.call("SESSION_SCROLLBACK panel-b two 0 64");
                if (last.rfind("OK ", 0) == 0) break;
                if (i + 1 >= polls) {
                    throw std::runtime_error(
                        "pinned mesh failed to recover after reconnect within "
                        + std::to_string(harness_wait_ms())
                        + "ms (tie-break quiet "
                        + std::to_string(MeshController::tie_break_outbound_quiet_ms())
                        + "ms + accept window "
                        + std::to_string(MeshController::tie_break_accept_window_ms())
                        + "ms); last reply: " + last);
                }
                sleep_poll();
            }
        }
        need(da.call("SESSION_INPUT panel-b two " + b64enc("AFTER_RECONNECT\n")) == "OK\n",
             "new transport rejected session input");
        marker(da, "panel-b", "two", "AFTER_RECONNECT");
        need(da.call("SESSION_KILL panel-b one") == "OK\n", "remote kill");
        error(da, "SESSION_INPUT panel-b one eA"); error(da, "SESSION_KILL panel-b one");
        need(da.call("SESSION_INPUT panel-b two " + b64enc("AFTER_KILL\n")) == "OK\n", "remote kill crossed sessions");
        marker(da, "panel-b", "two", "AFTER_KILL");
        need(da.call("SESSION_KILL . one") == "OK\n", "local kill");
        error(da, "SESSION_INPUT . one eA"); error(da, "SESSION_KILL . one");
        need(da.call("SESSION_KILL panel-b two") == "OK\n", "remaining session kill");
        need(da.call("SESSION_KILL panel-b flood") == "OK\n", "flood session kill");
        std::cout << "PASS: two isolated real daemons; pinned TLS and IPC auth; local/remote input, actual byte readback, kill, isolation, RESET, bounds, strict base64, legacy capability rejection, timeout, abandoned IPC and reconnect\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n"; return 1;
    }
#endif
}
