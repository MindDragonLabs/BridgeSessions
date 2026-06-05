#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "bsprotocol/codec.hpp"
#include <string>

using namespace bs::protocol;

// Helper: encode → decode roundtrip
template<typename T>
T roundtrip(const T& msg, uint16_t stream_id = 1) {
    auto frame = encode(msg, stream_id);
    auto decoded = decode(frame);
    return std::get<T>(decoded);
}

TEST_CASE("encode/decode roundtrip — every message type", "[codec]") {
    SECTION("KeystrokeMsg") {
        KeystrokeMsg m{"hello keystroke data"};
        auto m2 = roundtrip(m);
        REQUIRE(m2.data == m.data);
    }
    SECTION("OutputMsg — plain text") {
        OutputMsg m{"terminal output line\n"};
        auto m2 = roundtrip(m);
        REQUIRE(m2.data == m.data);
    }
    SECTION("OutputMsg — ANSI escape sequences") {
        OutputMsg m{"\x1b[32mgreen text\x1b[0m\n\x1b[1mbold\x1b[0m"};
        auto m2 = roundtrip(m);
        REQUIRE(m2.data == m.data);
    }
    SECTION("ResizeMsg") {
        ResizeMsg m{120, 40};
        auto m2 = roundtrip(m);
        REQUIRE(m2.cols == m.cols);
        REQUIRE(m2.rows == m.rows);
    }
    SECTION("ClipboardMsg") {
        ClipboardMsg m{"clipboard text content", "abc123def456"};
        auto m2 = roundtrip(m);
        REQUIRE(m2.text == m.text);
        REQUIRE(m2.hash == m.hash);
    }
    SECTION("ClipboardMsg — large text") {
        ClipboardMsg m{std::string(500, 'x'), "hash42"};
        auto m2 = roundtrip(m);
        REQUIRE(m2.text == m.text);
        REQUIRE(m2.hash == m.hash);
    }
    SECTION("ClipboardEchoMsg") {
        ClipboardEchoMsg m{"echoed_hash_value"};
        auto m2 = roundtrip(m);
        REQUIRE(m2.hash == m.hash);
    }
    SECTION("AttachMsg") {
        AttachMsg m{"hms", 120, 40, "xterm-kitty"};
        auto m2 = roundtrip(m);
        REQUIRE(m2.session_name == m.session_name);
        REQUIRE(m2.cols == m.cols);
        REQUIRE(m2.rows == m.rows);
        REQUIRE(m2.term == m.term);
    }
    SECTION("AttachMsg — long session name") {
        AttachMsg m{"my-very-long-session-name-that-goes-on-forever", 80, 24, "xterm-256color"};
        auto m2 = roundtrip(m);
        REQUIRE(m2.session_name == m.session_name);
    }
    SECTION("DetachMsg") { auto m2 = roundtrip(DetachMsg{}); REQUIRE(m2 == DetachMsg{}); }
    SECTION("PingMsg")    { auto m2 = roundtrip(PingMsg{});    REQUIRE(m2 == PingMsg{}); }
    SECTION("PongMsg")    { auto m2 = roundtrip(PongMsg{});    REQUIRE(m2 == PongMsg{}); }
    SECTION("ScrollbackAckMsg") { auto m2 = roundtrip(ScrollbackAckMsg{}); REQUIRE(m2 == ScrollbackAckMsg{}); }

    SECTION("SessionListMsg") {
        SessionListMsg m;
        m.sessions.push_back({"hms", "attached", 3600});
        m.sessions.push_back({"work", "detached", 7200});
        m.sessions.push_back({"logs", "died", 100});
        auto m2 = roundtrip(m);
        REQUIRE(m2.sessions.size() == 3);
        REQUIRE(m2.sessions[0].name == "hms");
        REQUIRE(m2.sessions[0].state == "attached");
        REQUIRE(m2.sessions[0].uptime_seconds == 3600);
        REQUIRE(m2.sessions[1].name == "work");
        REQUIRE(m2.sessions[2].name == "logs");
    }
    SECTION("ServerInfoMsg") {
        ServerInfoMsg m{"dev.example.com", "1.0.0", 0.75};
        auto m2 = roundtrip(m);
        REQUIRE(m2.hostname == m.hostname);
        REQUIRE(m2.version == m.version);
        REQUIRE(m2.load == m.load);
    }
    SECTION("ScrollbackMsg") {
        ScrollbackMsg m{"scrollback data chunk\nline 2\n", 2000, 3};
        auto m2 = roundtrip(m);
        REQUIRE(m2.data == m.data);
        REQUIRE(m2.total_lines == m.total_lines);
        REQUIRE(m2.chunk_index == m.chunk_index);
    }
    SECTION("SignalMsg — CtrlC") {
        SignalMsg m{SignalMsg::SignalType::CtrlC};
        auto m2 = roundtrip(m);
        REQUIRE(m2.signal == m.signal);
    }
    SECTION("SignalMsg — CtrlZ") {
        SignalMsg m{SignalMsg::SignalType::CtrlZ};
        auto m2 = roundtrip(m);
        REQUIRE(m2.signal == m.signal);
    }
    SECTION("ExitCodeMsg") {
        ExitCodeMsg m{42};
        auto m2 = roundtrip(m);
        REQUIRE(m2.code == m.code);
    }
    SECTION("ExitCodeMsg — negative") {
        ExitCodeMsg m{-1};
        auto m2 = roundtrip(m);
        REQUIRE(m2.code == m.code);
    }
    SECTION("SessionDiedMsg") {
        SessionDiedMsg m{137, 9};  // exit 137, SIGKILL
        auto m2 = roundtrip(m);
        REQUIRE(m2.exit_code == m.exit_code);
        REQUIRE(m2.signal_num == m.signal_num);
    }
    SECTION("ImageDataMsg") {
        ImageDataMsg m;
        m.format = 0;
        m.name = "screenshot.png";
        m.data.assign({0x89, 0x50, 0x4E, 0x47});
        auto m2 = roundtrip(m);
        REQUIRE(m2.format == m.format);
        REQUIRE(m2.name == m.name);
        REQUIRE(m2.data == m.data);
    }
    SECTION("ImageFrameMsg") {
        ImageFrameMsg m;
        m.format = 2;
        m.delay_ms = 42;
        m.loop_count = 7;
        m.data.assign({0x47, 0x49, 0x46, 0x38});
        auto m2 = roundtrip(m);
        REQUIRE(m2.format == m.format);
        REQUIRE(m2.delay_ms == m.delay_ms);
        REQUIRE(m2.loop_count == m.loop_count);
        REQUIRE(m2.data == m.data);
    }
    SECTION("ImageAckMsg") { auto m2 = roundtrip(ImageAckMsg{}); REQUIRE(m2 == ImageAckMsg{}); }
}

TEST_CASE("compression roundtrip — ANSI-heavy terminal output", "[codec][compression]") {
    // Build a realistic terminal frame: lots of ANSI escape sequences
    std::string ansi;
    for (int i = 0; i < 100; ++i) {
        ansi += "\x1b[32mline " + std::to_string(i) + " \x1b[1mbold\x1b[0m "
              + std::string(20, 'x') + "\n";
    }
    auto frame = encode(OutputMsg{ansi}, 5);
    // Must have compression flag set
    REQUIRE((frame[3] & FLAG_COMPRESSED) != 0);
    // Decode should match exactly
    auto msg = decode(frame);
    auto& out = std::get<OutputMsg>(msg);
    REQUIRE(out.data == ansi);
}

TEST_CASE("zstd compression actually reduces size", "[codec][compression]") {
    std::string big(2000, 'A');  // highly compressible
    auto frame = encode(OutputMsg{big}, 1);
    REQUIRE(frame.size() < big.size() + FRAME_HEADER_SIZE);  // must be smaller than raw
    // Decompressed content matches
    auto msg = decode(frame);
    REQUIRE(std::get<OutputMsg>(msg).data == big);
}

TEST_CASE("image payloads compress and respect size caps", "[codec][compression][security]") {
    std::string big(5000, 'Z');

    ImageDataMsg data;
    data.format = 0;
    data.name = "big.png";
    data.data.assign(big.begin(), big.end());
    auto data_frame = encode(data, 1);
    REQUIRE((data_frame[3] & FLAG_COMPRESSED) != 0);
    auto data_msg = decode(data_frame);
    REQUIRE(std::get<ImageDataMsg>(data_msg).format == data.format);
    REQUIRE(std::get<ImageDataMsg>(data_msg).name == data.name);
    REQUIRE(std::get<ImageDataMsg>(data_msg).data == data.data);

    ImageFrameMsg frame;
    frame.format = 2;
    frame.delay_ms = 33;
    frame.loop_count = 0;
    frame.data.assign(big.begin(), big.end());
    auto anim_frame = encode(frame, 1);
    REQUIRE((anim_frame[3] & FLAG_COMPRESSED) != 0);
    auto anim_msg = decode(anim_frame);
    REQUIRE(std::get<ImageFrameMsg>(anim_msg).format == frame.format);
    REQUIRE(std::get<ImageFrameMsg>(anim_msg).data == frame.data);
    REQUIRE(std::get<ImageFrameMsg>(anim_msg).delay_ms == frame.delay_ms);
    REQUIRE(std::get<ImageFrameMsg>(anim_msg).loop_count == frame.loop_count);

    ImageDataMsg too_big_data;
    too_big_data.name = "too-big.png";
    too_big_data.data = std::vector<uint8_t>(MAX_IMAGE_BYTES + 1, 0xAA);
    REQUIRE_THROWS(encode(too_big_data, 1));

    ImageFrameMsg too_big_frame;
    too_big_frame.data = std::vector<uint8_t>(MAX_IMAGE_BYTES + 1, 0xAA);
    REQUIRE_THROWS(encode(too_big_frame, 1));
}

TEST_CASE("small frames skip compression", "[codec]") {
    KeystrokeMsg m{"k"};  // 1 byte — well under threshold
    auto frame = encode(m, 1);
    REQUIRE((frame[3] & FLAG_COMPRESSED) == 0);
}

TEST_CASE("oversized logical payload is rejected before compression", "[codec][security]") {
    OutputMsg m{std::string(MAX_FRAME_SIZE + 1, 'A')};
    REQUIRE_THROWS(encode(m, 1));
}

TEST_CASE("u8-prefixed strings reject overflow", "[codec][security]") {
    AttachMsg m{std::string(256, 's'), 80, 24, "xterm-256color"};
    REQUIRE_THROWS(encode(m, 1));
}

TEST_CASE("control flag is set for stream 0", "[codec]") {
    auto frame = encode(PingMsg{}, CONTROL_STREAM_ID);
    REQUIRE((frame[3] & FLAG_CONTROL) != 0);
}

TEST_CASE("control flag not set for regular stream", "[codec]") {
    auto frame = encode(PingMsg{}, 42);
    REQUIRE((frame[3] & FLAG_CONTROL) == 0);
}

TEST_CASE("edge cases", "[codec]") {
    SECTION("empty Keystroke") {
        KeystrokeMsg m{""};
        auto m2 = roundtrip(m);
        REQUIRE(m2.data.empty());
    }
    SECTION("empty Output") {
        OutputMsg m{""};
        auto m2 = roundtrip(m);
        REQUIRE(m2.data.empty());
    }
    SECTION("empty SessionList") {
        SessionListMsg m;
        auto m2 = roundtrip(m);
        REQUIRE(m2.sessions.empty());
    }
    SECTION("empty ImageData") {
        ImageDataMsg m;
        m.name = "empty.png";
        auto m2 = roundtrip(m);
        REQUIRE(m2.data.empty());
        REQUIRE(m2.name == m.name);
    }
    SECTION("empty ImageFrame") {
        ImageFrameMsg m;
        auto m2 = roundtrip(m);
        REQUIRE(m2.data.empty());
        REQUIRE(m2.delay_ms == 0);
        REQUIRE(m2.loop_count == 0);
    }
    SECTION("max stream_id") {
        auto frame = encode(PingMsg{}, 65535);
        REQUIRE(frame[0] == 0xFF);
        REQUIRE(frame[1] == 0xFF);
    }
    SECTION("stream_id 0 is control") {
        auto frame = encode(PingMsg{}, 0);
        REQUIRE(frame[0] == 0x00);
        REQUIRE(frame[1] == 0x00);
    }
}

TEST_CASE("error handling", "[codec]") {
    SECTION("frame too short") {
        std::vector<uint8_t> short_frame{0x00, 0x01, 0x02, 0x03, 0x04}; // only 5 bytes
        REQUIRE_THROWS(decode(short_frame));
    }
    SECTION("truncated payload") {
        // Header says length=100 but only 10 bytes follow
        std::vector<uint8_t> frame(FRAME_HEADER_SIZE + 10, 0);
        frame[0] = 0; frame[1] = 1;  // stream_id = 1
        frame[2] = 0x01;  // Keystroke
        frame[3] = 0;
        frame[4] = 0; frame[5] = 100;  // length = 100
        REQUIRE_THROWS(decode(frame));
    }
    SECTION("unknown message type") {
        std::vector<uint8_t> frame(FRAME_HEADER_SIZE, 0);
        frame[0] = 0; frame[1] = 1;  // stream_id = 1
        frame[2] = 0xFF;  // invalid type
        frame[3] = 0;
        frame[4] = 0; frame[5] = 0;  // length = 0
        REQUIRE_THROWS(decode(frame));
    }
    SECTION("empty frame") {
        std::vector<uint8_t> empty;
        REQUIRE_THROWS(decode(empty));
    }
}

TEST_CASE("compression survives garbage collection", "[codec][compression]") {
    // Encode + decode 100 times — no memory corruption from ZSTD context reuse
    for (int i = 0; i < 100; ++i) {
        std::string data = "iteration " + std::to_string(i) + std::string(500, 'B');
        auto frame = encode(OutputMsg{data}, static_cast<uint16_t>(i));
        auto msg = decode(frame);
        REQUIRE(std::get<OutputMsg>(msg).data == data);
    }
}
