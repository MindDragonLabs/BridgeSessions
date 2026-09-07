// test_tui_menu.cpp — charm-style TUI menu renderer + session picker (26.09.06-r1)
//
// Covers:
//   - Rounded charm-style menu frame with exactly one highlighted row
//   - Fixed-width row padding (no reflow wobble)
//   - Dimmed help footer
//   - Session picker row building (new-session first, died filtered)
//   - Session picker choice → session name mapping (0/1 = new, out-of-range safe)

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include <cctype>
#include <string>
#include <vector>

#include "../bs-protocol.h"
// bs::tui (MenuStyle, menu_frame, menu_footer, session_picker_*) comes from
// bs-protocol.h → bs-tui.h.

namespace {

// Visible length (ANSI escape sequences stripped) — reused by several tests.
size_t visible_len(const std::string& s) {
    size_t vis = 0;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b') {
            if (s.compare(i, 2, "\x1b[") == 0) {
                i += 2;
                while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
                ++i;
            } else {
                i += 2;
            }
            continue;
        }
        ++vis; ++i;
    }
    return vis;
}

} // namespace

TEST_CASE("menu frame draws rounded charm-style box", "[tui][menu]") {
    auto f = bs::tui::menu_frame({"pick a server"}, {"host-a", "host-b"}, 0, 20);
    REQUIRE(f.find("╭") != std::string::npos);
    REQUIRE(f.find("╰") != std::string::npos);
    REQUIRE(f.find("\x1b[7m") != std::string::npos);
    size_t highlights = 0;
    for (size_t p = f.find("\x1b[7m"); p != std::string::npos;
         p = f.find("\x1b[7m", p + 1)) ++highlights;
    REQUIRE(highlights == 1);
}

TEST_CASE("menu frame pads rows to fixed width (no reflow wobble)", "[tui][menu]") {
    auto f = bs::tui::menu_frame({"t"}, {"a", "bbbbbbbbbbbbbbbbbbbb"}, 0, 20);
    size_t lines = 0;
    for (size_t p = 0; p < f.size(); ++p) if (f[p] == '\n') ++lines;
    REQUIRE(lines >= 4);  // top border, title, 2 rows, bottom border
    // Every line inside the box must render the same visible width (no
    // reflow wobble). visible_len counts bytes; multi-byte UTF-8 box glyphs
    // make absolute cell math fragile, so assert cross-line consistency by
    // comparing each line against its own UTF-8-normalized cell width:
    // border line has 24 cells (╭ + 22 + ╮), so every other line must also
    // be 24 cells. Convert bytes → cells by counting non-continuation bytes
    // outside escape sequences.
    auto visible_cells = [&](const std::string& s) {
        size_t cells = 0;
        for (size_t i = 0; i < s.size();) {
            if (s[i] == '\x1b') {  // strip escape sequence
                if (s.compare(i, 2, "\x1b[") == 0) {
                    i += 2;
                    while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
                    ++i;
                } else {
                    i += 2;
                }
                continue;
            }
            unsigned char c = static_cast<unsigned char>(s[i]);
            if ((c & 0xC0) != 0x80) ++cells;  // not a UTF-8 continuation byte
            ++i;
        }
        return cells;
    };
    std::vector<std::string> ls;
    for (size_t p = 0; p < f.size();) {
        size_t nl = f.find('\n', p);
        if (nl == std::string::npos) { ls.push_back(f.substr(p)); break; }
        ls.push_back(f.substr(p, nl - p));
        p = nl + 1;
    }
    REQUIRE(ls.size() >= 5);  // top, title, separator, 2 rows, bottom
    int top = visible_cells(ls[0]);
    REQUIRE(top == 24);       // ╭ + (width+2) dashes + ╮
    for (size_t i = 1; i < ls.size(); ++i)
        REQUIRE(visible_cells(ls[i]) == top);
}

TEST_CASE("footer is dimmed and shows the hint", "[tui][menu]") {
    auto f = bs::tui::menu_footer("↑/↓ move · Enter select · q quit");
    REQUIRE(f.find("↑/↓ move · Enter select · q quit") != std::string::npos);
    REQUIRE(f.find("\x1b[2m") != std::string::npos);
}

TEST_CASE("session picker lists new-session first, skips dead", "[tui][menu][session]") {
    bs::mesh::SessionListMsg list;
    list.sessions.push_back({"hermes", "attached", 3600});
    list.sessions.push_back({"old", "died", 99});
    list.sessions.push_back({"tty-ab12", "detached", 30});
    auto rows = bs::tui::session_picker_rows(list);
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[0].find("New session") != std::string::npos);
    REQUIRE(rows[1].find("hermes") != std::string::npos);
    REQUIRE(rows[1].find("attached") != std::string::npos);
    REQUIRE(rows[2].find("tty-ab12") != std::string::npos);
    REQUIRE(rows[2].find("detached") != std::string::npos);
    for (auto& r : rows) REQUIRE(r.find("old") == std::string::npos);
}

TEST_CASE("session picker maps choice to session name (0/1 = new)", "[tui][menu][session]") {
    bs::mesh::SessionListMsg list;
    list.sessions.push_back({"hermes", "attached", 3600});
    REQUIRE(bs::tui::session_picker_choice(list, 0) == "");
    REQUIRE(bs::tui::session_picker_choice(list, 1) == "");
    REQUIRE(bs::tui::session_picker_choice(list, 2) == "hermes");
    REQUIRE(bs::tui::session_picker_choice(list, 99) == "");
}

TEST_CASE("session picker frame wraps through menu_frame unchanged", "[tui][menu][session]") {
    bs::mesh::SessionListMsg list;
    list.sessions.push_back({"hermes", "attached", 3600});
    auto rows = bs::tui::session_picker_rows(list);
    auto f = bs::tui::menu_frame({"host-a — choose a session:"}, rows, 0, 40);
    REQUIRE(f.find("New session") != std::string::npos);
    REQUIRE(f.find("hermes") != std::string::npos);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
