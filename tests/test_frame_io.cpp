#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "bridgesessions.cpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSESOCK closesocket
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); } ~WsaInit() { WSACleanup(); } };
static WsaInit _wsa;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#define CLOSESOCK close
#endif

#include <thread>
#include <atomic>
#include <cstdio>
#include <fstream>

using namespace bs::mesh;

// ── Helpers (copied from test_tls.cpp) ────────────────────────────

struct TmpFile {
    std::string path;
    TmpFile(const std::string& content) {
#ifdef _WIN32
        char tmpl[MAX_PATH];
        char tmpPath[MAX_PATH];
        GetTempPathA(sizeof(tmpPath), tmpPath);
        GetTempFileNameA(tmpPath, "bst", 0, tmpl);
        path = tmpl;
#else
        char tmpl[] = "/tmp/bs-test-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) {
            path = tmpl;
            close(fd);
        }
#endif
        if (!path.empty()) {
            FILE* f = fopen(path.c_str(), "w");
            if (f) { fwrite(content.data(), 1, content.size(), f); fclose(f); }
        }
    }
    ~TmpFile() { if (!path.empty()) std::remove(path.c_str()); }
};

struct CertKeyOnce {
    std::string cert_pem;
    std::string key_pem;
    std::string pubkey_hex;
    TmpFile cert_file;
    TmpFile key_file;

    CertKeyOnce(const std::pair<std::string,std::string>& ck)
        : cert_pem(ck.first)
        , key_pem(ck.second)
        , pubkey_hex(pubkey_hex_from_pem(key_pem))
        , cert_file(cert_pem)
        , key_file(key_pem)
    {}
};

CertKeyOnce make_ck(const char* cn = "test.bridgesessions") {
    return CertKeyOnce(generate_cert_key_pair(cn));
}

int listen_socket() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(fd, 1) == 0);
    return fd;
}

int get_port(int lfd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}

int connect_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<u_short>(port));
    REQUIRE(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    return fd;
}

// ── Test 1: KeystrokeMsg round-trip ───────────────────────────────

TEST_CASE("read_frame/write_frame: KeystrokeMsg round-trip over TLS", "[frame_io]") {
    auto server_ck = make_ck("fio-server1");
    auto client_ck = make_ck("fio-client1");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    KeystrokeMsg received;
    std::atomic<bool> srv_ok{false};

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        if (!ctx) { CLOSESOCK(cfd); return; }
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        if (SSL_accept(ssl.get()) <= 0) { CLOSESOCK(cfd); return; }
        srv_ok = true;
        try {
            auto msg = read_frame(ssl.get());
            if (std::holds_alternative<KeystrokeMsg>(msg))
                received = std::get<KeystrokeMsg>(msg);
        } catch (...) {}
        CLOSESOCK(cfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    REQUIRE(ctx != nullptr);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));
    REQUIRE(SSL_connect(ssl.get()) > 0);

    KeystrokeMsg ks;
    ks.data = "hello world keystroke test";
    write_frame(ssl.get(), ks, CONTROL_STREAM_ID);

    server_thread.join();  // join before close avoids racing SSL_accept
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);

    REQUIRE(srv_ok);
    REQUIRE(received.data == "hello world keystroke test");
}

// ── Test 2: ResizeMsg round-trip ──────────────────────────────────

TEST_CASE("read_frame/write_frame: ResizeMsg round-trip over TLS", "[frame_io]") {
    auto server_ck = make_ck("fio-server2");
    auto client_ck = make_ck("fio-client2");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    ResizeMsg received;
    std::atomic<bool> srv_ok{false};

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        if (!ctx) { CLOSESOCK(cfd); return; }
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        if (SSL_accept(ssl.get()) <= 0) { CLOSESOCK(cfd); return; }
        srv_ok = true;
        try {
            auto msg = read_frame(ssl.get());
            if (std::holds_alternative<ResizeMsg>(msg))
                received = std::get<ResizeMsg>(msg);
        } catch (...) {}
        CLOSESOCK(cfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    REQUIRE(ctx != nullptr);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));
    REQUIRE(SSL_connect(ssl.get()) > 0);

    ResizeMsg rs;
    rs.cols = 132;
    rs.rows = 43;
    write_frame(ssl.get(), rs, CONTROL_STREAM_ID);

    server_thread.join();
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);

    REQUIRE(srv_ok);
    REQUIRE(received.cols == 132);
    REQUIRE(received.rows == 43);
}

// ── Test 3: FLAG_COMPRESSED — >256 byte OutputMsg ─────────────────

TEST_CASE("read_frame/write_frame: FLAG_COMPRESSED OutputMsg round-trip", "[frame_io][compression]") {
    auto server_ck = make_ck("fio-server3");
    auto client_ck = make_ck("fio-client3");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    OutputMsg received;

    std::atomic<bool> srv_ok{false};

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        if (!ctx) { CLOSESOCK(cfd); return; }
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        if (SSL_accept(ssl.get()) <= 0) { CLOSESOCK(cfd); return; }
        srv_ok = true;
        try {
            auto msg = read_frame(ssl.get());
            if (std::holds_alternative<OutputMsg>(msg))
                received = std::get<OutputMsg>(msg);
        } catch (...) {}
        CLOSESOCK(cfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    REQUIRE(ctx != nullptr);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));
    REQUIRE(SSL_connect(ssl.get()) > 0);

    // Build a payload > COMPRESSION_THRESHOLD (256 bytes) to trigger zstd
    OutputMsg out;
    std::string payload;
    for (int i = 0; i < 50; ++i) payload += "ABCDEFGHIJ";  // 500 bytes
    REQUIRE(payload.size() > COMPRESSION_THRESHOLD);
    out.data = payload;

    write_frame(ssl.get(), out, CONTROL_STREAM_ID);

    server_thread.join();
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);

    REQUIRE(srv_ok);
    REQUIRE(received.data == payload);
    REQUIRE(received.data.size() == 500);
}

// ── Test 4: Multiple messages in sequence ─────────────────────────

TEST_CASE("incremental frame drain preserves partial trailing frame", "[frame_io][buffering]") {
    auto first = encode(PingMsg{}, CONTROL_STREAM_ID);
    auto second = encode(PongMsg{}, CONTROL_STREAM_ID);
    std::vector<uint8_t> buffered(first.begin(), first.end());
    buffered.insert(buffered.end(), second.begin(), second.end() - 1);
    auto messages = drain_complete_frames(buffered);
    REQUIRE(messages.size() == 1);
    REQUIRE(std::holds_alternative<PingMsg>(messages[0]));
    REQUIRE(buffered.size() == second.size() - 1);
    buffered.push_back(second.back());
    messages = drain_complete_frames(buffered);
    REQUIRE(messages.size() == 1);
    REQUIRE(std::holds_alternative<PongMsg>(messages[0]));
    REQUIRE(buffered.empty());
}

TEST_CASE("buffered frame completeness rejects partial next frames", "[frame_io][buffering]") {
    auto frame = encode(PingMsg{}, CONTROL_STREAM_ID);
    REQUIRE(buffered_bytes_hold_complete_frame(frame));
    REQUIRE_FALSE(buffered_bytes_hold_complete_frame(std::span<const uint8_t>(frame.data(), FRAME_HEADER_SIZE - 1)));
    REQUIRE_FALSE(buffered_bytes_hold_complete_frame(std::span<const uint8_t>(frame.data(), frame.size() - 1)));
}

#ifndef _WIN32
TEST_CASE("socket_peer_half_closed detects a peer FIN", "[frame_io][disconnect]") {
    int sockets[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    REQUIRE(::shutdown(sockets[1], SHUT_WR) == 0);
    REQUIRE(socket_peer_half_closed(sockets[0]));
    ::close(sockets[0]);
    ::close(sockets[1]);
}
#endif

TEST_CASE("read_frame/write_frame: multiple messages in sequence", "[frame_io]") {
    auto server_ck = make_ck("fio-server4");
    auto client_ck = make_ck("fio-client4");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    KeystrokeMsg ks_received;
    ResizeMsg     rs_received;
    OutputMsg     out_received;

    std::atomic<bool> srv_ok{false};

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        if (!ctx) { CLOSESOCK(cfd); return; }
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        if (SSL_accept(ssl.get()) <= 0) { CLOSESOCK(cfd); return; }
        srv_ok = true;
        try {
            auto m1 = read_frame(ssl.get());
            if (std::holds_alternative<KeystrokeMsg>(m1))
                ks_received = std::get<KeystrokeMsg>(m1);
            auto m2 = read_frame(ssl.get());
            if (std::holds_alternative<ResizeMsg>(m2))
                rs_received = std::get<ResizeMsg>(m2);
            auto m3 = read_frame(ssl.get());
            if (std::holds_alternative<OutputMsg>(m3))
                out_received = std::get<OutputMsg>(m3);
        } catch (...) {}
        CLOSESOCK(cfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    REQUIRE(ctx != nullptr);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));
    REQUIRE(SSL_connect(ssl.get()) > 0);

    KeystrokeMsg ks;
    ks.data = "msg1";
    write_frame(ssl.get(), ks, CONTROL_STREAM_ID);

    ResizeMsg rs;
    rs.cols = 80;
    rs.rows = 24;
    write_frame(ssl.get(), rs, CONTROL_STREAM_ID);

    OutputMsg out;
    std::string big;
    for (int i = 0; i < 60; ++i) big += "0123456789";  // 600 bytes
    out.data = big;
    write_frame(ssl.get(), out, CONTROL_STREAM_ID);

    server_thread.join();
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);

    REQUIRE(srv_ok);
    REQUIRE(ks_received.data == "msg1");
    REQUIRE(rs_received.cols == 80);
    REQUIRE(rs_received.rows == 24);
    REQUIRE(out_received.data == big);
}

TEST_CASE("direct interactive client answers PingMsg with PongMsg", "[frame_io][heartbeat]") {
    auto server_ck = make_ck("fio-heartbeat-server");
    auto client_ck = make_ck("fio-heartbeat-client");
    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);
    std::atomic<bool> got_pong{false};

    std::thread server_thread([&] {
        int sfd = accept(lfd, nullptr, nullptr);
        if (sfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), sfd);
        if (SSL_accept(ssl.get()) <= 0) { CLOSESOCK(sfd); return; }
        write_frame(ssl.get(), PingMsg{}, CONTROL_STREAM_ID);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sfd, &rfds);
        timeval tv{1, 0};
        if (select(sfd + 1, &rfds, nullptr, nullptr, &tv) > 0) {
            try {
                Message response = read_frame(ssl.get());
                got_pong = std::holds_alternative<PongMsg>(response);
            } catch (...) {}
        }
        CLOSESOCK(sfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), cfd);
    REQUIRE(SSL_connect(ssl.get()) > 0);

    MeshConfig cfg;
    cfg.node_name = "direct-heartbeat-client";
    MeshController mc(cfg);
    REQUIRE(mc.process_shell_response(ssl.get()));

    server_thread.join();
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);
    REQUIRE(got_pong);
}

TEST_CASE("direct noninteractive client answers PingMsg with PongMsg", "[frame_io][heartbeat]") {
    auto server_ck = make_ck("fio-noninteractive-heartbeat-server");
    auto client_ck = make_ck("fio-noninteractive-heartbeat-client");
    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);
    std::atomic<bool> got_pong{false};

    std::thread server_thread([&] {
        int sfd = accept(lfd, nullptr, nullptr);
        if (sfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), sfd);
        if (SSL_accept(ssl.get()) <= 0) { CLOSESOCK(sfd); return; }
        write_frame(ssl.get(), PingMsg{}, CONTROL_STREAM_ID);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sfd, &rfds);
        timeval tv{1, 0};
        if (select(sfd + 1, &rfds, nullptr, nullptr, &tv) > 0) {
            try {
                Message response = read_frame(ssl.get());
                got_pong = std::holds_alternative<PongMsg>(response);
            } catch (...) {}
        }
        CLOSESOCK(sfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), cfd);
    REQUIRE(SSL_connect(ssl.get()) > 0);

    MeshConfig cfg;
    cfg.node_name = "direct-noninteractive-heartbeat-client";
    MeshController mc(cfg);
    int32_t exit_code = 0;
    REQUIRE(mc.process_noninteractive_response(ssl.get(), exit_code));

    server_thread.join();
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);
    REQUIRE(got_pong);
}

TEST_CASE("direct interactive transport loss is silent", "[frame_io][reconnect]") {
    auto server_ck = make_ck("fio-reconnect-server");
    auto client_ck = make_ck("fio-reconnect-client");
    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);
    std::thread server_thread([&] {
        int sfd = accept(lfd, nullptr, nullptr);
        if (sfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), sfd);
        if (SSL_accept(ssl.get()) > 0) {
            CLOSESOCK(sfd); // abrupt loss: the client must reconnect silently
        } else {
            CLOSESOCK(sfd);
        }
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), cfd);
    REQUIRE(SSL_connect(ssl.get()) > 0);

    MeshConfig cfg;
    cfg.node_name = "direct-reconnect-client";
    MeshController mc(cfg);
    std::ostringstream captured;
    auto* old = std::cerr.rdbuf(captured.rdbuf());
    const bool keep_connection = mc.process_shell_response(ssl.get());
    std::cerr.rdbuf(old);

    server_thread.join();
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);
    REQUIRE_FALSE(keep_connection);
    REQUIRE(captured.str().empty());
}

TEST_CASE("noninteractive transport loss is reported as failure", "[frame_io][reconnect]") {
    auto server_ck = make_ck("fio-noninteractive-loss-server");
    auto client_ck = make_ck("fio-noninteractive-loss-client");
    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);
    std::thread server_thread([&] {
        int sfd = accept(lfd, nullptr, nullptr);
        if (sfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), sfd);
        (void)SSL_accept(ssl.get());
        CLOSESOCK(sfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), cfd);
    REQUIRE(SSL_connect(ssl.get()) > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    MeshConfig cfg;
    cfg.node_name = "fio-noninteractive-loss-client";
    MeshController mc(cfg);
    int32_t exit_code = 0;
    bool transport_error = false;
    REQUIRE_FALSE(mc.process_noninteractive_response(ssl.get(), exit_code, &transport_error));
    REQUIRE(transport_error);
    REQUIRE(exit_code == 0);

    server_thread.join();
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
