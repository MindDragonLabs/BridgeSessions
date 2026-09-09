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
    auto line = bs::mesh::reconnect_status_line("host-a", 3, 128, 80, 24);
    REQUIRE(line.find("host-a") != std::string::npos);
    REQUIRE(line.find("attempt 3") != std::string::npos);
    REQUIRE(line.find("128B held") != std::string::npos);
    REQUIRE(line.find("Ctrl-D") != std::string::npos);
    // Draws on the bottom row via cursor addressing; saves/restores cursor.
    REQUIRE(line.find("\x1b[24;1H") != std::string::npos);
    REQUIRE(line.find(std::string("\x1b") + "7") != std::string::npos);
    REQUIRE(line.find(std::string("\x1b") + "8") != std::string::npos);
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
    REQUIRE(c.find(std::string("\x1b") + "7") != std::string::npos);  // cursor saved/restored
    REQUIRE(c.find(std::string("\x1b") + "8") != std::string::npos);
}

TEST_CASE("transport-lost notice draws on the bottom row, never raw to stderr",
          "[shell][reconnect]") {
    auto n = bs::mesh::reconnect_notice_line("host-a", 80, 24);
    REQUIRE(n.find("transport lost") != std::string::npos);
    REQUIRE(n.find("host-a") != std::string::npos);
    // Anchored: cursor-addressed to the bottom row, cursor protected.
    REQUIRE(n.find("\x1b[24;1H") != std::string::npos);
    REQUIRE(n.find(std::string("\x1b") + "7") != std::string::npos);
    REQUIRE(n.find(std::string("\x1b") + "8") != std::string::npos);
    // Never leaves the alternate screen and never clears the whole display.
    REQUIRE(n.find("\x1b[?1049l") == std::string::npos);
    REQUIRE(n.find("\x1b[2J") == std::string::npos);
}

TEST_CASE("badge refresh erases a stranded badge after a resize", "[shell][reconnect]") {
    // Badge was drawn when the terminal was 24 rows; now 20 rows. The refresh
    // must erase row 24 before drawing row 20.
    auto line = bs::mesh::reconnect_status_line("host-a", 2, 0, 80, 20, /*prev_rows=*/24);
    REQUIRE(line.find("\x1b[24;1H\x1b[2K") != std::string::npos);
    REQUIRE(line.find("\x1b[20;1H") != std::string::npos);
    // Same-row refresh emits no redundant erase.
    auto same = bs::mesh::reconnect_status_line("host-a", 2, 0, 80, 24, /*prev_rows=*/24);
    REQUIRE(same.find("\x1b[2K") == std::string::npos);
    // Default (no previous badge) behaves exactly like the old draw.
    auto fresh = bs::mesh::reconnect_status_line("host-a", 1, 0, 80, 24);
    REQUIRE(fresh.find("\x1b[2K") == std::string::npos);
    REQUIRE(fresh.find("\x1b[24;1H") != std::string::npos);
}

TEST_CASE("reattach surface reset clears the screen and shows the cursor",
          "[shell][reconnect]") {
    auto r = bs::mesh::reattach_surface_reset();
    REQUIRE(r.find("\x1b[2J") != std::string::npos);   // clear stale frame remnants
    REQUIRE(r.find("\x1b[H") != std::string::npos);    // home before scrollback replay
    REQUIRE(r.find("\x1b[?25h") != std::string::npos); // cursor always visible after
    REQUIRE(r.find("\x1b[?25l") == std::string::npos); // never hides the cursor
    REQUIRE(r.find("\x1b[?1049") == std::string::npos); // never touches alt-screen here
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
