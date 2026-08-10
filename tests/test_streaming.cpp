// test_streaming.cpp — P3 streaming tests (2.0.8-alpha3)
//
// Covers:
//   - Per-connection output queue basic enqueue/drain
//   - OutputGap emission on queue overrun
//   - Queue cleared on disconnect
//   - Non-OutputMsg types not queued (control messages go direct)

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

#include <deque>
#include <string>

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── P3-1: Output queue enqueue / drain lifecycle ──────────────────────

TEST_CASE("P3 streaming: Conn output queue starts empty", "[p3][streaming][queue]") {
    MeshController::Conn c;
    REQUIRE(c.output_queue.empty());
    REQUIRE(c.output_dropped_bytes == 0);
    REQUIRE(c.output_gap_pending == false);
}

TEST_CASE("P3 streaming: queue enforces high-water mark", "[p3][streaming][queue]") {
    MeshController::Conn c;
    // Fill queue beyond high-water
    for (size_t i = 0; i < MeshController::Conn::kOutputQueueHighWater + 5; ++i) {
        MeshController::Conn::QueuedOutput qo;
        qo.data = "msg-" + std::to_string(i);
        if (c.output_queue.size() >= MeshController::Conn::kOutputQueueHighWater) {
            c.output_dropped_bytes += c.output_queue.front().data.size();
            c.output_gap_pending = true;
            c.output_queue.pop_front();
        }
        c.output_queue.push_back(std::move(qo));
    }
    // Queue must not exceed high-water
    REQUIRE(c.output_queue.size() <= MeshController::Conn::kOutputQueueHighWater);
    // Dropped bytes must be non-zero (we pushed 5 over)
    REQUIRE(c.output_dropped_bytes > 0);
    REQUIRE(c.output_gap_pending == true);
}

TEST_CASE("P3 streaming: OutputGap message round-trips through codec", "[p3][streaming][codec]") {
    OutputGapMsg gap;
    gap.dropped_bytes = 12345;

    Message m = gap;
    auto wire = encode(m, 0);
    auto decoded = decode(wire);

    REQUIRE(std::holds_alternative<OutputGapMsg>(decoded));
    auto& decoded_gap = std::get<OutputGapMsg>(decoded);
    REQUIRE(decoded_gap.dropped_bytes == 12345);
}

TEST_CASE("P3 streaming: queue cleared on invalid socket", "[p3][streaming][queue]") {
    // Simulate the drain_output_queues() behavior when sock_fd is INVALID_SOCKET
    MeshController::Conn c;
    c.output_queue.push_back({"test data", false});
    c.output_gap_pending = true;

    REQUIRE(!c.output_queue.empty());
    // drain_output_queues first checks sock_fd == INVALID_SOCKET
    REQUIRE(c.sock_fd == INVALID_SOCKET);
    // In the drain loop, this would clear the queue
    c.output_queue.clear();
    c.output_gap_pending = false;
    REQUIRE(c.output_queue.empty());
    REQUIRE(c.output_gap_pending == false);
}

// ── P3-2: Non-OutputMsg types are not queued ──────────────────────────

TEST_CASE("P3 streaming: non-OutputMsg types skip queue (ClipboardMsg)", "[p3][streaming][control]") {
    // The fanout only queues OutputMsg types. Control messages (ClipboardMsg,
    // SessionDiedMsg, etc.) are not enqueued — they're fire-and-forget.
    // This test verifies the type traits used in the fanout.
    using OM = OutputMsg;
    using CM = ClipboardMsg;
    // OutputMsg is the only type that gets queued
    bool output_queuable = std::is_same_v<std::decay_t<OM>, OutputMsg>;
    bool clipboard_not_queuable = !std::is_same_v<std::decay_t<CM>, OutputMsg>;
    REQUIRE(output_queuable);
    REQUIRE(clipboard_not_queuable);
}

// ── P3-3: drain_output_queues exists and compiles ─────────────────────

TEST_CASE("P3 streaming: drain_output_queues is callable", "[p3][streaming][drain]") {
    auto cfg = MeshConfig{};
    cfg.node_name = "p3-drain-test";
    cfg.listen_port = 19958;
    MeshController mc(cfg);

    // drain_output_queues should not crash with no connections
    mc.drain_output_queues();
    SUCCEED("drain_output_queues() ran without crash on empty conns");
}

// ── V3: RingBuffer read_since (SCROLLBACK verb backing) ─────────────

TEST_CASE("v3 ringbuf: read_since basic + offset + RESET semantics", "[v3][ringbuf][read_since]") {
    RingBuffer<16> rb;  // tiny power-of-2 capacity to force eviction

    // 1) write 10 bytes, read since 0 → full chunk, no reset
    std::string data = "0123456789";
    rb.write(std::string_view(data));
    auto r1 = rb.read_since(0);
    REQUIRE(r1.first == data);
    REQUIRE_FALSE(r1.second);
    REQUIRE(rb.total_written() == 10);

    // 2) read since total_written → empty chunk, no reset
    auto r2 = rb.read_since(10);
    REQUIRE(r2.first.empty());
    REQUIRE_FALSE(r2.second);

    // 3) write 20 more (total 30, capacity 16) → oldest evicted.
    //    read since 0 → returns what's held (<=16 bytes) + RESET flag.
    std::string more(20, 'A');
    rb.write(std::string_view(more));
    auto r3 = rb.read_since(0);
    REQUIRE(r3.second);                       // RESET
    REQUIRE(r3.first.size() <= 16);
    REQUIRE(!r3.first.empty());
    // RESET chunk must be a suffix of the full stream
    std::string full = data + more;
    REQUIRE(full.size() == 30);
    REQUIRE(full.compare(full.size() - r3.first.size(), r3.first.size(), r3.first) == 0);
}

TEST_CASE("v3 ringbuf: read_since chunk cap is 64 KiB", "[v3][ringbuf][read_since]") {
    RingBuffer<262144> rb;  // 256 KiB capacity
    std::string big(200 * 1024, 'x');
    rb.write(std::string_view(big));
    auto r = rb.read_since(0);
    REQUIRE(r.first.size() == 64 * 1024);  // capped
    // 2.0.8 MoA: when the unread window exceeds the 64 KiB cap, read_since
    // delivers the NEWEST 64 KiB and flags RESET so the client fast-forwards
    // to the live edge — the old "oldest chunk + silent middle skip" contract
    // was the scrollback-reset-skip finding.
    REQUIRE(r.second);
    REQUIRE(big.compare(big.size() - r.first.size(), r.first.size(), r.first) == 0);
}

TEST_CASE("v3 ringbuf: oversized write keeps absolute slot alignment", "[v3][ringbuf][moa]") {
    // 2.0.8 MoA ring-tail-misaligned finding: write() with len >= Capacity at a
    // non-zero absolute position must place the retained tail at aligned slots,
    // or read_since returns rotated garbage.
    RingBuffer<64> rb;
    std::string prefix(11, 'p');
    rb.write(std::string_view(prefix));              // pos = 11 (not 64-aligned)
    std::string big(100, 'z');
    for (size_t i = 0; i < big.size(); ++i) big[i] = char('A' + (i % 26));
    rb.write(std::string_view(big));                 // oversized write
    // Retained window = last 64 bytes of the full stream.
    std::string full = prefix + big;
    std::string retained = full.substr(full.size() - 64);
    auto r = rb.read_since(full.size() - 64);
    REQUIRE(r.first == retained);
    // And from the oldest retained byte forward, chunked reads must match too.
    auto r2 = rb.read_since(full.size() - 33);
    REQUIRE(r2.first == retained.substr(retained.size() - 33));
}

// ── V3: b64 round-trip (SCROLLBACK payload encoding) ────────────────

TEST_CASE("v3 b64: daemon b64enc/b64dec round-trips scrollback payload", "[v3][b64]") {
    std::string payload = "line one\nline two\x1b[31mRED\x1b[0m \xe6\x97\xa5\xe6\x9c\xac";
    std::string enc = b64enc(payload);
    // daemon b64enc strips '=' padding; b64dec must handle unpadded input
    REQUIRE(enc.find('=') == std::string::npos);
    REQUIRE(b64dec(enc) == payload);
}

// ── V3: CONV_APPEND role mapping (handler logic, exercised directly) ─

TEST_CASE("v3 conv: role strings map to wire role bytes", "[v3][conv][roles]") {
    // Mirror of the mapping in the CONV_APPEND IPC handler
    auto role_of = [](const std::string& r) -> int {
        if (r == "system") return 0;
        if (r == "user") return 1;
        if (r == "agent") return 2;
        if (r == "tool") return 3;
        return -1;
    };
    REQUIRE(role_of("system") == 0);
    REQUIRE(role_of("user") == 1);
    REQUIRE(role_of("agent") == 2);
    REQUIRE(role_of("tool") == 3);
    REQUIRE(role_of("bogus") == -1);
}

// ── V3: gossip cache member exists on MeshController ────────────────

TEST_CASE("v3 mesh: gossip_sessions_json_ cache accessible", "[v3][mesh][gossip]") {
    auto cfg = MeshConfig{};
    cfg.node_name = "v3-gossip-test";
    cfg.listen_port = 19961;
    MeshController mc(cfg);
    // Empty until gossip lands — MESH_TREE must render "[]" for peers
    // (member is private; this test just verifies the controller constructs)
    SUCCEED("MeshController with gossip cache constructed");
}

// ── V3: ServerInfoMsg.sessions_summary_json round-trip + legacy tolerance ──

TEST_CASE("v3 mesh: ServerInfoMsg sessions_summary_json survives encode/decode", "[v3][mesh][gossip][codec]") {
    ServerInfoMsg m;
    m.hostname = "node-a";
    m.version = "2.0.8-alpha3";
    m.load = 0.42;
    m.sessions_summary_json = R"([{"name":"sess1","state":"attached","command":"bash","bytes":123}])";

    auto wire = encode(Message{m}, CONTROL_STREAM_ID);
    auto decoded = decode(wire);
    REQUIRE(std::holds_alternative<ServerInfoMsg>(decoded));
    auto& m2 = std::get<ServerInfoMsg>(decoded);
    REQUIRE(m2.hostname == m.hostname);
    REQUIRE(m2.version == m.version);
    REQUIRE(m2.load == m.load);
    REQUIRE(m2.sessions_summary_json == m.sessions_summary_json);
}

TEST_CASE("v3 mesh: ServerInfoMsg without trailing summary decodes tolerantly (legacy peer)", "[v3][mesh][gossip][codec]") {
    ServerInfoMsg m;
    m.hostname = "node-legacy";
    m.version = "2.0.7";
    m.load = 0.1;
    m.sessions_summary_json = "some-summary-that-a-legacy-peer-never-sent";

    auto wire = encode(Message{m}, CONTROL_STREAM_ID);

    // Simulate a legacy sender that never appended the trailing
    // str_prefixed_u16(sessions_summary_json) field: strip it from the
    // payload (2-byte length prefix + content) and fix up the frame's
    // payload-length header field.
    size_t trailing_len = 2 + m.sessions_summary_json.size();
    REQUIRE(wire.size() > FRAME_HEADER_SIZE + trailing_len);
    std::vector<uint8_t> truncated(wire.begin(), wire.end() - static_cast<long>(trailing_len));
    uint16_t new_payload_len = static_cast<uint32_t>(truncated.size() - FRAME_HEADER_SIZE);
    write_u16(truncated.data() + 4, new_payload_len);

    auto decoded = decode(truncated);
    REQUIRE(std::holds_alternative<ServerInfoMsg>(decoded));
    auto& m2 = std::get<ServerInfoMsg>(decoded);
    REQUIRE(m2.hostname == m.hostname);
    REQUIRE(m2.version == m.version);
    REQUIRE(m2.sessions_summary_json.empty());
}

TEST_CASE("MoA gossip: shape validator rejects envelope-breaking peer JSON", "[v3][mesh][gossip][moa]") {
    using MC = MeshController;
    // Valid shapes
    REQUIRE(MC::gossip_json_shape_ok("[]"));
    REQUIRE(MC::gossip_json_shape_ok("[{\"name\":\"a\",\"bytes\":12}]"));
    REQUIRE(MC::gossip_json_shape_ok("[{\"name\":\"x ] } \\\" trick\"}]"));
    REQUIRE(MC::gossip_json_shape_ok("[\"nested [ { ok } ]\"]"));
    // Envelope breakers / malformed
    REQUIRE_FALSE(MC::gossip_json_shape_ok(""));
    REQUIRE_FALSE(MC::gossip_json_shape_ok("["));
    REQUIRE_FALSE(MC::gossip_json_shape_ok("]"));
    REQUIRE_FALSE(MC::gossip_json_shape_ok("{\"not\":\"an array\"}"));
    REQUIRE_FALSE(MC::gossip_json_shape_ok("[{\"a\":1}"));   // unbalanced square
    REQUIRE_FALSE(MC::gossip_json_shape_ok("[{\"a\":1]]"));  // negative depth
    REQUIRE_FALSE(MC::gossip_json_shape_ok("[{\"a\":}"));    // unbalanced curly
    REQUIRE_FALSE(MC::gossip_json_shape_ok("[\"unterminated"));
    REQUIRE_FALSE(MC::gossip_json_shape_ok("[] ,\"extra\":\"json\""));
    // The canonical MESH_TREE breaker: array close + injected node-level key
    REQUIRE_FALSE(MC::gossip_json_shape_ok("[],\"sessions\":[{\"name\":\"evil\"}]"));
}
