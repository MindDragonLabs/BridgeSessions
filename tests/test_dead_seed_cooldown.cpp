// test_dead_seed_cooldown.cpp — Regression tests for B2 (dead-seed cooldown)
//
// Covers:
//   - 3 consecutive handshake_deadline failures engage a cooldown window
//   - fewer than 3 consecutive failures do NOT engage cooldown
//   - an active cooldown skips scheduling (try_connect_to_seeds never calls
//     start_outbound_handshake for the cooled-down addr)
//   - success (record_dead_seed_success) resets the streak and clears cooldown
//   - once the cooldown window elapses, exactly one probe is allowed through
//   - seed_dial_health() reports ok / backoff / cooldown(dead Nm) correctly

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

static MeshConfig cooldown_cfg(const std::string& name) {
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

// ── 1. Fewer than threshold failures do not engage cooldown ────────────

TEST_CASE("dead_seed_cooldown: 1-2 consecutive failures do not engage cooldown",
          "[dead_seed][cooldown]") {
    auto cfg = cooldown_cfg("cooldown-under-threshold");
    MeshController mc(cfg);
    const std::string addr = "127.0.0.1:1";

    REQUIRE_FALSE(mc.dead_seed_in_cooldown_for_test(addr));
    mc.record_dead_seed_failure_for_test(addr);
    REQUIRE_FALSE(mc.dead_seed_in_cooldown_for_test(addr));
    mc.record_dead_seed_failure_for_test(addr);
    REQUIRE_FALSE(mc.dead_seed_in_cooldown_for_test(addr));
    REQUIRE(mc.dead_seed_failure_streak_for_test(addr) == 2);
}

// ── 2. Exactly 3 consecutive failures engage cooldown ───────────────────

TEST_CASE("dead_seed_cooldown: 3rd consecutive failure engages cooldown",
          "[dead_seed][cooldown]") {
    auto cfg = cooldown_cfg("cooldown-threshold");
    MeshController mc(cfg);
    const std::string addr = "127.0.0.1:1";

    mc.record_dead_seed_failure_for_test(addr);
    mc.record_dead_seed_failure_for_test(addr);
    mc.record_dead_seed_failure_for_test(addr);

    REQUIRE(mc.dead_seed_in_cooldown_for_test(addr));
    REQUIRE(mc.dead_seed_failure_streak_for_test(addr) == 3);
    REQUIRE(mc.seed_dial_health_for_test(addr).rfind("cooldown(", 0) == 0);
}

// ── 3. Active cooldown skips scheduling in try_connect_to_seeds ────────

TEST_CASE("dead_seed_cooldown: active cooldown skips dialing in try_connect_to_seeds",
          "[dead_seed][cooldown][scheduling]") {
    auto cfg = cooldown_cfg("cooldown-skip-dial");
    PeerEntry seed;
    seed.name = "dead-seed";
    seed.addr = "127.0.0.1:1";
    cfg.seeds.push_back(seed);
    MeshController mc(cfg);

    mc.record_dead_seed_failure_for_test(seed.addr);
    mc.record_dead_seed_failure_for_test(seed.addr);
    mc.record_dead_seed_failure_for_test(seed.addr);
    REQUIRE(mc.dead_seed_in_cooldown_for_test(seed.addr));

    mc.try_connect_to_seeds_for_test();

    // The gate must have skipped this addr entirely — no pending handshake,
    // regardless of what a real connect() to the dead port would have done.
    REQUIRE_FALSE(mc.has_pending_handshake_for_addr_for_test(seed.addr));
}

// ── 4. Success resets the streak and clears cooldown ───────────────────

TEST_CASE("dead_seed_cooldown: success resets streak and clears cooldown",
          "[dead_seed][cooldown][reset]") {
    auto cfg = cooldown_cfg("cooldown-reset");
    MeshController mc(cfg);
    const std::string addr = "127.0.0.1:1";

    mc.record_dead_seed_failure_for_test(addr);
    mc.record_dead_seed_failure_for_test(addr);
    mc.record_dead_seed_failure_for_test(addr);
    REQUIRE(mc.dead_seed_in_cooldown_for_test(addr));

    mc.record_dead_seed_success_for_test(addr);

    REQUIRE_FALSE(mc.dead_seed_in_cooldown_for_test(addr));
    REQUIRE(mc.dead_seed_failure_streak_for_test(addr) == 0);
    REQUIRE(mc.seed_dial_health_for_test(addr) == "ok");
}

// ── 5. After the window elapses, one probe is allowed through ──────────

TEST_CASE("dead_seed_cooldown: expired cooldown allows exactly one probe",
          "[dead_seed][cooldown][probe]") {
    auto cfg = cooldown_cfg("cooldown-probe");
    PeerEntry seed;
    seed.name = "dead-seed";
    seed.addr = "10.255.255.1:65500"; // non-local: connect() won't fail synchronously
    cfg.seeds.push_back(seed);
    MeshController mc(cfg);

    mc.record_dead_seed_failure_for_test(seed.addr);
    mc.record_dead_seed_failure_for_test(seed.addr);
    mc.record_dead_seed_failure_for_test(seed.addr);
    REQUIRE(mc.dead_seed_in_cooldown_for_test(seed.addr));

    // Simulate the 10-minute window elapsing.
    mc.expire_dead_seed_cooldown_for_test(seed.addr);
    REQUIRE_FALSE(mc.dead_seed_in_cooldown_for_test(seed.addr));

    mc.try_connect_to_seeds_for_test();

    // The one allowed probe should have started a dial (non-blocking connect
    // to a non-local, non-refusing address stays pending).
    REQUIRE(mc.has_pending_handshake_for_addr_for_test(seed.addr));

    // The streak survives the probe (only cleared by explicit success) — a
    // renewed handshake_deadline failure would immediately re-cooldown it.
    REQUIRE(mc.dead_seed_failure_streak_for_test(seed.addr) == 3);
}

// ── 6. seed_dial_health reports "ok" with no failure history ───────────

TEST_CASE("dead_seed_cooldown: seed_dial_health is ok with no history",
          "[dead_seed][cooldown][health]") {
    auto cfg = cooldown_cfg("cooldown-health-ok");
    MeshController mc(cfg);
    REQUIRE(mc.seed_dial_health_for_test("127.0.0.1:1") == "ok");
}

// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
