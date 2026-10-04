#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "../bs-protocol.h"
#include <filesystem>
#include <thread>
#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

template <typename T>
T panel_roundtrip(const T& value, bool allow_large = true) {
    Message wire_value = value;
    auto wire = encode(wire_value, CONTROL_STREAM_ID, allow_large);
    auto decoded = decode(wire);
    REQUIRE(message_type(wire_value) == message_type(decoded));
    return std::get<T>(decoded);
}

TEST_CASE("panel session control codec has matched request/reply variants",
          "[panel][codec]") {
    SessionInputMsg input;
    input.request_id = 7;
    input.session_name = "build-1";
    input.data = "echo panel\n\x03";
    REQUIRE(panel_roundtrip(input) == input);

    SessionInputReplyMsg input_reply;
    input_reply.request_id = 7;
    input_reply.ok = false;
    input_reply.error = "no live session";
    REQUIRE(panel_roundtrip(input_reply) == input_reply);

    SessionScrollbackMsg read;
    read.request_id = 8;
    read.session_name = "build-1";
    read.offset = 1234;
    read.limit = 65536;
    REQUIRE(panel_roundtrip(read) == read);

    SessionScrollbackReplyMsg read_reply;
    read_reply.request_id = 8;
    read_reply.ok = true;
    read_reply.next_offset = 1300;
    read_reply.data = "actual PTY bytes\r\n";
    read_reply.reset = true;
    REQUIRE(panel_roundtrip(read_reply) == read_reply);

    SessionKillMsg kill;
    kill.request_id = 9;
    kill.session_name = "build-1";
    REQUIRE(panel_roundtrip(kill) == kill);

    SessionKillReplyMsg kill_reply;
    kill_reply.request_id = 9;
    kill_reply.ok = true;
    REQUIRE(panel_roundtrip(kill_reply) == kill_reply);
}

TEST_CASE("panel session control rejects oversized wire payloads", "[panel][codec]") {
    SessionInputMsg input;
    input.session_name = "safe";
    input.data.assign(kSessionControlMaxBytes + 1, 'x');
    REQUIRE_THROWS_AS(encode(Message{input}, CONTROL_STREAM_ID, true), std::runtime_error);

    SessionScrollbackMsg read;
    read.session_name = "safe";
    read.limit = static_cast<uint32_t>(kSessionControlMaxBytes + 1);
    REQUIRE_THROWS_AS(encode(Message{read}, CONTROL_STREAM_ID, true), std::runtime_error);

    SessionScrollbackReplyMsg reply;
    reply.data.assign(kSessionControlMaxBytes + 1, 'x');
    REQUIRE_THROWS_AS(encode(Message{reply}, CONTROL_STREAM_ID, true), std::runtime_error);
}

TEST_CASE("panel names and strict base64 fail closed", "[panel][ipc]") {
    REQUIRE(session_control_machine_valid("."));
    REQUIRE(session_control_machine_valid("linux-1"));
    REQUIRE(session_control_name_valid("tty-20261003-1"));
    REQUIRE_FALSE(session_control_name_valid("bad name"));
    REQUIRE_FALSE(session_control_name_valid("bad\nname"));
    REQUIRE_FALSE(session_control_name_valid("../session"));
    REQUIRE_FALSE(session_control_machine_valid("unknown/machine"));

    REQUIRE(b64_valid_strict(""));
    REQUIRE(b64_valid_strict("aGk"));
    REQUIRE(b64dec_strict("aGk", kSessionControlMaxBytes).value() == "hi");
    REQUIRE_FALSE(b64_valid_strict("aGk="));
    REQUIRE_FALSE(b64_valid_strict("aGk!"));
    REQUIRE_FALSE(b64_valid_strict("A"));
    REQUIRE_FALSE(b64dec_strict(b64enc(std::string(65537, 'x')),
                                 kSessionControlMaxBytes));
}

TEST_CASE("scrollback bytes preserve reset and absolute offsets", "[panel][ring]") {
    RingBuffer<8> ring;
    ring.write(std::string_view("abcdefghijk"));
    auto [stale, reset] = ring.read_since(0);
    REQUIRE(reset);
    REQUIRE(stale == "defghijk");
    REQUIRE(ring.total_written() == 11);

    auto [tail, no_reset] = ring.read_since(9);
    REQUIRE_FALSE(no_reset);
    REQUIRE(tail == "jk");
}

TEST_CASE("auto-upgrade platform matching is fail-safe", "[upgrade][platform]") {
    REQUIRE(upgrade_platform_supported("windows"));
    REQUIRE(upgrade_platform_supported("linux"));
    REQUIRE(upgrade_platform_supported("macos"));
    REQUIRE_FALSE(upgrade_platform_supported("darwin"));
    REQUIRE_FALSE(upgrade_platform_supported("freebsd"));
    REQUIRE_FALSE(upgrade_platform_supported(""));
}

TEST_CASE("panel reads keep limited RESET cursors contiguous", "[panel][ring]") {
    RingBuffer<8> ring;
    ring.write(std::string_view("abcdefghijk"));
    auto first = session_control_read(ring, 0, 3);
    REQUIRE(first.reset);
    REQUIRE(first.data == "def");
    REQUIRE(first.next_offset == 6);
    auto second = session_control_read(ring, first.next_offset, 3);
    REQUIRE_FALSE(second.reset);
    REQUIRE(second.data == "ghi");
    REQUIRE(second.next_offset == 9);
    auto third = session_control_read(ring, second.next_offset, 3);
    REQUIRE(third.data == "jk");
    REQUIRE(third.next_offset == 11);
    REQUIRE(session_control_read(ring, 11, 3).data.empty());
    REQUIRE(session_control_read(ring, 0, 0).next_offset == 0);
    REQUIRE_THROWS(session_control_read(ring, 12, 1));
    REQUIRE_THROWS(session_control_read(ring, 0, 65537));

    RingBuffer<131072> large;
    const std::string large_bytes(100000, 'x');
    large.write(std::string_view(large_bytes));
    auto capped = session_control_read(large, 0, 17);
    REQUIRE_FALSE(capped.reset);
    REQUIRE(capped.next_offset == 17);
    REQUIRE(capped.data.size() == 17);
}

TEST_CASE("panel capability gate requires exact tags and a mesh transport", "[panel][compat]") {
    MeshController::Conn conn;
    conn.sock_fd = 1; // policy test only; never owns or touches this descriptor
    conn.remote_version = version_string_with_local_caps();
    REQUIRE(MeshController::session_control_peer_capable(conn));
    for (const auto& version : {"26.10.03", "26.10.03+frm2", "26.10.03+sessionctl",
                               "26.10.03+frm2+sessionctl2"}) {
        conn.remote_version = version;
        REQUIRE_FALSE(MeshController::session_control_peer_capable(conn));
    }
    conn.remote_version = "2026.10.03-beta1+frm2+sessionctl";
    REQUIRE(MeshController::session_control_peer_capable(conn));
    conn.purpose = MeshController::ConnectionPurpose::DirectSession;
    REQUIRE_FALSE(MeshController::session_control_peer_capable(conn));
    conn.purpose = MeshController::ConnectionPurpose::Mesh;
    conn.sock_fd = INVALID_SOCKET;
    REQUIRE_FALSE(MeshController::session_control_peer_capable(conn));
    REQUIRE(version_core(version_string_with_local_caps()) == kBridgeSessionsVersion);
    REQUIRE(message_type(Message{DirectoryEnrollMsg{}}) == MessageType::DirectoryEnroll);
    REQUIRE(Message{DirectoryEnrollMsg{}}.index() == 41);
    REQUIRE(Message{SessionInputMsg{}}.index() == 42);
    REQUIRE(Message{SessionKillReplyMsg{}}.index() == 47);
}

TEST_CASE("panel base64 preserves arbitrary bytes and rejects noncanonical tokens", "[panel][ipc]") {
    std::string bytes;
    for (int i = 0; i < 256; ++i) bytes.push_back(static_cast<char>(i));
    REQUIRE(b64dec_strict(b64enc(bytes), 256).value() == bytes);
    for (const auto& bad : {"AB", "AAB", "aGk=", "aG k", "aGk\n", "_w", "-w"})
        REQUIRE_FALSE(b64_valid_strict(bad));
    REQUIRE(b64dec_strict(b64enc(std::string(65536, '\0')), 65536)->size() == 65536);
    REQUIRE(session_control_error_line("no live session") == "ERROR no live session\n");
    REQUIRE(session_control_error_line("bad\nOK") == "ERROR invalid remote error\n");
    REQUIRE(session_control_error_line(std::string("bad\0OK", 6)) == "ERROR invalid remote error\n");
}

TEST_CASE("auto upgrade dispatch guards platform origin version and cooldown", "[panel][upgrade]") {
    MeshConfig config;
    config.node_name = "origin";
    config.auto_upgrade = true;
    config.auto_upgrade_origin = "origin";
    for (const auto& os : {"windows", "linux", "macos"}) {
        REQUIRE(MeshController::auto_upgrade_dispatch_allowed(config, "target", "26.09.01+frm2", os));
        config.auto_upgrade_origin = "other";
        REQUIRE_FALSE(MeshController::auto_upgrade_dispatch_allowed(config, "target", "26.09.01", os));
        config.auto_upgrade_origin = "origin";
    }
    for (const auto& os : {"", "unknown", "freebsd", "darwin", "Windows", "linux;evil"})
        REQUIRE_FALSE(MeshController::auto_upgrade_dispatch_allowed(config, "target", "26.09.01", os));
    REQUIRE_FALSE(MeshController::auto_upgrade_dispatch_allowed(config, "origin", "26.09.01", "linux"));
    REQUIRE_FALSE(MeshController::auto_upgrade_dispatch_allowed(config, "bad;command", "26.09.01", "linux"));
    REQUIRE_FALSE(MeshController::auto_upgrade_dispatch_allowed(config, "target", kBridgeSessionsVersion, "linux"));
    config.auto_upgrade = false;
    REQUIRE_FALSE(MeshController::auto_upgrade_dispatch_allowed(config, "target", "26.09.01", "linux"));
    REQUIRE(MeshController::auto_upgrade_cooldown(0, 1800).count() == 60);
    REQUIRE(MeshController::auto_upgrade_cooldown(1, 1800).count() == 60);
    REQUIRE(MeshController::auto_upgrade_cooldown(2, 1800).count() == 300);
    REQUIRE(MeshController::auto_upgrade_cooldown(3, 1800).count() == 1800);
    REQUIRE(MeshController::auto_upgrade_cooldown(4, 0).count() == 60);
}

#ifndef _WIN32
struct PanelTempRoot {
    std::string path;
    PanelTempRoot() {
        char pattern[] = "/tmp/bs-panel-control-XXXXXX";
        char* root = ::mkdtemp(pattern);
        REQUIRE(root != nullptr);
        path = root;
    }
    ~PanelTempRoot() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

static std::string local_control_bytes(const std::string& response) {
    REQUIRE(response.rfind("OK ", 0) == 0);
    std::istringstream fields(response);
    std::string ok, encoded;
    uint64_t offset;
    fields >> ok >> offset >> encoded;
    return encoded == "-" ? "" : b64dec_strict(encoded, 65536).value();
}

TEST_CASE("panel local commands target existing real PTYs and isolate sessions", "[panel][local]") {
    PanelTempRoot root;
    MeshConfig config;
    config.node_name = "local-test";
    config.auto_upgrade = false;
    config.mdns_enabled = false;
    MeshController controller(config, root.path);
    MeshController::Conn attach;
    AttachMsg request;
    request.session_name = "one";
    request.command = "stty -echo; printf READY_ONE; exec cat";
    controller.inject_attach_for_test(attach, request);
    REQUIRE(controller.session_exists_for_test("one"));
    MeshController::Conn other_attach;
    request.session_name = "two";
    request.command = "stty -echo; printf READY_TWO; exec cat";
    controller.inject_attach_for_test(other_attach, request);
    REQUIRE(controller.session_exists_for_test("two"));
    MeshController::Conn dead_attach;
    request.session_name = "dead";
    request.command = "printf DEAD_MARKER; exit 0";
    controller.inject_attach_for_test(dead_attach, request);
    bool handoff = false;
    auto bytes = [&](const std::string& session) {
        controller.pty_output_poller();
        return local_control_bytes(controller.begin_panel_session_scrollback(
            INVALID_SOCKET, ".", session, 0, 65536, handoff));
    };
    auto wait_marker = [&](const std::string& session, const std::string& marker) {
        for (int i = 0; i < 200; ++i) {
            if (bytes(session).find(marker) != std::string::npos) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    };
    REQUIRE(wait_marker("one", "READY_ONE"));
    REQUIRE(wait_marker("two", "READY_TWO"));
    REQUIRE(wait_marker("dead", "DEAD_MARKER"));
    // Poll once more to consume the terminal EOF and record the dead state.
    for (int i = 0; i < 20; ++i) {
        controller.pty_output_poller();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(controller.begin_panel_session_input(INVALID_SOCKET, ".", "dead", "x", handoff).rfind("ERROR ", 0) == 0);
    REQUIRE(bytes("dead").find("DEAD_MARKER") != std::string::npos);
    REQUIRE(controller.begin_panel_session_input(INVALID_SOCKET, ".", "one", "MARKER_ONE\n", handoff) == "OK\n");
    REQUIRE_FALSE(handoff);
    REQUIRE(controller.begin_panel_session_input(INVALID_SOCKET, ".", "two", "MARKER_TWO\n", handoff) == "OK\n");
    REQUIRE(wait_marker("one", "MARKER_ONE"));
    REQUIRE(wait_marker("two", "MARKER_TWO"));
    REQUIRE(bytes("one").find("MARKER_TWO") == std::string::npos);
    REQUIRE(bytes("two").find("MARKER_ONE") == std::string::npos);
    REQUIRE(controller.begin_panel_session_input(INVALID_SOCKET, ".", "missing", "x", handoff).rfind("ERROR ", 0) == 0);
    REQUIRE(controller.begin_panel_session_input(INVALID_SOCKET, ".", "one", std::string(65537, 'x'), handoff).rfind("ERROR ", 0) == 0);
    REQUIRE(controller.begin_panel_session_scrollback(INVALID_SOCKET, ".", "one", UINT64_MAX, 1, handoff) == "ERROR invalid offset\n");
    REQUIRE(controller.begin_panel_session_scrollback(INVALID_SOCKET, ".", "one", 0, 65537, handoff).rfind("ERROR ", 0) == 0);
    REQUIRE(controller.begin_panel_session_scrollback(INVALID_SOCKET, ".", "missing", 0, 1, handoff).rfind("ERROR ", 0) == 0);
    REQUIRE(controller.begin_panel_session_kill(INVALID_SOCKET, ".", "one", handoff) == "OK\n");
    REQUIRE(controller.begin_panel_session_input(INVALID_SOCKET, ".", "one", "x", handoff).rfind("ERROR ", 0) == 0);
    REQUIRE(controller.begin_panel_session_kill(INVALID_SOCKET, ".", "one", handoff).rfind("ERROR ", 0) == 0);
    REQUIRE(controller.begin_panel_session_input(INVALID_SOCKET, ".", "two", "STILL_ALIVE\n", handoff) == "OK\n");
    REQUIRE(wait_marker("two", "STILL_ALIVE"));
    REQUIRE(controller.begin_panel_session_kill(INVALID_SOCKET, ".", "two", handoff) == "OK\n");
}
#endif

TEST_CASE("panel decoder rejects truncated and oversized declared wire fields", "[panel][codec]") {
    SessionInputMsg input;
    input.session_name = "safe";
    auto input_wire = encode(Message{input}, CONTROL_STREAM_ID, true);
    auto header = frame_header_layout(input_wire.data(), input_wire.size()).first;
    write_u32be(input_wire.data() + header + 4 + 1 + input.session_name.size(), 65537);
    REQUIRE_THROWS_AS(decode(input_wire), std::runtime_error);

    SessionScrollbackMsg scroll;
    scroll.session_name = "safe";
    auto scroll_wire = encode(Message{scroll}, CONTROL_STREAM_ID, true);
    header = frame_header_layout(scroll_wire.data(), scroll_wire.size()).first;
    write_u32be(scroll_wire.data() + header + 4 + 1 + scroll.session_name.size() + 8, 65537);
    REQUIRE_THROWS_AS(decode(scroll_wire), std::runtime_error);

    SessionScrollbackReplyMsg reply;
    auto reply_wire = encode(Message{reply}, CONTROL_STREAM_ID, true);
    header = frame_header_layout(reply_wire.data(), reply_wire.size()).first;
    write_u32be(reply_wire.data() + header + 4 + 1 + 8 + 1, 65537);
    REQUIRE_THROWS_AS(decode(reply_wire), std::runtime_error);

    for (const auto& message : {Message{input}, Message{SessionInputReplyMsg{}}, Message{scroll},
                               Message{reply}, Message{SessionKillMsg{}}, Message{SessionKillReplyMsg{}}}) {
        auto wire = encode(message, CONTROL_STREAM_ID, true);
        wire.pop_back();
        REQUIRE_THROWS_AS(decode(wire), std::runtime_error);
    }
    // Use incompressible deterministic bytes to exercise u32 frames, rather
    // than allowing a tiny compressed frame to hide the full-size boundary.
    uint32_t state = 0x12345678;
    for (size_t i = 0; i < 65536; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        input.data.push_back(static_cast<char>(state));
    }
    auto maximum = encode(Message{input}, CONTROL_STREAM_ID, true);
    REQUIRE((maximum[3] & FLAG_LENGTH_U32) != 0);
    REQUIRE(std::get<SessionInputMsg>(decode(maximum)) == input);
    REQUIRE_THROWS_AS(encode(Message{input}, CONTROL_STREAM_ID, false), std::runtime_error);
    reply.ok = true; reply.data = input.data;
    REQUIRE(panel_roundtrip(reply) == reply);
}

TEST_CASE("panel uses existing TLS pin certificate and Hello identity binding", "[panel][auth]") {
    REQUIRE(verify_outbound_peer_identity("key-a", "key-a", "key-a", "peer-a", "peer-a", true).ok);
    REQUIRE_FALSE(verify_outbound_peer_identity("key-a", "key-b", "key-b", "peer-a", "peer-a", true).ok);
    REQUIRE_FALSE(verify_outbound_peer_identity("key-a", "key-a", "key-b", "peer-a", "peer-a", true).ok);
    REQUIRE_FALSE(verify_outbound_peer_identity("", "key-a", "key-a", "peer-a", "peer-a", true).ok);
    MeshConfig config;
    config.seeds.push_back(PeerEntry{.name="peer-a", .addr="127.0.0.1:1", .pubkey_hex="key-a"});
    REQUIRE(verify_inbound_peer_identity(config, "key-a", "key-a", "peer-a").ok);
    REQUIRE_FALSE(verify_inbound_peer_identity(config, "key-b", "key-b", "peer-a").ok);
}
