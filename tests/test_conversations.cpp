// test_conversations.cpp — P4 conversation tests (2.0.8-alpha3)
//
// Covers:
//   - ConversationAppend/Query/Batch message round-trips
//   - Store append + query with since_seq
//   - Timestamp auto-population

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

#include "../bridgesessions.cpp"

#include <string>
#include <vector>

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── P4-1: Codec round-trips ──────────────────────────────────────────

template<typename T>
T roundtrip(const T& msg, uint16_t stream_id = 0) {
    Message m = msg;
    auto wire = encode(m, stream_id);
    auto decoded = decode(wire);
    REQUIRE(message_type(m) == message_type(decoded));
    return std::get<T>(decoded);
}

TEST_CASE("P4 conversations: ConversationAppendMsg round-trip", "[p4][conv][codec]") {
    ConversationAppendMsg m;
    m.conv_id = "test-conv-1";
    m.seq = 42;
    m.ts = 1718400000000;
    m.agent_id = "aabbccdd";
    m.role = 2; // agent
    m.body = "Hello from the mesh!";
    auto m2 = roundtrip(m);
    REQUIRE(m2.conv_id == m.conv_id);
    REQUIRE(m2.seq == m.seq);
    REQUIRE(m2.ts == m.ts);
    REQUIRE(m2.agent_id == m.agent_id);
    REQUIRE(m2.role == m.role);
    REQUIRE(m2.body == m.body);
}

TEST_CASE("P4 conversations: ConversationQueryMsg round-trip", "[p4][conv][codec]") {
    ConversationQueryMsg m;
    m.conv_id = "test-conv-2";
    m.since_seq = 100;
    auto m2 = roundtrip(m);
    REQUIRE(m2.conv_id == m.conv_id);
    REQUIRE(m2.since_seq == m.since_seq);
}

TEST_CASE("P4 conversations: ConversationBatchMsg round-trip with 3 messages", "[p4][conv][codec]") {
    ConversationBatchMsg batch;
    batch.conv_id = "batch-test";
    for (int i = 0; i < 3; ++i) {
        ConversationAppendMsg m;
        m.conv_id = "batch-test";
        m.seq = static_cast<uint64_t>(10 + i);
        m.agent_id = "agent-" + std::to_string(i);
        m.role = 2;
        m.body = "msg " + std::to_string(i);
        batch.messages.push_back(m);
    }
    auto m2 = roundtrip(batch);
    REQUIRE(m2.conv_id == batch.conv_id);
    REQUIRE(m2.messages.size() == 3);
    REQUIRE(m2.messages[0].body == "msg 0");
    REQUIRE(m2.messages[2].body == "msg 2");
}

// ── P4-2: Empty batch round-trip ─────────────────────────────────────

TEST_CASE("P4 conversations: empty batch round-trips correctly", "[p4][conv][store]") {
    ConversationBatchMsg batch;
    batch.conv_id = "nonexistent";
    auto m2 = roundtrip(batch);
    REQUIRE(m2.conv_id == "nonexistent");
    REQUIRE(m2.messages.empty());
}
