// test_upgrade_validation.cpp — tests for bs upgrade tag validation (W4-P1)
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

TEST_CASE("upgrade tag validation accepts valid tags", "[audit][p1][upgrade]") {
    REQUIRE(bs::mesh::bs_upgrade_tag_valid("latest"));
    REQUIRE(bs::mesh::bs_upgrade_tag_valid("26.08.10-beta2"));
    REQUIRE(bs::mesh::bs_upgrade_tag_valid("v1.2.3"));
    REQUIRE(bs::mesh::bs_upgrade_tag_valid("1.0"));
}

TEST_CASE("upgrade tag validation rejects injection tags", "[audit][p1][upgrade]") {
    // Single quote breaks out of shell quoting in system() calls
    REQUIRE_FALSE(bs::mesh::bs_upgrade_tag_valid("';rm -rf /;'"));
    REQUIRE_FALSE(bs::mesh::bs_upgrade_tag_valid("'; id;'"));
    REQUIRE_FALSE(bs::mesh::bs_upgrade_tag_valid("x;y"));
    REQUIRE_FALSE(bs::mesh::bs_upgrade_tag_valid("$(whoami)"));
    REQUIRE_FALSE(bs::mesh::bs_upgrade_tag_valid("`whoami`"));
    REQUIRE_FALSE(bs::mesh::bs_upgrade_tag_valid("a b"));       // space
    REQUIRE_FALSE(bs::mesh::bs_upgrade_tag_valid("../main"));   // path traversal
    REQUIRE_FALSE(bs::mesh::bs_upgrade_tag_valid(""));          // empty
}

TEST_CASE("upgrade --all peer names must be shell-safe", "[audit][p1][upgrade]") {
    REQUIRE(bs::mesh::bs_peer_name_shell_safe("linux-peer"));
    REQUIRE(bs::mesh::bs_peer_name_shell_safe("win-host"));
    REQUIRE_FALSE(bs::mesh::bs_peer_name_shell_safe("x;rm -rf /"));
    REQUIRE_FALSE(bs::mesh::bs_peer_name_shell_safe("a'b"));
    REQUIRE_FALSE(bs::mesh::bs_peer_name_shell_safe(""));
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
