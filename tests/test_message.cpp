#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bridgesessions.cpp"

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

TEST_CASE("Message variant holds correct types", "[message]") {
    SECTION("all 21 types are constructible via variant") {
        Message m0  = KeystrokeMsg{};
        Message m1  = OutputMsg{};
        Message m2  = ResizeMsg{};
        Message m3  = ClipboardMsg{};
        Message m4  = ClipboardEchoMsg{};
        Message m5  = AttachMsg{};
        Message m6  = DetachMsg{};
        Message m7  = SessionListMsg{};
        Message m8  = ServerInfoMsg{};
        Message m9  = PingMsg{};
        Message m10 = PongMsg{};
        Message m11 = ScrollbackMsg{};
        Message m12 = SignalMsg{};
        Message m13 = ExitCodeMsg{};
        Message m14 = ScrollbackAckMsg{};
        Message m15 = SessionDiedMsg{};
        Message m16 = ImageDataMsg{};
        Message m17 = ImageFrameMsg{};
        Message m18 = ImageAckMsg{};
        Message m19 = HelloMsg{};
        Message m20 = GossipMsg{};

        // Access checks on original types
        REQUIRE(std::get<KeystrokeMsg>(m0).data.empty());
        REQUIRE(std::get<ResizeMsg>(m2).cols == 0);
        REQUIRE(std::get<DetachMsg>(m6) == DetachMsg{});
    }

    SECTION("all 21 types are held via std::holds_alternative") {
        Message mk = KeystrokeMsg{};
        Message mo = OutputMsg{};
        Message mr = ResizeMsg{};
        Message mc = ClipboardMsg{};
        Message mce = ClipboardEchoMsg{};
        Message ma = AttachMsg{};
        Message md = DetachMsg{};
        Message msl = SessionListMsg{};
        Message msi = ServerInfoMsg{};
        Message mp = PingMsg{};
        Message mpg = PongMsg{};
        Message msb = ScrollbackMsg{};
        Message msg = SignalMsg{};
        Message me = ExitCodeMsg{};
        Message msa = ScrollbackAckMsg{};
        Message msd = SessionDiedMsg{};
        Message mid = ImageDataMsg{};
        Message mif = ImageFrameMsg{};
        Message mia = ImageAckMsg{};
        Message mh = HelloMsg{};
        Message mg = GossipMsg{};

        REQUIRE(std::holds_alternative<KeystrokeMsg>(mk));
        REQUIRE(std::holds_alternative<OutputMsg>(mo));
        REQUIRE(std::holds_alternative<ResizeMsg>(mr));
        REQUIRE(std::holds_alternative<ClipboardMsg>(mc));
        REQUIRE(std::holds_alternative<ClipboardEchoMsg>(mce));
        REQUIRE(std::holds_alternative<AttachMsg>(ma));
        REQUIRE(std::holds_alternative<DetachMsg>(md));
        REQUIRE(std::holds_alternative<SessionListMsg>(msl));
        REQUIRE(std::holds_alternative<ServerInfoMsg>(msi));
        REQUIRE(std::holds_alternative<PingMsg>(mp));
        REQUIRE(std::holds_alternative<PongMsg>(mpg));
        REQUIRE(std::holds_alternative<ScrollbackMsg>(msb));
        REQUIRE(std::holds_alternative<SignalMsg>(msg));
        REQUIRE(std::holds_alternative<ExitCodeMsg>(me));
        REQUIRE(std::holds_alternative<ScrollbackAckMsg>(msa));
        REQUIRE(std::holds_alternative<SessionDiedMsg>(msd));
        REQUIRE(std::holds_alternative<ImageDataMsg>(mid));
        REQUIRE(std::holds_alternative<ImageFrameMsg>(mif));
        REQUIRE(std::holds_alternative<ImageAckMsg>(mia));
        REQUIRE(std::holds_alternative<HelloMsg>(mh));
        REQUIRE(std::holds_alternative<GossipMsg>(mg));
    }
}

TEST_CASE("message_type maps variant index to enum byte", "[message]") {
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
    REQUIRE(message_type(HelloMsg{})          == MessageType::Hello);
    REQUIRE(message_type(GossipMsg{})         == MessageType::Gossip);
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

TEST_CASE("HelloMsg and GossipMsg defaults and equality", "[message]") {
    SECTION("HelloMsg defaults") {
        HelloMsg h;
        REQUIRE(h.node_name.empty());
        REQUIRE(h.version.empty());
        REQUIRE(h.pubkey_hex.empty());
        REQUIRE(h.known_peers.empty());
    }

    SECTION("GossipMsg defaults") {
        GossipMsg g;
        REQUIRE(g.peers.empty());
    }

    SECTION("HelloMsg equality") {
        HelloMsg h1;
        h1.node_name = "shadow";
        h1.version = "1.0.0";
        h1.pubkey_hex = "abc123";

        HelloMsg h2;
        h2.node_name = "shadow";
        h2.version = "1.0.0";
        h2.pubkey_hex = "abc123";

        REQUIRE(h1 == h2);
    }

    SECTION("GossipMsg equality") {
        GossipMsg g1;
        g1.peers.push_back(PeerInfo{"shadow", "203.0.113.10:19948", "abc123", 1000});

        GossipMsg g2;
        g2.peers.push_back(PeerInfo{"shadow", "203.0.113.10:19948", "abc123", 1000});

        REQUIRE(g1 == g2);
    }

    SECTION("PeerInfo equality and defaults") {
        PeerInfo p;
        REQUIRE(p.name.empty());
        REQUIRE(p.addr.empty());
        REQUIRE(p.pubkey_hex.empty());
        REQUIRE(p.last_seen == 0);

        PeerInfo p1{"node1", "addr1", "key1", 42};
        PeerInfo p2{"node1", "addr1", "key1", 42};
        REQUIRE(p1 == p2);

        PeerInfo p3{"node1", "addr1", "key1", 99};
        REQUIRE_FALSE(p1 == p3);
    }
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

TEST_CASE("Image types have operator==", "[message]") {
    ImageDataMsg id1;
    id1.format = 0;
    id1.name = "test.png";
    id1.data = {0x89, 0x50, 0x4E, 0x47};

    ImageDataMsg id2;
    id2.format = 0;
    id2.name = "test.png";
    id2.data = {0x89, 0x50, 0x4E, 0x47};

    REQUIRE(id1 == id2);

    id2.format = 1;
    REQUIRE_FALSE(id1 == id2);

    ImageFrameMsg ifr1;
    ifr1.format = 2;
    ifr1.delay_ms = 100;
    ifr1.loop_count = 0;
    ifr1.data = {0x47, 0x49, 0x46};

    ImageFrameMsg ifr2;
    ifr2.format = 2;
    ifr2.delay_ms = 100;
    ifr2.loop_count = 0;
    ifr2.data = {0x47, 0x49, 0x46};

    REQUIRE(ifr1 == ifr2);
}

TEST_CASE("Frame constants are accessible", "[message]") {
    REQUIRE(FRAME_HEADER_SIZE == 6);
    REQUIRE(MAX_FRAME_SIZE == 65535);
    REQUIRE(COMPRESSION_THRESHOLD == 256);
    REQUIRE(MAX_IMAGE_BYTES == 50ull * 1024ull * 1024ull);
    REQUIRE(CONTROL_STREAM_ID == 0);
    REQUIRE(FLAG_COMPRESSED == 0x01);
    REQUIRE(FLAG_CONTROL == 0x02);
}

TEST_CASE("MessageType enum values are correct", "[message]") {
    REQUIRE(static_cast<uint8_t>(MessageType::Keystroke)      == 0x01);
    REQUIRE(static_cast<uint8_t>(MessageType::Output)         == 0x02);
    REQUIRE(static_cast<uint8_t>(MessageType::Resize)         == 0x03);
    REQUIRE(static_cast<uint8_t>(MessageType::ClipboardGet)   == 0x04);
    REQUIRE(static_cast<uint8_t>(MessageType::ClipboardPut)   == 0x05);
    REQUIRE(static_cast<uint8_t>(MessageType::Attach)         == 0x06);
    REQUIRE(static_cast<uint8_t>(MessageType::Detach)         == 0x07);
    REQUIRE(static_cast<uint8_t>(MessageType::SessionList)    == 0x08);
    REQUIRE(static_cast<uint8_t>(MessageType::ServerInfo)     == 0x09);
    REQUIRE(static_cast<uint8_t>(MessageType::Ping)           == 0x0A);
    REQUIRE(static_cast<uint8_t>(MessageType::Pong)           == 0x0B);
    REQUIRE(static_cast<uint8_t>(MessageType::Scrollback)     == 0x0C);
    REQUIRE(static_cast<uint8_t>(MessageType::Signal)         == 0x0D);
    REQUIRE(static_cast<uint8_t>(MessageType::ProcExited)     == 0x0E);
    REQUIRE(static_cast<uint8_t>(MessageType::ScrollbackAck)  == 0x0F);
    REQUIRE(static_cast<uint8_t>(MessageType::SessionDied)    == 0x10);
    REQUIRE(static_cast<uint8_t>(MessageType::ClipboardEcho)  == 0x11);
    REQUIRE(static_cast<uint8_t>(MessageType::ImageData)      == 0x12);
    REQUIRE(static_cast<uint8_t>(MessageType::ImageFrame)     == 0x13);
    REQUIRE(static_cast<uint8_t>(MessageType::ImageAck)       == 0x14);
    REQUIRE(static_cast<uint8_t>(MessageType::Hello)          == 0x15);
    REQUIRE(static_cast<uint8_t>(MessageType::Gossip)         == 0x16);
    REQUIRE(static_cast<uint8_t>(MessageType::SessionSearch)  == 0x17);
    REQUIRE(static_cast<uint8_t>(MessageType::FileMeta)       == 0x1C);
    REQUIRE(static_cast<uint8_t>(MessageType::FileChunk)      == 0x1D);
    REQUIRE(static_cast<uint8_t>(MessageType::FileAck)        == 0x1E);
}

// ── File Transfer Message Tests (v1.5, P1) ──────────────────────

TEST_CASE("FileMetaMsg defaults and equality", "[message][file]") {
    SECTION("defaults") {
        FileMetaMsg m;
        REQUIRE(m.filename.empty());
        REQUIRE(m.filesize == 0);
        REQUIRE(m.checksum.empty());
        REQUIRE(m.total_chunks == 0);
    }

    SECTION("equality") {
        FileMetaMsg a;
        a.filename = "test.bin";
        a.filesize = 1024;
        a.checksum = "abc123";
        a.total_chunks = 4;

        FileMetaMsg b;
        b.filename = "test.bin";
        b.filesize = 1024;
        b.checksum = "abc123";
        b.total_chunks = 4;

        REQUIRE(a == b);
        b.filesize = 2048;
        REQUIRE_FALSE(a == b);
    }
}

TEST_CASE("FileChunkMsg defaults and equality", "[message][file]") {
    SECTION("defaults") {
        FileChunkMsg m;
        REQUIRE(m.chunk_index == 0);
        REQUIRE(m.total_chunks == 0);
        REQUIRE(m.data.empty());
    }

    SECTION("equality") {
        FileChunkMsg a;
        a.chunk_index = 2;
        a.total_chunks = 10;
        a.data = {0xDE, 0xAD, 0xBE, 0xEF};

        FileChunkMsg b;
        b.chunk_index = 2;
        b.total_chunks = 10;
        b.data = {0xDE, 0xAD, 0xBE, 0xEF};

        REQUIRE(a == b);
        b.data = {0x00};
        REQUIRE_FALSE(a == b);
    }

    SECTION("empty data") {
        FileChunkMsg m;
        m.chunk_index = 0;
        m.total_chunks = 1;
        REQUIRE(m.data.empty());
    }
}

TEST_CASE("FileAckMsg defaults and equality", "[message][file]") {
    SECTION("defaults") {
        FileAckMsg m;
        REQUIRE(m.chunk_index == 0);
        REQUIRE(m.next_requested == 0);
        REQUIRE(m.error == false);
        REQUIRE(m.error_msg.empty());
    }

    SECTION("error ack") {
        FileAckMsg a;
        a.chunk_index = 5;
        a.next_requested = 5;
        a.error = true;
        a.error_msg = "disk full";

        FileAckMsg b;
        b.chunk_index = 5;
        b.next_requested = 5;
        b.error = true;
        b.error_msg = "disk full";

        REQUIRE(a == b);
        REQUIRE_FALSE(a == FileAckMsg{});
    }
}

TEST_CASE("File message types are constructible via variant", "[message][file]") {
    Message m0 = FileMetaMsg{};
    Message m1 = FileChunkMsg{};
    Message m2 = FileAckMsg{};

    REQUIRE(std::holds_alternative<FileMetaMsg>(m0));
    REQUIRE(std::holds_alternative<FileChunkMsg>(m1));
    REQUIRE(std::holds_alternative<FileAckMsg>(m2));
}

TEST_CASE("message_type maps file types to correct enums", "[message][file]") {
    REQUIRE(message_type(FileMetaMsg{})  == MessageType::FileMeta);
    REQUIRE(message_type(FileChunkMsg{}) == MessageType::FileChunk);
    REQUIRE(message_type(FileAckMsg{})   == MessageType::FileAck);
}

TEST_CASE("AttachMsg round-trip with command field", "[message]") {
    SECTION("command round-trips through encode/decode") {
        AttachMsg a;
        a.cols = 120;
        a.rows = 40;
        a.term = "xterm-256color";
        a.session_name = "test-session";
        a.routing = "target-node";
        a.command = "echo hello";

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
        REQUIRE(b.command == "echo hello");
    }

    SECTION("empty command round-trips (backward compat)") {
        AttachMsg a;
        a.session_name = "session-no-cmd";
        a.command = "";

        Message msg = a;
        auto frame = encode(msg, 0);
        Message decoded = decode(frame);
        REQUIRE(std::holds_alternative<AttachMsg>(decoded));
        const auto& b = std::get<AttachMsg>(decoded);
        REQUIRE(b.session_name == "session-no-cmd");
        REQUIRE(b.command.empty());
    }

    SECTION("long command round-trips") {
        AttachMsg a;
        a.session_name = "long-cmd";
        std::string long_cmd;
        for (int i = 0; i < 300; ++i) long_cmd += "echo hello world; ";
        a.command = long_cmd;

        Message msg = a;
        auto frame = encode(msg, 0);
        Message decoded = decode(frame);
        REQUIRE(std::holds_alternative<AttachMsg>(decoded));
        const auto& b = std::get<AttachMsg>(decoded);
        REQUIRE(b.session_name == "long-cmd");
        REQUIRE(b.command == long_cmd);
    }
}

TEST_CASE("Decoder rejects impossible lengths without pointer arithmetic overflow",
          "[message][codec][security][alpha2]") {
    std::vector<uint8_t> one{0};
    Decoder decoder{one.data(), one.data() + one.size()};
    REQUIRE_FALSE(decoder.ok(std::numeric_limits<size_t>::max()));
    REQUIRE_THROWS_AS(decoder.bytes_size(std::numeric_limits<size_t>::max()),
                      std::runtime_error);
}
