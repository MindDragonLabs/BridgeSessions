// Tests for the local-clipboard sink behind the OSC 52 relay (26.10.04).
//
// The scanner half of OSC 52 (scan_osc52) has always worked; the sink half did
// not exist on the client, so a ClipboardMsg was read off the wire and dropped.
// These tests cover the sink: that it accepts text, refuses empty text rather
// than handing it to a helper, and that the OSC 52 fallback encodes base64
// correctly including all three padding cases.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

using namespace bs::mesh;

namespace {

// Reference base64, so the hand-rolled encoder in bs-clipboard-local.h is
// checked against something other than itself.
std::string to_base64(std::string_view in) {
    static const char k[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        out.push_back(k[(v >> 18) & 63]);
        out.push_back(k[(v >> 12) & 63]);
        out.push_back(k[(v >> 6) & 63]);
        out.push_back(k[v & 63]);
        i += 3;
    }
    if (i < in.size()) {
        const size_t rem = in.size() - i;
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) |
                           (rem == 2 ? static_cast<unsigned char>(in[i + 1]) << 8 : 0);
        out.push_back(k[(v >> 18) & 63]);
        out.push_back(k[(v >> 12) & 63]);
        out.push_back(rem == 2 ? k[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

}  // namespace

TEST_CASE("set_local_clipboard accepts non-empty text", "[clipboard]") {
    std::string method;
    // On a headless box every system backend is absent, so the OSC 52
    // fallback must accept it. Either way the call must succeed and name the
    // backend it used.
    const bool ok = set_local_clipboard("hello-from-remote", &method);
    REQUIRE(ok);
    REQUIRE_FALSE(method.empty());
}

TEST_CASE("set_local_clipboard rejects empty text", "[clipboard]") {
    // An empty clipboard payload must not be handed to a helper: it would
    // clear the operator's clipboard on a stray or malformed frame.
    std::string method = "untouched";
    REQUIRE_FALSE(set_local_clipboard("", &method));
    REQUIRE(method.empty());
}

TEST_CASE("osc52 passthrough encodes base64 correctly", "[clipboard][osc52]") {
    // The fallback must produce a payload the terminal can decode. Checked
    // against an independent encoder, including all three padding cases
    // (rem 0, 1, 2) which are the easy ones to get wrong by hand.
    REQUIRE(detail::osc52_b64("") == "");
    REQUIRE(detail::osc52_b64("a") == to_base64("a"));
    REQUIRE(detail::osc52_b64("ab") == to_base64("ab"));
    REQUIRE(detail::osc52_b64("abc") == to_base64("abc"));
    REQUIRE(detail::osc52_b64("hello-from-remote")
            == to_base64("hello-from-remote"));
    REQUIRE(detail::osc52_b64("https://example.com/a?b=c")
            == to_base64("https://example.com/a?b=c"));
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
