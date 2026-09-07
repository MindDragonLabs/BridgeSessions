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

TEST_CASE("soft cleanup keeps the alternate screen alive", "[shell][reconnect]") {
    auto soft = bs::mesh::terminal_cleanup_sequence(/*leave_alt_screen=*/false);
    REQUIRE(soft.find("\x1b[?1049l") == std::string::npos);   // no alt-screen exit
    REQUIRE(soft.find("\x1b[?1006l") != std::string::npos);   // mouse modes still reset
    auto full = bs::mesh::terminal_cleanup_sequence(/*leave_alt_screen=*/true);
    REQUIRE(full.find("\x1b[?1049l") != std::string::npos);   // exit path unchanged
    REQUIRE(full == terminal_cleanup_sequence());             // default = full restore
}

TEST_CASE("reconnect status line shows peer, attempt, held keystrokes", "[shell][reconnect]") {
    auto line = bs::mesh::reconnect_status_line("fecv4", 3, 128, 80, 24);
    REQUIRE(line.find("fecv4") != std::string::npos);
    REQUIRE(line.find("attempt 3") != std::string::npos);
    REQUIRE(line.find("128B held") != std::string::npos);
    REQUIRE(line.find("Ctrl-D") != std::string::npos);
    // Draws on the bottom row via cursor addressing; saves/restores cursor.
    REQUIRE(line.find("\x1b[24;1H") != std::string::npos);
    REQUIRE(line.find("\x1b7") != std::string::npos);
    REQUIRE(line.find("\x1b8") != std::string::npos);
    // Never exceeds terminal width (visible cells, escapes stripped).
    size_t vis = 0;
    bool in_esc = false;
    // strip: ESC [ ... alpha
    std::string stripped;
    for (size_t i = 0; i < line.size();) {
        if (line[i] == '\x1b' && i + 1 < line.size() && line[i+1] == '[') {
            i += 2;
            while (i < line.size() && !std::isalpha(static_cast<unsigned char>(line[i]))) ++i;
            ++i;
            continue;
        }
        stripped += line[i]; ++i;
    }
    for (unsigned char c : stripped)
        if ((c & 0xC0) != 0x80) ++vis;  // UTF-8 cells, not bytes
    REQUIRE(vis <= 80);
}

TEST_CASE("status-line clear erases the bottom row only", "[shell][reconnect]") {
    auto c = bs::mesh::reconnect_status_clear(24);
    REQUIRE(c.find("\x1b[24;1H\x1b[2K") != std::string::npos);
    REQUIRE(c.find("\x1b[?1049l") == std::string::npos);
    REQUIRE(c.find("\x1b7") != std::string::npos);  // cursor saved/restored
    REQUIRE(c.find("\x1b8") != std::string::npos);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
