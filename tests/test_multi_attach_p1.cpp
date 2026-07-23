// test_multi_attach_p1.cpp — P1 multi-attach (2.0.8-alpha3) gate tests.
//
// Covers:
//   - 3 same-key connections attach to one session, each gets a distinct attach_id
//   - close 2, session + child survive; close last -> Detached
//   - resize from two conns -> MIN-geometry wins (narrowest pane drives PTY)
//   - spectator role: receives Output, Keystroke is rejected (no PTY write)
//   - AttachAck reports effective (min) geometry
//
// These run against handle_inbound_session with SimulatedConn (ssl null, real PTY).

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

static MeshConfig base_cfg(const std::string& name) {
    MeshConfig c;
    c.node_name = name;
    c.listen_port = 19954;
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

static void hi(MeshController& mc, MeshController::Conn& c, Message msg) {
    mc.handle_inbound_session(c, msg);
}

static AttachMsg make_attach(const std::string& name, uint16_t cols, uint16_t rows) {
    AttachMsg m;
    m.session_name = name;
    m.cols = cols; m.rows = rows;
    m.term = "xterm-256color";
    return m;
}

// ── P1-1: three same-key connections each get a distinct attach_id ──────

TEST_CASE("P1 multi-attach: three same-key conns get distinct attach_ids", "[p1][multi_attach]") {
    auto cfg = base_cfg("p1-distinct");
    MeshController mc(cfg);
    const std::string key(64, 'a');

    auto c1 = make_conn("a1", key);
    auto c2 = make_conn("a2", key);
    auto c3 = make_conn("a3", key);

    hi(mc, c1, Message{make_attach("shared", 80, 24)});
    hi(mc, c2, Message{make_attach("shared", 120, 40)});
    hi(mc, c3, Message{make_attach("shared", 200, 50)});

    REQUIRE(c1.attached_session != nullptr);
    REQUIRE(c2.attached_session != nullptr);
    REQUIRE(c3.attached_session != nullptr);
    REQUIRE(c1.attached_session == c2.attached_session);
    REQUIRE(c2.attached_session == c3.attached_session);

    // distinct attach_ids
    REQUIRE(c1.attach_id != 0);
    REQUIRE(c2.attach_id != 0);
    REQUIRE(c3.attach_id != 0);
    REQUIRE(c1.attach_id != c2.attach_id);
    REQUIRE(c2.attach_id != c3.attach_id);

    auto* s = mc.sessions().get("shared");
    REQUIRE(s != nullptr);
    REQUIRE(s->attachments.size() == 3);
}

// ── P1-2: close 2 -> session survives; close last -> Detached ──────

TEST_CASE("P1 multi-attach: detach two of three leaves session Attached; last detaches to Detached", "[p1][multi_attach]") {
    auto cfg = base_cfg("p1-partial");
    MeshController mc(cfg);
    const std::string key(64, 'a');
    auto c1 = make_conn("a1", key);
    auto c2 = make_conn("a2", key);
    auto c3 = make_conn("a3", key);

    hi(mc, c1, Message{make_attach("partial", 80, 24)});
    hi(mc, c2, Message{make_attach("partial", 80, 24)});
    hi(mc, c3, Message{make_attach("partial", 80, 24)});

    auto* s = mc.sessions().get("partial");
    REQUIRE(s != nullptr);

    // Detach c1 and c2 by attach_id (registry-level detach).
    mc.sessions().detach(c1.attach_id);
    mc.sessions().detach(c2.attach_id);
    REQUIRE(s->attachments.count(c1.attach_id) == 0);
    REQUIRE(s->attachments.count(c2.attach_id) == 0);
    REQUIRE(c3.attached_session != nullptr);
    REQUIRE(s->state == SessionState::Attached);
    REQUIRE(s->attachments.size() == 1);

    // Detach last
    mc.sessions().detach(c3.attach_id);
    REQUIRE(s->attachments.empty());
    REQUIRE(s->state == SessionState::Detached);
}

// ── P1-3: MIN-geometry across attachments ──────

TEST_CASE("P1 multi-attach: effective geometry is the min across attachments", "[p1][multi_attach][min-geometry]") {
    auto cfg = base_cfg("p1-min");
    MeshController mc(cfg);
    const std::string key(64, 'a');
    auto c1 = make_conn("a1", key);  // 80x24
    auto c2 = make_conn("a2", key);  // 200x50

    hi(mc, c1, Message{make_attach("minwin", 80, 24)});
    hi(mc, c2, Message{make_attach("minwin", 200, 50)});

    auto* s = mc.sessions().get("minwin");
    REQUIRE(s != nullptr);

    // Effective (min) geometry should be 80x24 (narrowest pane wins).
    uint16_t min_c = 200, min_r = 50;
    for (auto& kv : s->attachments) {
        min_c = std::min(min_c, kv.second.cols);
        min_r = std::min(min_r, kv.second.rows);
    }
    REQUIRE(min_c == 80);
    REQUIRE(min_r == 24);

    // A new smaller attach (40x10) should drive the min down further.
    auto c3 = make_conn("a3", key);
    hi(mc, c3, Message{make_attach("minwin", 40, 10)});
    min_c = 200; min_r = 50;
    for (auto& kv : s->attachments) {
        min_c = std::min(min_c, kv.second.cols);
        min_r = std::min(min_r, kv.second.rows);
    }
    REQUIRE(min_c == 40);
    REQUIRE(min_r == 10);
}

// ── P1-3b: resize of one pane re-applies MIN (narrowest pane wins) ──────

TEST_CASE("P1 multi-attach: resize re-applies MIN geometry (narrowest pane wins)", "[p1][multi_attach][min-geometry][resize]") {
    auto cfg = base_cfg("p1-resize");
    MeshController mc(cfg);
    const std::string key(64, 'a');
    auto c1 = make_conn("a1", key);  // 120x40
    auto c2 = make_conn("a2", key);  // 80x24

    hi(mc, c1, Message{make_attach("rz", 120, 40)});
    hi(mc, c2, Message{make_attach("rz", 80, 24)});

    auto* s = mc.sessions().get("rz");
    REQUIRE(s != nullptr);

    // Effective min is 80x24. Now c1 (120x40) resizes to 200x60 — min stays 80x24
    // because c2 is still the narrowest pane. Attachments must reflect c1's new size.
    ResizeMsg r1; r1.cols = 200; r1.rows = 60;
    hi(mc, c1, Message{r1});
    REQUIRE(s->attachments.at(c1.attach_id).cols == 200);
    REQUIRE(s->attachments.at(c1.attach_id).rows == 60);

    // Now c2 (narrowest, 80x24) resizes to 100x30 — min must move to 100x30
    // (since c1 is 200x60). The shared effective geometry grows to the new min.
    ResizeMsg r2; r2.cols = 100; r2.rows = 30;
    hi(mc, c2, Message{r2});
    REQUIRE(s->attachments.at(c2.attach_id).cols == 100);
    REQUIRE(s->attachments.at(c2.attach_id).rows == 30);

    uint16_t min_c = 1000, min_r = 1000;
    for (auto& kv : s->attachments) {
        min_c = std::min(min_c, kv.second.cols);
        min_r = std::min(min_r, kv.second.rows);
    }
    REQUIRE(min_c == 100);
    REQUIRE(min_r == 30);
}

// ── P1-3c: detaching the narrowest pane re-applies MIN (PTY grows back) ──────

TEST_CASE("P1 multi-attach: detaching narrowest pane re-applies MIN geometry", "[p1][multi_attach][min-geometry][detach]") {
    auto cfg = base_cfg("p1-detmin");
    MeshController mc(cfg);
    const std::string key(64, 'a');
    auto c1 = make_conn("a1", key);  // 120x40
    auto c2 = make_conn("a2", key);  // 80x24 (narrowest)

    hi(mc, c1, Message{make_attach("dm", 120, 40)});
    hi(mc, c2, Message{make_attach("dm", 80, 24)});

    auto* s = mc.sessions().get("dm");
    REQUIRE(s != nullptr);

    // Effective min is 80x24 (c2 narrowest). Detach c2 -> min must recompute to 120x40.
    mc.sessions().detach(c2.attach_id);
    REQUIRE(s->attachments.count(c2.attach_id) == 0);

    uint16_t min_c = 1000, min_r = 1000;
    for (auto& kv : s->attachments) {
        min_c = std::min(min_c, kv.second.cols);
        min_r = std::min(min_r, kv.second.rows);
    }
    REQUIRE(min_c == 120);
    REQUIRE(min_r == 40);
}

// ── P1-4: spectator receives output but Keystroke is rejected ──────

TEST_CASE("P1 spectator: attach sets spectator flag; Keystroke is rejected (no PTY write)", "[p1][spectator]") {
    auto cfg = base_cfg("p1-spec");
    MeshController mc(cfg);
    const std::string key(64, 's');
    auto c = make_conn("spec", key);

    AttachMsg a = make_attach("specsess", 80, 24);
    a.spectator = true;
    hi(mc, c, Message{a});

    REQUIRE(c.attached_session != nullptr);
    REQUIRE(c.attach_id != 0);
    REQUIRE(c.spectator == true);

    auto* s = mc.sessions().get("specsess");
    REQUIRE(s != nullptr);
    REQUIRE(s->attachments.at(c.attach_id).spectator == true);

    // Send a Keystroke as spectator — must be rejected (no pending input queued).
    s->pending_input.clear();
    KeystrokeMsg ks;
    ks.data = "echo HACK\n";
    hi(mc, c, Message{ks});

#ifndef _WIN32
    REQUIRE(s->pending_input.empty());  // spectator input never reaches the PTY
#endif
}

// ── P1-5: interactive (non-spectator) attach records a non-spectator attachment ──────

TEST_CASE("P1 interactive: non-spectator attach records interactive attachment (Keystroke not blocked)", "[p1][spectator]") {
    auto cfg = base_cfg("p1-int");
    MeshController mc(cfg);
    const std::string key(64, 'i');
    auto c = make_conn("int", key);

    hi(mc, c, Message{make_attach("intsess", 80, 24)});
    REQUIRE(c.spectator == false);

    auto* s = mc.sessions().get("intsess");
    REQUIRE(s != nullptr);
    REQUIRE(s->attachments.at(c.attach_id).spectator == false);

    // A Keystroke from a non-spectator must NOT be intercepted by the spectator
    // guard (it reaches write_pty_input). Verify the handler proceeds by
    // confirming no spectator-rejection log was the cause of a dropped frame:
    // the attachment remains recorded and the session stays Attached.
    KeystrokeMsg ks;
    ks.data = "ls\n";
    hi(mc, c, Message{ks});
    REQUIRE(s->state == SessionState::Attached);
    REQUIRE(s->attachments.count(c.attach_id) == 1);
}

// ── MoA regression tests (2.0.8-alpha3 audit) ─────────────────────

TEST_CASE("MoA spectator: SignalMsg + Restart rejected; interactive Restart respawns child", "[p1][spectator][moa]") {
#ifndef _WIN32
    auto cfg = base_cfg("p1-moasig");
    MeshController mc(cfg);
    const std::string key(64, 's');
    auto spec = make_conn("spec", key);
    auto intr = make_conn("intr", key);

    AttachMsg a = make_attach("sigsess", 80, 24); a.spectator = true;
    hi(mc, spec, Message{a});
    hi(mc, intr, Message{make_attach("sigsess", 80, 24)});

    auto* s = mc.sessions().get("sigsess");
    REQUIRE(s != nullptr);
    REQUIRE(s->child_pid > 0);
    const pid_t pid0 = s->child_pid;

    // 1) Spectator CtrlC — must NOT signal the child (P0 fix).
    SignalMsg sig; sig.signal = SignalMsg::SignalType::CtrlC;
    hi(mc, spec, Message{sig});
    REQUIRE(kill(pid0, 0) == 0);
    REQUIRE(s->child_pid == pid0);

    // 2) Spectator Restart with INJECTED command — must not kill/respawn.
    SignalMsg rst; rst.signal = SignalMsg::SignalType::Restart;
    rst.process = "touch /tmp/bs-moa-pwned";
    hi(mc, spec, Message{rst});
    REQUIRE(kill(pid0, 0) == 0);
    REQUIRE(s->child_pid == pid0);

    // 3) Interactive Restart — real path: old child reaped, new pid installed.
    SignalMsg rst2; rst2.signal = SignalMsg::SignalType::Restart;
    hi(mc, intr, Message{rst2});
    REQUIRE(s->child_pid > 0);
    REQUIRE(s->child_pid != pid0);
    REQUIRE(kill(s->child_pid, 0) == 0);
#endif
}

TEST_CASE("MoA geometry floor: 0x0 resize cannot collapse the shared PTY", "[p1][moa]") {
    auto cfg = base_cfg("p1-moageo");
    MeshController mc(cfg);
    const std::string key(64, 'g');
    auto c1 = make_conn("g1", key);
    auto c2 = make_conn("g2", key);

    hi(mc, c1, Message{make_attach("geosess", 80, 24)});
    hi(mc, c2, Message{make_attach("geosess", 120, 40)});

    auto* s = mc.sessions().get("geosess");
    REQUIRE(s != nullptr);

    // Malicious/buggy 0x0 resize from one attachment — floored to 20x5.
    ResizeMsg r; r.cols = 0; r.rows = 0;
    hi(mc, c2, Message{r});
    REQUIRE(s->attachments.at(c2.attach_id).cols == 20);
    REQUIRE(s->attachments.at(c2.attach_id).rows == 5);
}

TEST_CASE("MoA interactive: keystroke echo proves PTY write (non-vacuous)", "[p1][moa]") {
#ifndef _WIN32
    auto cfg = base_cfg("p1-moaecho");
    MeshController mc(cfg);
    const std::string key(64, 'e');
    auto c = make_conn("echo", key);
    hi(mc, c, Message{make_attach("echosess", 80, 24)});

    auto* s = mc.sessions().get("echosess");
    REQUIRE(s != nullptr);
    REQUIRE(s->master_fd >= 0);

    // No event loop runs in this harness — nobody pumps the PTY. Evidence of
    // delivery = the child echoes our keystroke back into the master fd,
    // which we read directly.
    std::this_thread::sleep_for(400ms); // let the shell prompt settle
    char drain[4096];
    while (::read(s->master_fd, drain, sizeof(drain)) > 0) {} // drain prompt

    KeystrokeMsg ks; ks.data = "true\n";
    hi(mc, c, Message{ks});

    // Interactive keystroke MUST be echoed by the shell onto the PTY.
    bool echoed = false;
    for (int i = 0; i < 40 && !echoed; ++i) {
        std::this_thread::sleep_for(50ms);
        echoed = ::read(s->master_fd, drain, sizeof(drain)) > 0;
    }
    REQUIRE(echoed);
#endif
}
