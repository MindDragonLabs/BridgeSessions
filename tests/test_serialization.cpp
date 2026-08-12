// test_serialization.cpp — verify JoinReply u16 serialization + message round-trips
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

using namespace bs::mesh;

TEST_CASE("JoinReply peer_pubkeys_json > 255 bytes round-trips (join failure bug)", "[serialization]") {
    JoinReplyMsg original;
    original.ok = true;
    original.node_name = "test-node";
    original.host_pubkey = std::string(64, 'a');
    original.host_addr = "10.0.0.1:19949";
    // Create a JSON array > 255 bytes (this was the bug — u8 prefix overflow)
    std::string json = "[";
    for (int i = 0; i < 6; i++) {
        if (i > 0) json += ",";
        json += "{\"name\":\"peer" + std::to_string(i) + "\",\"addr\":\"10.0.0." + std::to_string(i) + ":19949\",\"pubkey_hex\":\"" + std::string(64, 'a' + i) + "\"}";
    }
    json += "]";
    REQUIRE(json.size() > 255);
    original.peer_pubkeys_json = json;

    Message msg = original;
    auto frame = encode(msg, 0);
    Message decoded = decode(frame);

    REQUIRE(std::holds_alternative<JoinReplyMsg>(decoded));
    auto& reply = std::get<JoinReplyMsg>(decoded);
    REQUIRE(reply.ok == true);
    REQUIRE(reply.node_name == original.node_name);
    REQUIRE(reply.host_pubkey == original.host_pubkey);
    REQUIRE(reply.peer_pubkeys_json == original.peer_pubkeys_json);
    REQUIRE(reply.peer_pubkeys_json.size() > 255);
}

TEST_CASE("Serializer str_prefixed_u16 handles large strings", "[serialization]") {
    std::string large(500, 'x');
    std::vector<uint8_t> buf;
    Serializer s{buf};
    s.str_prefixed_u16(large);
    REQUIRE(buf.size() == 2 + 500); // u16 length (2 bytes) + payload
}

TEST_CASE("Serializer str_prefixed handles small strings (backward compat)", "[serialization]") {
    std::string small = "hello";
    std::vector<uint8_t> buf;
    Serializer s{buf};
    s.str_prefixed(small);
    REQUIRE(buf.size() == 1 + 5); // u8 length (1 byte) + payload
}

TEST_CASE("Serializer str_prefixed throws on > 255 bytes", "[serialization]") {
    std::string too_large(256, 'x');
    std::vector<uint8_t> buf;
    Serializer s{buf};
    REQUIRE_THROWS(s.str_prefixed(too_large));
}

TEST_CASE("JoinRequestMsg round-trip", "[serialization]") {
    JoinRequestMsg original;
    original.token = "abc123def45678901234567890123456abcd";

    Message msg = original;
    auto frame = encode(msg, 0);
    Message decoded = decode(frame);
    REQUIRE(std::holds_alternative<JoinRequestMsg>(decoded));
    REQUIRE(std::get<JoinRequestMsg>(decoded).token == original.token);
}

TEST_CASE("JoinReply error case round-trip", "[serialization]") {
    JoinReplyMsg original;
    original.ok = false;
    original.error = "invalid or expired token";

    Message msg = original;
    auto frame = encode(msg, 0);
    Message decoded = decode(frame);
    REQUIRE(std::holds_alternative<JoinReplyMsg>(decoded));
    auto& reply = std::get<JoinReplyMsg>(decoded);
    REQUIRE(reply.ok == false);
    REQUIRE(reply.error == original.error);
}

TEST_CASE("JoinReply seeds_csv > 255 bytes round-trips (fleet join regression)", "[serialization][regression]") {
    // Bug 2026-08-11: seeds_csv used u8 str_prefixed (255B cap). Large fleets
    // exceeded 255B, JoinReply serialization threw, and fresh joins hung.
    // Spec: long fields use str_prefixed_u16. Synthetic peers only (no real IPs).
    JoinReplyMsg original;
    original.ok = true;
    original.node_name = "node-test-host";
    // 12 synthetic seeds — intentionally >255 chars to pin u16 path.
    original.seeds_csv =
        "peer-01:10.0.1.1:19949|peer-02:10.0.2.1:19949|peer-03:10.0.3.1:19949|"
        "peer-04:10.0.4.1:19949|peer-05:10.0.5.1:19949|peer-06:10.0.6.1:19949|"
        "peer-07:10.0.7.1:19949|peer-08:10.0.8.1:19949|peer-09:10.0.9.1:19949|"
        "peer-10:10.0.10.1:19949|peer-11:10.0.11.1:19949|peer-12:10.0.12.1:19949";
    REQUIRE(original.seeds_csv.size() > 255); // guard: must exercise u16 prefix
    original.host_pubkey = std::string(64, 'a');
    original.host_addr = "0.0.0.0:19949";
    original.peer_pubkeys_json = "[]";

    Message msg = original;
    std::vector<uint8_t> frame;
    REQUIRE_NOTHROW(frame = encode(msg, 0)); // must not throw at fleet size
    Message decoded = decode(frame);
    REQUIRE(std::holds_alternative<JoinReplyMsg>(decoded));
    auto& reply = std::get<JoinReplyMsg>(decoded);
    REQUIRE(reply.ok);
    REQUIRE(reply.seeds_csv == original.seeds_csv);
    REQUIRE(reply.node_name == original.node_name);
    REQUIRE(reply.host_pubkey == original.host_pubkey);
    REQUIRE(reply.host_addr == original.host_addr);
}


// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
