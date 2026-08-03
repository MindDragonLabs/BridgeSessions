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
