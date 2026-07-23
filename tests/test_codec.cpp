#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// Helper: round-trip encode→decode a message, verify type and content
template<typename T>
T roundtrip(const T& msg, uint16_t stream_id = 0) {
    Message m = msg;
    auto wire = encode(m, stream_id);
    auto decoded = decode(wire);
    // Verify type mapping
    REQUIRE(message_type(m) == message_type(decoded));
    // Verify stream_id in frame
    REQUIRE(wire.size() >= FRAME_HEADER_SIZE);
    uint16_t sid = (static_cast<uint16_t>(wire[0]) << 8) | wire[1];
    REQUIRE(sid == stream_id);
    return std::get<T>(decoded);
}

// ── Round-trip tests for EVERY message type ────────────────────

TEST_CASE("Round-trip: KeystrokeMsg", "[codec][roundtrip]") {
    KeystrokeMsg m;
    m.data = "hello world";
    auto m2 = roundtrip(m);
    REQUIRE(m2.data == m.data);
}

TEST_CASE("Round-trip: OutputMsg", "[codec][roundtrip]") {
    OutputMsg m;
    m.data = "ansi output text";
    auto m2 = roundtrip(m);
    REQUIRE(m2.data == m.data);
}

TEST_CASE("Round-trip: ResizeMsg", "[codec][roundtrip]") {
    ResizeMsg m;
    m.cols = 120;
    m.rows = 40;
    auto m2 = roundtrip(m);
    REQUIRE(m2.cols == m.cols);
    REQUIRE(m2.rows == m.rows);
}

TEST_CASE("Round-trip: ClipboardMsg", "[codec][roundtrip]") {
    ClipboardMsg m;
    m.hash = "abc123def456";
    m.text = "clipboard contents here";
    auto m2 = roundtrip(m);
    REQUIRE(m2.hash == m.hash);
    REQUIRE(m2.text == m.text);
}

TEST_CASE("Round-trip: ClipboardEchoMsg", "[codec][roundtrip]") {
    ClipboardEchoMsg m;
    m.hash = "abcdef0123456789abcdef0123456789abcdef01";
    auto m2 = roundtrip(m);
    REQUIRE(m2.hash == m.hash);
}

TEST_CASE("Round-trip: AttachMsg", "[codec][roundtrip]") {
    AttachMsg m;
    m.cols = 120;
    m.rows = 40;
    m.term = "xterm-256color";
    m.session_name = "my-session";
    auto m2 = roundtrip(m);
    REQUIRE(m2.cols == m.cols);
    REQUIRE(m2.rows == m.rows);
    REQUIRE(m2.term == m.term);
    REQUIRE(m2.session_name == m.session_name);
}

TEST_CASE("Round-trip: DetachMsg", "[codec][roundtrip]") {
    DetachMsg m;
    auto m2 = roundtrip(m);
    REQUIRE(m2 == m);
}

TEST_CASE("Round-trip: PingMsg", "[codec][roundtrip]") {
    PingMsg m;
    auto m2 = roundtrip(m);
    REQUIRE(m2 == m);
}

TEST_CASE("Round-trip: PongMsg", "[codec][roundtrip]") {
    PongMsg m;
    auto m2 = roundtrip(m);
    REQUIRE(m2 == m);
}

TEST_CASE("Round-trip: ScrollbackAckMsg", "[codec][roundtrip]") {
    ScrollbackAckMsg m;
    auto m2 = roundtrip(m);
    REQUIRE(m2 == m);
}

TEST_CASE("Round-trip: ImageAckMsg", "[codec][roundtrip]") {
    ImageAckMsg m;
    auto m2 = roundtrip(m);
    REQUIRE(m2 == m);
}

TEST_CASE("Round-trip: SessionListMsg", "[codec][roundtrip]") {
    SessionListMsg m;
    m.sessions.push_back(SessionInfo{"sess1", "attached", 3600});
    m.sessions.push_back(SessionInfo{"sess2", "detached", 7200});
    m.sessions.push_back(SessionInfo{"sess3", "died", 0});
    auto m2 = roundtrip(m);
    REQUIRE(m2.sessions.size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        REQUIRE(m2.sessions[i].name == m.sessions[i].name);
        REQUIRE(m2.sessions[i].state == m.sessions[i].state);
        REQUIRE(m2.sessions[i].uptime_seconds == m.sessions[i].uptime_seconds);
    }
}

TEST_CASE("Round-trip: ServerInfoMsg", "[codec][roundtrip]") {
    ServerInfoMsg m;
    m.hostname = "myhost";
    m.version = "1.2.3";
    m.load = 0.75;
    auto m2 = roundtrip(m);
    REQUIRE(m2.hostname == m.hostname);
    REQUIRE(m2.version == m.version);
    REQUIRE(m2.load == m.load);
}

TEST_CASE("Round-trip: ScrollbackMsg", "[codec][roundtrip]") {
    ScrollbackMsg m;
    m.total_lines = 5000;
    m.chunk_index = 7;
    m.data = "scrollback data here";
    auto m2 = roundtrip(m);
    REQUIRE(m2.total_lines == m.total_lines);
    REQUIRE(m2.chunk_index == m.chunk_index);
    REQUIRE(m2.data == m.data);
}

TEST_CASE("Round-trip: SignalMsg", "[codec][roundtrip]") {
    SECTION("CtrlC") {
        SignalMsg m;
        m.signal = SignalMsg::SignalType::CtrlC;
        auto m2 = roundtrip(m);
        REQUIRE(m2.signal == SignalMsg::SignalType::CtrlC);
    }
    SECTION("CtrlZ") {
        SignalMsg m;
        m.signal = SignalMsg::SignalType::CtrlZ;
        auto m2 = roundtrip(m);
        REQUIRE(m2.signal == SignalMsg::SignalType::CtrlZ);
    }
    SECTION("CtrlBackslash") {
        SignalMsg m;
        m.signal = SignalMsg::SignalType::CtrlBackslash;
        auto m2 = roundtrip(m);
        REQUIRE(m2.signal == SignalMsg::SignalType::CtrlBackslash);
    }
}

TEST_CASE("Round-trip: ExitCodeMsg", "[codec][roundtrip]") {
    ExitCodeMsg m;
    m.code = 42;
    auto m2 = roundtrip(m);
    REQUIRE(m2.code == m.code);

    ExitCodeMsg m3;
    m3.code = -1;
    auto m4 = roundtrip(m3);
    REQUIRE(m4.code == m3.code);
}

TEST_CASE("Round-trip: SessionDiedMsg", "[codec][roundtrip]") {
    SessionDiedMsg m;
    m.exit_code = 1;
    m.signal_num = 9;
    auto m2 = roundtrip(m);
    REQUIRE(m2.exit_code == m.exit_code);
    REQUIRE(m2.signal_num == m.signal_num);
}

TEST_CASE("Round-trip: ImageDataMsg", "[codec][roundtrip]") {
    ImageDataMsg m;
    m.format = 0; // PNG
    m.name = "screenshot.png";
    m.data = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    auto m2 = roundtrip(m);
    REQUIRE(m2.format == m.format);
    REQUIRE(m2.name == m.name);
    REQUIRE(m2.data == m.data);
}

TEST_CASE("Round-trip: ImageFrameMsg", "[codec][roundtrip]") {
    ImageFrameMsg m;
    m.format = 2; // GIF
    m.delay_ms = 100;
    m.loop_count = 3;
    m.data = {0x47, 0x49, 0x46, 0x38, 0x39, 0x61};
    auto m2 = roundtrip(m);
    REQUIRE(m2.format == m.format);
    REQUIRE(m2.delay_ms == m.delay_ms);
    REQUIRE(m2.loop_count == m.loop_count);
    REQUIRE(m2.data == m.data);
}

TEST_CASE("Round-trip: HelloMsg", "[codec][roundtrip]") {
    SECTION("basic HelloMsg") {
        HelloMsg m;
        m.node_name = "shadow";
        m.version = "1.0.0";
        m.pubkey_hex = "abc123def456";
        auto m2 = roundtrip(m);
        REQUIRE(m2.node_name == m.node_name);
        REQUIRE(m2.version == m.version);
        REQUIRE(m2.pubkey_hex == m.pubkey_hex);
        REQUIRE(m2.known_peers.empty());
    }
    SECTION("HelloMsg with multiple peers") {
        HelloMsg m;
        m.node_name = "shadow";
        m.version = "2.0.0";
        m.pubkey_hex = "key123";
        m.known_peers.push_back(PeerInfo{"test-pc1", "203.0.113.10:19948", "key-test-pc1", 1000000});
        m.known_peers.push_back(PeerInfo{"node2", "10.0.0.2:9999", "key-node2", 2000000});
        auto m2 = roundtrip(m);
        REQUIRE(m2 == m);
    }
    SECTION("HelloMsg with empty known_peers") {
        HelloMsg m;
        m.node_name = "lone-node";
        m.version = "0.1.0";
        m.pubkey_hex = "loner-key";
        auto m2 = roundtrip(m);
        REQUIRE(m2 == m);
    }
}

TEST_CASE("Round-trip: GossipMsg", "[codec][roundtrip]") {
    SECTION("basic GossipMsg") {
        GossipMsg m;
        m.peers.push_back(PeerInfo{"shadow", "203.0.113.10:19948", "abc123", 1000});
        m.peers.push_back(PeerInfo{"test-pc1", "203.0.113.11:19948", "def456", 2000});
        auto m2 = roundtrip(m);
        REQUIRE(m2 == m);
    }
    SECTION("GossipMsg empty") {
        GossipMsg m;
        auto m2 = roundtrip(m);
        REQUIRE(m2 == m);
    }
}

TEST_CASE("Compression threshold", "[codec][compression]") {
    // Payload <= 256: no FLAG_COMPRESSED
    KeystrokeMsg m;
    m.data = std::string(250, 'x');
    auto wire_small = encode(m, 0);
    REQUIRE((wire_small[3] & FLAG_COMPRESSED) == 0);

    // Payload > 256: FLAG_COMPRESSED set
    m.data = std::string(300, 'y');
    auto wire_big = encode(m, 300);
    REQUIRE((wire_big[3] & FLAG_COMPRESSED) != 0);

    // Both should round-trip correctly
    auto decoded_small = decode(wire_small);
    REQUIRE(std::get<KeystrokeMsg>(decoded_small).data == std::string(250, 'x'));

    auto decoded_big = decode(wire_big);
    REQUIRE(std::get<KeystrokeMsg>(decoded_big).data == std::string(300, 'y'));
}

TEST_CASE("Truncated frames throw", "[codec][error]") {
    // Empty data
    REQUIRE_THROWS_AS(decode(std::span<const uint8_t>{}), std::runtime_error);
    // Too short for header
    REQUIRE_THROWS_AS(decode(std::span<const uint8_t>(std::vector<uint8_t>(4, 0))), std::runtime_error);
    // Header enough but length says more than available
    std::vector<uint8_t> truncated(FRAME_HEADER_SIZE + 10, 0);
    truncated[0] = 0; truncated[1] = 0; // stream_id
    truncated[2] = 0x01; // Keystroke
    truncated[3] = 0;    // no flags
    truncated[4] = 0xFF; // length
    truncated[5] = 0xFF; // = 65535, way more than available
    REQUIRE_THROWS_AS(decode(truncated), std::runtime_error);
}

TEST_CASE("Malformed / unknown message type throws", "[codec][error]") {
    // Make a valid-looking frame with unknown type
    std::vector<uint8_t> bad(FRAME_HEADER_SIZE + 1, 0);
    bad[0] = 0; bad[1] = 0; // stream_id
    bad[2] = 0xFF; // unknown type byte
    bad[3] = 0;
    bad[4] = 0; bad[5] = 1; // length=1
    bad[6] = 0; // payload byte
    REQUIRE_THROWS_AS(decode(bad), std::runtime_error);
}

TEST_CASE("SHA-256 known vector", "[codec][sha256]") {
    std::string result = sha256_hex("abc");
    REQUIRE(result == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("Image payloads > 50MB cap rejected", "[codec][image]") {
    ImageDataMsg m;
    m.format = 0;
    m.name = "huge.png";
    m.data.resize(MAX_IMAGE_BYTES + 1, 0x00);
    REQUIRE_THROWS_AS(encode(m, 0), std::runtime_error);
}

TEST_CASE("Empty frame data round-trips", "[codec][empty]") {
    // Empty messages that have no fields should encode/decode fine
    auto wire_ping = encode(PingMsg{}, 0);
    auto decoded_ping = decode(wire_ping);
    REQUIRE(std::holds_alternative<PingMsg>(decoded_ping));

    auto wire_pong = encode(PongMsg{}, 0);
    auto decoded_pong = decode(wire_pong);
    REQUIRE(std::holds_alternative<PongMsg>(decoded_pong));

    auto wire_detach = encode(DetachMsg{}, 0);
    auto decoded_detach = decode(wire_detach);
    REQUIRE(std::holds_alternative<DetachMsg>(decoded_detach));

    auto wire_ack = encode(ScrollbackAckMsg{}, 0);
    auto decoded_ack = decode(wire_ack);
    REQUIRE(std::holds_alternative<ScrollbackAckMsg>(decoded_ack));

    auto wire_ia = encode(ImageAckMsg{}, 0);
    auto decoded_ia = decode(wire_ia);
    REQUIRE(std::holds_alternative<ImageAckMsg>(decoded_ia));
}

TEST_CASE("Control flag is set for CONTROL_STREAM_ID", "[codec][control]") {
    auto wire_ping = encode(PingMsg{}, CONTROL_STREAM_ID);
    REQUIRE((wire_ping[3] & FLAG_CONTROL) != 0);

    auto wire_ping2 = encode(PingMsg{}, 42); // non-control stream
    REQUIRE((wire_ping2[3] & FLAG_CONTROL) == 0);
}

TEST_CASE("max_encoded_size returns MAX_FRAME_SIZE", "[codec]") {
    REQUIRE(max_encoded_size(PingMsg{}) == MAX_FRAME_SIZE);
    REQUIRE(max_encoded_size(KeystrokeMsg{}) == MAX_FRAME_SIZE);
    REQUIRE(max_encoded_size(HelloMsg{}) == MAX_FRAME_SIZE);
}
