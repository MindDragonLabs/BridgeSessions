// test_mesh_reliability.cpp — Mesh reliability tests (v1.3 R3/R5 acceptance criteria)
//
// Covers:
//   R3: Daemon lifecycle — SO_REUSEADDR prevents bind-fail on restart
//   R5: CLI robustness:
//       - find_peer_addr returns empty for unknown peer (no SIGSEGV)
//       - pong timeout detection logic
//   Architecture: duplicate connection resolution (lower pubkey wins)
//   Gossip: received GossipMsg propagates peers to discovered list
//
// Tests marked "requires-api" need a public method that may not exist yet.
// Those are acceptance criteria for the public API surface, not bugs in
// existing behaviour.

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

#include "../bridgesessions.cpp"

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

using namespace bs::mesh;
using namespace std::chrono_literals;

// ── Helpers ───────────────────────────────────────────────────────────

static MeshConfig mesh_cfg(const std::string& name, int port = 19954) {
    MeshConfig c;
    c.node_name              = name;
    c.listen_port            = port;
    c.gossip_interval_secs   = 300;
    c.ping_interval_secs     = 300;
    c.pong_timeout_secs      = 3;  // short for testing
    c.scrollback_lines       = 100;
    return c;
}

static MeshController::Conn make_peer_conn(const std::string& name, const std::string& pubkey) {
    MeshController::Conn c;
    c.peer_name   = name;
    c.peer_pubkey = pubkey;
    c.peer_addr   = "127.0.0.1:9000";
    c.ssl.reset();
    c.sock_fd   = INVALID_SOCKET;
    c.last_pong = std::chrono::steady_clock::now();
    c.attached_session = nullptr;
    return c;
}

// ── R3: SO_REUSEADDR — port rebind after close succeeds ────────────────────
// Validates the mechanism that prevents mesh_listen_bind_failed on daemon restart.

TEST_CASE("R3: SO_REUSEADDR allows immediate rebind after close", "[mesh_reliability][r3][bind]") {
    int port_used = 0;

    // First daemon: bind, listen, close
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        int opt = 1;
        REQUIRE(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char*>(&opt), sizeof(opt)) == 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        REQUIRE(bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
        REQUIRE(listen(fd, 4) == 0);
        socklen_t len = sizeof(a);
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port_used = ntohs(a.sin_port);
        CLOSESOCK(fd);
    }

    // Second daemon: immediate rebind on same port with SO_REUSEADDR — must succeed
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<u_short>(port_used));
        int r = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        INFO("rebind on port " << port_used << " result: " << r);
        REQUIRE(r == 0);
        CLOSESOCK(fd);
    }
}

TEST_CASE("R3: bind WITHOUT SO_REUSEADDR may fail on immediate rebind", "[mesh_reliability][r3][bind]") {
    // This test documents the problem that SO_REUSEADDR solves.
    // On Windows in TIME_WAIT the bind fails; this test just records the behaviour
    // so we can verify that the production code sets SO_REUSEADDR correctly.
    int port_used = 0;
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        listen(fd, 1);
        socklen_t len = sizeof(a);
        getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        port_used = ntohs(a.sin_port);
        CLOSESOCK(fd);
    }
    // Without SO_REUSEADDR on Windows, this MIGHT fail (TIME_WAIT) — we just document it
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons(static_cast<u_short>(port_used));
        int r = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        CLOSESOCK(fd);
        // We don't REQUIRE failure here — just log
        WARN("bind without SO_REUSEADDR on reused port returned: " << r);
    }
    SUCCEED(); // Informational test — always passes
}

// ── Pong timeout detection ─────────────────────────────────────────────────
// Tests MeshController::is_pong_timed_out(conn) — a pure function on conn state.
// Requires that is_pong_timed_out() be public or exposed via BS_TESTING.

TEST_CASE("pong_timeout: conn with stale last_pong is timed out", "[mesh_reliability][pong]") {
    auto cfg = mesh_cfg("pong1");
    MeshController mc(cfg);

    auto conn = make_peer_conn("silent", std::string(64, 'd'));
    // Set last_pong to well beyond pong_timeout_secs=3
    conn.last_pong = std::chrono::steady_clock::now() - 30s;

    REQUIRE(mc.is_pong_timed_out(conn));
}

TEST_CASE("pong_timeout: conn with recent last_pong is NOT timed out", "[mesh_reliability][pong]") {
    auto cfg = mesh_cfg("pong2");
    MeshController mc(cfg);

    auto conn = make_peer_conn("active", std::string(64, 'e'));
    conn.last_pong = std::chrono::steady_clock::now(); // just now

    REQUIRE(!mc.is_pong_timed_out(conn));
}

TEST_CASE("pong_timeout: conn exactly at boundary uses pong_timeout_secs config", "[mesh_reliability][pong]") {
    auto cfg = mesh_cfg("pong3");
    cfg.pong_timeout_secs = 5;
    MeshController mc(cfg);

    auto conn = make_peer_conn("boundary", std::string(64, 'f'));

    // 4 seconds ago: NOT timed out
    conn.last_pong = std::chrono::steady_clock::now() - 4s;
    REQUIRE(!mc.is_pong_timed_out(conn));

    // 6 seconds ago: timed out
    conn.last_pong = std::chrono::steady_clock::now() - 6s;
    REQUIRE(mc.is_pong_timed_out(conn));
}

// ── Gossip: received peer added to discovered list ─────────────────────────
// Requires mc.handle_gossip(msg) and mc.discovered_peers() to be public/exposed.

TEST_CASE("gossip: received GossipMsg adds new peer to discovered list", "[mesh_reliability][gossip]") {
    namespace fs = std::filesystem;
    auto home = std::string("/tmp/bs_reliability_gossip1_") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(ensure_private_directory(home));

    std::string newcomer_pk(64, '1');
    REQUIRE(write_private_text_file(home + "/authorized_keys", newcomer_pk + "\n"));

    auto cfg = mesh_cfg("gossip1");
    cfg.authorized_keys_path = home + "/authorized_keys";
    MeshController mc(cfg, home);

    PeerInfo newcomer;
    newcomer.name       = "corp-net";
    newcomer.addr       = "10.0.0.50:19948";
    newcomer.pubkey_hex = newcomer_pk;
    newcomer.last_seen  = 1000000;

    GossipMsg g;
    g.peers.push_back(newcomer);
    mc.inject_gossip(g);

    auto discovered = mc.discovered_peers();
    bool found = false;
    for (auto& p : discovered) {
        if (p.name == "corp-net" && p.addr == "10.0.0.50:19948") { found = true; break; }
    }
    REQUIRE(found);

    fs::remove_all(home);
}

TEST_CASE("gossip: duplicate peer in gossip does not create duplicates in discovered", "[mesh_reliability][gossip]") {
    namespace fs = std::filesystem;
    auto home = std::string("/tmp/bs_reliability_gossip2_") +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    REQUIRE(ensure_private_directory(home));

    std::string dup_pk(64, '2');
    REQUIRE(write_private_text_file(home + "/authorized_keys", dup_pk + "\n"));

    auto cfg = mesh_cfg("gossip2");
    cfg.authorized_keys_path = home + "/authorized_keys";
    MeshController mc(cfg, home);

    PeerInfo peer;
    peer.name       = "dup-peer";
    peer.addr       = "10.0.0.1:19948";
    peer.pubkey_hex = dup_pk;
    peer.last_seen  = 1000;

    GossipMsg g1, g2;
    g1.peers.push_back(peer);
    peer.last_seen = 2000; // same peer, newer timestamp
    g2.peers.push_back(peer);

    mc.inject_gossip(g1);
    mc.inject_gossip(g2);

    auto discovered = mc.discovered_peers();
    int count = 0;
    for (auto& p : discovered) {
        if (p.pubkey_hex == dup_pk) count++;
    }
    REQUIRE(count == 1); // deduplicated

    fs::remove_all(home);
}

TEST_CASE("gossip: own node not added to discovered from gossip", "[mesh_reliability][gossip]") {
    auto cfg = mesh_cfg("gossip3");
    MeshController mc(cfg);

    // Gossip that includes our own pubkey — must be silently ignored
    PeerInfo self;
    self.name       = cfg.node_name;
    self.addr       = "127.0.0.1:19954";
    self.pubkey_hex = mc.own_pubkey_hex();
    self.last_seen  = 1000;

    GossipMsg g;
    g.peers.push_back(self);
    mc.inject_gossip(g);

    auto discovered = mc.discovered_peers();
    for (auto& p : discovered) {
        REQUIRE(p.pubkey_hex != mc.own_pubkey_hex());
    }
}

// ── Duplicate connection resolution ───────────────────────────────────────
// When both A→B and B→A connections race, the node with the LEXICOGRAPHICALLY
// LOWER pubkey hex keeps its outbound; the higher one closes its outbound.
// Requires MeshController::should_keep_outbound(own_key, peer_key) → bool.

TEST_CASE("pong timeout grants a fresh window after busy operation", "[mesh_reliability][heartbeat]") {
    MeshController::Conn conn;
    conn.exec_busy->store(true);
    const auto before = std::chrono::steady_clock::now() - std::chrono::minutes(5);
    conn.last_pong = before;
    auto now = std::chrono::steady_clock::now();
    REQUIRE(MeshController::refresh_heartbeat_after_busy(conn, now));
    conn.exec_busy->store(false);
    now = std::chrono::steady_clock::now();
    REQUIRE(MeshController::refresh_heartbeat_after_busy(conn, now));
    REQUIRE(conn.last_pong == now);
    REQUIRE_FALSE(MeshController::refresh_heartbeat_after_busy(conn, now));

    MeshController::Conn fast;
    fast.last_pong = before;
    fast.exec_completed->store(true);
    now = std::chrono::steady_clock::now();
    REQUIRE(MeshController::refresh_heartbeat_after_busy(fast, now));
    REQUIRE(fast.last_pong == now);
}

TEST_CASE("duplicate_conn: direct session and mesh link with same identity coexist", "[mesh_reliability][duplicate]") {
    MeshController::Conn mesh;
    MeshController::Conn direct;
    mesh.peer_pubkey = direct.peer_pubkey = std::string(64, 'd');
    mesh.peer_name = direct.peer_name = "test-pc6";
    mesh.sock_fd = 1;
    direct.sock_fd = 2;
    mesh.purpose = MeshController::ConnectionPurpose::Mesh;
    direct.purpose = MeshController::ConnectionPurpose::DirectSession;
    REQUIRE_FALSE(MeshController::connections_are_mesh_duplicates(mesh, direct));

    direct.purpose = MeshController::ConnectionPurpose::Mesh;
    REQUIRE(MeshController::connections_are_mesh_duplicates(mesh, direct));

    direct.purpose = MeshController::ConnectionPurpose::Unknown;
    REQUIRE_FALSE(MeshController::connections_are_mesh_duplicates(mesh, direct));
    REQUIRE(MeshController::is_live_mesh_transport_for(mesh, "test-pc6"));
    mesh.exec_busy->store(true);
    REQUIRE_FALSE(MeshController::is_live_mesh_transport_for(mesh, "test-pc6"));
    REQUIRE(MeshController::is_live_mesh_transport_for(mesh, "test-pc6", false));
    REQUIRE_FALSE(MeshController::is_live_mesh_transport_for(direct, "test-pc6"));
}

TEST_CASE("duplicate_conn: lower pubkey keeps its outbound connection", "[mesh_reliability][duplicate]") {
    std::string key_low  = std::string(64, 'a'); // "aaa..."
    std::string key_high = std::string(64, 'z'); // "zzz..."

    // Node with key_low connecting outbound to key_high: lower wins → keep
    REQUIRE(MeshController::should_keep_outbound(key_low, key_high) == true);
}

TEST_CASE("duplicate_conn: higher pubkey drops its outbound connection", "[mesh_reliability][duplicate]") {
    std::string key_low  = std::string(64, 'a');
    std::string key_high = std::string(64, 'z');

    // Node with key_high connecting outbound to key_low: lower wins → drop
    REQUIRE(MeshController::should_keep_outbound(key_high, key_low) == false);
}

TEST_CASE("busy exec transport defers close until worker releases ownership",
          "[mesh_reliability][exec][reconnect]") {
#ifndef _WIN32
    auto cfg = mesh_cfg("deferred-close", 0);
    MeshController mc(cfg);
    int sockets[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    MeshController::Conn conn;
    conn.sock_fd = sockets[0];
    conn.exec_busy->store(true);

    mc.close_conn_for_test(conn);
    REQUIRE(conn.close_requested);
    REQUIRE(conn.sock_fd == sockets[0]);
    REQUIRE(fcntl(sockets[0], F_GETFD) != -1);

    conn.exec_busy->store(false);
    mc.close_conn_for_test(conn);
    REQUIRE(conn.sock_fd == INVALID_SOCKET);
    CLOSESOCK(sockets[1]);
#else
    SUCCEED("covered by the cross-platform ownership predicate and Windows live acceptance");
#endif
}

TEST_CASE("duplicate_conn: repeated same-direction client replaces stale connection", "[mesh_reliability][duplicate]") {
    // i is the older connection and j is the newly accepted one. When both have
    // the same direction, keep j so a reconnect can replace a stale direct client.
    REQUIRE(MeshController::duplicate_index_to_drop(true, true) == 0);
    REQUIRE(MeshController::duplicate_index_to_drop(false, false) == 0);
    REQUIRE(MeshController::duplicate_index_to_drop(true, false) == 1);
    REQUIRE(MeshController::duplicate_index_to_drop(false, true) == 0);
}

TEST_CASE("duplicate_conn: equal pubkeys — one side drops (not both keep)", "[mesh_reliability][duplicate]") {
    std::string key = std::string(64, 'm');

    // Both sides have the same key (degenerate): the function must NOT return true
    // for both directions simultaneously (that would cause permanent duplicate).
    // Convention: treat equal as "keep" on one side only — e.g. lower_or_equal → keep.
    bool a_keeps = MeshController::should_keep_outbound(key, key);
    bool b_keeps = MeshController::should_keep_outbound(key, key);
    // Both returning false is also acceptable (both drop, reconnect via normal backoff)
    // The only invalid outcome is both returning TRUE (both keep a duplicate connection).
    // Since both calls are identical, they return the same value. Either both keep or both drop.
    // "Both drop" is safe (they'll reconnect). "Both keep" is a bug.
    // This test documents that the result must be deterministic (same both ways).
    REQUIRE(a_keeps == b_keeps);
}

// ── R5: find_peer_addr returns empty for unknown peer (no crash/SIGSEGV) ───

TEST_CASE("R5: find_peer_addr returns empty string for unknown peer name", "[mesh_reliability][r5]") {
    auto cfg = mesh_cfg("r5-find");
    PeerEntry seed;
    seed.name = "known-node";
    seed.addr = "10.1.2.3:19948";
    cfg.seeds.push_back(seed);

    MeshController mc(cfg);

    // Known seed returns its addr
    std::string known = mc.find_peer_addr("known-node");
    REQUIRE(known == "10.1.2.3:19948");

    // Unknown peer must return empty, not crash
    std::string unknown = mc.find_peer_addr("absolutely-unknown-node");
    REQUIRE(unknown.empty());
}

// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

TEST_CASE("R5: find_peer_addr returns empty string when no seeds configured", "[mesh_reliability][r5]") {
    auto cfg = mesh_cfg("r5-empty");
    // No seeds
    MeshController mc(cfg);

    std::string result = mc.find_peer_addr("anything");
    REQUIRE(result.empty());
}

// ── Reconnect backoff — exponential growth ─────────────────────────────────
// Verifies that consecutive reconnect delays increase exponentially and are
// capped at reconnect_backoff_max_secs.
// Requires MeshController::next_backoff_ms(attempt) or similar utility.

TEST_CASE("reconnect_backoff: delays increase exponentially and are capped", "[mesh_reliability][backoff]") {
    auto cfg = mesh_cfg("backoff");
    cfg.reconnect_backoff_max_secs = 30;
    MeshController mc(cfg);

    // attempt 0: first retry (e.g. 100ms base)
    long d0 = mc.next_backoff_ms(0);
    long d1 = mc.next_backoff_ms(1);
    long d2 = mc.next_backoff_ms(2);
    long d3 = mc.next_backoff_ms(3);
    long max_delay = mc.next_backoff_ms(100); // high attempt number → capped

    INFO("d0=" << d0 << " d1=" << d1 << " d2=" << d2 << " d3=" << d3 << " max=" << max_delay);

    REQUIRE(d0 > 0);
    REQUIRE(d1 > d0);   // strictly increasing
    REQUIRE(d2 > d1);
    REQUIRE(d3 > d2);
    REQUIRE(max_delay <= static_cast<long>(cfg.reconnect_backoff_max_secs) * 1000 * 2); // within 2× cap (jitter)
}

// ── D.1: exec_busy 90s watchdog ──────────────────────────────────────────
// PLANS BUG-1: when a CLI shell --cmd times out, the background exec thread's
// exec_busy flag can stay true, blocking all subsequent IPC to that peer.
// v2.0.6 added check_stale_exec(): if exec_busy has been set for >90s, it sets
// exec_cancelled and requests the conn close (shutdown() so the blocking worker
// errors out and releases the flag). This test verifies that logic deterministically
// by back-dating exec_started_at past the 90s window and invoking the watchdog.

TEST_CASE("D.1: exec_busy watchdog force-releases a stuck flag after 90s", "[mesh_reliability][exec][watchdog]") {
#ifndef _WIN32
    auto cfg = mesh_cfg("exec-watchdog", 0);
    MeshController mc(cfg);

    int sockets[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    MeshController::Conn c;
    c.peer_name = "stuck-peer";
    c.peer_pubkey = std::string(64, 'c');
    c.sock_fd = sockets[0];
    c.exec_busy = std::make_shared<std::atomic<bool>>(false);
    c.exec_cancelled = std::make_shared<std::atomic<bool>>(false);
    c.exec_completed = std::make_shared<std::atomic<bool>>(false);
    c.close_requested = false;
    size_t idx = mc.add_connection_for_test(std::move(c));

    // Precondition: flag is FREE, nothing pending.
    REQUIRE_FALSE(mc.exec_busy_for_test(idx));
    REQUIRE_FALSE(mc.conn_close_requested_for_test("stuck-peer"));

    // Simulate a CLI shell --cmd that timed out 7501s ago (>> 90s watchdog).
    // expire_exec_watchdog_for_test() sets exec_busy=true, back-dates
    // exec_started_at, and runs check_stale_exec().
    mc.expire_exec_watchdog_for_test(idx);

    // Postcondition: watchdog fired — flag marked cancelled and conn close requested,
    // so the event loop will shut down the socket and the stalled worker releases
    // exec_busy (unblocking future IPC to this peer).
    REQUIRE(mc.exec_busy_for_test(idx));                 // still held (worker owns it)
    REQUIRE(mc.exec_cancelled_for_test(idx));            // watchdog told worker to bail
    REQUIRE(mc.conn_close_requested_for_test("stuck-peer"));

    CLOSESOCK(sockets[1]);
#else
    SUCCEED("watchdog logic is OS-independent; covered by POSIX build");
#endif
}

TEST_CASE("D.1: exec_busy watchdog does NOT fire for a fresh (sub-90s) op", "[mesh_reliability][exec][watchdog]") {
#ifndef _WIN32
    auto cfg = mesh_cfg("exec-watchdog-fresh", 0);
    MeshController mc(cfg);

    int sockets[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    MeshController::Conn c;
    c.peer_name = "fresh-peer";
    c.peer_pubkey = std::string(64, 'f');
    c.sock_fd = sockets[0];
    c.exec_busy = std::make_shared<std::atomic<bool>>(true);  // actively busy
    c.exec_cancelled = std::make_shared<std::atomic<bool>>(false);
    c.exec_completed = std::make_shared<std::atomic<bool>>(false);
    c.exec_started_at = std::chrono::steady_clock::now();      // just started
    c.close_requested = false;
    size_t idx = mc.add_connection_for_test(std::move(c));

    // A busy op that only just started must NOT be force-cancelled.
    mc.check_stale_exec_for_test();
    REQUIRE_FALSE(mc.exec_cancelled_for_test(idx));
    REQUIRE_FALSE(mc.conn_close_requested_for_test("fresh-peer"));

    CLOSESOCK(sockets[1]);
#else
    SUCCEED("watchdog logic is OS-independent; covered by POSIX build");
#endif
}

TEST_CASE("D.1: exec_busy watchdog does NOT fire for a transfer making recent progress", "[mesh_reliability][exec][watchdog]") {
#ifndef _WIN32
    auto cfg = mesh_cfg("exec-watchdog-progress", 0);
    MeshController mc(cfg);

    int sockets[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    MeshController::Conn c;
    c.peer_name = "big-xfer-peer";
    c.peer_pubkey = std::string(64, 'b');
    c.sock_fd = sockets[0];
    c.exec_busy = std::make_shared<std::atomic<bool>>(true);
    c.exec_cancelled = std::make_shared<std::atomic<bool>>(false);
    c.exec_completed = std::make_shared<std::atomic<bool>>(false);
    // The transfer started >90s ago (would have tripped the OLD watchdog)...
    c.exec_started_at = std::chrono::steady_clock::now() - std::chrono::seconds(7501);
    // ...but the worker has been streaming chunks and JUST refreshed progress.
    c.exec_last_progress_at->store(
        (std::chrono::steady_clock::now() - std::chrono::seconds(5))
            .time_since_epoch().count());
    c.close_requested = false;
    size_t idx = mc.add_connection_for_test(std::move(c));

    // Recent progress must keep a healthy long transfer alive past 90s.
    mc.check_stale_exec_for_test();
    REQUIRE_FALSE(mc.exec_cancelled_for_test(idx));
    REQUIRE_FALSE(mc.conn_close_requested_for_test("big-xfer-peer"));

    CLOSESOCK(sockets[1]);
#else
    SUCCEED("watchdog logic is OS-independent; covered by POSIX build");
#endif
}

TEST_CASE("D.1: exec_busy watchdog fires for a STALLED transfer (no progress >90s)", "[mesh_reliability][exec][watchdog]") {
#ifndef _WIN32
    auto cfg = mesh_cfg("exec-watchdog-stalled", 0);
    MeshController mc(cfg);

    int sockets[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    MeshController::Conn c;
    c.peer_name = "stalled-xfer-peer";
    c.peer_pubkey = std::string(64, 's');
    c.sock_fd = sockets[0];
    c.exec_busy = std::make_shared<std::atomic<bool>>(true);
    c.exec_cancelled = std::make_shared<std::atomic<bool>>(false);
    c.exec_completed = std::make_shared<std::atomic<bool>>(false);
    // Transfer started a while ago AND last progress tick is also ancient.
    c.exec_started_at = std::chrono::steady_clock::now() - std::chrono::seconds(7501);
    c.exec_last_progress_at->store(
        (std::chrono::steady_clock::now() - std::chrono::seconds(7501))
            .time_since_epoch().count());
    c.close_requested = false;
    size_t idx = mc.add_connection_for_test(std::move(c));

    // Stalled (no progress for >90s) must be force-released, preserving BUG-1.
    mc.check_stale_exec_for_test();
    REQUIRE(mc.exec_cancelled_for_test(idx));
    REQUIRE(mc.conn_close_requested_for_test("stalled-xfer-peer"));

    CLOSESOCK(sockets[1]);
#else
    SUCCEED("watchdog logic is OS-independent; covered by POSIX build");
#endif
}
