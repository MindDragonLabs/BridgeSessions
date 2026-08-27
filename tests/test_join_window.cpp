// test_join_window.cpp — verify per-controller join-window lifecycle
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"
#include <filesystem>

using namespace bs::mesh;

TEST_CASE("join windows are scoped to one controller listener", "[join-window]") {
    const auto base = std::filesystem::temp_directory_path() /
        ("bs_join_scope_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto home_a = base / "a";
    const auto home_b = base / "b";
    std::filesystem::create_directories(home_a);
    std::filesystem::create_directories(home_b);
    MeshController controller_a(MeshConfig{}, home_a.string());
    MeshController controller_b(MeshConfig{}, home_b.string());
    REQUIRE_FALSE(controller_a.test_join_window_open());
    REQUIRE_FALSE(controller_b.test_join_window_open());
    controller_a.test_add_invite("invite-a");
    REQUIRE(controller_a.test_join_window_open());
    REQUIRE_FALSE(controller_b.test_join_window_open());
    std::filesystem::remove_all(base);
}


// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
