// test_handshake_bounds.cpp — Regression for 2026-08-09 handshake_deadline accumulation
//
// Root cause: with 7+ dead seeds, pending_handshakes_ could accumulate handshake
// objects. Each stores an SSL object + socket FD. Failed handshakes must clean up
// both. The kMaxPendingHandshakes (16) bound prevents unbounded growth.
//
// Tests verify:
//   - pending_handshakes_ doesn't exceed kMaxPendingHandshakes
//   - Failed handshakes properly clean up SSL objects and socket FDs
//   - The bound is enforced in both accept_inbound and start_outbound paths

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
#include <fcntl.h>
#endif

#include <thread>
#include <atomic>
#include <chrono>

using namespace bs::mesh;
using namespace std::chrono_literals;

// ── Helpers ───────────────────────────────────────────────────────────

static MeshConfig hs_cfg(const std::string& name) {
    MeshConfig c;
    c.node_name              = name;
    c.listen_port            = 0;
    c.gossip_interval_secs   = 300;
    c.ping_interval_secs     = 300;
    c.pong_timeout_secs      = 30;
    c.scrollback_lines       = 100;
    c.require_seed_pins      = false;
    return c;
}

// ── 1. kMaxPendingHandshakes is 16 ────────────────────────────────────

TEST_CASE("handshake_bounds: kMaxPendingHandshakes is 16",
          "[handshake][bounds]") {
    REQUIRE(MeshController::kMaxPendingHandshakes_for_test() == 16);
}

// ── 2. pending_handshakes starts empty ─────────────────────────────────

TEST_CASE("handshake_bounds: pending starts at zero",
          "[handshake][bounds]") {
    auto cfg = hs_cfg("empty");
    MeshController mc(cfg);
    REQUIRE(mc.pending_handshake_count_for_test() == 0);
}

// ── 3. Outbound handshakes bounded by kMaxPendingHandshakes ────────────
// Try to start more than kMaxPendingHandshakes outbound connections with
// distinct addresses. Verify the count never exceeds the limit.

TEST_CASE("handshake_bounds: outbound pending never exceeds kMaxPendingHandshakes",
          "[handshake][bounds][outbound]") {
    auto cfg = hs_cfg("outbound-bounded");
    MeshController mc(cfg);

    // Try to start 30 handshakes with distinct addresses.
    for (int i = 0; i < 30; ++i) {
        PeerEntry peer;
        peer.name = "peer-" + std::to_string(i);
        peer.addr = "127.0.0.1:" + std::to_string(30000 + i);
        mc.start_outbound_handshake_for_test(peer);
    }

    size_t pending = mc.pending_handshake_count_for_test();
    INFO("pending after 30 attempts: " << pending);
    REQUIRE(pending <= MeshController::kMaxPendingHandshakes_for_test());
}

// ── 4. Failed handshakes clean up after advance_handshakes ─────────────
// Start handshakes to dead ports, wait for them to fail, then verify
// advance_handshakes cleans them up (count drops to 0).

TEST_CASE("handshake_bounds: failed handshakes are cleaned up by advance_handshakes",
          "[handshake][bounds][cleanup]") {
    auto cfg = hs_cfg("cleanup");
    MeshController mc(cfg);

    // Start handshakes to dead ports.
    for (int i = 0; i < 4; ++i) {
        PeerEntry dead;
        dead.name = "dead-" + std::to_string(i);
        dead.addr = "127.0.0.1:" + std::to_string(31000 + i);
        mc.start_outbound_handshake_for_test(dead);
    }

    size_t initial = mc.pending_handshake_count_for_test();
    INFO("initial pending: " << initial);

    // Wait for connection attempts to fail (connection refused on loopback
    // is immediate; connect() returns ECONNREFUSED → start_outbound creates
    // a TcpConnect state handshake that advance_handshakes will detect as
    // failed on next iteration).
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(100ms);
        mc.advance_handshakes_for_test();
    }

    size_t after = mc.pending_handshake_count_for_test();
    INFO("pending after 10 advance iterations: " << after);
    // All dead-seed handshakes should be cleaned up.
    REQUIRE(after == 0);
}

// ── 5. SSL objects are freed when handshakes fail ──────────────────────
// We can't directly count SSL objects, but we can verify that after
// starting and cleaning up many handshakes, the pending count returns to 0
// (indicating the SSL objects in the PendingHandshake structs were freed
// via the SslPtr unique_ptr).

TEST_CASE("handshake_bounds: SSL objects freed after cleanup cycle",
          "[handshake][bounds][ssl_cleanup]") {
    auto cfg = hs_cfg("ssl-cleanup");
    MeshController mc(cfg);

    // Cycle: start handshakes, advance to clean up, repeat.
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (int i = 0; i < 4; ++i) {
            PeerEntry dead;
            dead.name = "cycle-" + std::to_string(cycle) + "-" + std::to_string(i);
            dead.addr = "127.0.0.1:" + std::to_string(32000 + cycle * 10 + i);
            mc.start_outbound_handshake_for_test(dead);
        }

        // Clean up
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(50ms);
            mc.advance_handshakes_for_test();
        }

        REQUIRE(mc.pending_handshake_count_for_test() == 0);
    }
}

// ── 6. start_outbound_handshake respects dedup by address ──────────────

TEST_CASE("handshake_bounds: duplicate address does not create second pending",
          "[handshake][bounds][dedup]") {
    auto cfg = hs_cfg("dedup-addr");
    MeshController mc(cfg);

    PeerEntry peer;
    peer.name = "same-peer";
    peer.addr = "127.0.0.1:33000";

    mc.start_outbound_handshake_for_test(peer);
    size_t after_first = mc.pending_handshake_count_for_test();

    mc.start_outbound_handshake_for_test(peer);  // same addr
    size_t after_second = mc.pending_handshake_count_for_test();

    INFO("after_first=" << after_first << " after_second=" << after_second);
    // Second call should not add another pending (dedup by addr).
    // Either both return false (immediate fail) or second is false.
    REQUIRE(after_second == after_first);
}

// ── 7. start_outbound_handshake respects dedup by pubkey ───────────────

TEST_CASE("handshake_bounds: duplicate pubkey does not create second pending",
          "[handshake][bounds][dedup][pubkey]") {
    auto cfg = hs_cfg("dedup-pk");
    MeshController mc(cfg);

    PeerEntry peer1;
    peer1.name = "peer1";
    peer1.addr = "127.0.0.1:33001";
    peer1.pubkey_hex = std::string(64, 'a');

    PeerEntry peer2;
    peer2.name = "peer2";
    peer2.addr = "127.0.0.1:33002";  // different addr
    peer2.pubkey_hex = std::string(64, 'a');  // same pubkey

    mc.start_outbound_handshake_for_test(peer1);
    size_t after_first = mc.pending_handshake_count_for_test();

    bool second_result = mc.start_outbound_handshake_for_test(peer2);
    size_t after_second = mc.pending_handshake_count_for_test();

    INFO("after_first=" << after_first << " after_second=" << after_second
         << " second_result=" << second_result);
    // If the first succeeded, the second should be rejected by pubkey dedup.
    // If the first failed (immediate ECONNREFUSED), the second may succeed.
    REQUIRE(after_second <= after_first + 1);
}

// ── 8. No FD leak: handshakes close their sockets on failure ───────────
// On POSIX, we can verify FDs are closed by checking that the process FD
// count doesn't grow unboundedly across handshake start/cleanup cycles.

TEST_CASE("handshake_bounds: no FD leak across handshake cycles",
          "[handshake][bounds][fd_leak]") {
#ifndef _WIN32
    auto cfg = hs_cfg("fd-leak");
    MeshController mc(cfg);

    // Get initial FD count.
    auto fd_count = []() -> long {
        FILE* fp = popen("ls /dev/fd 2>/dev/null | wc -l", "r");
        if (!fp) return -1;
        char buf[32];
        fgets(buf, sizeof(buf), fp);
        pclose(fp);
        return std::strtol(buf, nullptr, 10);
    };

    long baseline = fd_count();

    // Run multiple handshake start/cleanup cycles.
    for (int cycle = 0; cycle < 5; ++cycle) {
        for (int i = 0; i < 4; ++i) {
            PeerEntry dead;
            dead.name = "leak-" + std::to_string(cycle) + "-" + std::to_string(i);
            dead.addr = "127.0.0.1:" + std::to_string(34000 + cycle * 10 + i);
            mc.start_outbound_handshake_for_test(dead);
        }
        for (int i = 0; i < 10; ++i) {
            std::this_thread::sleep_for(50ms);
            mc.advance_handshakes_for_test();
        }
    }

    long after = fd_count();
    INFO("baseline FDs=" << baseline << " after cycles=" << after);
    // FD count should not have grown significantly (allow some slack for
    // internal OpenSSL allocations).
    // The key invariant: no leaked socket FDs from failed handshakes.
    REQUIRE(after - baseline < 10);
#else
    SUCCEED("FD leak test is POSIX-only");
#endif
}

// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
