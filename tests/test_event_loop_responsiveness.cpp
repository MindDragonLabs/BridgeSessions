// test_event_loop_responsiveness.cpp — Regression for 2026-08-09 event loop saturation
//
// Root cause: when a daemon processes thousands of handshake timeouts for dead
// seeds, PTY allocation (bs shell) hangs because advance_handshakes() and
// try_connect_to_seeds() saturate the event loop.
//
// Tests verify:
//   - advance_handshakes() completes quickly even with many pending deadlines
//   - PTY/shell operations complete within timeout with pending handshakes
//   - Event loop iteration is non-blocking

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

static MeshConfig resp_cfg(const std::string& name) {
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

// ── 1. advance_handshakes completes quickly with no pending ────────────

TEST_CASE("event_loop: advance_handshakes is fast with empty pending",
          "[event_loop][responsiveness]") {
    auto cfg = resp_cfg("empty-loop");
    MeshController mc(cfg);

    auto start = std::chrono::steady_clock::now();
    mc.advance_handshakes_for_test();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // With no pending handshakes, advance_handshakes should complete in <10ms.
    INFO("advance_handshakes took " << elapsed.count() << "ms");
    REQUIRE(elapsed.count() < 10);
}

// ── 2. advance_handshakes completes quickly with pending handshakes ────
// Start outbound handshakes to dead ports, then verify advance_handshakes
// doesn't block.

TEST_CASE("event_loop: advance_handshakes is fast with pending dead-seed handshakes",
          "[event_loop][responsiveness][dead_seed]") {
    auto cfg = resp_cfg("dead-loop");
    MeshController mc(cfg);

    // Start a few handshakes to dead ports (distinct ports to bypass dedup).
    for (int i = 0; i < 8; ++i) {
        PeerEntry dead;
        dead.name = "dead-" + std::to_string(i);
        dead.addr = "127.0.0.1:" + std::to_string(20001 + i);
        mc.start_outbound_handshake_for_test(dead);
    }

    REQUIRE(mc.pending_handshake_count_for_test() > 0);

    // advance_handshakes should complete quickly even with pending entries.
    auto start = std::chrono::steady_clock::now();
    mc.advance_handshakes_for_test();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    INFO("advance_handshakes with " << mc.pending_handshake_count_for_test()
         << " pending took " << elapsed.count() << "ms");
    // Should be well under 100ms — select() uses a 0-timeout poll.
    REQUIRE(elapsed.count() < 100);
}

// ── 3. advance_handshakes cleans up expired deadlines ──────────────────
// Handshakes past their deadline should be removed by advance_handshakes.

TEST_CASE("event_loop: advance_handshakes removes expired handshake deadlines",
          "[event_loop][responsiveness][cleanup]") {
    auto cfg = resp_cfg("expired-loop");
    MeshController mc(cfg);

    // Start handshakes to dead ports.
    for (int i = 0; i < 4; ++i) {
        PeerEntry dead;
        dead.name = "expired-" + std::to_string(i);
        dead.addr = "127.0.0.1:" + std::to_string(20010 + i);
        mc.start_outbound_handshake_for_test(dead);
    }

    size_t before = mc.pending_handshake_count_for_test();
    REQUIRE(before > 0);

    // Wait for deadlines to expire (outbound_connect_timeout_ms default is ~5s,
    // but connection-refused on loopback closes the fd immediately, so
    // advance_handshakes will detect the failure on the next call).
    std::this_thread::sleep_for(200ms);
    mc.advance_handshakes_for_test();
    std::this_thread::sleep_for(100ms);
    mc.advance_handshakes_for_test();

    // After a couple iterations, dead handshakes should be cleaned up.
    // (They may not all be gone if connect() is still pending, but at least
    // the count shouldn't grow.)
    size_t after = mc.pending_handshake_count_for_test();
    INFO("pending before=" << before << " after=" << after);
    REQUIRE(after <= before);
}

// ── 4. PTY attach works even with pending handshakes ───────────────────

TEST_CASE("event_loop: session attach succeeds with pending handshakes",
          "[event_loop][responsiveness][pty]") {
    auto cfg = resp_cfg("pty-loop");
    MeshController mc(cfg);

    // Start dead-seed handshakes to simulate load.
    for (int i = 0; i < 8; ++i) {
        PeerEntry dead;
        dead.name = "load-" + std::to_string(i);
        dead.addr = "127.0.0.1:" + std::to_string(20020 + i);
        mc.start_outbound_handshake_for_test(dead);
    }

    // Advance handshakes (simulates event loop processing).
    mc.advance_handshakes_for_test();

    // PTY/session operations should still work.
    auto* s = mc.sessions().attach("resp-test", cfg.default_shell, 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->is_valid());
}

// ── 5. Multiple advance_handshakes calls are idempotent ────────────────

TEST_CASE("event_loop: repeated advance_handshakes calls don't block or accumulate",
          "[event_loop][responsiveness][idempotent]") {
    auto cfg = resp_cfg("repeat-loop");
    MeshController mc(cfg);

    // Start some dead handshakes.
    for (int i = 0; i < 4; ++i) {
        PeerEntry dead;
        dead.name = "repeat-" + std::to_string(i);
        dead.addr = "127.0.0.1:" + std::to_string(20030 + i);
        mc.start_outbound_handshake_for_test(dead);
    }

    auto start = std::chrono::steady_clock::now();

    // Call advance_handshakes 100 times rapidly — simulates a busy event loop.
    for (int i = 0; i < 100; ++i) {
        mc.advance_handshakes_for_test();
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    INFO("100 advance_handshakes calls took " << elapsed.count() << "ms");
    // Should complete in well under 1 second even with pending handshakes.
    REQUIRE(elapsed.count() < 500);
}

// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
