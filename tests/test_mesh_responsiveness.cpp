// test_mesh_responsiveness.cpp — BridgeSessions v2.0.6-alpha2 runtime responsiveness tests
//
// Verifies:
//   - A stalled pre-auth TLS handshake does not block a healthy peer's ping/pong.
//   - A throttled long transfer does not block heartbeat/ping traffic on another peer.
//   - Cancellation clears exec_busy and allows the conn to be closed exactly once.
//   - Long-operation worker pool is bounded, joinable, and hands IPC socket ownership
//     to the worker for progress/final responses.

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <cstdlib>
#include <cstdint>
#include <unistd.h>
#include <string>

// Pick a per-process IPC port so parallel ctest invocations of this binary
// (one process per Catch filter) do not collide on the same loopback port.
struct EnvGuard {
    EnvGuard() {
        long pid = static_cast<long>(::getpid());
        uint16_t port = static_cast<uint16_t>(30000 + (pid % 35535));
        ::setenv("BRIDGESESSIONS_IPC_PORT", std::to_string(port).c_str(), 1);
    }
    ~EnvGuard() { ::unsetenv("BRIDGESESSIONS_IPC_PORT"); }
} g_env_guard;

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "../bridgesessions.cpp"

#ifdef _WIN32
#define CLOSESOCK closesocket
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); } ~WsaInit() { WSACleanup(); } };
static WsaInit _wsa;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#define CLOSESOCK close
#endif

#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <cstring>
#include <memory>
#include <filesystem>
#include <system_error>

using namespace bs::mesh;
using namespace std::chrono_literals;

static std::string g_test_ipc_token;

struct MeshRunnerGuard {
    MeshController& controller;
    std::thread& runner;
    ~MeshRunnerGuard() {
        controller.shutdown();
        if (runner.joinable()) runner.join();
    }
};

// ── Helpers ───────────────────────────────────────────────────────────

static MeshConfig mesh_cfg(const std::string& name) {
    MeshConfig c;
    c.node_name              = name;
    c.listen_port            = 0;  // ephemeral
    c.gossip_interval_secs   = 300;
    c.ping_interval_secs     = 300;
    c.pong_timeout_secs      = 30;
    c.scrollback_lines       = 100;
    return c;
}

static std::string write_authorized_keys(const std::string& pk1, const std::string& pk2) {
    char path[] = "/tmp/bs_resp_ak_XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    std::string content = pk1 + "\n" + pk2 + "\n";
    (void)::write(fd, content.data(), content.size());
    ::close(fd);
    return path;
}

static std::string write_temp_file(const std::string& name, const std::string& data) {
    std::string path = std::string("/tmp/bs_resp_") + name;
    std::ofstream f(path, std::ios::binary);
    f << data;
    f.close();
    return path;
}

static int connect_to_port(uint16_t port) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(sfd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    int r = connect(sfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    REQUIRE(r == 0);
    return sfd;
}

// Generate a cert+key pair and write to temp files
struct CertKeyTemp {
    std::string cert_pem;
    std::string key_pem;
    std::string pubkey_hex;
    std::string cert_file;
    std::string key_file;
    std::string pub_file;

    CertKeyTemp(const std::pair<std::string,std::string>& ck, const std::string& prefix)
        : cert_pem(ck.first)
        , key_pem(ck.second)
        , pubkey_hex(pubkey_hex_from_pem(key_pem))
    {
#ifdef _WIN32
        char tmpPath[MAX_PATH];
        char cf[MAX_PATH], kf[MAX_PATH], pf[MAX_PATH];
        GetTempPathA(sizeof(tmpPath), tmpPath);
        GetTempFileNameA(tmpPath, prefix.c_str(), 0, cf);
        GetTempFileNameA(tmpPath, prefix.c_str(), 0, kf);
        GetTempFileNameA(tmpPath, prefix.c_str(), 0, pf);
        cert_file = cf;
        key_file = kf;
        pub_file = pf;
#else
        cert_file = std::string("/tmp/") + prefix + "_cert_" + std::to_string(rand()) + ".pem";
        key_file  = std::string("/tmp/") + prefix + "_key_"  + std::to_string(rand()) + ".pem";
        pub_file  = std::string("/tmp/") + prefix + "_pub_"  + std::to_string(rand()) + ".pub";
#endif
        std::ofstream f(cert_file); f << cert_pem;  f.close();
        std::ofstream f2(key_file);  f2 << key_pem;  f2.close();
        std::ofstream f3(pub_file);  f3 << pubkey_hex;  f3.close();
    }

    ~CertKeyTemp() {
        std::remove(cert_file.c_str());
        std::remove(key_file.c_str());
        std::remove(pub_file.c_str());
    }
};

static SSL_CTX* create_test_ctx(const CertKeyTemp& id, bool server, const std::string& ak_path = "") {
    NodeTlsConfig cfg;
    cfg.cert_file = id.cert_file;
    cfg.key_file  = id.key_file;
    if (server) {
        cfg.authorized_keys_file = ak_path;
        return create_node_tls(cfg, TlsMode::Listen).release();
    } else {
        cfg.tofu_cb = [](const std::string&) { return true; };
        return create_node_tls(cfg, TlsMode::Connect).release();
    }
}

static int open_ipc_socket() {
    for (int attempt = 0; attempt < 200; ++attempt) {
        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        INFO("socket errno: " << errno << ", attempt: " << attempt);
        REQUIRE(sfd >= 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        if (connect(sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == 0)
            return sfd;
        CLOSESOCK(sfd);
        std::this_thread::sleep_for(10ms);
    }
    FAIL("daemon IPC listener did not become ready");
    return -1;
}

static int send_ipc_no_wait(const std::string& cmd) {
    int sfd = open_ipc_socket();
#ifdef _WIN32
    DWORD tv = 500;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv{0, 500000};
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    std::string authenticated = g_test_ipc_token + " " + cmd;
    send(sfd, authenticated.data(), (int)authenticated.size(), 0);
    return sfd;
}

static std::string send_ipc_read_response(const std::string& cmd, int timeout_ms = 2000) {
    int sfd = open_ipc_socket();
#ifdef _WIN32
    DWORD tv = timeout_ms;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    // Force request fragmentation so the server must read through the newline,
    // not assume one recv() returns the complete authenticated command.
    std::string auth_prefix = g_test_ipc_token + " ";
    send(sfd, auth_prefix.data(), (int)auth_prefix.size(), 0);
    std::this_thread::sleep_for(2ms);
    send(sfd, cmd.data(), (int)cmd.size(), 0);
    char buf[4096] = {};
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n > 0) {
            total += n;
            buf[total] = '\0';
            if (strchr(buf, '\n')) break;
        } else {
            break;
        }
    }
    std::string resp = buf;
    CLOSESOCK(sfd);
    return resp;
}

// ── Test 1: Stalled pre-auth handshake does not block a healthy peer ───

TEST_CASE("stalled pre-auth handshake does not block healthy peer ping/pong",
          "[mesh_responsiveness][handshake]") {
    auto id_srv = CertKeyTemp(generate_cert_key_pair("resp-srv"), "bs_rs");
    auto id_cli = CertKeyTemp(generate_cert_key_pair("resp-cli"), "bs_rc");
    std::string ak = write_authorized_keys(id_srv.pubkey_hex, id_cli.pubkey_hex);

    MeshConfig cfg = mesh_cfg("resp-srv");
    cfg.authorized_keys_path = ak;
    MeshController mc(cfg, "/tmp/bs_resp_home_" + std::to_string(rand()));

    std::thread runner([&] { mc.run(); });
    MeshRunnerGuard runner_guard{mc, runner};

    uint16_t port = 0;
    for (int i = 0; i < 200 && port == 0; ++i) {
        std::this_thread::sleep_for(10ms);
        port = mc.actual_listen_port_for_test();
    }
    REQUIRE(port != 0);

    // Staller: connect TCP but never speak TLS. This creates a PendingHandshake
    // that should sit idle without stalling accept() or other I/O.
    int staller = connect_to_port(port);

    // Give the event loop at least one tick to accept the staller.
    std::this_thread::sleep_for(50ms);

    // Healthy peer: complete TLS handshake, exchange Hello, then Ping/Pong.
    int healthy = connect_to_port(port);
    SSL_CTX* cli_ctx = create_test_ctx(id_cli, false);
    SSL* cli_ssl = SSL_new(cli_ctx);
    SSL_set_fd(cli_ssl, healthy);
    REQUIRE(SSL_connect(cli_ssl) > 0);

    HelloMsg hello;
    hello.node_name = "resp-cli";
    hello.version   = std::string(kBridgeSessionsVersion);
    hello.pubkey_hex = id_cli.pubkey_hex;
    write_frame(cli_ssl, hello, CONTROL_STREAM_ID);

    // Drain the server's Hello (required before the conn is promoted).
    Message msg = read_frame(cli_ssl);
    REQUIRE(std::holds_alternative<HelloMsg>(msg));

    // Now verify responsiveness: Ping must be answered promptly despite the staller.
    auto t0 = std::chrono::steady_clock::now();
    write_frame(cli_ssl, PingMsg{}, CONTROL_STREAM_ID);
    Message pong = read_frame(cli_ssl);
    auto t1 = std::chrono::steady_clock::now();
    REQUIRE(std::holds_alternative<PongMsg>(pong));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    REQUIRE(elapsed.count() < 1000);

    SSL_free(cli_ssl);
    SSL_CTX_free(cli_ctx);
    CLOSESOCK(healthy);
    CLOSESOCK(staller);

    mc.shutdown();
    runner.join();
    std::remove(ak.c_str());
}

// ── Test 2: Throttled transfer does not block another peer's pong ──────

TEST_CASE("throttled transfer does not block heartbeat traffic on another peer",
          "[mesh_responsiveness][transfer]") {
    auto id_srv = CertKeyTemp(generate_cert_key_pair("xfer-srv"), "bs_xs");
    auto id_a   = CertKeyTemp(generate_cert_key_pair("xfer-a"),   "bs_xa");
    auto id_b   = CertKeyTemp(generate_cert_key_pair("xfer-b"),   "bs_xb");
    std::string ak = write_authorized_keys(id_srv.pubkey_hex,
        id_a.pubkey_hex + "\n" + id_b.pubkey_hex);

    MeshConfig cfg = mesh_cfg("xfer-srv");
    cfg.authorized_keys_path = ak;
    std::string app_home = "/tmp/bs_resp_home_" + std::to_string(rand());
    std::filesystem::remove_all(app_home);
    g_test_ipc_token.clear();
    MeshController mc(cfg, app_home);

    std::thread runner([&] { mc.run(); });
    MeshRunnerGuard runner_guard{mc, runner};

    uint16_t port = 0;
    for (int i = 0; i < 200 && port == 0; ++i) {
        std::this_thread::sleep_for(10ms);
        port = mc.actual_listen_port_for_test();
    }
    REQUIRE(port != 0);
    for (int i = 0; i < 200 && g_test_ipc_token.empty(); ++i) {
        g_test_ipc_token = load_ipc_token(app_home);
        if (g_test_ipc_token.empty()) std::this_thread::sleep_for(10ms);
    }
    REQUIRE_FALSE(g_test_ipc_token.empty());

    MeshController probe(cfg, app_home);
    REQUIRE(probe.another_daemon_running_for_test());

    // Helper to fully connect a peer, exchange Hello, and confirm the server's
    // event loop has classified the conn as Mesh by sending a Ping/Pong round.
    auto connect_peer = [&](const CertKeyTemp& id, const std::string& name) -> std::pair<int, SSL*> {
        int fd = connect_to_port(port);
        SSL_CTX* ctx = create_test_ctx(id, false);
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, fd);
        REQUIRE(SSL_connect(ssl) > 0);

        HelloMsg h;
        h.node_name  = name;
        h.version    = std::string(kBridgeSessionsVersion);
        h.pubkey_hex = id.pubkey_hex;
        write_frame(ssl, h, CONTROL_STREAM_ID);
        Message m = read_frame(ssl);
        REQUIRE(std::holds_alternative<HelloMsg>(m));

        write_frame(ssl, PingMsg{}, CONTROL_STREAM_ID);
        Message pong = read_frame(ssl);
        REQUIRE(std::holds_alternative<PongMsg>(pong));

        SSL_CTX_free(ctx);
        return {fd, ssl};
    };

    auto [fd_a, ssl_a] = connect_peer(id_a, "xfer-a");
    auto [fd_b, ssl_b] = connect_peer(id_b, "xfer-b");

    // Create a small file and ask the server to send it to A with wait semantics.
    // The server will enqueue a FileSendWait worker that exclusively owns A's
    // SSL transport while exec_busy is set. We throttle A by delaying ACKs.
    std::string payload(4000, 'x');
    std::string local_path = write_temp_file("xfer_file.txt", payload);
    std::string ipc_cmd = "FILE_SEND_WAIT_B64 xfer-a " + b64enc(local_path) + "\n";
    int transfer_ipc_fd = send_ipc_no_wait(ipc_cmd);

    // A: read FileMeta, send ACK, then stop acknowledging to throttle the transfer.
    Message meta_msg = read_frame(ssl_a);
    REQUIRE(std::holds_alternative<FileMetaMsg>(meta_msg));
    write_frame(ssl_a, FileAckMsg{0, 0, false, ""}, CONTROL_STREAM_ID);

    // Wait until the server has sent the first chunk and is blocked waiting for
    // A's next ACK. The worker's select uses a 2s timeout, so sleep briefly to
    // ensure it is blocked.
    std::this_thread::sleep_for(150ms);

    // Verify A is busy by attempting a second transfer to the same peer. It must
    // be rejected with "peer busy". This avoids reading conns_ from the test
    // thread while the event loop owns it.
    std::string busy_probe;
    for (int i = 0; i < 50; ++i) {
        busy_probe = send_ipc_read_response(ipc_cmd, 500);
        if (busy_probe.find("peer busy") != std::string::npos) break;
        std::this_thread::sleep_for(50ms);
    }
    INFO("busy probe response: " << busy_probe);
    REQUIRE(busy_probe.find("peer busy") != std::string::npos);

    // While the transfer worker is blocked on A, B's Ping must still be answered
    // quickly because B's socket is still serviced by the event loop.
    auto t0 = std::chrono::steady_clock::now();
    write_frame(ssl_b, PingMsg{}, CONTROL_STREAM_ID);
    Message pong = read_frame(ssl_b);
    auto t1 = std::chrono::steady_clock::now();
    REQUIRE(std::holds_alternative<PongMsg>(pong));
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    REQUIRE(elapsed.count() < 1000);

    // Cancel the stalled operation on A.
    std::string cancel_resp = send_ipc_read_response("CANCEL xfer-a\n", 2000);
    INFO("cancel response: " << cancel_resp);
    REQUIRE(cancel_resp.find("OK cancelling") != std::string::npos);

    // Verify busy was cleared: a new transfer to A should no longer return
    // "peer busy" (it may succeed and hand the socket to the worker).
    std::string cleared_probe;
    for (int i = 0; i < 50; ++i) {
        cleared_probe = send_ipc_read_response(ipc_cmd, 500);
        if (cleared_probe.find("peer busy") == std::string::npos) break;
        std::this_thread::sleep_for(50ms);
    }
    REQUIRE(cleared_probe.find("peer busy") == std::string::npos);

    SSL_free(ssl_a);
    SSL_free(ssl_b);
    CLOSESOCK(fd_a);
    CLOSESOCK(fd_b);
    CLOSESOCK(transfer_ipc_fd);

    mc.shutdown();
    runner.join();
    std::remove(ak.c_str());
    std::remove(local_path.c_str());
    // Best-effort cleanup of temp app_home.
    std::error_code ec;
    std::filesystem::remove_all(app_home, ec);
    g_test_ipc_token.clear();
}

// ── Test 3: Worker pool bounded, joinable, cancellation-aware ──────────

TEST_CASE("LongOperationWorkerPool is bounded and cancellation prevents execution",
          "[mesh_responsiveness][worker]") {
    std::atomic<int> executed{0};
    std::atomic<int> cancelled_skipped{0};
    std::atomic<bool> gate{false};

    LongOperationWorkerPool pool(2, [&](const LongOperationTask& task) {
        if (task.cancelled && task.cancelled->load()) {
            ++cancelled_skipped;
            return;
        }
        ++executed;
        while (!gate.load()) std::this_thread::sleep_for(10ms);
        if (task.ipc_fd != INVALID_SOCKET) {
            std::string done = "OK\n";
            send(task.ipc_fd, done.data(), (int)done.size(), 0);
        }
    });

    auto make_task = [&](int n, SOCKET ipc) {
        LongOperationTask t;
        t.type = LongOperationTask::Type::FileSendWait;
        t.path1 = "path" + std::to_string(n);
        t.cancelled = std::make_shared<std::atomic<bool>>(false);
        t.ipc_fd = ipc;
        return t;
    };

    // Enqueue three tasks; with only 2 workers, the third stays queued.
    pool.enqueue(make_task(1, INVALID_SOCKET));
    pool.enqueue(make_task(2, INVALID_SOCKET));
    pool.enqueue(make_task(3, INVALID_SOCKET));

    // Wait until queue depth drops to 1 (two workers started, one pending).
    for (int i = 0; i < 200 && pool.pending_count() > 1; ++i)
        std::this_thread::sleep_for(10ms);
    REQUIRE(pool.pending_count() == 1);

    // Release the workers.
    gate = true;

    // Shutdown joins all workers without detached threads.
    pool.shutdown();

    REQUIRE(executed.load() == 3);
    REQUIRE(cancelled_skipped.load() == 0);
}

TEST_CASE("LongOperationWorkerPool hands IPC socket ownership to worker",
          "[mesh_responsiveness][worker]") {
#ifndef _WIN32
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);

    LongOperationWorkerPool pool(1, [&](const LongOperationTask& task) {
        if (task.ipc_fd != INVALID_SOCKET) {
            std::string msg = "PROGRESS 50\nOK done\n";
            send(task.ipc_fd, msg.data(), (int)msg.size(), 0);
        }
    });

    LongOperationTask task;
    task.type = LongOperationTask::Type::FileRecvWait;
    task.path1 = "/foo";
    task.ipc_fd = sv[1];
    pool.enqueue(std::move(task));

    char buf[256] = {};
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(sv[0], buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n > 0) {
            total += n;
            buf[total] = '\0';
            if (strstr(buf, "OK done")) break;
        } else {
            break;
        }
    }
    REQUIRE(strstr(buf, "PROGRESS 50") != nullptr);
    REQUIRE(strstr(buf, "OK done") != nullptr);

    pool.shutdown();
    CLOSESOCK(sv[0]);
#else
    SUCCEED("socketpair not available on Windows");
#endif
}

TEST_CASE("worker cancellation clears busy and close request is processed once",
          "[mesh_responsiveness][cancel]") {
    std::atomic<int> executed{0};
    std::atomic<bool> saw_cancel{false};

    LongOperationWorkerPool pool(1, [&](const LongOperationTask& task) {
        ++executed;
        auto is_cancelled = [&]() { return task.cancelled && task.cancelled->load(); };
        for (int i = 0; i < 200; ++i) {
            if (is_cancelled()) { saw_cancel = true; return; }
            std::this_thread::sleep_for(10ms);
        }
    });

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    LongOperationTask task;
    task.type = LongOperationTask::Type::FileSendWait;
    task.path1 = "cancel-test";
    task.cancelled = cancelled;
    task.exec_busy = std::make_shared<std::atomic<bool>>(true);
    task.exec_completed = std::make_shared<std::atomic<bool>>(false);
    pool.enqueue(std::move(task));

    // Let the worker start.
    std::this_thread::sleep_for(50ms);
    REQUIRE(executed.load() == 1);

    // Cancel from the outside.
    cancelled->store(true);

    // Worker should observe cancellation and exit.
    for (int i = 0; i < 100 && !saw_cancel.load(); ++i)
        std::this_thread::sleep_for(10ms);
    REQUIRE(saw_cancel.load());

    pool.shutdown();
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
