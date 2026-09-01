// r4 follow-up: unit-name sanitization + stop-helper charset gate.
// sanitize_systemd_unit_name must map session names into systemd's
// [A-Za-z0-9:._-] charset; stop_session_worker_unit must refuse (return
// false, execute nothing) any unit string outside that charset.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "../bs-protocol.h"

#ifndef _WIN32
TEST_CASE("sanitize_systemd_unit_name keeps legal charset verbatim", "[r4]") {
    using bs::mesh::sanitize_systemd_unit_name;
    REQUIRE(sanitize_systemd_unit_name("alice-term") == "alice-term");
    REQUIRE(sanitize_systemd_unit_name("Box_2.fish:work") == "Box_2.fish:work");
    REQUIRE(sanitize_systemd_unit_name("") .empty());
    REQUIRE(sanitize_systemd_unit_name("0-._:") == "0-._:");
}

TEST_CASE("sanitize_systemd_unit_name maps shell/systemd-hostile chars", "[r4]") {
    using bs::mesh::sanitize_systemd_unit_name;
    // whitespace, separators, globs, quotes, dollars, backticks, redirections
    REQUIRE(sanitize_systemd_unit_name("my session") == "my_session");
    REQUIRE(sanitize_systemd_unit_name("a/b\\c") == "a_b_c");
    REQUIRE(sanitize_systemd_unit_name("$(id)") == "__id_");
    REQUIRE(sanitize_systemd_unit_name("`id`") == "_id_");
    REQUIRE(sanitize_systemd_unit_name("x;y|z&") == "x_y_z_");
    REQUIRE(sanitize_systemd_unit_name("it's \"q\"") == "it_s__q_");
    REQUIRE(sanitize_systemd_unit_name("*?![") == "____");
    // UTF-8 / high bytes map per-byte, never pass through
    REQUIRE(sanitize_systemd_unit_name("caf\xc3\xa9") == "caf__");
    REQUIRE(sanitize_systemd_unit_name("\x01\x7f") == "__");
}

TEST_CASE("sanitize output is idempotent", "[r4]") {
    using bs::mesh::sanitize_systemd_unit_name;
    const std::string dirty = "weIRD name/with:stuff.txt";
    const std::string once = sanitize_systemd_unit_name(dirty);
    REQUIRE(sanitize_systemd_unit_name(once) == once);
}

TEST_CASE("stop_session_worker_unit refuses injection attempts", "[r4]") {
    // Refusal happens on charset validation BEFORE any ::system call, so
    // these are safe to run in any environment.
    REQUIRE_FALSE(bs::mesh::stop_session_worker_unit(""));
    REQUIRE_FALSE(bs::mesh::stop_session_worker_unit("foo; rm -rf /"));
    REQUIRE_FALSE(bs::mesh::stop_session_worker_unit("bs-worker-$(id)"));
    REQUIRE_FALSE(bs::mesh::stop_session_worker_unit("u `id`"));
    REQUIRE_FALSE(bs::mesh::stop_session_worker_unit("u\nx"));
    REQUIRE_FALSE(bs::mesh::stop_session_worker_unit("u\tx"));
    REQUIRE_FALSE(bs::mesh::stop_session_worker_unit("a b"));
}
#endif

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
