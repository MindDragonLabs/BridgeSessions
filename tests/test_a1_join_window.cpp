#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../bs-protocol.h"

#include <chrono>
#include <filesystem>
#include <fstream>

using namespace bs::mesh;
namespace fs = std::filesystem;

namespace {

fs::path unique_temp_dir(const char* label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() /
        (std::string("bs_a1_") + label + "_" + std::to_string(stamp));
    fs::create_directories(path);
    return path;
}

} // namespace

TEST_CASE("join window hard cap defaults parses and serializes", "[a1][config]") {
    REQUIRE(MeshConfig{}.join_window_max_secs == 300);

    const auto dir = unique_temp_dir("config");
    const auto input = dir / "input.conf";
    {
        std::ofstream out(input);
        out << "mesh.join_window_max_secs 17\n";
    }
    auto cfg = load_config(input.string());
    REQUIRE(cfg.join_window_max_secs == 17);

    const auto output = dir / "output.conf";
    REQUIRE(save_config(output.string(), cfg));
    const auto saved = load_config(output.string());
    REQUIRE(saved.join_window_max_secs == 17);
    fs::remove_all(dir);
}

TEST_CASE("join window hard cap closes with an unclaimed invite", "[a1][join-window]") {
    const auto home = unique_temp_dir("cap");
    MeshConfig cfg;
    cfg.join_window_max_secs = 1;
    {
        MeshController controller(cfg, home.string());
        controller.test_add_invite("still-unclaimed");
        REQUIRE(controller.test_pending_invite_count() == 1);
        REQUIRE(controller.test_join_window_open());

        controller.test_age_join_window(std::chrono::seconds(2));
        controller.test_close_join_window();

        REQUIRE_FALSE(controller.test_join_window_open());
        REQUIRE(controller.test_pending_invite_count() == 1);
    }
    fs::remove_all(home);
}

TEST_CASE("expired invite is dropped and emits exactly one event", "[a1][invite]") {
    const auto home = unique_temp_dir("expiry");
    MeshConfig cfg;
    {
        MeshController controller(cfg, home.string());
        const auto before = controller.test_invite_expired_event_count();
        controller.test_add_invite("expired", std::chrono::hours(2) + std::chrono::seconds(1));

        controller.test_close_join_window();
        REQUIRE(controller.test_pending_invite_count() == 0);
        REQUIRE(controller.test_invite_expired_event_count() == before + 1);
        REQUIRE_FALSE(controller.test_join_window_open());

        controller.test_close_join_window();
        REQUIRE(controller.test_invite_expired_event_count() == before + 1);
    }
    fs::remove_all(home);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
