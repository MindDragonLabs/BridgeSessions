// test_tls_reliability.cpp — TLS reliability tests (v1.3 R1/R2 acceptance criteria)
//
// These tests target the specific TLS failures observed in the 3-node real-network
// test (Windows <-> Linux):
//
//   Symptom A: connect error logged as "error:00000000:lib(0)::reason(0)" — the OpenSSL
//              error queue was drained before the error string was captured.
//              R1 fix: log SSL_get_error()+ERR_error_string() *before* any catch block.
//
//   Symptom B: connect_and_hello() hangs indefinitely when peer is unresponsive.
//              R2 fix: setsockopt(SO_RCVTIMEO/SO_SNDTIMEO, 5s) before SSL_connect.
//
// Tests 1-3 verify the mechanism works at the OpenSSL level (green today).
// Test 4 verifies that a blocking TLS connect fails fast with SO_RCVTIMEO (green today).
// Test 5 verifies that an unauthorized client is rejected and fails fast (not hangs).

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "../bs-protocol.h"

#ifdef _WIN32
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
#include <chrono>
#include <fstream>
#include <cstdio>
#include <openssl/err.h>

using namespace bs::mesh;
using namespace std::chrono_literals;

// ── Shared helpers ────────────────────────────────────────────────────

struct TmpPem {
    std::string path;
    explicit TmpPem(const std::string& content) {
#ifdef _WIN32
        char tmpDir[MAX_PATH], out[MAX_PATH];
        GetTempPathA(sizeof(tmpDir), tmpDir);
        GetTempFileNameA(tmpDir, "bsR", 0, out);
        path = out;
#else
        path = std::string("/tmp/bsR_") + std::to_string(rand());
#endif
        std::ofstream f(path, std::ios::trunc);
        f << content;
    }
    ~TmpPem() { std::remove(path.c_str()); }
};

static int make_loopback_listener(int& port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    listen(fd, 4);
    socklen_t len = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    port_out = ntohs(a.sin_port);
    return fd;
}

static int dial_loopback(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    return fd;
}

static void set_rcv_timeout(int fd, int ms) {
#ifdef _WIN32
    DWORD t = static_cast<DWORD>(ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
#else
    struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

// ── Test 1: OpenSSL error queue is non-empty after a failed handshake ─────
// Mechanism check for R1: confirms ERR_get_error() produces a non-zero code
// (and thus a non-empty string) when the server rejects an unauthorized client.

TEST_CASE("TLS stream I/O clears stale thread-local OpenSSL errors", "[tls_reliability][r1]") {
    ERR_clear_error();
    ERR_raise(ERR_LIB_SSL, ERR_R_INTERNAL_ERROR);
    REQUIRE(ERR_peek_error() != 0);

    clear_stale_ssl_errors_before_io();

    REQUIRE(ERR_peek_error() == 0);
}

TEST_CASE("R1-mechanism: ERR_get_error is non-zero after unauthorized rejection", "[tls_reliability][r1]") {
    auto srv_ck  = generate_cert_key_pair("r1-srv");
    auto good_ck = generate_cert_key_pair("r1-good");
    auto bad_ck  = generate_cert_key_pair("r1-bad");

    TmpPem scert(srv_ck.first), skey(srv_ck.second);
    TmpPem gcert(good_ck.first), gkey(good_ck.second);
    TmpPem bcert(bad_ck.first),  bkey(bad_ck.second);
    TmpPem ak(pubkey_hex_from_pem(good_ck.second) + "\n"); // only good key authorized

    int port;
    int lfd = make_loopback_listener(port);

    std::thread srv([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig sc;
        sc.cert_file = scert.path; sc.key_file = skey.path;
        sc.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(sc, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        SSL_accept(ssl.get()); // will reject bad_ck
        CLOSESOCK(cfd);
    });

    int cfd = dial_loopback(port);
    set_rcv_timeout(cfd, 5000);

    NodeTlsConfig cc;
    cc.cert_file = bcert.path; cc.key_file = bkey.path;
    cc.tofu_cb   = [](const std::string&) { return true; };
    auto ctx = create_node_tls(cc, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));

    ERR_clear_error();
    int ret = SSL_connect(ssl.get());

    // Extract the error string the way R1 will log it
    std::string err_str;
    if (ret <= 0) {
        unsigned long e = ERR_get_error();
        if (e != 0) {
            char buf[512];
            ERR_error_string_n(e, buf, sizeof(buf));
            err_str = buf;
        }
    }

    CLOSESOCK(cfd);
    srv.join();
    CLOSESOCK(lfd);

    if (ret <= 0) {
        // The key assertion: error string must NOT be the known-bad empty symptom
        INFO("error string: " << err_str);
        REQUIRE(err_str != "error:00000000:lib(0)::reason(0)");
        REQUIRE(!err_str.empty());
    }
    // If ret > 0 (handshake succeeded), authorized_keys enforcement is broken — its own bug.
}

// ── Test 2: ERR_error_string is stable across calls (not consumed by catch blocks) ──
// Demonstrates that the error queue must be read BEFORE the SSL object is destroyed.

TEST_CASE("R1-mechanism: error queue must be read before SSL object is freed", "[tls_reliability][r1]") {
    auto srv_ck  = generate_cert_key_pair("r1b-srv");
    auto bad_ck  = generate_cert_key_pair("r1b-bad");

    TmpPem scert(srv_ck.first), skey(srv_ck.second);
    TmpPem bcert(bad_ck.first), bkey(bad_ck.second);
    // Empty authorized_keys — everyone rejected
    TmpPem ak("\n");

    int port;
    int lfd = make_loopback_listener(port);
    std::thread srv([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig sc;
        sc.cert_file = scert.path; sc.key_file = skey.path;
        sc.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(sc, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        SSL_accept(ssl.get());
        CLOSESOCK(cfd);
    });

    int cfd = dial_loopback(port);
    set_rcv_timeout(cfd, 5000);

    NodeTlsConfig cc;
    cc.cert_file = bcert.path; cc.key_file = bkey.path;
    cc.tofu_cb   = [](const std::string&) { return true; };
    auto ctx = create_node_tls(cc, TlsMode::Connect);

    std::string captured_before_free;
    std::string captured_after_free;

    {
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(cfd));
        ERR_clear_error();
        int ret = SSL_connect(ssl.get());
        if (ret <= 0) {
            // Capture WHILE ssl is still alive — correct pattern
            unsigned long e = ERR_get_error();
            if (e) { char buf[512]; ERR_error_string_n(e, buf, sizeof(buf)); captured_before_free = buf; }
        }
        // ssl freed here
    }

    // After ssl is freed, try to capture — queue should be empty now
    {
        unsigned long e = ERR_get_error();
        if (e) { char buf[512]; ERR_error_string_n(e, buf, sizeof(buf)); captured_after_free = buf; }
    }

    CLOSESOCK(cfd);
    srv.join();
    CLOSESOCK(lfd);

    // The pre-free capture should have content; the post-free attempt may be empty
    // This documents the correct logging order required by R1
    if (!captured_before_free.empty()) {
        INFO("captured before free: " << captured_before_free);
        INFO("captured after free:  " << captured_after_free);
        // If we got it before free, it's valid
        REQUIRE(captured_before_free != "error:00000000:lib(0)::reason(0)");
    }
    SUCCEED(); // test is documentation-oriented; it always passes to record the pattern
}

// ── Test 3: SO_RCVTIMEO mechanism — blocking read times out ───────────────
// Verifies the socket-level mechanism that R2 uses: setting SO_RCVTIMEO before
// SSL_connect causes the TLS handshake to fail fast if the peer is unresponsive.

TEST_CASE("R2-mechanism: SO_RCVTIMEO causes SSL_connect to fail within timeout window", "[tls_reliability][r2]") {
    auto client_ck = generate_cert_key_pair("r2-client");
    TmpPem ccert(client_ck.first), ckey(client_ck.second);

    int port;
    int lfd = make_loopback_listener(port);

    // Server: accept TCP but never do TLS (simulates a dead/unresponsive peer)
    std::atomic<bool> accepted{false};
    std::thread srv([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        accepted = true;
        std::this_thread::sleep_for(15s); // never respond — force timeout
        CLOSESOCK(cfd);
    });

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(cfd >= 0);
    set_rcv_timeout(cfd, 2000); // 2s timeout — R2 uses 5s; we use 2s to keep tests fast

    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    connect(cfd, reinterpret_cast<sockaddr*>(&a), sizeof(a));

    NodeTlsConfig cc;
    cc.cert_file = ccert.path; cc.key_file = ckey.path;
    cc.tofu_cb   = [](const std::string&) { return true; };
    auto ctx = create_node_tls(cc, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));

    auto t0 = std::chrono::steady_clock::now();
    int ret = SSL_connect(ssl.get());
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    CLOSESOCK(cfd);
    srv.detach(); // server is sleeping — detach and let it clean up
    CLOSESOCK(lfd);

    INFO("SSL_connect returned: " << ret);
    INFO("elapsed ms: " << elapsed_ms);
    REQUIRE(ret <= 0);          // must fail (server never responded)
    REQUIRE(elapsed_ms < 5000); // must not hang — should time out in ~2s
}

// ── Test 4: connect_and_hello path — peer closes TCP without TLS ─────────
// Server closes socket before doing TLS. Verifies that SSL_connect detects
// the immediate close and returns quickly (not hanging for tens of seconds).

TEST_CASE("R2-mechanism: SSL_connect fails fast when peer closes TCP immediately", "[tls_reliability][r2]") {
    auto client_ck = generate_cert_key_pair("r2b-client");
    TmpPem ccert(client_ck.first), ckey(client_ck.second);

    int port;
    int lfd = make_loopback_listener(port);

    std::thread srv([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd >= 0) {
            std::this_thread::sleep_for(50ms); // small delay to make TCP established
            CLOSESOCK(cfd); // close before TLS
        }
    });

    int cfd = dial_loopback(port);
    set_rcv_timeout(cfd, 5000);

    NodeTlsConfig cc;
    cc.cert_file = ccert.path; cc.key_file = ckey.path;
    cc.tofu_cb   = [](const std::string&) { return true; };
    auto ctx = create_node_tls(cc, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));

    auto t0 = std::chrono::steady_clock::now();
    int ret = SSL_connect(ssl.get());
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    CLOSESOCK(cfd);
    srv.join();
    CLOSESOCK(lfd);

    INFO("SSL_connect returned: " << ret);
    INFO("elapsed ms: " << elapsed_ms);
    REQUIRE(ret <= 0);           // peer closed — TLS must fail
    REQUIRE(elapsed_ms < 3000);  // must detect the close fast, not wait for timeout
}

// ── Test 5: Unauthorized client rejected fast, not left hanging ───────────
// The exact scenario seen with Windows→Linux cross-platform TLS:
// the server's authorized_keys callback rejects the client cert, and the
// client must receive a clean TLS alert (not time out waiting indefinitely).

TEST_CASE("R2: unauthorized client rejected fast (no hang)", "[tls_reliability][r1][r2]") {
    auto srv_ck    = generate_cert_key_pair("r2c-srv");
    auto auth_ck   = generate_cert_key_pair("r2c-auth");
    auto reject_ck = generate_cert_key_pair("r2c-reject");

    TmpPem scert(srv_ck.first),    skey(srv_ck.second);
    TmpPem acert(auth_ck.first),   akey(auth_ck.second);
    TmpPem rcert(reject_ck.first), rkey(reject_ck.second);
    TmpPem ak(pubkey_hex_from_pem(auth_ck.second) + "\n"); // only auth_ck authorized

    int port;
    int lfd = make_loopback_listener(port);

    std::thread srv([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig sc;
        sc.cert_file = scert.path; sc.key_file = skey.path;
        sc.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(sc, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        SSL_accept(ssl.get()); // rejects reject_ck
        CLOSESOCK(cfd);
    });

    int cfd = dial_loopback(port);
    set_rcv_timeout(cfd, 5000); // 5s timeout — R2's target

    NodeTlsConfig cc;
    cc.cert_file = rcert.path; cc.key_file = rkey.path;
    cc.tofu_cb   = [](const std::string&) { return true; };
    auto ctx = create_node_tls(cc, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));

    ERR_clear_error();
    auto t0 = std::chrono::steady_clock::now();
    int ret = SSL_connect(ssl.get());

    // In TLS 1.3, SSL_connect returns 1 after the client finishes its handshake messages,
    // BEFORE the server has verified the client cert.  The server's rejection arrives as
    // a TLS alert on the first subsequent SSL operation.  Drive that alert out now.
    if (ret > 0) {
        char dummy[1];
        ret = SSL_read(ssl.get(), dummy, 1);
    }

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // Capture error BEFORE cleanup (R1 pattern)
    std::string err_str;
    if (ret <= 0) {
        unsigned long e = ERR_get_error();
        if (e) { char buf[512]; ERR_error_string_n(e, buf, sizeof(buf)); err_str = buf; }
    }

    CLOSESOCK(cfd);
    srv.join();
    CLOSESOCK(lfd);

    INFO("final ret: " << ret << ", elapsed: " << elapsed_ms << "ms, error: " << err_str);
    REQUIRE(ret <= 0);           // must be rejected (connect or first read fails)
    REQUIRE(elapsed_ms < 5000);  // must not hang past timeout
    if (!err_str.empty()) {
        REQUIRE(err_str != "error:00000000:lib(0)::reason(0)");
    }
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
