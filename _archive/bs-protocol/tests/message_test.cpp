#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "bsprotocol/message.hpp"
#include "bsprotocol/codec.hpp"

using namespace bs::protocol;

TEST_CASE("Message variant holds correct types", "[message]") {
    SECTION("simple messages construct") {
        KeystrokeMsg k; k.data = "hello";
        OutputMsg    o; o.data = "world";
        DetachMsg    d;
        PingMsg      p;
        PongMsg      pg;
        ScrollbackAckMsg sa;
        ImageDataMsg id;
        ImageFrameMsg ifr;
        ImageAckMsg ia;
    }

    SECTION("all 19 types are constructible via variant") {
        Message m1 = KeystrokeMsg{};
        Message m2 = OutputMsg{};
        Message m3 = ResizeMsg{};
        Message m4 = ClipboardMsg{};
        Message m5 = ClipboardEchoMsg{};
        Message m6 = AttachMsg{};
        Message m7 = DetachMsg{};
        Message m8 = SessionListMsg{};
        Message m9 = ServerInfoMsg{};
        Message m10 = PingMsg{};
        Message m11 = PongMsg{};
        Message m12 = ScrollbackMsg{};
        Message m13 = SignalMsg{};
        Message m14 = ExitCodeMsg{};
        Message m15 = ScrollbackAckMsg{};
        Message m16 = SessionDiedMsg{};
        Message m17 = ImageDataMsg{};
        Message m18 = ImageFrameMsg{};
        Message m19 = ImageAckMsg{};
        // Access checks
        REQUIRE(std::get<KeystrokeMsg>(m1).data.empty());
        REQUIRE(std::get<ResizeMsg>(m3).cols == 0);
        REQUIRE(std::get<DetachMsg>(m7) == DetachMsg{});
    }
}

TEST_CASE("message_type maps variant index to enum", "[message]") {
    REQUIRE(message_type(KeystrokeMsg{})      == MessageType::Keystroke);
    REQUIRE(message_type(OutputMsg{})         == MessageType::Output);
    REQUIRE(message_type(ResizeMsg{})         == MessageType::Resize);
    REQUIRE(message_type(ClipboardMsg{})      == MessageType::ClipboardGet);
    REQUIRE(message_type(ClipboardEchoMsg{}) == MessageType::ClipboardEcho);
    REQUIRE(message_type(AttachMsg{})         == MessageType::Attach);
    REQUIRE(message_type(DetachMsg{})         == MessageType::Detach);
    REQUIRE(message_type(SessionListMsg{})    == MessageType::SessionList);
    REQUIRE(message_type(ServerInfoMsg{})     == MessageType::ServerInfo);
    REQUIRE(message_type(PingMsg{})           == MessageType::Ping);
    REQUIRE(message_type(PongMsg{})           == MessageType::Pong);
    REQUIRE(message_type(ScrollbackMsg{})     == MessageType::Scrollback);
    REQUIRE(message_type(SignalMsg{})         == MessageType::Signal);
    REQUIRE(message_type(ExitCodeMsg{})       == MessageType::ProcExited);
    REQUIRE(message_type(ScrollbackAckMsg{})  == MessageType::ScrollbackAck);
    REQUIRE(message_type(SessionDiedMsg{})    == MessageType::SessionDied);
    REQUIRE(message_type(ImageDataMsg{})      == MessageType::ImageData);
    REQUIRE(message_type(ImageFrameMsg{})     == MessageType::ImageFrame);
    REQUIRE(message_type(ImageAckMsg{})       == MessageType::ImageAck);
}

TEST_CASE("struct defaults are sensible", "[message]") {
    AttachMsg a;
    REQUIRE(a.cols == 80);
    REQUIRE(a.rows == 24);
    REQUIRE(a.term == "xterm-256color");
    REQUIRE(a.session_name.empty());

    ResizeMsg r;
    REQUIRE(r.cols == 0);
    REQUIRE(r.rows == 0);

    SignalMsg s;
    REQUIRE(s.signal == SignalMsg::SignalType::CtrlC);

    ExitCodeMsg e;
    REQUIRE(e.code == 0);

    SessionDiedMsg d;
    REQUIRE(d.exit_code == 0);
    REQUIRE(d.signal_num == 0);

    ImageDataMsg id;
    REQUIRE(id.name.empty());
    REQUIRE(id.data.empty());

    ImageFrameMsg fr;
    REQUIRE(fr.delay_ms == 0);
    REQUIRE(fr.loop_count == 0);
    REQUIRE(fr.data.empty());
}

TEST_CASE("SessionInfo and SessionListMsg", "[message]") {
    SessionListMsg sl;
    REQUIRE(sl.sessions.empty());

    SessionInfo si{"hms", "attached", 3600};
    sl.sessions.push_back(si);
    REQUIRE(sl.sessions.size() == 1);
    REQUIRE(sl.sessions[0].name == "hms");
    REQUIRE(sl.sessions[0].state == "attached");
    REQUIRE(sl.sessions[0].uptime_seconds == 3600);
}

TEST_CASE("ScrollbackMsg fields", "[message]") {
    ScrollbackMsg s;
    s.data = "ansi output";
    s.total_lines = 2000;
    s.chunk_index = 3;
    REQUIRE(s.total_lines == 2000);
    REQUIRE(s.chunk_index == 3);
    REQUIRE(s.data == "ansi output");
}

TEST_CASE("ClipboardMsg carries text and hash", "[message]") {
    ClipboardMsg c;
    c.text = "clipboard content";
    c.hash = "abc123def456";
    REQUIRE(c.text == "clipboard content");
    REQUIRE(c.hash == "abc123def456");
}
