// winsock2 must come BEFORE windows.h
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
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <fstream>
#endif

using namespace bs::mesh;

static MeshConfig make_test_config(const std::string& node_name) {
    MeshConfig c;
    c.node_name = node_name;
    c.listen_port = 19949;
    c.gossip_interval_secs = 300;
    c.ping_interval_secs = 300;
    c.scrollback_lines = 100;
    return c;
}

#ifndef _WIN32
// Wait until `path` exists (child wrote its signal sentinel) or timeout.
static bool wait_for_file(const std::string& path, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (::access(path.c_str(), F_OK) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return ::access(path.c_str(), F_OK) == 0;
}
#endif

// ── B.1: Ctrl-C (byte 0x03) reaches the remote child as SIGINT ─────
// The interactive path forwards 0x03 as a keystroke; the child's PTY must
// translate it to SIGINT. The bridge session itself must survive (not Died).
TEST_CASE("Interactive Ctrl-C delivers SIGINT to child, session survives",
          "[cua][signal][posix-only]") {
#ifndef _WIN32
    auto cfg = make_test_config("test-node-cua-sigint");
    MeshController mc(cfg);

    std::string sentinel = "/tmp/bs_cua_sigint_" + std::to_string(getpid()) + ".sent";
    std::remove(sentinel.c_str());
    // Child installs a SIGINT trap that writes a sentinel file, then keeps running.
    std::string cmd = "bash -lc 'trap \"touch " + sentinel + "\" INT; sleep 30'";

    auto* s = mc.sessions().attach("cua-sigint", cmd, 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->is_valid());
    REQUIRE(s->child_pid > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Forward the literal Ctrl-C byte the way the interactive loop would.
    REQUIRE(mc.write_pty_input(*s, "\x03", 1));

    // The child must have received SIGINT (sentinel written).
    REQUIRE(wait_for_file(sentinel, 3000));

    // Session must still be alive (not Died) and child still running.
    REQUIRE(s->state != SessionState::Died);
    REQUIRE(s->is_valid());
    REQUIRE(::kill(s->child_pid, 0) == 0); // child process still exists
    std::remove(sentinel.c_str());
#else
    // Windows ConPTY signal forwarding is covered by the interactive E2E
    // (Phase A.3) on real hardware; this unit is POSIX-targeted.
    REQUIRE(true);
#endif
}

// ── B.4: --signal-on-detach delivers the requested signal on last detach ──
TEST_CASE("Detach signal is delivered to child on last-peer detach",
          "[cua][detach-signal][b4-only][posix-only]") {
#ifndef _WIN32
    auto cfg = make_test_config("test-node-detach-sig");
    MeshController mc(cfg);

    // A plain long-running child that terminates on the requested signal.
    // (Non-interactive bash ignores SIGHUP by default, so we assert with TERM,
    // which reliably terminates `sleep`. The delivery path is signal-agnostic.)
    // create_session spawns `/bin/sh -c <cmd>` and the direct child is `sh`.
    // To deterministically prove the detach signal reaches `sh` and is handled,
    // use a command that blocks in a builtin (`read`, no foreground subprocess)
    // with a TERM trap that writes a sentinel. A TERM trap fires reliably here
    // (unlike when `sh` is waiting on a foreground child, which it defers).
    std::string sentinel = "/tmp/bs_cua_term_" + std::to_string(getpid()) + ".sent";
    std::remove(sentinel.c_str());
    std::string cmd = "trap 'touch " + sentinel + "' TERM; read";

    auto* s = mc.sessions().attach("cua-detach", cmd, 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->is_valid());
    REQUIRE(s->child_pid > 0);

    // Simulate the attaching peer requesting TERM on detach.
    s->detach_signal = "TERM";

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Last peer detaches (empty peer_pubkey clears all attached peers).
    mc.sessions().detach("cua-detach", "");

    // The child (`sh`) must have received SIGTERM and run its trap (sentinel written).
    bool sentinel_seen = false;
    for (int i = 0; i < 60; ++i) {
        if (::access(sentinel.c_str(), F_OK) == 0) { sentinel_seen = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(sentinel_seen);
    std::remove(sentinel.c_str());
    // The session stays Detached (we signaled the child, we didn't drop the session).
    REQUIRE(s->state == SessionState::Detached);
#else
    REQUIRE(true);
#endif
}

// ── B.4: unknown signal name is ignored (no crash, no kill) ──────────
TEST_CASE("Detach with unknown signal name is a no-op, no crash",
          "[cua][detach-signal][posix-only]") {
#ifndef _WIN32
    auto cfg = make_test_config("test-node-detach-bad");
    MeshController mc(cfg);

    auto* s = mc.sessions().attach("cua-detach-bad", "sleep 30", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->is_valid());
    s->detach_signal = "NOTAREALSIGNAL";

    mc.sessions().detach("cua-detach-bad", "");

    // Child still alive immediately after detach (we did not kill it).
    REQUIRE(s->state == SessionState::Detached);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    REQUIRE(::kill(s->child_pid, 0) == 0);
#else
    REQUIRE(true);
#endif
}

// ── B.4 regression: --signal-on-detach survives the wire round-trip ──
// The P0 this catches: the CLI never assigned signal_on_detach onto the
// client AttachMsg, so the field was always empty on the wire and the
// advertised feature was a no-op. A round-trip through encode()/decode()
// proves the field is now serialized and parsed correctly.
TEST_CASE("signal_on_detach survives AttachMsg wire round-trip",
          "[cua][detach-signal][wire][posix-only]") {
#ifndef _WIN32
    AttachMsg out;
    out.session_name = "cua-rt";
    out.cols = 80;
    out.rows = 24;
    out.term = "xterm-256color";
    out.command = "sleep 30";
    out.signal_on_detach = "TERM";

    auto frame = encode(Message{out}, CONTROL_STREAM_ID);
    auto back = decode(frame);
    REQUIRE(std::holds_alternative<AttachMsg>(back));
    const auto& in = std::get<AttachMsg>(back);
    REQUIRE(in.signal_on_detach == "TERM");
    REQUIRE(in.session_name == "cua-rt");
    REQUIRE(in.command == "sleep 30");
#else
    REQUIRE(true);
#endif
}

// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
