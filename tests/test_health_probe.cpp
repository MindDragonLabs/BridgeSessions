#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../bs-protocol.h"

using namespace bs::mesh;

TEST_CASE("health probe waits for late nonce after zero exit",
          "[health][regression][ordering]") {
    constexpr std::string_view nonce = "bs-health-late-output";

    REQUIRE_FALSE(health_probe_drain_complete(0, "", nonce, false));
    REQUIRE_FALSE(health_probe_drain_complete(0, "prefix", nonce, false));
    REQUIRE(health_probe_drain_complete(0, "prefix bs-health-late-output suffix", nonce, false));
    REQUIRE(health_probe_drain_complete(0, "", nonce, true));
}

TEST_CASE("failed health probe does not wait for output",
          "[health][regression][ordering]") {
    REQUIRE(health_probe_drain_complete(7, "", "nonce", false));
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
