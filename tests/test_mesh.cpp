// winsock2 must come BEFORE windows.h
// NOMINMAX must come BEFORE any header that might include windows.h
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

// Paranoia: ensure min/max macros are undefined before including bridgesessions
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
#include <cstdio>
#include <fstream>

using namespace bs::mesh;

// ── Helpers ──────────────────────────────────────────────────────

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

static std::string write_authorized_keys(const std::string& pk1, const std::string& pk2) {
#ifdef _WIN32
    char af[MAX_PATH], tmpPath[MAX_PATH];
    GetTempPathA(sizeof(tmpPath), tmpPath);
    GetTempFileNameA(tmpPath, "bsak", 0, af);
    std::string path = af;
#else
    std::string path = std::string("/tmp/bs_ak_") + std::to_string(rand());
#endif
    std::ofstream f(path);
    f << pk1 << "\n" << pk2 << "\n";
    f.close();
    return path;
}

// Write a config file with the given content
static std::string write_config(const std::string& content) {
#ifdef _WIN32
    char tmpPath[MAX_PATH], cf[MAX_PATH];
    GetTempPathA(sizeof(tmpPath), tmpPath);
    GetTempFileNameA(tmpPath, "bscfg", 0, cf);
    std::string path = cf;
#else
    std::string path = std::string("/tmp/bs_cfg_") + std::to_string(rand());
#endif
    std::ofstream f(path);
    f << content;
    f.close();
    return path;
}

// ── Test 1: MeshController constructor initializes correctly ────

TEST_CASE("MeshController constructor initializes TLS contexts", "[mesh][controller]") {
    MeshConfig cfg;
    cfg.node_name = "test-node";
    cfg.listen_port = 19950;

    MeshController mc(cfg);
    REQUIRE(mc.conn_count() == 0);
}

// ── Test 2: Two nodes: one listens, one connects via MeshController ──

TEST_CASE("MeshController: two nodes connect via event loop", "[mesh][controller][integration]") {
    // This test creates two MeshControllers with different ports,
    // runs one in a thread, and connects the other manually.

    // Setup: create identities in temp locations
    // We need to work with the actual ~/.bridgesessions dir
    // Strategy: use a temp dir as "home"
}

// ── Test 3: Ping/Pong at protocol level ─────────────────────────

TEST_CASE("mesh: ping/pong exchange over TLS", "[mesh][integration]") {
    auto id_a = CertKeyTemp(generate_cert_key_pair("ping-a"), "bs_pa");
    auto id_b = CertKeyTemp(generate_cert_key_pair("ping-b"), "bs_pb");
    std::string ak_path = write_authorized_keys(id_a.pubkey_hex, id_b.pubkey_hex);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(lfd, 1) == 0);

    sockaddr_in addr2{};
    socklen_t len2 = sizeof(addr2);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr2), &len2);
    int port = ntohs(addr2.sin_port);

    std::atomic<bool> a_got_ping{false};
    std::atomic<bool> b_got_pong{false};

    std::thread thread_a([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        REQUIRE(cfd >= 0);

        NodeTlsConfig scfg;
        scfg.cert_file = id_a.cert_file;
        scfg.key_file = id_a.key_file;
        scfg.authorized_keys_file = ak_path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        REQUIRE(SSL_accept(ssl.get()) > 0);

        Message msg = read_frame(ssl.get());
        REQUIRE(std::holds_alternative<PingMsg>(msg));
        a_got_ping = true;

        write_frame(ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
        CLOSESOCK(cfd);
    });

    std::thread thread_b([&] {
        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(sfd >= 0);

        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = htons(static_cast<u_short>(port));
        REQUIRE(connect(sfd, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == 0);

        NodeTlsConfig ccfg;
        ccfg.cert_file = id_b.cert_file;
        ccfg.key_file = id_b.key_file;
        ccfg.tofu_cb = [](const std::string&) { return true; };
        auto ctx = create_node_tls(ccfg, TlsMode::Connect);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(sfd));
        REQUIRE(SSL_connect(ssl.get()) > 0);

        auto t0 = std::chrono::steady_clock::now();
        write_frame(ssl.get(), PingMsg{}, CONTROL_STREAM_ID);

        Message msg = read_frame(ssl.get());
        auto t1 = std::chrono::steady_clock::now();
        REQUIRE(std::holds_alternative<PongMsg>(msg));
        b_got_pong = true;

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
        REQUIRE(elapsed.count() < 2000);

        CLOSESOCK(sfd);
    });

    thread_a.join();
    thread_b.join();
    CLOSESOCK(lfd);

    REQUIRE(a_got_ping);
    REQUIRE(b_got_pong);
}

// ── Test 4: Two nodes connect and exchange Hello ────────────────

TEST_CASE("mesh: two nodes connect and exchange Hello", "[mesh][integration]") {
    auto id_a = CertKeyTemp(generate_cert_key_pair("node-a"), "bs_a");
    auto id_b = CertKeyTemp(generate_cert_key_pair("node-b"), "bs_b");
    std::string ak_path = write_authorized_keys(id_a.pubkey_hex, id_b.pubkey_hex);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(lfd, 1) == 0);

    sockaddr_in addr2{};
    socklen_t len = sizeof(addr2);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr2), &len);
    int port = ntohs(addr2.sin_port);

    std::atomic<bool> a_got_hello{false};
    std::atomic<bool> b_got_hello{false};
    std::string a_peer_name;
    std::string b_peer_name;

    std::thread thread_a([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        REQUIRE(cfd >= 0);

        NodeTlsConfig scfg;
        scfg.cert_file = id_a.cert_file;
        scfg.key_file = id_a.key_file;
        scfg.authorized_keys_file = ak_path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        REQUIRE(ctx != nullptr);

        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        REQUIRE(SSL_accept(ssl.get()) > 0);

        HelloMsg hello_a;
        hello_a.node_name = "node-a";
        hello_a.version = "1.0.0";
        hello_a.pubkey_hex = id_a.pubkey_hex;
        write_frame(ssl.get(), hello_a, CONTROL_STREAM_ID);

        Message msg_b = read_frame(ssl.get());
        REQUIRE(std::holds_alternative<HelloMsg>(msg_b));
        auto& hb = std::get<HelloMsg>(msg_b);
        REQUIRE(hb.node_name == "node-b");
        b_peer_name = hb.node_name;
        a_got_hello = true;

        CLOSESOCK(cfd);
    });

    std::thread thread_b([&] {
        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(sfd >= 0);

        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = htons(static_cast<u_short>(port));
        REQUIRE(connect(sfd, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == 0);

        NodeTlsConfig ccfg;
        ccfg.cert_file = id_b.cert_file;
        ccfg.key_file = id_b.key_file;
        ccfg.tofu_cb = [](const std::string&) { return true; };
        auto ctx = create_node_tls(ccfg, TlsMode::Connect);
        REQUIRE(ctx != nullptr);

        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(sfd));
        REQUIRE(SSL_connect(ssl.get()) > 0);

        HelloMsg hello_b;
        hello_b.node_name = "node-b";
        hello_b.version = "1.0.0";
        hello_b.pubkey_hex = id_b.pubkey_hex;
        write_frame(ssl.get(), hello_b, CONTROL_STREAM_ID);

        Message msg_a = read_frame(ssl.get());
        REQUIRE(std::holds_alternative<HelloMsg>(msg_a));
        auto& ha = std::get<HelloMsg>(msg_a);
        REQUIRE(ha.node_name == "node-a");
        a_peer_name = ha.node_name;
        b_got_hello = true;

        CLOSESOCK(sfd);
    });

    thread_a.join();
    thread_b.join();
    CLOSESOCK(lfd);

    REQUIRE(a_got_hello);
    REQUIRE(b_got_hello);
    REQUIRE(a_peer_name == "node-a");
    REQUIRE(b_peer_name == "node-b");
}

// ── Test 5: Gossip exchange transfers peer info ─────────────────

TEST_CASE("mesh: gossip exchange transfers peer info", "[mesh][integration]") {
    auto id_a = CertKeyTemp(generate_cert_key_pair("gos-a"), "bs_ga");
    auto id_b = CertKeyTemp(generate_cert_key_pair("gos-b"), "bs_gb");
    std::string ak_path = write_authorized_keys(id_a.pubkey_hex, id_b.pubkey_hex);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(lfd, 1) == 0);

    sockaddr_in addr2{};
    socklen_t len2 = sizeof(addr2);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr2), &len2);
    int port = ntohs(addr2.sin_port);

    std::atomic<bool> b_got_gossip{false};
    std::string b_gossip_peer_name;

    PeerInfo peer_c;
    peer_c.name = "node-c";
    peer_c.addr = "192.168.1.100:19948";

    std::thread thread_a([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        REQUIRE(cfd >= 0);

        NodeTlsConfig scfg;
        scfg.cert_file = id_a.cert_file;
        scfg.key_file = id_a.key_file;
        scfg.authorized_keys_file = ak_path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        REQUIRE(SSL_accept(ssl.get()) > 0);

        GossipMsg g;
        g.peers.push_back(peer_c);
        write_frame(ssl.get(), g, CONTROL_STREAM_ID);

        CLOSESOCK(cfd);
    });

    std::thread thread_b([&] {
        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(sfd >= 0);

        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = htons(static_cast<u_short>(port));
                REQUIRE(connect(sfd, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == 0);
#ifndef _WIN32
        timeval tv{5, 0};
        setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif


        NodeTlsConfig ccfg;
        ccfg.cert_file = id_b.cert_file;
        ccfg.key_file = id_b.key_file;
        ccfg.tofu_cb = [](const std::string&) { return true; };
        auto ctx = create_node_tls(ccfg, TlsMode::Connect);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(sfd));
        REQUIRE(SSL_connect(ssl.get()) > 0);

        Message msg = read_frame(ssl.get());
        REQUIRE(std::holds_alternative<GossipMsg>(msg));
        auto& gm = std::get<GossipMsg>(msg);
        REQUIRE(gm.peers.size() >= 1);

        for (auto& p : gm.peers) {
            if (p.name == "node-c") {
                b_gossip_peer_name = p.name;
                b_got_gossip = true;
                break;
            }
        }

        CLOSESOCK(sfd);
    });

    thread_a.join();
    thread_b.join();
    CLOSESOCK(lfd);

    REQUIRE(b_got_gossip);
    REQUIRE(b_gossip_peer_name == "node-c");
}

// ── Test 6: Session list over mesh ──────────────────────────────

TEST_CASE("mesh: session list exchange", "[mesh][integration]") {
    auto id_a = CertKeyTemp(generate_cert_key_pair("sl-a"), "bs_sla");
    auto id_b = CertKeyTemp(generate_cert_key_pair("sl-b"), "bs_slb");
    std::string ak_path = write_authorized_keys(id_a.pubkey_hex, id_b.pubkey_hex);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(lfd, 1) == 0);

    sockaddr_in addr2{};
    socklen_t len2 = sizeof(addr2);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr2), &len2);
    int port = ntohs(addr2.sin_port);

    std::atomic<bool> b_got_sessions{false};
    size_t num_sessions = 0;

    std::thread thread_a([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        REQUIRE(cfd >= 0);

        NodeTlsConfig scfg;
        scfg.cert_file = id_a.cert_file;
        scfg.key_file = id_a.key_file;
        scfg.authorized_keys_file = ak_path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        REQUIRE(SSL_accept(ssl.get()) > 0);

        SessionListMsg slm;
        SessionInfo si;
        si.name = "mysession";
        si.state = "running";
        si.uptime_seconds = 42;
        slm.sessions.push_back(si);

        SessionInfo si2;
        si2.name = "backup";
        si2.state = "detached";
        si2.uptime_seconds = 3600;
        slm.sessions.push_back(si2);

        write_frame(ssl.get(), slm, CONTROL_STREAM_ID);
        CLOSESOCK(cfd);
    });

    std::thread thread_b([&] {
        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(sfd >= 0);

        sockaddr_in saddr{};
        saddr.sin_family = AF_INET;
        saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        saddr.sin_port = htons(static_cast<u_short>(port));
        REQUIRE(connect(sfd, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) == 0);

        NodeTlsConfig ccfg;
        ccfg.cert_file = id_b.cert_file;
        ccfg.key_file = id_b.key_file;
        ccfg.tofu_cb = [](const std::string&) { return true; };
        auto ctx = create_node_tls(ccfg, TlsMode::Connect);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), static_cast<int>(sfd));
        REQUIRE(SSL_connect(ssl.get()) > 0);

        Message msg = read_frame(ssl.get());
        REQUIRE(std::holds_alternative<SessionListMsg>(msg));
        auto& sl = std::get<SessionListMsg>(msg);
        REQUIRE(sl.sessions.size() == 2);
        REQUIRE(sl.sessions[0].name == "mysession");
        REQUIRE(sl.sessions[0].state == "running");
        REQUIRE(sl.sessions[0].uptime_seconds == 42);
        REQUIRE(sl.sessions[1].name == "backup");
        REQUIRE(sl.sessions[1].state == "detached");
        REQUIRE(sl.sessions[1].uptime_seconds == 3600);

        b_got_sessions = true;
        num_sessions = sl.sessions.size();

        CLOSESOCK(sfd);
    });

    thread_a.join();
    thread_b.join();
    CLOSESOCK(lfd);

    REQUIRE(b_got_sessions);
    REQUIRE(num_sessions == 2);
}

// ── Test 7: MeshController connect_to_peer (auto-connect) ───────

TEST_CASE("MeshController: connect_to_peer establishes TLS connection", "[mesh][controller][integration]") {
    // We need to set up a real .bridgesessions directory with identities
    // Strategy: Create a temp dir, set USERPROFILE, bootstrap identity,
    // then create MeshController with a seed pointing to a listener.

    // Create a temp home dir
#ifdef _WIN32
    char tmpPath[MAX_PATH], thome[MAX_PATH];
    GetTempPathA(sizeof(tmpPath), tmpPath);
    GetTempFileNameA(tmpPath, "bsh", 0, thome);
    DeleteFileA(thome); // GetTempFileName creates the file, we want a dir
    CreateDirectoryA(thome, nullptr);
    std::string home_dir = thome;
#else
    std::string home_dir = std::string("/tmp/bs_test_home_") + std::to_string(rand());
    mkdir(home_dir.c_str(), 0700);
#endif

    // Create .bridgesessions directory in home
    std::string bs_dir = home_dir + "/.bridgesessions";
#ifdef _WIN32
    CreateDirectoryA(bs_dir.c_str(), nullptr);
#else
    mkdir(bs_dir.c_str(), 0700);
#endif

    // Create a fake id_ed25519.pub (must not be empty for the constructor to work)
    std::ofstream pf(bs_dir + "/id_ed25519.pub");
    pf << "0000000000000000000000000000000000000000000000000000000000000000";
    pf.close();

    // We also need the cert and key for TLS. Generate them.
    auto ck = generate_cert_key_pair("test-node");
    std::ofstream cf(bs_dir + "/id_ed25519-cert.pem");
    cf << ck.first;
    cf.close();
    std::ofstream kf(bs_dir + "/id_ed25519.pem");
    kf << ck.second;
    kf.close();

    // Update the .pub file with the real pubkey
    std::string real_pubkey = pubkey_hex_from_pem(ck.second);
    std::ofstream pf2(bs_dir + "/id_ed25519.pub", std::ios::trunc);
    pf2 << real_pubkey;
    pf2.close();

    // Set USERPROFILE for the duration of the test.
    // Must use _putenv_s (not SetEnvironmentVariableA) — SetEnvironmentVariableA updates
    // the Win32 environment but NOT the MSVC C runtime's getenv cache, so std::getenv
    // inside MeshController would still return the real user's home.
#ifdef _WIN32
    char old_userprofile[MAX_PATH] = {};
    GetEnvironmentVariableA("USERPROFILE", old_userprofile, sizeof(old_userprofile));
    _putenv_s("USERPROFILE", home_dir.c_str());
#else
    const char* old_home = getenv("HOME");
    std::string old_home_str = old_home ? old_home : "";
    setenv("HOME", home_dir.c_str(), 1);
#endif

    // Set up a simple listener that will accept ONE connection
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(lfd >= 0);
    int opt = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(listen(lfd, 1) == 0);

    sockaddr_in addr2{};
    socklen_t len = sizeof(addr2);
    getsockname(lfd, reinterpret_cast<sockaddr*>(&addr2), &len);
    int port = ntohs(addr2.sin_port);

    std::string peer_addr = "127.0.0.1:" + std::to_string(port);

    // Create server listener in background
    std::atomic<bool> server_got_hello{false};
    std::string server_peer_name;

    // Need authorized_keys that trusts our test node's pubkey
    std::string ak_path = home_dir + "/.bridgesessions/authorized_keys";
    std::ofstream akf(ak_path);
    akf << real_pubkey << "\n";
    akf.close();

    // Generate server identity
    auto server_ck = generate_cert_key_pair("server");
    std::string server_cert = bs_dir + "/server-cert.pem";
    std::string server_key = bs_dir + "/server-key.pem";
    std::ofstream scf(server_cert); scf << server_ck.first; scf.close();
    std::ofstream skf(server_key);  skf << server_ck.second; skf.close();

    std::thread server_thread([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;

        NodeTlsConfig scfg;
        scfg.cert_file = server_cert;
        scfg.key_file = server_key;
        scfg.authorized_keys_file = ak_path;
        auto ctx = create_node_tls(scfg, TlsMode::Listen);
        auto ssl = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), cfd);
        if (SSL_accept(ssl.get()) <= 0) { CLOSESOCK(cfd); return; }

        // Server: read Hello first (since client sends first in connect_to_peer)
        try {
            Message msg = read_frame(ssl.get());
            if (std::holds_alternative<HelloMsg>(msg)) {
                auto& h = std::get<HelloMsg>(msg);
                server_got_hello = true;
                server_peer_name = h.node_name;

                // Send Hello back
                HelloMsg resp;
                resp.node_name = "server";
                resp.version = "1.0.0";
                resp.pubkey_hex = pubkey_hex_from_pem(server_ck.second);
                write_frame(ssl.get(), resp, CONTROL_STREAM_ID);
            }
        } catch (...) {}

        CLOSESOCK(cfd);
    });

    // Create MeshController and call connect_to_peer
    MeshConfig cfg;
    cfg.node_name = "test-node";
    cfg.listen_port = 29999; // different port from listener
    // Pin the listener's Ed25519 pubkey (require_seed_pins default true since v2.0.3).
    PeerEntry seed;
    seed.name = "server";
    seed.addr = peer_addr;
    seed.pubkey_hex = pubkey_hex_from_pem(server_ck.second);
    cfg.seeds.push_back(seed);

    // App home = isolated bs_dir (not $HOME alone).
    MeshController mc(cfg, bs_dir);

    // Now call connect_to_peer
    bool connected = mc.connect_to_peer(peer_addr);

    server_thread.join();
    CLOSESOCK(lfd);

    REQUIRE(connected);
    REQUIRE(server_got_hello);
    REQUIRE(server_peer_name == "test-node");
    REQUIRE(mc.conn_count() == 1);

    // Restore environment variable
#ifdef _WIN32
    _putenv_s("USERPROFILE", old_userprofile);
#else
    if (old_home_str.empty()) unsetenv("HOME");
    else setenv("HOME", old_home_str.c_str(), 1);
#endif

    // Clean up
    std::remove((bs_dir + "/server-cert.pem").c_str());
    std::remove((bs_dir + "/server-key.pem").c_str());
    std::remove(ak_path.c_str());
}

TEST_CASE("MeshController: mdns_enabled defaults false and is configurable",
          "[mesh][mdns][security]") {
    MeshConfig cfg;
    REQUIRE_FALSE(cfg.mdns_enabled);
    cfg.mdns_enabled = true;
    REQUIRE(cfg.mdns_enabled);
}

TEST_CASE("MeshController: gossip merge filters untrusted pubkeys",
          "[mesh][security][gossip]") {
    namespace fs = std::filesystem;
    auto home = std::string("/tmp/bs_test_gossip_") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(ensure_private_directory(home));

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    std::string trusted_pk(64, 'b');
    std::string untrusted_pk(64, 'c');

    std::string ak_path = home + "/authorized_keys";
    REQUIRE(write_private_text_file(ak_path, trusted_pk + "\n"));

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = ak_path;
    cfg.seeds.push_back(
        PeerEntry{.name = "seed-peer", .addr = "10.0.0.1:19949", .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    GossipMsg g;
    g.peers.push_back(PeerInfo{
        .name = "untrusted-node",
        .addr = "10.0.0.2:19949",
        .pubkey_hex = untrusted_pk,
        .last_seen = 1});
    g.peers.push_back(PeerInfo{
        .name = "trusted-node",
        .addr = "10.0.0.3:19949",
        .pubkey_hex = trusted_pk,
        .last_seen = 2});

    mc.inject_gossip(g);

    auto discovered = mc.discovered_peers();
    REQUIRE(discovered.size() == 1);
    REQUIRE(discovered[0].name == "trusted-node");
    REQUIRE(discovered[0].pubkey_hex == trusted_pk);

    fs::remove_all(home);
}

TEST_CASE("MeshController: mDNS only adds trusted announcements",
          "[mesh][security][mdns]") {
    namespace fs = std::filesystem;
    auto home = std::string("/tmp/bs_test_mdns_") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(ensure_private_directory(home));

    auto ck = generate_cert_key_pair("test");
    std::string seed_pk = pubkey_hex_from_pem(ck.second);
    std::string trusted_pk(64, 'b');
    std::string untrusted_pk(64, 'c');

    std::string ak_path = home + "/authorized_keys";
    REQUIRE(write_private_text_file(ak_path, trusted_pk + "\n"));

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = ak_path;
    cfg.seeds.push_back(
        PeerEntry{.name = "seed-peer", .addr = "10.0.0.1:19949", .pubkey_hex = seed_pk});

    MeshController mc(cfg, home);

    mc.process_mdns_announcement_for_test(
        "untrusted-mdns", "10.0.0.5:19949", untrusted_pk);
    mc.process_mdns_announcement_for_test(
        "trusted-mdns", "10.0.0.6:19949", trusted_pk);

    auto discovered = mc.discovered_peers();
    REQUIRE(discovered.size() == 1);
    REQUIRE(discovered[0].name == "trusted-mdns");
    REQUIRE(discovered[0].pubkey_hex == trusted_pk);

    reset_logger_for_test();
    fs::remove_all(home);
}

TEST_CASE("MeshController: discovery never learns a missing seed pin and trusted mDNS updates address",
          "[mesh][security][mdns][gossip]") {
    namespace fs = std::filesystem;
    auto home = std::string("/tmp/bs_test_discovery_pin_") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(ensure_private_directory(home));

    std::string trusted_pk(64, 'd');
    std::string attacker_pk(64, 'e');
    std::string ak_path = home + "/authorized_keys";
    REQUIRE(write_private_text_file(ak_path, trusted_pk + "\n"));

    MeshConfig cfg;
    cfg.node_name = "test";
    cfg.authorized_keys_path = ak_path;
    cfg.seeds.push_back(PeerEntry{.name = "unpinned", .addr = "10.0.0.1:19949"});
    cfg.seeds.push_back(PeerEntry{
        .name = "trusted-seed", .addr = "10.0.0.2:19949", .pubkey_hex = trusted_pk});
    MeshController mc(cfg, home);

    GossipMsg forged;
    forged.peers.push_back(PeerInfo{
        .name = "unpinned", .addr = "10.0.0.99:19949", .pubkey_hex = attacker_pk});
    mc.inject_gossip(forged);
    REQUIRE(mc.discovered_peers().empty());
    REQUIRE(mc.configured_peer_addr_for_test("unpinned") == "10.0.0.1:19949");

    mc.process_mdns_announcement_for_test(
        "trusted-seed", "10.0.0.22:19949", trusted_pk);
    REQUIRE(mc.configured_peer_addr_for_test("trusted-seed") == "10.0.0.22:19949");

    reset_logger_for_test();
    fs::remove_all(home);
}

TEST_CASE("MeshController: identical duplicate Hello is ignored, different Hello closes",
          "[mesh][security][hello]") {
    namespace fs = std::filesystem;
    auto home = std::string("/tmp/bs_test_hello_") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(ensure_private_directory(home));

    MeshConfig cfg;
    cfg.node_name = "test";
    MeshController mc(cfg, home);

    MeshController::Conn c;
    HelloMsg h;
    h.node_name = "peer";
    h.pubkey_hex = std::string(64, 'a');
    h.version = std::string(kBridgeSessionsVersion);

    mc.test_set_initial_hello_for_test(c, h);
    REQUIRE(mc.test_handle_hello_for_test(c, h)); // identical -> ignored
    REQUIRE_FALSE(c.close_requested);

    HelloMsg h2 = h;
    h2.node_name = "attacker";
    REQUIRE_FALSE(mc.test_handle_hello_for_test(c, h2)); // different -> close
    REQUIRE(c.close_requested);

    fs::remove_all(home);
}

TEST_CASE("IPC token: generate, write, load round-trip",
          "[ipc][security][token]") {
    namespace fs = std::filesystem;
    auto home = std::string("/tmp/bs_test_ipctoken_") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(ensure_private_directory(home));

    std::string token = generate_ipc_token();
    REQUIRE(token.size() == 64);
    REQUIRE(std::all_of(token.begin(), token.end(),
                        [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); }));

    REQUIRE(write_ipc_token_file(home, token));
    REQUIRE(load_ipc_token(home) == token);

    fs::remove_all(home);
}

TEST_CASE("MeshController: IPC request requires valid token",
          "[ipc][security][token]") {
    namespace fs = std::filesystem;
    auto home = std::string("/tmp/bs_test_ipcauth_") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(ensure_private_directory(home));

    MeshConfig cfg;
    cfg.node_name = "test";
    MeshController mc(cfg, home);

    mc.set_ipc_token_for_test("deadbeef");
    REQUIRE(mc.ipc_request_is_authorized_for_test("deadbeef HEALTH x"));
    REQUIRE_FALSE(mc.ipc_request_is_authorized_for_test("deadbeefx HEALTH x"));
    REQUIRE_FALSE(mc.ipc_request_is_authorized_for_test("deadbeef"));
    REQUIRE_FALSE(mc.ipc_request_is_authorized_for_test("badbeef HEALTH x"));
    REQUIRE_FALSE(mc.ipc_request_is_authorized_for_test("HEALTH x"));
    REQUIRE_FALSE(mc.ipc_request_is_authorized_for_test(""));

    fs::remove_all(home);
}

TEST_CASE("MeshController: revoked authorized key closes an existing connection",
          "[mesh][security][revocation]") {
#ifdef _WIN32
    SUCCEED("covered by the platform-neutral trust check; socketpair is POSIX-only");
#else
    auto peer = CertKeyTemp(generate_cert_key_pair("revoked-peer"), "bs_rev");
    std::string authorized = write_authorized_keys(peer.pubkey_hex, peer.pubkey_hex);

    MeshConfig cfg;
    cfg.node_name = "revocation-test";
    cfg.authorized_keys_path = authorized;
    MeshController mc(cfg);

    int fds[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    MeshController::Conn conn;
    conn.peer_name = "revoked-peer";
    conn.peer_pubkey = peer.pubkey_hex;
    conn.sock_fd = fds[0];
    const size_t index = mc.add_connection_for_test(std::move(conn));

    mc.prune_revoked_connections_for_test();
    REQUIRE_FALSE(mc.connection_closed_for_test(index));

    { std::ofstream revoked(authorized, std::ios::trunc); }
    mc.prune_revoked_connections_for_test();
    REQUIRE(mc.connection_closed_for_test(index));

    CLOSESOCK(fds[1]);
    std::remove(authorized.c_str());
#endif
}

TEST_CASE("MeshController: transfer watchdog cancels without releasing SSL ownership",
          "[mesh][security][worker]") {
    MeshConfig cfg;
    cfg.node_name = "watchdog-test";
    MeshController mc(cfg, "/tmp/bs-watchdog-test");

    MeshController::Conn conn;
    conn.peer_name = "busy-peer";
    conn.peer_pubkey = std::string(64, 'e');
    const size_t index = mc.add_connection_for_test(std::move(conn));

    mc.expire_exec_watchdog_for_test(index);
    REQUIRE(mc.exec_busy_for_test(index));
    REQUIRE(mc.exec_cancelled_for_test(index));
}

TEST_CASE("select descriptor guard rejects out-of-range POSIX descriptors",
          "[mesh][security][fdset]") {
#ifdef _WIN32
    SUCCEED("Winsock fd_set stores SOCKET handles rather than POSIX fd indices");
#else
    REQUIRE(socket_selectable(0));
    REQUIRE_FALSE(socket_selectable(FD_SETSIZE));
    REQUIRE_FALSE(socket_selectable(INVALID_SOCKET));
#endif
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
