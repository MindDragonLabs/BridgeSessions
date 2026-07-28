#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../bs-protocol.h"

using namespace bs::mesh;

TEST_CASE("explicit shell command remains finite when stdin is interactive",
          "[cli][shell][regression]") {
    REQUIRE(shell_command_uses_interactive_mode("", false));
    REQUIRE(shell_command_uses_interactive_mode("", true));
    REQUIRE_FALSE(shell_command_uses_interactive_mode("hermes --tui --yolo", true));
    REQUIRE_FALSE(shell_command_uses_interactive_mode("uname -a", false));
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
