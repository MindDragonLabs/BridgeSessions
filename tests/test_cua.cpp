// test_cua.cpp — P5 CUA tests (2.0.8-alpha3)
//
// Covers:
//   - CuaRequestMsg / CuaResponseMsg codec round-trips
//   - CUA execute dispatch (Linux xdotool path exercises)
//   - Spectator rejection invariant

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

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

template<typename T>
T roundtrip(const T& msg, uint16_t stream_id = 0) {
    Message m = msg;
    auto wire = encode(m, stream_id);
    auto decoded = decode(wire);
    REQUIRE(message_type(m) == message_type(decoded));
    return std::get<T>(decoded);
}

// ── P5-1: Codec round-trips ──────────────────────────────────────────

TEST_CASE("P5 CUA: CuaRequestMsg key action round-trip", "[p5][cua][codec]") {
    CuaRequestMsg m;
    m.request_id = 1;
    m.action = 1; // key
    m.hid_key = 0x04; // USB HID 'a'
    m.modifiers = 0;
    auto m2 = roundtrip(m);
    REQUIRE(m2.request_id == 1);
    REQUIRE(m2.action == 1);
    REQUIRE(m2.hid_key == 0x04);
}

TEST_CASE("P5 CUA: CuaRequestMsg text action round-trip", "[p5][cua][codec]") {
    CuaRequestMsg m;
    m.request_id = 2;
    m.action = 2; // text
    m.text = "hello mesh";
    auto m2 = roundtrip(m);
    REQUIRE(m2.request_id == 2);
    REQUIRE(m2.action == 2);
    REQUIRE(m2.text == "hello mesh");
}

TEST_CASE("P5 CUA: CuaRequestMsg mouse move round-trip", "[p5][cua][codec]") {
    CuaRequestMsg m;
    m.request_id = 3;
    m.action = 3; // mouse move
    m.x = 1920; m.y = 1080;
    auto m2 = roundtrip(m);
    REQUIRE(m2.action == 3);
    REQUIRE(m2.x == 1920);
    REQUIRE(m2.y == 1080);
}

TEST_CASE("P5 CUA: CuaResponseMsg success round-trip", "[p5][cua][codec]") {
    CuaResponseMsg m;
    m.request_id = 99;
    m.status = 0; // ok
    m.screen_w = 1920; m.screen_h = 1080;
    auto m2 = roundtrip(m);
    REQUIRE(m2.request_id == 99);
    REQUIRE(m2.status == 0);
    REQUIRE(m2.screen_w == 1920);
}

TEST_CASE("P5 CUA: CuaResponseMsg error round-trip", "[p5][cua][codec]") {
    CuaResponseMsg m;
    m.request_id = 100;
    m.status = 1;
    m.error = "permission denied";
    auto m2 = roundtrip(m);
    REQUIRE(m2.status == 1);
    REQUIRE(m2.error == "permission denied");
}

// ── P5-2: cua_execute dispatch ───────────────────────────────────────

TEST_CASE("P5 CUA: cua_execute unknown action returns error", "[p5][cua][dispatch]") {
    CuaRequestMsg req;
    req.action = 99; // unknown
    auto resp = cua_execute(req);
    REQUIRE(resp.status == 1);
    REQUIRE(!resp.error.empty());
}

TEST_CASE("P5 CUA: cua_execute key action does not crash", "[p5][cua][dispatch]") {
    CuaRequestMsg req;
    req.action = 1; // key
    req.hid_key = 0x04;
    auto resp = cua_execute(req);
    // On headless CI without xdotool, expect an error.
    // On X11 desktop, expect success.
    // Either way, must not crash.
    REQUIRE((resp.status == 0 || resp.status == 1));
}

TEST_CASE("P5 CUA: cua_execute text action does not crash", "[p5][cua][dispatch]") {
    CuaRequestMsg req;
    req.action = 2; // text
    req.text = "test";
    auto resp = cua_execute(req);
    REQUIRE((resp.status == 0 || resp.status == 1));
}

// ── P5-3: Spectator rejection (already tested in test_multi_attach_p1) ─

TEST_CASE("P5 CUA: CuaRequest wire type is 0x26", "[p5][cua][wire]") {
    // Verify the wire type enum maps correctly
    CuaRequestMsg m;
    Message msg = m;
    REQUIRE(message_type(msg) == MessageType::CuaRequest);
    REQUIRE(static_cast<uint8_t>(MessageType::CuaRequest) == 0x26);
}

TEST_CASE("P5 CUA: CuaResponse wire type is 0x27", "[p5][cua][wire]") {
    CuaResponseMsg m;
    Message msg = m;
    REQUIRE(message_type(msg) == MessageType::CuaResponse);
    REQUIRE(static_cast<uint8_t>(MessageType::CuaResponse) == 0x27);
}
