// test_tiebreak_collision.cpp — Regression tests for the simultaneous-dial
// collision fix (2026-08-21).
//
// Failure mode observed in production (peer-a ↔ peer-b):
//   1. Two peers restart and dial each other simultaneously.
//   2. The deterministic duplicate rule keeps the connection opened by the
//      smaller pubkey; the loser TCP is torn down — its outbound dial
//      eventually hits handshake_deadline.
//   3. OLD behavior: that deadline fed the B2 dead-seed streak on BOTH sides,
//      and the larger endpoint's 12s tie-break defer expired long before the
//      smaller side's exponential backoff (up to 300s) — so they collided
//      again, both streaks climbed, and both edges parked in cooldown
//      forever while direct health checks passed.
//
// Fixes under test:
//   F1. should_defer_outbound_for extends the accept-only window
//       exponentially (12s·2^n, n ≤ 5) instead of dialing into a probable
//       collision; after the budget is spent, exactly one probe is allowed.
//   F2. clear_accept_only_for also clears the extension budget.
//   F3. A handshake_deadline taken while inside the tie-break window does NOT
//       count toward the dead-seed streak (collision ≠ dead peer).

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

using namespace bs::mesh;

// ── Helpers ───────────────────────────────────────────────────────────

static MeshConfig tiebreak_cfg(const std::string& name) {
    MeshConfig c;
    c.node_name              = name;
    c.listen_port            = 0;  // ephemeral; no listener
    c.gossip_interval_secs   = 300;
    c.ping_interval_secs     = 300;
    c.pong_timeout_secs      = 30;
    c.scrollback_lines       = 100;
    c.require_seed_pins      = false;
    return c;
}

// Larger-pubkey endpoint (should_accept_only_for == true): a peer whose
// pubkey is lexicographically smaller than ours.
static PeerEntry smaller_peer() {
    PeerEntry p;
    p.name = "smaller-peer";
    p.addr = "127.0.0.1:1"; // unreachable; scheduling is what we assert on
    p.pubkey_hex =
        "0000000000000000000000000000000000000000000000000000000000000000";
    return p;
}

static constexpr const char* kLargePk =
    "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
static constexpr const char* kSmallPk =
    "0000000000000000000000000000000000000000000000000000000000000000";

// ── F1: exponential extension instead of collision dial ────────────────

TEST_CASE("tiebreak: expired defer extends exponentially instead of dialing",
          "[tiebreak][collision]") {
    auto cfg = tiebreak_cfg("larger-endpoint");
    MeshController mc(cfg);

    // Our pubkey must be known for the pubkey-based tie-break rule.
    mc.set_our_pubkey_for_test(kLargePk);
    auto peer = smaller_peer();

    // First pass: initial 12s defer is created.
    REQUIRE(mc.should_defer_outbound_for_test(peer));
    REQUIRE(mc.tiebreak_extension_count_for_test(peer.addr) == 0);

    // Window expires → extension 1 (defer continues, no dial).
    mc.expire_tiebreak_window_for_test(peer.addr);
    REQUIRE(mc.should_defer_outbound_for_test(peer));
    REQUIRE(mc.tiebreak_extension_count_for_test(peer.addr) == 1);

    // Window expires again → extension 2.
    mc.expire_tiebreak_window_for_test(peer.addr);
    REQUIRE(mc.should_defer_outbound_for_test(peer));
    REQUIRE(mc.tiebreak_extension_count_for_test(peer.addr) == 2);
}

TEST_CASE("tiebreak: budget exhausted allows exactly one probe then re-defers",
          "[tiebreak][collision]") {
    auto cfg = tiebreak_cfg("larger-endpoint");
    MeshController mc(cfg);
    mc.set_our_pubkey_for_test(kLargePk);
    auto peer = smaller_peer();

    mc.should_defer_outbound_for_test(peer);
    // Burn the full extension budget (5 extends).
    for (int i = 0; i < 5; ++i) {
        mc.expire_tiebreak_window_for_test(peer.addr);
        REQUIRE(mc.should_defer_outbound_for_test(peer));
    }
    REQUIRE(mc.tiebreak_extension_count_for_test(peer.addr) == 5);

    // 6th expiry: budget spent → probe allowed (returns false = dial).
    mc.expire_tiebreak_window_for_test(peer.addr);
    REQUIRE_FALSE(mc.should_defer_outbound_for_test(peer));
    // Budget was reset for the next cycle.
    REQUIRE(mc.tiebreak_extension_count_for_test(peer.addr) == 0);
}

TEST_CASE("tiebreak: smaller-pubkey endpoint never defers",
          "[tiebreak][collision]") {
    auto cfg = tiebreak_cfg("smaller-endpoint");
    MeshController mc(cfg);
    mc.set_our_pubkey_for_test(kSmallPk); // we are the SMALLER side
    PeerEntry p;
    p.name = "larger-peer";
    p.addr = "127.0.0.2:2";
    p.pubkey_hex = kLargePk;

    REQUIRE_FALSE(mc.should_defer_outbound_for_test(p));
    REQUIRE(mc.tiebreak_extension_count_for_test(p.addr) == 0);
}

// ── F2: successful inbound clears the extension budget ─────────────────

TEST_CASE("tiebreak: clear_accept_only_for resets extension budget",
          "[tiebreak][collision]") {
    auto cfg = tiebreak_cfg("larger-endpoint");
    MeshController mc(cfg);
    mc.set_our_pubkey_for_test(kLargePk);
    auto peer = smaller_peer();

    mc.should_defer_outbound_for_test(peer);
    mc.expire_tiebreak_window_for_test(peer.addr);
    mc.should_defer_outbound_for_test(peer);
    REQUIRE(mc.tiebreak_extension_active_for_test(peer.addr));

    // Inbound handshake promoted → all defer state for this peer cleared.
    mc.clear_accept_only_for_test(peer.name, peer.addr, peer.pubkey_hex);
    REQUIRE_FALSE(mc.tiebreak_extension_active_for_test(peer.addr));
    REQUIRE(mc.tiebreak_extension_count_for_test(peer.addr) == 0);
    // And the next scheduling pass starts a fresh (initial) defer, not an extension.
    REQUIRE(mc.should_defer_outbound_for_test(peer));
    REQUIRE(mc.tiebreak_extension_count_for_test(peer.addr) == 0);
}

// ── F3: collision deadline does not feed the dead-seed streak ──────────

TEST_CASE("tiebreak: deadline inside defer window is not a dead-seed failure",
          "[tiebreak][collision][dead_seed]") {
    auto cfg = tiebreak_cfg("larger-endpoint");
    MeshController mc(cfg);
    mc.set_our_pubkey_for_test(kLargePk);
    auto peer = smaller_peer();
    const std::string other = "127.0.0.9:9";

    mc.should_defer_outbound_for_test(peer);
    // Deadline lands while the tie-break defer window is active → excused.
    REQUIRE_FALSE(mc.handshake_deadline_counts_as_dead_seed_for_test(peer.addr));

    // An addr with no defer window (normal dead seed) still counts.
    REQUIRE(mc.handshake_deadline_counts_as_dead_seed_for_test(other));
    // And the accounting matches the real B2 streak when applied:
    mc.record_dead_seed_failure_for_test(other);
    mc.record_dead_seed_failure_for_test(other);
    mc.record_dead_seed_failure_for_test(other);
    REQUIRE(mc.dead_seed_in_cooldown_for_test(other));
}

// ── Test entry ─────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    return Catch::Session().run(argc, argv);
}
