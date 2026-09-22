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
struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); } ~WsaInit() { WSACleanup(); } } static _wsa;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <csignal>
#define CLOSESOCK close
#endif

#include <thread>
#include <atomic>
#include <chrono>

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── Helpers ──────────────────────────────────────────────────────

static MeshConfig make_shell_test_config(const std::string& node_name) {
    MeshConfig c;
    c.node_name = node_name;
    c.listen_port = 19960;
    c.gossip_interval_secs = 300;
    return c;
}

// ── Test Cases ───────────────────────────────────────────────────

TEST_CASE("AttachMsg command field serialization round-trip", "[shell][protocol]") {
    SECTION("with command") {
        AttachMsg a;
        a.cols = 120;
        a.rows = 40;
        a.term = "xterm-256color";
        a.session_name = "test-session";
        a.routing = "target-node";
        a.command = "echo hello world";

        Message msg = a;
        auto frame = encode(msg, 0);
        Message decoded = decode(frame);
        REQUIRE(std::holds_alternative<AttachMsg>(decoded));
        const auto& b = std::get<AttachMsg>(decoded);
        REQUIRE(b.cols == 120);
        REQUIRE(b.rows == 40);
        REQUIRE(b.term == "xterm-256color");
        REQUIRE(b.session_name == "test-session");
        REQUIRE(b.routing == "target-node");
        REQUIRE(b.command == "echo hello world");
    }

    SECTION("empty command (backward compat)") {
        AttachMsg a;
        a.session_name = "no-cmd-session";
        a.command = "";

        Message msg = a;
        auto frame = encode(msg, 0);
        Message decoded = decode(frame);
        REQUIRE(std::holds_alternative<AttachMsg>(decoded));
        const auto& b = std::get<AttachMsg>(decoded);
        REQUIRE(b.session_name == "no-cmd-session");
        REQUIRE(b.command.empty());
    }

    SECTION("long command (>255 bytes)") {
        AttachMsg a;
        a.session_name = "long-cmd";
        std::string long_cmd;
        for (int i = 0; i < 500; ++i) long_cmd += "echo step-" + std::to_string(i) + "; ";
        a.command = long_cmd;

        Message msg = a;
        auto frame = encode(msg, 0);
        Message decoded = decode(frame);
        REQUIRE(std::holds_alternative<AttachMsg>(decoded));
        const auto& b = std::get<AttachMsg>(decoded);
        REQUIRE(b.command == long_cmd);
        REQUIRE(b.command.size() > 255);
    }

    SECTION("command with special characters") {
        AttachMsg a;
        a.command = "powershell -Command \"Get-Process | Where-Object {$_.Name -like '*roblox*'}\"";

        Message msg = a;
        auto frame = encode(msg, 0);
        Message decoded = decode(frame);
        const auto& b = std::get<AttachMsg>(decoded);
        REQUIRE(b.command == a.command);
    }

    SECTION("10KB command field") {
        AttachMsg a;
        a.session_name = "big-cmd";
        std::string big_cmd(10000, 'x');
        big_cmd[0] = 'e'; big_cmd[1] = 'c'; big_cmd[2] = 'h'; big_cmd[3] = 'o';
        a.command = big_cmd;

        Message msg = a;
        auto frame = encode(msg, 0);
        Message decoded = decode(frame);
        const auto& b = std::get<AttachMsg>(decoded);
        REQUIRE(b.command == big_cmd);
        REQUIRE(b.command.size() == 10000);
    }
}

TEST_CASE("AttachMsg backward compat: empty command decodes cleanly", "[shell][protocol]") {
    AttachMsg a;
    a.cols = 80;
    a.rows = 24;
    a.term = "xterm-256color";
    a.session_name = "v16-session";
    a.routing = "";
    a.command = "";

    Message msg = a;
    auto frame = encode(msg, 0);
    Message decoded = decode(frame);
    REQUIRE(std::holds_alternative<AttachMsg>(decoded));
    const auto& b = std::get<AttachMsg>(decoded);
    REQUIRE(b.cols == 80);
    REQUIRE(b.rows == 24);
    REQUIRE(b.session_name == "v16-session");
    REQUIRE(b.command.empty());
}

TEST_CASE("ExitCodeMsg and SessionDiedMsg semantics", "[shell]") {
    ExitCodeMsg e;
    REQUIRE(e.code == 0);

    SessionDiedMsg d;
    REQUIRE(d.exit_code == 0);
    REQUIRE(d.signal_num == 0);

    // Exit code propagation: 0 = success, 255 = connection lost
    e.code = 255;
    REQUIRE(e.code == 255);

    e.code = 42;
    REQUIRE(e.code == 42);
}

TEST_CASE("strip_mode_sequences removes only mode-setters (26.09.18 item 5)", "[shell]") {
    // Mouse tracking enable/disable must be stripped from scrollback replay.
    std::string replay = "\033[?1000h\033[?1006htext here\033[?1000l";
    std::string out = strip_mode_sequences(replay);
    REQUIRE(out.find("text here") != std::string::npos);
    REQUIRE(out.find("1000") == std::string::npos);
    REQUIRE(out.find("1006") == std::string::npos);

    SECTION("colors survive") {
        std::string c = "\033[1;31mred\033[0m";
        REQUIRE(strip_mode_sequences(c) == c);
    }
    SECTION("DECCKM (app cursor keys) stripped") {
        REQUIRE(strip_mode_sequences("\033[?1h") == "");
        REQUIRE(strip_mode_sequences("a\033[?1lb") == "ab");
    }
    SECTION("alt-screen and bracketed paste stripped") {
        REQUIRE(strip_mode_sequences("x\033[?1049hy\033[?2004hz") == "xyz");
    }
    SECTION("non-private CSI untouched") {
        std::string c = "\033[2J\033[H";
        REQUIRE(strip_mode_sequences(c) == c);
    }
    SECTION("cursor moves preserved") {
        std::string c = "a\033[10;20Hb";
        REQUIRE(strip_mode_sequences(c) == c);
    }
    SECTION("plain text passthrough") {
        REQUIRE(strip_mode_sequences("hello world") == "hello world");
    }
    SECTION("empty") { REQUIRE(strip_mode_sequences("").empty()); }
}

TEST_CASE("strip_ansi_escapes removes escape sequences", "[shell]") {
    std::string with_ansi = "\033[1;31mHello\033[0m World";
    std::string stripped = strip_ansi_escapes(with_ansi);
    REQUIRE(stripped.find("Hello") != std::string::npos);
    REQUIRE(stripped.find("World") != std::string::npos);
    REQUIRE(stripped.find("\033[") == std::string::npos);

    SECTION("passthrough for clean text") {
        std::string clean = "no escapes here";
        REQUIRE(strip_ansi_escapes(clean) == clean);
    }

    SECTION("empty string") {
        REQUIRE(strip_ansi_escapes("").empty());
    }

    SECTION("CSI sequences") {
        std::string csi = "\033[2J\033[H";  // clear screen + home
        std::string result = strip_ansi_escapes(csi);
        REQUIRE(result.empty());
    }

    SECTION("OSC sequences") {
        std::string osc = "\033]0;window title\007";
        std::string result = strip_ansi_escapes(osc);
        REQUIRE(result.empty());
    }
}

TEST_CASE("peer_name_eq is case-insensitive", "[shell]") {
    auto cfg = make_shell_test_config("test-node");
    MeshController mc(cfg);
    REQUIRE(mc.peer_name_eq("Shadow", "shadow"));
    REQUIRE(mc.peer_name_eq("SHADOW", "shadow"));
    REQUIRE(mc.peer_name_eq("TEST-PC1", "test-pc1"));
    REQUIRE_FALSE(mc.peer_name_eq("test-pc2", "test-pc1"));
    REQUIRE(mc.peer_name_eq("", ""));  // empty strings are equal
}

TEST_CASE("MeshController constructor with config", "[shell]") {
    auto cfg = make_shell_test_config("test-node");
    MeshController mc(cfg);
    // Controller should exist and be functional
    // peer_name_eq is a basic sanity check that the object is valid
    REQUIRE(mc.peer_name_eq("a", "A"));
}

TEST_CASE("daemon IPC shell relay explicitly delegates to direct TLS",
          "[shell][ipc]") {
    REQUIRE(MeshController::shell_ipc_relay_policy_response() ==
            "ERROR direct TLS required\n");
    REQUIRE(MeshController::should_fallback_to_direct_shell(
        -1, "direct TLS required"));
    REQUIRE_FALSE(MeshController::should_fallback_to_direct_shell(
        -1, "permission denied"));
}

TEST_CASE("prune_ephemeral_sessions removes finished health/cmd sessions",
          "[shell][oneshot][reaper]") {
    auto cfg = make_shell_test_config("reaper-node");
    MeshController mc(cfg);
#ifdef _WIN32
    const std::string cmd = "cmd.exe /c ping -n 30 127.0.0.1 >nul";
#else
    const std::string cmd = "sleep 30";
#endif
    auto* s = mc.sessions().attach(
        "health-testnonce",
        ResolvedSessionCommand{cmd, SessionCommandSource::ClientOverride},
        80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->is_valid());
    // Mark as aged detached so the reaper is allowed to consider it.
    s->state = SessionState::Detached;
    s->last_attach_at = std::chrono::steady_clock::now() - std::chrono::seconds(120);
    s->last_output_at = s->last_attach_at;
    // A2 correctness: prune is state-aware — a live child must not be pruned
    // even when aged (output silence != death).
    mc.sessions().prune_ephemeral_sessions(std::chrono::seconds(30));
    REQUIRE(mc.sessions().get("health-testnonce") != nullptr);
#ifdef _WIN32
    if (s->child_pid) TerminateProcess(s->child_pid, 1);
#else
    if (s->child_pid > 0) ::kill(s->child_pid, SIGKILL);
#endif
    for (int i = 0; i < 100 && s->is_valid(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        (void)read_available_pty_output(*s);
        mc.sessions().reap_dead(false);
    }
    mc.sessions().prune_ephemeral_sessions(std::chrono::seconds(30));
    REQUIRE(mc.sessions().get("health-testnonce") == nullptr);
}

TEST_CASE("ephemeral cmd session names are unique and non-default",
          "[shell][oneshot]") {
    auto a = make_ephemeral_cmd_session_name();
    auto b = make_ephemeral_cmd_session_name();
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    REQUIRE(a != "default");
    REQUIRE(a.rfind("cmd-", 0) == 0);
    // Second call may share pid but must not collide on the counter/time suffix.
    REQUIRE(a != b);
    REQUIRE(noninteractive_shell_timeout_sec() >= 5);
    REQUIRE(noninteractive_shell_timeout_sec() <= 7200);
}

TEST_CASE("unnamed quick-connect always starts a new tty session",
          "[shell][quick-connect]") {
    REQUIRE(resolve_quick_connect_session_name("work") == "work");
    REQUIRE(resolve_quick_connect_session_name("shell") == "shell");
    auto a = resolve_quick_connect_session_name("");
    auto b = resolve_quick_connect_session_name("");
    REQUIRE(a.rfind("tty-", 0) == 0);
    REQUIRE(b.rfind("tty-", 0) == 0);
    REQUIRE(a != b);
    REQUIRE(a != "shell");
    REQUIRE(a != "default");
    REQUIRE(is_ephemeral_session_name(a));
    REQUIRE_FALSE(is_ephemeral_session_name("shell"));
    REQUIRE_FALSE(is_ephemeral_session_name("work"));
    REQUIRE(is_ephemeral_session_name("cmd-1-2"));
}

TEST_CASE("new harness quick-connect sessions do not reuse profile names",
          "[shell][quick-connect][harness]") {
    // Harness selection chooses a command profile; each New action must still
    // receive a unique terminal session name so it cannot reattach to the
    // prior harness terminal.
    auto first = resolve_quick_connect_session_name("");
    auto second = resolve_quick_connect_session_name("");
    REQUIRE(first.rfind("tty-", 0) == 0);
    REQUIRE(second.rfind("tty-", 0) == 0);
    REQUIRE(first != second);
    REQUIRE(first != "hermes");
    REQUIRE(second != "opencode");
}

TEST_CASE("ephemeral session names carry a UTC datetime segment",
          "[shell][quick-connect][naming]") {
    // Format: tty-YYYYMMDD-HHMMSS-pid-seq (26.09.13). The date must parse as
    // a real calendar date so operators can date stale sessions by eye.
    auto name = resolve_quick_connect_session_name("");
    REQUIRE(name.rfind("tty-", 0) == 0);
    auto body = name.substr(4);                    // YYYYMMDD-HHMMSS-pid-seq
    auto dash = body.find('-');
    REQUIRE(dash == 8);
    auto date = body.substr(0, dash);
    auto time = body.substr(dash + 1, 6);
    REQUIRE(date.size() == 8);
    REQUIRE(time.size() == 6);
    REQUIRE(date.find_first_not_of("0123456789") == std::string::npos);
    REQUIRE(time.find_first_not_of("0123456789") == std::string::npos);
    int mon  = std::stoi(date.substr(4, 2));
    int day  = std::stoi(date.substr(6, 2));
    int hour = std::stoi(time.substr(0, 2));
    int min  = std::stoi(time.substr(2, 2));
    int sec  = std::stoi(time.substr(4, 2));
    REQUIRE(mon >= 1);  REQUIRE(mon <= 12);
    REQUIRE(day >= 1);  REQUIRE(day <= 31);
    REQUIRE(hour <= 23); REQUIRE(min <= 59); REQUIRE(sec <= 59);
    // format_utc_datetime_compact sanity: 15 chars, digits + the one dash.
    auto dt = format_utc_datetime_compact();
    REQUIRE(dt.size() == 15);
    REQUIRE(dt[8] == '-');
    REQUIRE(dt.find_first_not_of("0123456789-") == std::string::npos);
}

TEST_CASE("SessionRegistry::kill reports existence honestly",
          "[shell][kill]") {
    auto cfg = make_shell_test_config("kill-node");
    MeshController mc(cfg);
    // Kill on an empty registry: miss, not crash, not silent success.
    REQUIRE_FALSE(mc.sessions().kill("tty-nope-19700101-000000-0"));
#ifdef _WIN32
    const std::string cmd = "cmd.exe /c ping -n 30 127.0.0.1 >nul";
#else
    const std::string cmd = "sleep 30";
#endif
    auto* s = mc.sessions().attach(
        "tty-killtest-19700101-000000-0",
        ResolvedSessionCommand{cmd, SessionCommandSource::ClientOverride},
        80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(mc.sessions().kill("tty-killtest-19700101-000000-0"));
    REQUIRE(mc.sessions().get("tty-killtest-19700101-000000-0") == nullptr);
    // Second kill of the same name: honest miss (already gone).
    REQUIRE_FALSE(mc.sessions().kill("tty-killtest-19700101-000000-0"));
}

TEST_CASE("ClientOverride force-respawns live default session",
          "[shell][oneshot][attach]") {
    auto cfg = make_shell_test_config("oneshot-node");
    MeshController mc(cfg);

#ifdef _WIN32
    const std::string long_shell = "cmd.exe /Q";
#else
    const std::string long_shell = "sleep 60";
#endif
    // First attach creates a long-lived "default" session (no override).
    auto* s = mc.sessions().attach(
        "default",
        ResolvedSessionCommand{long_shell, SessionCommandSource::ConfigDefault},
        80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->is_valid());
    const uint64_t gen0 = s->generation;

#ifdef _WIN32
    const std::string oneshot = "cmd.exe /c echo oneshot-ok";
#else
    const std::string oneshot = "echo oneshot-ok";
#endif
    uint16_t ec = 0, er = 0;
    uint32_t aid = mc.sessions().attach_connection(
        "default",
        ResolvedSessionCommand{oneshot, SessionCommandSource::ClientOverride},
        80, 24, "xterm-256color", "", 0, false, ec, er);
    REQUIRE(aid != 0);
    auto* s2 = mc.sessions().get("default");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2->is_valid());
    // install_spawned_runtime assigns a new generation on respawn.
    REQUIRE(s2->generation > gen0);
    const bool cmd_looks_like_override =
        s2->command.find("oneshot") != std::string::npos
        || s2->command.find("echo") != std::string::npos
        || s2->command.find("cmd.exe") != std::string::npos;
    REQUIRE(cmd_looks_like_override);
}

TEST_CASE("IPC protocol constants are correct", "[shell]") {
    // These constants are used by the SHELL IPC handler
    REQUIRE(FRAME_HEADER_SIZE == 6);
    REQUIRE(MAX_FRAME_SIZE == 65535);
    REQUIRE(COMPRESSION_THRESHOLD == 256);
}

TEST_CASE("MessageType for shell-related messages", "[shell][protocol]") {
    REQUIRE(static_cast<uint8_t>(MessageType::Attach) == 0x06);
    REQUIRE(static_cast<uint8_t>(MessageType::ProcExited) == 0x0E);
    REQUIRE(static_cast<uint8_t>(MessageType::SessionDied) == 0x10);
    REQUIRE(static_cast<uint8_t>(MessageType::Output) == 0x02);
}

TEST_CASE("MeshConfig defaults for daemon", "[shell]") {
    MeshConfig c;
    REQUIRE(c.node_name == "unnamed");
    REQUIRE(c.listen_port == 19949);
    // gossip_interval_secs should be settable
    c.gossip_interval_secs = 60;
    REQUIRE(c.gossip_interval_secs == 60);
}
