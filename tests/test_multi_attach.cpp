// test_multi_attach.cpp — Multi-attach feature tests (v1.1)
//
// Tests that multiple peers can attach to the same session simultaneously:
//   - peer_ids vector tracks all attached peers (not just the last one)
//   - session stays in Attached state while at least one peer is connected
//   - session transitions to Detached when all peers detach (not to Dead)
//   - reattach after full-detach works on the same session object
//   - scrollback is intact after all-detach and available on reattach
//
// Uses the SimulatedConn + handle_inbound_session pattern from test_relay.cpp.

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

#include <thread>
#include <chrono>
#include <string>
#include <vector>

using namespace bs::mesh;
using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── Helpers ───────────────────────────────────────────────────────────

static MeshConfig base_cfg(const std::string& name) {
    MeshConfig c;
    c.node_name = name;
    c.listen_port = 19953;
    c.gossip_interval_secs = 300;
    c.ping_interval_secs   = 300;
    c.pong_timeout_secs    = 60;
    c.scrollback_lines     = 200;
    return c;
}

static MeshController::Conn make_conn(const std::string& name, const std::string& pubkey) {
    MeshController::Conn c;
    c.peer_name   = name;
    c.peer_pubkey = pubkey;
    c.peer_addr   = "127.0.0.1:9999";
    c.ssl.reset();
    c.sock_fd = INVALID_SOCKET;
    c.last_pong = std::chrono::steady_clock::now();
    c.attached_session = nullptr;
    return c;
}

// handle_inbound_session takes Message&, so we need an lvalue — this helper wraps it
static void hi(MeshController& mc, MeshController::Conn& c, Message msg) {
    mc.handle_inbound_session(c, msg);
}

static AttachMsg make_attach(const std::string& session_name) {
    AttachMsg m;
    m.session_name = session_name;
    m.cols = 80; m.rows = 24;
    m.term = "xterm-256color";
    return m;
}

// ── Test 1: Two peers attach to same session — peer_ids tracks both ────────

TEST_CASE("multi_attach: two peers attach; peer_ids contains both pubkeys", "[multi_attach]") {
    auto cfg = base_cfg("ma1");
    MeshController mc(cfg);

    auto conn_a = make_conn("peer-a", std::string(64, 'a'));
    auto conn_b = make_conn("peer-b", std::string(64, 'b'));

    hi(mc, conn_a, make_attach("shared"));
    REQUIRE(conn_a.attached_session != nullptr);
    REQUIRE(conn_a.attached_session->name == "shared");

    hi(mc, conn_b, make_attach("shared"));
    REQUIRE(conn_b.attached_session != nullptr);

    // Both must point to the exact same Session object
    REQUIRE(conn_a.attached_session == conn_b.attached_session);

    auto* s = mc.sessions().get("shared");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Attached);

    // Both peer IDs must be tracked
    REQUIRE(s->peer_ids.size() == 2);
    auto& ids = s->peer_ids;
    bool has_a = std::find(ids.begin(), ids.end(), conn_a.peer_pubkey) != ids.end();
    bool has_b = std::find(ids.begin(), ids.end(), conn_b.peer_pubkey) != ids.end();
    REQUIRE(has_a);
    REQUIRE(has_b);
}

// ── Test 2: Partial detach — session stays Attached while one peer remains ──

TEST_CASE("multi_attach: partial detach leaves session Attached", "[multi_attach]") {
    auto cfg = base_cfg("ma2");
    MeshController mc(cfg);

    auto conn_a = make_conn("peer-a", std::string(64, 'a'));
    auto conn_b = make_conn("peer-b", std::string(64, 'b'));

    hi(mc, conn_a, Message{make_attach("partial")});
    hi(mc, conn_b, Message{make_attach("partial")});

    // Peer A detaches
    hi(mc, conn_a, Message{DetachMsg{}});
    REQUIRE(conn_a.attached_session == nullptr);

    auto* s = mc.sessions().get("partial");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Attached); // peer B still there
    REQUIRE(s->peer_ids.size() == 1);
    REQUIRE(s->peer_ids[0] == conn_b.peer_pubkey);
}

// ── Test 3: All peers detach — session transitions to Detached, not Dead ───

TEST_CASE("multi_attach: all peers detach; session becomes Detached not Dead", "[multi_attach]") {
    auto cfg = base_cfg("ma3");
    MeshController mc(cfg);

    auto conn_a = make_conn("peer-a", std::string(64, 'a'));
    auto conn_b = make_conn("peer-b", std::string(64, 'b'));

    hi(mc, conn_a, Message{make_attach("alldetach")});
    hi(mc, conn_b, Message{make_attach("alldetach")});

    hi(mc, conn_a, Message{DetachMsg{}});
    hi(mc, conn_b, Message{DetachMsg{}});

    REQUIRE(conn_a.attached_session == nullptr);
    REQUIRE(conn_b.attached_session == nullptr);

    auto* s = mc.sessions().get("alldetach");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Detached);
    REQUIRE(s->peer_ids.empty());
}

// ── Test 4: Reattach to fully-detached session works ─────────────────────

TEST_CASE("multi_attach: peer can reattach after all others detached", "[multi_attach]") {
    auto cfg = base_cfg("ma4");
    MeshController mc(cfg);

    auto conn_a = make_conn("peer-a", std::string(64, 'a'));
    hi(mc, conn_a, Message{make_attach("reattach-session")});
    hi(mc, conn_a, Message{DetachMsg{}});

    auto* s = mc.sessions().get("reattach-session");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Detached);

    // New peer attaches to the detached session
    auto conn_c = make_conn("peer-c", std::string(64, 'c'));
    hi(mc, conn_c, Message{make_attach("reattach-session")});

    REQUIRE(conn_c.attached_session != nullptr);
    REQUIRE(conn_c.attached_session == s); // same object
    REQUIRE(s->state == SessionState::Attached);
    REQUIRE(s->peer_ids.size() == 1);
    REQUIRE(s->peer_ids[0] == conn_c.peer_pubkey);
}

// ── Test 5: Three peers, sequential detach — peer_ids shrinks correctly ───

TEST_CASE("multi_attach: three peers sequential detach tracks peer_ids correctly", "[multi_attach]") {
    auto cfg = base_cfg("ma5");
    MeshController mc(cfg);

    auto conn_a = make_conn("peer-a", std::string(64, 'a'));
    auto conn_b = make_conn("peer-b", std::string(64, 'b'));
    auto conn_c = make_conn("peer-c", std::string(64, 'c'));

    hi(mc, conn_a, Message{make_attach("three-party")});
    hi(mc, conn_b, Message{make_attach("three-party")});
    hi(mc, conn_c, Message{make_attach("three-party")});

    auto* s = mc.sessions().get("three-party");
    REQUIRE(s != nullptr);
    REQUIRE(s->peer_ids.size() == 3);

    hi(mc, conn_b, Message{DetachMsg{}});
    REQUIRE(s->peer_ids.size() == 2);
    REQUIRE(s->state == SessionState::Attached);
    // peer-b must be gone; a and c still present
    bool has_b = std::find(s->peer_ids.begin(), s->peer_ids.end(), conn_b.peer_pubkey) != s->peer_ids.end();
    REQUIRE(!has_b);

    hi(mc, conn_a, Message{DetachMsg{}});
    REQUIRE(s->peer_ids.size() == 1);
    REQUIRE(s->state == SessionState::Attached);

    hi(mc, conn_c, Message{DetachMsg{}});
    REQUIRE(s->peer_ids.empty());
    REQUIRE(s->state == SessionState::Detached);
}

// ── Test 6: Separate sessions on same node are independent ────────────────

TEST_CASE("multi_attach: two sessions on same node are independent", "[multi_attach]") {
    auto cfg = base_cfg("ma6");
    MeshController mc(cfg);

    auto conn_a = make_conn("peer-a", std::string(64, 'a'));
    auto conn_b = make_conn("peer-b", std::string(64, 'b'));

    hi(mc, conn_a, Message{make_attach("session-alpha")});
    hi(mc, conn_b, Message{make_attach("session-beta")});

    // Different sessions — different objects
    REQUIRE(conn_a.attached_session != conn_b.attached_session);

    auto* sa = mc.sessions().get("session-alpha");
    auto* sb = mc.sessions().get("session-beta");
    REQUIRE(sa != nullptr);
    REQUIRE(sb != nullptr);
    REQUIRE(sa != sb);

    REQUIRE(sa->peer_ids.size() == 1);
    REQUIRE(sb->peer_ids.size() == 1);
    REQUIRE(sa->peer_ids[0] == conn_a.peer_pubkey);
    REQUIRE(sb->peer_ids[0] == conn_b.peer_pubkey);

    // Detach A from alpha — beta unaffected
    hi(mc, conn_a, Message{DetachMsg{}});
    REQUIRE(sa->state == SessionState::Detached);
    REQUIRE(sb->state == SessionState::Attached);
}

// ── Test 7: Scrollback intact after all-detach (survives for reattach) ────

TEST_CASE("multi_attach: scrollback intact after all peers detach", "[multi_attach]") {
    auto cfg = base_cfg("ma7");
    MeshController mc(cfg);

    // Create session and manually push known data into scrollback
    auto* s = mc.sessions().attach("scrollback-test", cfg.default_shell, 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);

    std::string_view marker = "MULTI_ATTACH_SCROLLBACK_OK\r\n";
    s->scrollback.write(marker);

    // Detach
    mc.sessions().detach("scrollback-test");
    REQUIRE(s->state == SessionState::Detached);

    // Scrollback must still be present
    auto snap = s->scrollback.snapshot();
    std::string snap_str(snap.begin(), snap.end());
    INFO("scrollback snapshot: " << snap_str);
    REQUIRE(snap_str.find("MULTI_ATTACH_SCROLLBACK_OK") != std::string::npos);
}

TEST_CASE("connection loss detaches only the disconnected peer and preserves the PTY", "[multi_attach][disconnect]") {
    auto cfg = base_cfg("ma-disconnect");
    MeshController mc(cfg);
    auto conn_a = make_conn("peer-a", std::string(64, 'a'));
    auto conn_b = make_conn("peer-b", std::string(64, 'b'));
    hi(mc, conn_a, Message{make_attach("survives-loss")});
    hi(mc, conn_b, Message{make_attach("survives-loss")});

    auto* session = conn_a.attached_session;
    REQUIRE(session != nullptr);
    const auto child_before = session->child_pid;
    mc.detach_connection_session(conn_a, false);

    REQUIRE(conn_a.attached_session == nullptr);
    REQUIRE(conn_b.attached_session == session);
    REQUIRE(session->state == SessionState::Attached);
    REQUIRE(session->peer_ids == std::vector<std::string>{conn_b.peer_pubkey});
    REQUIRE(session->child_pid == child_before);
}
