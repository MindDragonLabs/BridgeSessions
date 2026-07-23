#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// Helper: build the literal ESC character
static std::string esc() { return std::string("\x1b", 1); }
static std::string bel() { return std::string("\x07", 1); }
static std::string st()  { return esc() + "\\"; }

// Build a complete OSC 52 sequence: ESC ] 52 ; Pc ; <base64> <terminator>
static std::string osc52(const std::string& pc, const std::string& b64, bool use_bel = true) {
    return esc() + "]52;" + pc + ";" + b64 + (use_bel ? bel() : st());
}

TEST_CASE("Plain text passes through unchanged", "[osc52][pass-through]") {
    auto result = scan_osc52("hello\nworld");
    REQUIRE(result.cleaned_text == "hello\nworld");
    REQUIRE_FALSE(result.clipboard_text.has_value());
}

TEST_CASE("Detect and extract with BEL terminator", "[osc52][detect]") {
    // "Hello" in base64 = "SGVsbG8="
    auto seq = osc52("c", "SGVsbG8=", true);
    auto result = scan_osc52(seq);
    REQUIRE(result.cleaned_text.empty());
    REQUIRE(result.clipboard_text.has_value());
    REQUIRE(*result.clipboard_text == "Hello");
}

TEST_CASE("Detect and extract with ST terminator", "[osc52][detect]") {
    // "world" in base64 = "d29ybGQ="
    auto seq = osc52("c", "d29ybGQ=", false);
    auto result = scan_osc52(seq);
    REQUIRE(result.cleaned_text.empty());
    REQUIRE(result.clipboard_text.has_value());
    REQUIRE(*result.clipboard_text == "world");
}

TEST_CASE("OSC 52 embedded in surrounding text", "[osc52][embedded]") {
    auto seq = std::string("prefix ") + osc52("c", "SGVsbG8=", true) + " suffix";
    auto result = scan_osc52(seq);
    REQUIRE(result.cleaned_text == "prefix  suffix");
    REQUIRE(result.clipboard_text.has_value());
    REQUIRE(*result.clipboard_text == "Hello");
}

TEST_CASE("Multiple sequences: last one wins", "[osc52][multiple]") {
    auto seq = osc52("c", "SGVsbG8=", true) + osc52("c", "d29ybGQ=", true);
    auto result = scan_osc52(seq);
    REQUIRE(result.cleaned_text.empty());
    REQUIRE(result.clipboard_text.has_value());
    REQUIRE(*result.clipboard_text == "world");
}

TEST_CASE("Multiple sequences: text between them preserved", "[osc52][multiple]") {
    auto seq = osc52("c", "SGVsbG8=", true) + " between " + osc52("c", "d29ybGQ=", true);
    auto result = scan_osc52(seq);
    REQUIRE(result.cleaned_text == " between ");
    REQUIRE(result.clipboard_text.has_value());
    REQUIRE(*result.clipboard_text == "world");
}

TEST_CASE("Incomplete sequence (no terminator) left in cleaned_text", "[osc52][incomplete]") {
    // ESC ] 52 ; c ; SGVsbG8= with no terminator
    auto incomplete = esc() + "]52;c;SGVsbG8=";
    auto result = scan_osc52(incomplete);
    // The incomplete sequence should be left as-is
    REQUIRE(result.cleaned_text == incomplete);
    REQUIRE_FALSE(result.clipboard_text.has_value());
}

TEST_CASE("Empty base64 (clipboard clear): clipboard_text is nullopt", "[osc52][empty]") {
    auto seq = osc52("c", "", true);
    auto result = scan_osc52(seq);
    // Sequence is stripped from cleaned_text
    REQUIRE(result.cleaned_text.empty());
    // Empty base64 means no clipboard text
    REQUIRE_FALSE(result.clipboard_text.has_value());
}

TEST_CASE("Base64 with padding (=) decodes correctly", "[osc52][padding]") {
    // "Hello" → SGVsbG8= (one padding)
    auto seq = osc52("c", "SGVsbG8=", true);
    auto result = scan_osc52(seq);
    REQUIRE(result.clipboard_text.has_value());
    REQUIRE(*result.clipboard_text == "Hello");
}

TEST_CASE("OSC sequence that is NOT 52: passed through", "[osc52][not52]") {
    // ESC ] 0 ; title BEL — window title OSC, not clipboard
    auto seq = esc() + "]0;my terminal title" + bel();
    auto result = scan_osc52(seq);
    // Should pass through completely unchanged
    REQUIRE(result.cleaned_text == seq);
    REQUIRE_FALSE(result.clipboard_text.has_value());
}

TEST_CASE("OSC 52 with different Pc (p, q) also detected", "[osc52][pc]") {
    // OSC 52 can use c, p, or q as clipboard target
    auto seq = osc52("p", "dGVzdA==", true);  // "test"
    auto result = scan_osc52(seq);
    REQUIRE(result.cleaned_text.empty());
    REQUIRE(result.clipboard_text.has_value());
    REQUIRE(*result.clipboard_text == "test");
}

TEST_CASE("Multiple OSC 52 with mixed terminators", "[osc52][mixed]") {
    auto seq = osc52("c", "SGVsbG8=", true) + osc52("c", "d29ybGQ=", false);
    auto result = scan_osc52(seq);
    REQUIRE(result.cleaned_text.empty());
    REQUIRE(result.clipboard_text.has_value());
    REQUIRE(*result.clipboard_text == "world");
}

TEST_CASE("Incomplete near ESC not consumed when not 52", "[osc52][edge]") {
    // An ESC not followed by ] just passes through
    auto seq = std::string("text\x1b") + "more";
    auto result = scan_osc52(seq);
    REQUIRE(result.cleaned_text == seq);
    REQUIRE_FALSE(result.clipboard_text.has_value());
}

TEST_CASE("Empty input", "[osc52][edge]") {
    auto result = scan_osc52("");
    REQUIRE(result.cleaned_text.empty());
    REQUIRE_FALSE(result.clipboard_text.has_value());
}

TEST_CASE("Only ESC at end", "[osc52][edge]") {
    auto result = scan_osc52("\x1b");
    REQUIRE(result.cleaned_text == "\x1b");
    REQUIRE_FALSE(result.clipboard_text.has_value());
}
