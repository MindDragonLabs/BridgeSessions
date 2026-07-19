#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bridgesessions.cpp"

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
#ifndef _WIN32
#include <signal.h>
struct SigpipeIgnore { SigpipeIgnore() { signal(SIGPIPE, SIG_IGN); } };
static SigpipeIgnore _sigpipe_ignore;
#endif

using namespace bs::mesh;

// ── Helpers ──────────────────────────────────────────────────────

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

// Single-keypair helper: generates once, writes cert+key to temp files.
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

// ── Test 1 ────────────────────────────────────────────────────────

TEST_CASE("generate_cert_key_pair creates valid PEM output", "[tls]") {
    auto [cert_pem, key_pem] = generate_cert_key_pair("test.bridgesessions");
    REQUIRE(cert_pem.find("BEGIN CERTIFICATE") != std::string::npos);
    REQUIRE(key_pem.find("BEGIN PRIVATE KEY") != std::string::npos);
    REQUIRE(cert_pem.find("END CERTIFICATE") != std::string::npos);
    REQUIRE(key_pem.find("END PRIVATE KEY") != std::string::npos);
}

// ── Test 2 ────────────────────────────────────────────────────────

TEST_CASE("pubkey_hex_from_pem returns 64-char hex string", "[tls]") {
    auto ck = make_ck();
    REQUIRE(ck.pubkey_hex.size() == 64);
    for (char c : ck.pubkey_hex) {
        REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
    }
}

// ── Test 3 ────────────────────────────────────────────────────────

TEST_CASE("peer_public_key_hex after TLS handshake", "[tls][integration]") {
    auto server_ck = make_ck("server");
    auto client_ck = make_ck("client");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    std::string s_peer_hex;
    std::string c_peer_hex;

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        REQUIRE(ctx != nullptr);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        REQUIRE(SSL_accept(ssl.get()) > 0);
        s_peer_hex = peer_public_key_hex(ssl.get());
        char buf[64];
        int n = SSL_read(ssl.get(), buf, sizeof(buf));
        REQUIRE(n > 0);
        SSL_write(ssl.get(), "pong", 4);
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
    c_peer_hex = peer_public_key_hex(ssl.get());
    SSL_write(ssl.get(), "ping", 4);
    char buf[64];
    int n = SSL_read(ssl.get(), buf, sizeof(buf));
    REQUIRE(n == 4);
    REQUIRE(std::string(buf, 4) == "pong");

    CLOSESOCK(cfd);
    server_thread.join();
    CLOSESOCK(lfd);

    REQUIRE(s_peer_hex == client_ck.pubkey_hex);
    REQUIRE(c_peer_hex == server_ck.pubkey_hex);
    REQUIRE(peer_identity_matches(server_ck.pubkey_hex, c_peer_hex));
    REQUIRE_FALSE(peer_identity_matches(client_ck.pubkey_hex, c_peer_hex));
}

// ── Test 4 ────────────────────────────────────────────────────────

TEST_CASE("TLS loopback with mutual ed25519 auth succeeds", "[tls][integration]") {
    auto server_ck = make_ck("server4");
    auto client_ck = make_ck("client4");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    bool tofu_called = false;
    std::atomic<bool> handshake_ok{false};

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        REQUIRE(cfd >= 0);
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        REQUIRE(ctx != nullptr);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        REQUIRE(SSL_accept(ssl.get()) > 0);
        char buf[64];
        SSL_read(ssl.get(), buf, sizeof(buf));
        SSL_write(ssl.get(), "ok", 2);
        CLOSESOCK(cfd);
        handshake_ok = true;
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [&](const std::string&) { tofu_called = true; return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    REQUIRE(ctx != nullptr);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));
    REQUIRE(SSL_connect(ssl.get()) > 0);
    SSL_write(ssl.get(), "hello", 5);
    char buf[64];
    int n = SSL_read(ssl.get(), buf, sizeof(buf));
    REQUIRE(n == 2);
    REQUIRE(std::string(buf, 2) == "ok");

    CLOSESOCK(cfd);
    server_thread.join();
    CLOSESOCK(lfd);

    REQUIRE(tofu_called);
    REQUIRE(handshake_ok);
}

// ── Test 5 ────────────────────────────────────────────────────────

TEST_CASE("authorized_keys: allowed key passes verification", "[tls][auth]") {
    auto server_ck = make_ck("server5");
    auto good_ck = make_ck("good-client5");

    TmpFile ak(good_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    std::atomic<bool> server_accepted{false};

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        if (SSL_accept(ssl.get()) > 0) {
            server_accepted = true;
            char buf[1];
            SSL_read(ssl.get(), buf, 1);
        }
        CLOSESOCK(cfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = good_ck.cert_file.path;
    ccfg.key_file = good_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));
    int ret = SSL_connect(ssl.get());
    bool client_ok = (ret > 0);
    if (client_ok) SSL_write(ssl.get(), "x", 1);

    server_thread.join();  // join before close avoids racing SSL_accept
    CLOSESOCK(cfd);
    CLOSESOCK(lfd);

    REQUIRE(client_ok);
    REQUIRE(server_accepted);
}

// ── Test 6 ────────────────────────────────────────────────────────

TEST_CASE("authorized_keys: wrong key fails SSL handshake", "[tls][auth]") {
    auto server_ck = make_ck("server6");
    auto good_ck = make_ck("good-client6");
    auto wrong_ck = make_ck("wrong-client6");

    // Server ONLY authorizes good client, NOT wrong client
    TmpFile ak(good_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    std::atomic<bool> server_rejected{false};

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) { server_rejected = true; return; }
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        if (SSL_accept(ssl.get()) <= 0) {
            server_rejected = true;
        }
        CLOSESOCK(cfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = wrong_ck.cert_file.path;
    ccfg.key_file = wrong_ck.key_file.path;
    ccfg.tofu_cb = [](const std::string&) { return true; };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));
    int ret = SSL_connect(ssl.get());
    bool client_failed = (ret <= 0);

    CLOSESOCK(cfd);
    server_thread.join();
    CLOSESOCK(lfd);

    REQUIRE((client_failed || server_rejected));
}

// ── Test 7 ────────────────────────────────────────────────────────

TEST_CASE("TOFU: first connect callback receives fingerprint and accepts", "[tls][tofu]") {
    auto server_ck = make_ck("tofu-server7");
    auto client_ck = make_ck("tofu-client7");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    std::string captured_fp;
    bool callback_called = false;

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        NodeTlsConfig scfg;
        scfg.cert_file = server_ck.cert_file.path;
        scfg.key_file = server_ck.key_file.path;
        scfg.authorized_keys_file = ak.path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        SSL_accept(ssl.get());
        char buf[1];
        SSL_read(ssl.get(), buf, 1);
        CLOSESOCK(cfd);
    });

    int cfd = connect_socket(port);
    NodeTlsConfig ccfg;
    ccfg.cert_file = client_ck.cert_file.path;
    ccfg.key_file = client_ck.key_file.path;
    ccfg.tofu_cb = [&](const std::string& fp) {
        callback_called = true;
        captured_fp = fp;
        return true;
    };
    auto ctx = create_node_tls(ccfg, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(cfd));
    REQUIRE(SSL_connect(ssl.get()) > 0);
    SSL_write(ssl.get(), "x", 1);

    CLOSESOCK(cfd);
    server_thread.join();
    CLOSESOCK(lfd);

    REQUIRE(callback_called);
    REQUIRE(!captured_fp.empty());
}

// ── Test 8 ────────────────────────────────────────────────────────

TEST_CASE("TOFU: second connect with same fingerprint accepts", "[tls][tofu]") {
    auto server_ck = make_ck("tofu-server8");
    auto client_ck = make_ck("tofu-client8");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    int lfd = listen_socket();
    int port = get_port(lfd);

    std::string first_fp;
    int callback_count = 0;

    // First connection
    {
        std::thread server_thread([&] {
            int cfd = accept(lfd, nullptr, nullptr);
            if (cfd < 0) return;
            NodeTlsConfig scfg;
            scfg.cert_file = server_ck.cert_file.path;
            scfg.key_file = server_ck.key_file.path;
            scfg.authorized_keys_file = ak.path;
            auto ctx = create_node_tls(scfg, TlsMode::Listen);
            auto ssl = SslPtr(SSL_new(ctx.get()));
            SSL_set_fd(ssl.get(), cfd);
            if (SSL_accept(ssl.get()) > 0) {
                char buf[1];
                SSL_read(ssl.get(), buf, 1);
            }
            CLOSESOCK(cfd);
        });

        int cfd = connect_socket(port);
        NodeTlsConfig ccfg;
        ccfg.cert_file = client_ck.cert_file.path;
        ccfg.key_file = client_ck.key_file.path;
        ccfg.tofu_cb = [&](const std::string& fp) {
            first_fp = fp;
            callback_count++;
            return true;
        };
        auto ctx = create_node_tls(ccfg, TlsMode::Connect);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(cfd));
        REQUIRE(SSL_connect(ssl.get()) > 0);
        SSL_write(ssl.get(), "x", 1);
        server_thread.join();  // join before close avoids racing SSL_accept
        CLOSESOCK(cfd);
    }

    REQUIRE(callback_count == 1);
    REQUIRE(!first_fp.empty());

    // Second connection with same server cert
    {
        int lfd2 = listen_socket();
        int port2 = get_port(lfd2);

        std::string second_fp;
        std::thread server_thread([&] {
            int cfd = accept(lfd2, nullptr, nullptr);
            if (cfd < 0) return;
            NodeTlsConfig scfg;
            scfg.cert_file = server_ck.cert_file.path;
            scfg.key_file = server_ck.key_file.path;
            scfg.authorized_keys_file = ak.path;
            auto ctx = create_node_tls(scfg, TlsMode::Listen);
            auto ssl = SslPtr(SSL_new(ctx.get()));
            SSL_set_fd(ssl.get(), cfd);
            SSL_accept(ssl.get());
            char buf[1];
            SSL_read(ssl.get(), buf, 1);
            CLOSESOCK(cfd);
        });

        int cfd = connect_socket(port2);
        NodeTlsConfig ccfg;
        ccfg.cert_file = client_ck.cert_file.path;
        ccfg.key_file = client_ck.key_file.path;
        ccfg.tofu_cb = [&](const std::string& fp) {
            second_fp = fp;
            callback_count++;
            return (fp == first_fp);
        };
        auto ctx = create_node_tls(ccfg, TlsMode::Connect);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(cfd));
        REQUIRE(SSL_connect(ssl.get()) > 0);
        SSL_write(ssl.get(), "x", 1);
        CLOSESOCK(cfd);
        server_thread.join();
        CLOSESOCK(lfd2);

        REQUIRE(callback_count == 2);
        REQUIRE(second_fp == first_fp);
    }

    CLOSESOCK(lfd);
}

// ── Test 9 ────────────────────────────────────────────────────────

TEST_CASE("TOFU: different fingerprint rejects handshake", "[tls][tofu]") {
    auto server_ck = make_ck("tofu-server9");
    auto rogue_ck = make_ck("tofu-rogue9");
    auto client_ck = make_ck("tofu-client9");

    TmpFile ak(client_ck.pubkey_hex + "\n");

    // First connect to establish the "known" fingerprint
    std::string known_fp;
    {
        int lfd = listen_socket();
        int port = get_port(lfd);

        std::thread server_thread([&] {
            int cfd = accept(lfd, nullptr, nullptr);
            if (cfd < 0) return;
            NodeTlsConfig scfg;
            scfg.cert_file = server_ck.cert_file.path;
            scfg.key_file = server_ck.key_file.path;
            scfg.authorized_keys_file = ak.path;
            auto ctx = create_node_tls(scfg, TlsMode::Listen);
            auto ssl = SslPtr(SSL_new(ctx.get()));
            SSL_set_fd(ssl.get(), cfd);
            SSL_accept(ssl.get());
            char buf[1];
            SSL_read(ssl.get(), buf, 1);
            CLOSESOCK(cfd);
        });

        int cfd = connect_socket(port);
        NodeTlsConfig ccfg;
        ccfg.cert_file = client_ck.cert_file.path;
        ccfg.key_file = client_ck.key_file.path;
        ccfg.tofu_cb = [&](const std::string& fp) { known_fp = fp; return true; };
        auto ctx = create_node_tls(ccfg, TlsMode::Connect);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(cfd));
        SSL_connect(ssl.get());
        SSL_write(ssl.get(), "x", 1);
        CLOSESOCK(cfd);
        server_thread.join();
        CLOSESOCK(lfd);
    }
    REQUIRE(!known_fp.empty());

    // Now connect to a DIFFERENT server (rogue) — TOFU should reject
    {
        int lfd = listen_socket();
        int port = get_port(lfd);

        std::thread server_thread([&] {
            int cfd = accept(lfd, nullptr, nullptr);
            if (cfd >= 0) {
                NodeTlsConfig scfg;
                scfg.cert_file = rogue_ck.cert_file.path;
                scfg.key_file = rogue_ck.key_file.path;
                scfg.authorized_keys_file = ak.path;
                auto ctx = create_node_tls(scfg, TlsMode::Listen);
                auto ssl = SslPtr(SSL_new(ctx.get()));
                SSL_set_fd(ssl.get(), cfd);
                SSL_accept(ssl.get());
                CLOSESOCK(cfd);
            }
        });

        int cfd = connect_socket(port);
        NodeTlsConfig ccfg;
        ccfg.cert_file = client_ck.cert_file.path;
        ccfg.key_file = client_ck.key_file.path;
        ccfg.tofu_cb = [&](const std::string& fp) -> bool {
            return (fp == known_fp);  // reject if different
        };
        auto ctx = create_node_tls(ccfg, TlsMode::Connect);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(cfd));
        int ret = SSL_connect(ssl.get());
        REQUIRE(ret <= 0);

        CLOSESOCK(cfd);
        server_thread.join();
        CLOSESOCK(lfd);
    }
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
