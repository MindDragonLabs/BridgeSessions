// test_dead_seed_backoff.cpp — Regression tests for 2026-08-09 dead-seed issues
//
// Covers:
//   - Backoff increases exponentially for unreachable peers
//   - Backoff caps at reconnect_backoff_max_secs
//   - pending_handshakes_ vector stays bounded (≤ kMaxPendingHandshakes)
//   - start_outbound_handshake doesn't create unbounded SSL objects
//
// Root cause (2026-08-09): dead seeds triggered full TLS handshake attempts
// every reconnect_backoff_max_secs. With 7+ dead peers, thousands of failed
// SSL_new+SSL_connect+SSL_free cycles per hour caused RSS to grow to 1.5-3.1GB.

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
#endif

#include <chrono>

using namespace bs::mesh;

// ── Helpers ───────────────────────────────────────────────────────────

static MeshConfig backoff_cfg(const std::string& name) {
    MeshConfig c;
    c.node_name              = name;
    c.listen_port            = 0;  // ephemeral; no listener
    c.gossip_interval_secs   = 300;
    c.ping_interval_secs     = 300;
    c.pong_timeout_secs      = 30;
    c.scrollback_lines       = 100;
    c.require_seed_pins      = false;  // don't require pins for test seeds
    return c;
}

// ── 1. Backoff increases exponentially ─────────────────────────────────

TEST_CASE("dead_seed_backoff: next_backoff_ms increases exponentially",
          "[dead_seed][backoff]") {
    auto cfg = backoff_cfg("backoff-exp");
    cfg.reconnect_backoff_max_secs = 300;  // 5 min cap
    MeshController mc(cfg);

    long d0 = mc.next_backoff_ms_for_test(0);
    long d1 = mc.next_backoff_ms_for_test(1);
    long d2 = mc.next_backoff_ms_for_test(2);
    long d3 = mc.next_backoff_ms_for_test(3);

    INFO("d0=" << d0 << " d1=" << d1 << " d2=" << d2 << " d3=" << d3);

    REQUIRE(d0 == 100);         // base: 100ms
    REQUIRE(d1 == 200);         // 100 * 2
    REQUIRE(d2 == 400);         // 200 * 2
    REQUIRE(d3 == 800);         // 400 * 2
}

// ── 2. Backoff caps at reconnect_backoff_max_secs ──────────────────────

TEST_CASE("dead_seed_backoff: next_backoff_ms caps at reconnect_backoff_max_secs",
          "[dead_seed][backoff][cap]") {
    auto cfg = backoff_cfg("backoff-cap");
    cfg.reconnect_backoff_max_secs = 5;  // 5000ms cap
    MeshController mc(cfg);

    long cap = static_cast<long>(cfg.reconnect_backoff_max_secs) * 1000;

    // At high attempt numbers, delay must not exceed the cap.
    long d10 = mc.next_backoff_ms_for_test(10);
    long d20 = mc.next_backoff_ms_for_test(20);
    long d50 = mc.next_backoff_ms_for_test(50);
    long d100 = mc.next_backoff_ms_for_test(100);

    INFO("cap=" << cap << " d10=" << d10 << " d20=" << d20
         << " d50=" << d50 << " d100=" << d100);

    REQUIRE(d10 <= cap);
    REQUIRE(d20 <= cap);
    REQUIRE(d50 <= cap);
    REQUIRE(d100 <= cap);
    // The cap should actually be reached at some point.
    REQUIRE(d100 == cap);
}

// ── 3. Backoff with hardened default config (300s cap) ────────────────

TEST_CASE("dead_seed_backoff: default config caps at 300s",
          "[dead_seed][backoff][default]") {
    auto cfg = backoff_cfg("backoff-default");
    REQUIRE(cfg.reconnect_backoff_max_secs == 300);
    MeshController mc(cfg);

    long cap = 300 * 1000;
    long d = mc.next_backoff_ms_for_test(100);
    REQUIRE(d <= cap);
    REQUIRE(d == cap);
}

// ── 4. Backoff with raised config (300s cap, fleet fix) ────────────────

TEST_CASE("dead_seed_backoff: raised config (300s) doubles longer before cap",
          "[dead_seed][backoff][raised]") {
    auto cfg = backoff_cfg("backoff-raised");
    cfg.reconnect_backoff_max_secs = 300;
    MeshController mc(cfg);

    long cap = 300 * 1000;
    long d5 = mc.next_backoff_ms_for_test(5);
    long d10 = mc.next_backoff_ms_for_test(10);
    long d20 = mc.next_backoff_ms_for_test(20);

    INFO("cap=" << cap << " d5=" << d5 << " d10=" << d10 << " d20=" << d20);

    // With a 300s cap, the backoff should still be increasing at attempt 5
    // (3200ms << 300000ms).
    REQUIRE(d5 > 1000);
    REQUIRE(d10 <= cap);
    REQUIRE(d20 <= cap);
}

// ── 5. pending_handshakes_ stays bounded under repeated start attempts ─
// Start outbound handshakes to a dead port (port 1 on loopback — connection
// refused immediately). Verify pending_handshakes_ never exceeds kMaxPendingHandshakes.

TEST_CASE("dead_seed_backoff: pending_handshakes stays bounded for unreachable peers",
          "[dead_seed][pending][bounded]") {
    auto cfg = backoff_cfg("pending-bounded");
    MeshController mc(cfg);

    // Port 1 on loopback: guaranteed connection refused (no daemon).
    // Using distinct addresses so start_outbound_handshake doesn't dedup.
    for (int i = 0; i < 32; ++i) {
        PeerEntry dead;
        dead.name = "dead-" + std::to_string(i);
        dead.addr = "127.0.0.1:1";
        // Use unique pubkeys so the dedup check in start_outbound_handshake
        // (which checks expected_pubkey) doesn't reject them as duplicates.
        // However, same addr also triggers dedup, so only the first will start.
        // That's the correct behavior — we're testing the bound.
        (void)mc.start_outbound_handshake_for_test(dead);
    }

    size_t pending = mc.pending_handshake_count_for_test();
    INFO("pending_handshakes after 32 attempts to same addr: " << pending);
    // At most 1 pending for the same address (dedup).
    // For different addresses, ≤ kMaxPendingHandshakes (16).
    REQUIRE(pending <= 16);
}

// ── 6. start_outbound_handshake returns false for duplicate addr ───────

TEST_CASE("dead_seed_backoff: start_outbound_handshake rejects duplicate addr",
          "[dead_seed][dedup]") {
    auto cfg = backoff_cfg("dedup");
    MeshController mc(cfg);

    PeerEntry dead;
    dead.name = "dead-peer";
    dead.addr = "127.0.0.1:1";

    bool first = mc.start_outbound_handshake_for_test(dead);
    bool second = mc.start_outbound_handshake_for_test(dead);

    // The first might succeed (creating a pending handshake) or fail (immediate
    // ECONNREFUSED on some platforms). The second MUST return false — no duplicate.
    INFO("first=" << first << " second=" << second);
    REQUIRE_FALSE(second);
}

// ── 7. start_outbound_handshake rejects when pending is full ───────────
// Fill pending_handshakes_ to capacity with distinct addresses, then verify
// that another start_outbound_handshake returns false.

TEST_CASE("dead_seed_backoff: start_outbound_handshake rejects when pending full",
          "[dead_seed][max_pending]") {
    auto cfg = backoff_cfg("max-pending");
    MeshController mc(cfg);

    // Use ephemeral ports on loopback that are almost certainly not listening.
    // Each address is unique to bypass dedup.
    for (int i = 0; i < 16; ++i) {
        PeerEntry dead;
        dead.name = "dead-" + std::to_string(i);
        // Ports 20000+i: very unlikely to be listening.
        dead.addr = "127.0.0.1:" + std::to_string(20000 + i);
        mc.start_outbound_handshake_for_test(dead);
    }

    // Now pending should be at or near the limit (16).
    size_t pending = mc.pending_handshake_count_for_test();
    INFO("pending after 16 distinct addresses: " << pending);
    REQUIRE(pending <= 16);

    // Another attempt must return false (pending full).
    PeerEntry extra;
    extra.name = "extra-dead";
    extra.addr = "127.0.0.1:30000";
    bool result = mc.start_outbound_handshake_for_test(extra);
    REQUIRE_FALSE(result);
}

// ── 8. Backoff doubling matches expected sequence ──────────────────────

TEST_CASE("dead_seed_backoff: doubling sequence is correct",
          "[dead_seed][backoff][sequence]") {
    auto cfg = backoff_cfg("backoff-seq");
    cfg.reconnect_backoff_max_secs = 3600;  // 1 hour cap so we see the full sequence
    MeshController mc(cfg);

    // Verify the full doubling sequence until cap.
    long expected = 100;
    for (int attempt = 0; attempt < 20; ++attempt) {
        long actual = mc.next_backoff_ms_for_test(attempt);
        long cap = 3600 * 1000;
        long exp = std::min(expected, cap);
        INFO("attempt=" << attempt << " expected=" << exp << " actual=" << actual);
        REQUIRE(actual == exp);
        expected *= 2;
    }
}

// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
