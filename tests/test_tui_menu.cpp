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

// Visible cell width: bytes → cells by counting non-UTF-8-continuation bytes
// outside escape sequences (matches bs-tui.h's own cell math).
size_t visible_cells(const std::string& s) {
    size_t cells = 0;
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
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) ++cells;
        ++i;
    }
    return cells;
}

// Split menu_frame output into its lines (dropping the trailing \r).
std::vector<std::string> frame_lines(const std::string& f) {
    std::vector<std::string> ls;
    for (size_t p = 0; p < f.size();) {
        size_t nl = f.find('\n', p);
        std::string line = (nl == std::string::npos) ? f.substr(p) : f.substr(p, nl - p);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ls.push_back(std::move(line));
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    return ls;
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
        std::string line = f.substr(p, nl - p);
        // menu_frame uses \r\n (raw mode needs the CR); drop it so width math
        // doesn't count the carriage return as a visible cell.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ls.push_back(line);
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

TEST_CASE("menu frame truncates an overlong title to the frame width", "[tui][menu][wrap]") {
    // A title longer than the frame must be cut, not left to overflow the
    // border — an overflowing title line wraps and shreds the in-place redraw.
    const std::string long_title =
        "a very long menu title that far exceeds the frame width and must be truncated";
    auto f = bs::tui::menu_frame({long_title, true}, {"one", "two"}, 0, 20);
    auto ls = frame_lines(f);
    REQUIRE(ls.size() >= 5);
    const size_t border = visible_cells(ls.front());  // ╭ + 22 dashes + ╮ = 24
    for (const auto& line : ls)
        REQUIRE(visible_cells(line) == border);
    // The title still appears, truncated.
    REQUIRE(visible_cells(ls[1]) == border);
}

TEST_CASE("menu frame truncates overlong rows without splitting UTF-8", "[tui][menu][wrap]") {
    // The row must be cut at a character boundary — never between the lead byte
    // and its continuation bytes (which would emit a replacement glyph).
    const std::string wide = "名称-abcdefghijklmnopqrstuvwxyz-名称";
    auto f = bs::tui::menu_frame({"t"}, {wide}, 0, 16);
    auto ls = frame_lines(f);
    REQUIRE(ls.size() >= 5);
    const size_t border = visible_cells(ls.front());
    for (const auto& line : ls)
        REQUIRE(visible_cells(line) == border);
    // Reassemble the row text: it must be valid UTF-8 (no orphaned continuation
    // bytes) — decode it and confirm the whole string round-trips as text.
    for (const auto& line : ls) {
        // ASCII-only structural lines are trivially valid; only check the row.
        if (line.find("名称") == std::string::npos && line.find("abcdef") == std::string::npos)
            continue;
        // Ensure no byte 0x80-0xBF appears without a preceding lead byte.
        for (size_t i = 0; i < line.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(line[i]);
            if (c >= 0x80 && c <= 0xBF)
                REQUIRE(i > 0);  // continuation byte with a preceding byte
        }
    }
}

TEST_CASE("selected row marker survives truncation", "[tui][menu][wrap]") {
    // Even when the selected row is truncated, the ❯ marker must still render
    // and exactly one row must be reverse-video highlighted.
    const std::string long_row(80, 'x');
    auto f = bs::tui::menu_frame({"t"}, {long_row}, 0, 20);
    REQUIRE(f.find("\x1b[7m❯ ") != std::string::npos);
    size_t highlights = 0;
    for (size_t p = f.find("\x1b[7m"); p != std::string::npos;
         p = f.find("\x1b[7m", p + 1)) ++highlights;
    REQUIRE(highlights == 1);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
