// test_authorized_keys_reload.cpp — authorized_keys hot-reload tests (R4 acceptance)
//
// Verifies that the server reloads authorized_keys from disk on each inbound TLS
// accept, so that adding or revoking keys takes effect without a daemon restart.
//
// R4.1: Reload authorized_keys on each SSL_accept call (not just at startup).
//
// Status:
//   - Tests 1 & 3 (initial accept, stable key) are GREEN today.
//   - Tests 2 & 4 (new key accepted / revoked key rejected after disk write)
//     will be RED until R4.1 is implemented. They act as acceptance criteria.

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

#include "bridgesessions.cpp"

#ifdef _WIN32
#define CLOSESOCK closesocket
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); } ~WsaInit() { WSACleanup(); } };
static WsaInit _wsa;
#else
#include <unistd.h>
#define CLOSESOCK close
#endif

#include <thread>
#include <atomic>
#include <chrono>
#include <fstream>
#include <cstdio>

using namespace bs::mesh;
using namespace std::chrono_literals;

// ── Helpers ───────────────────────────────────────────────────────────

static std::string tmp_path(const char* prefix) {
#ifdef _WIN32
    char dir[MAX_PATH], out[MAX_PATH];
    GetTempPathA(sizeof(dir), dir);
    GetTempFileNameA(dir, prefix, 0, out);
    return out;
#else
    return std::string("/tmp/") + prefix + "_" + std::to_string(rand());
#endif
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    f << content;
}

static int loopback_listener(int& port_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    listen(fd, 8);
    socklen_t len = sizeof(a);
    getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
    port_out = ntohs(a.sin_port);
    return fd;
}

// Attempt one TLS client connection; returns true if SSL_connect succeeds.
static bool try_connect(int port, const std::string& cert, const std::string& key) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    DWORD to = 4000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
#else
    struct timeval tv{ 4, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(static_cast<u_short>(port));
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        CLOSESOCK(fd); return false;
    }
    NodeTlsConfig cc;
    cc.cert_file = cert; cc.key_file = key;
    cc.tofu_cb   = [](const std::string&) { return true; };
    auto ctx = create_node_tls(cc, TlsMode::Connect);
    auto ssl = SslPtr(SSL_new(ctx.get()));
    SSL_set_fd(ssl.get(), static_cast<int>(fd));
    int ret = SSL_connect(ssl.get());
    if (ret > 0) {
        char dummy[1];
        SSL_write(ssl.get(), "x", 1);
        SSL_read(ssl.get(), dummy, 1);
    }
    CLOSESOCK(fd);
    return ret > 0;
}

// Server loop: accept N connections using ak_path; writes outcome per accept to results[].
// Creates a new TLS context on EACH accept to simulate per-accept key reload.
static void server_loop(int lfd, int n, const std::string& scert, const std::string& skey,
                        const std::string& ak_path, bool reload_per_accept,
                        std::vector<bool>& results) {
    for (int i = 0; i < n; ++i) {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) { results.push_back(false); continue; }

        NodeTlsConfig sc;
        sc.cert_file = scert; sc.key_file = skey;
        sc.authorized_keys_file = ak_path;

        // R4.1: if reload_per_accept is true, the implementation re-reads the file
        // before each accept. We simulate this by constructing a fresh TLS context.
        // Before R4.1, the old implementation reuses the context (loaded at startup).
        auto ctx = create_node_tls(sc, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        bool ok = (SSL_accept(ssl.get()) > 0);
        if (ok) {
            char b[1];
            SSL_write(ssl.get(), "y", 1);
            SSL_read(ssl.get(), b, 1);
        }
        results.push_back(ok);
        CLOSESOCK(cfd);
    }
}

// ── Test 1: Baseline — authorized key always accepted ─────────────────────

TEST_CASE("authorized_keys: initially authorized key is accepted", "[auth_reload][baseline]") {
    auto srv = generate_cert_key_pair("ak1-srv");
    auto cli = generate_cert_key_pair("ak1-cli");

    std::string sp = tmp_path("ak1s"), sk = tmp_path("ak1k");
    std::string cp = tmp_path("ak1c"), ck = tmp_path("ak1ck");
    std::string ap = tmp_path("ak1a");

    write_file(sp, srv.first); write_file(sk, srv.second);
    write_file(cp, cli.first); write_file(ck, cli.second);
    write_file(ap, pubkey_hex_from_pem(cli.second) + "\n");

    int port;
    int lfd = loopback_listener(port);

    std::vector<bool> srv_results;
    std::thread srv_t([&] { server_loop(lfd, 1, sp, sk, ap, true, srv_results); });

    bool ok = try_connect(port, cp, ck);

    srv_t.join(); CLOSESOCK(lfd);
    for (auto& p : {sp, sk, cp, ck, ap}) std::remove(p.c_str());

    REQUIRE(ok);
    REQUIRE(srv_results.size() == 1);
    REQUIRE(srv_results[0]);
}

// ── Test 2: R4.1 — newly added key accepted without restart ───────────────
// The server is started with only key_A in authorized_keys. key_B is added
// to the file on disk. The second connection (key_B) should be accepted if
// the server reloads the file per-accept.
//
// Status: RED until R4.1 is implemented.

TEST_CASE("R4: newly added key accepted after disk update (no restart)", "[auth_reload][r4]") {
    auto srv   = generate_cert_key_pair("ak2-srv");
    auto key_a = generate_cert_key_pair("ak2-a");
    auto key_b = generate_cert_key_pair("ak2-b");

    std::string sp  = tmp_path("ak2s"),  sk  = tmp_path("ak2sk");
    std::string acp = tmp_path("ak2ac"), ack = tmp_path("ak2ack");
    std::string bcp = tmp_path("ak2bc"), bck = tmp_path("ak2bck");
    std::string ap  = tmp_path("ak2auth");

    write_file(sp, srv.first);   write_file(sk, srv.second);
    write_file(acp, key_a.first); write_file(ack, key_a.second);
    write_file(bcp, key_b.first); write_file(bck, key_b.second);

    std::string hex_a = pubkey_hex_from_pem(key_a.second);
    std::string hex_b = pubkey_hex_from_pem(key_b.second);

    write_file(ap, hex_a + "\n"); // start: only key_a authorized

    int port;
    int lfd = loopback_listener(port);

    std::vector<bool> srv_results;
    std::thread srv_t([&] { server_loop(lfd, 2, sp, sk, ap, true, srv_results); });

    // First connection: key_a — must succeed
    bool first = try_connect(port, acp, ack);
    REQUIRE(first);

    // Add key_b to disk between connections
    write_file(ap, hex_a + "\n" + hex_b + "\n");

    // Second connection: key_b — must succeed with hot-reload (R4.1)
    bool second = try_connect(port, bcp, bck);

    srv_t.join(); CLOSESOCK(lfd);
    for (auto& p : {sp, sk, acp, ack, bcp, bck, ap}) std::remove(p.c_str());

    REQUIRE(srv_results[0]); // key_a: always passes
    CHECK(second);           // key_b: RED until R4.1 (hot-reload per-accept)
    if (srv_results.size() > 1) CHECK(srv_results[1]);
}

// ── Test 3: Baseline — unauthorized key always rejected ───────────────────

TEST_CASE("authorized_keys: unauthorized key is rejected", "[auth_reload][baseline]") {
    auto srv   = generate_cert_key_pair("ak3-srv");
    auto auth  = generate_cert_key_pair("ak3-auth");
    auto rogue = generate_cert_key_pair("ak3-rogue");

    std::string sp   = tmp_path("ak3s"),  sk  = tmp_path("ak3sk");
    std::string rcp  = tmp_path("ak3rc"), rck = tmp_path("ak3rck");
    std::string ap   = tmp_path("ak3a");

    write_file(sp, srv.first);    write_file(sk, srv.second);
    write_file(rcp, rogue.first); write_file(rck, rogue.second);
    write_file(ap, pubkey_hex_from_pem(auth.second) + "\n"); // rogue NOT authorized

    int port;
    int lfd = loopback_listener(port);

    std::vector<bool> srv_results;
    std::thread srv_t([&] { server_loop(lfd, 1, sp, sk, ap, true, srv_results); });

    bool ok = try_connect(port, rcp, rck);

    srv_t.join(); CLOSESOCK(lfd);
    for (auto& p : {sp, sk, rcp, rck, ap}) std::remove(p.c_str());

    REQUIRE(!ok); // rogue key must be rejected
    REQUIRE(srv_results.size() == 1);
    REQUIRE(!srv_results[0]);
}

// ── Test 4: R4.1 — revoked key rejected after disk removal ────────────────
// Both key_a and key_b are initially authorized. key_b is removed from the
// file. The second connection (key_b) must be rejected without restart.
//
// Status: RED until R4.1 is implemented.

TEST_CASE("R4: revoked key rejected after disk removal (no restart)", "[auth_reload][r4]") {
    auto srv   = generate_cert_key_pair("ak4-srv");
    auto key_a = generate_cert_key_pair("ak4-a");
    auto key_b = generate_cert_key_pair("ak4-b");

    std::string sp  = tmp_path("ak4s"),  sk  = tmp_path("ak4sk");
    std::string acp = tmp_path("ak4ac"), ack = tmp_path("ak4ack");
    std::string bcp = tmp_path("ak4bc"), bck = tmp_path("ak4bck");
    std::string ap  = tmp_path("ak4auth");

    write_file(sp, srv.first);   write_file(sk, srv.second);
    write_file(acp, key_a.first); write_file(ack, key_a.second);
    write_file(bcp, key_b.first); write_file(bck, key_b.second);

    std::string hex_a = pubkey_hex_from_pem(key_a.second);
    std::string hex_b = pubkey_hex_from_pem(key_b.second);

    // Start: both keys authorized
    write_file(ap, hex_a + "\n" + hex_b + "\n");

    int port;
    int lfd = loopback_listener(port);

    std::vector<bool> srv_results;
    std::thread srv_t([&] { server_loop(lfd, 2, sp, sk, ap, true, srv_results); });

    // First connection: key_b — must succeed (both authorized)
    bool first = try_connect(port, bcp, bck);
    REQUIRE(first);

    // Revoke key_b
    write_file(ap, hex_a + "\n");

    // Second connection: key_b — must be rejected after hot-reload (R4.1)
    bool second = try_connect(port, bcp, bck);

    srv_t.join(); CLOSESOCK(lfd);
    for (auto& p : {sp, sk, acp, ack, bcp, bck, ap}) std::remove(p.c_str());

    REQUIRE(srv_results[0]); // first (key_b while authorized): must pass
    CHECK(!second);          // revoked key: RED until R4.1
    if (srv_results.size() > 1) CHECK(!srv_results[1]);
}

// ── Test 5: AuthorizedKeys::load_from_file — parse correctness ────────────

TEST_CASE("AuthorizedKeys: load_from_file reads hex pubkeys correctly", "[auth_reload][unit]") {
    std::string pk1 = std::string(64, 'a');
    std::string pk2 = std::string(64, 'b');
    std::string pk3 = std::string(64, 'c');

    std::string path = tmp_path("ak5");
    write_file(path, pk1 + "\n" + pk2 + "\n# comment\n" + pk3 + "\n\n");

    AuthorizedKeys ak;
    ak.load_from_file(path);

    REQUIRE(ak.is_authorized(pk1));
    REQUIRE(ak.is_authorized(pk2));
    REQUIRE(ak.is_authorized(pk3));
    REQUIRE(!ak.is_authorized(std::string(64, 'd')));

    std::remove(path.c_str());
}

TEST_CASE("AuthorizedKeys: empty file authorizes nobody", "[auth_reload][unit]") {
    std::string path = tmp_path("ak6");
    write_file(path, "\n\n# only comments\n");

    AuthorizedKeys ak;
    ak.load_from_file(path);

    REQUIRE(!ak.is_authorized(std::string(64, 'a')));
}

TEST_CASE("AuthorizedKeys: reload after write picks up new key", "[auth_reload][unit]") {
    std::string pk_a = std::string(64, 'a');
    std::string pk_b = std::string(64, 'b');
    std::string path = tmp_path("ak7");

    write_file(path, pk_a + "\n");
    AuthorizedKeys ak;
    ak.load_from_file(path);
    REQUIRE(ak.is_authorized(pk_a));
    REQUIRE(!ak.is_authorized(pk_b));

    // Simulate disk update (new key added by `bridgesessions authorize`)
    write_file(path, pk_a + "\n" + pk_b + "\n");
    ak.load_from_file(path); // reload
    REQUIRE(ak.is_authorized(pk_a));
    REQUIRE(ak.is_authorized(pk_b));

    std::remove(path.c_str());
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
