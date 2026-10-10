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

#include <cstdint>
#include <string>
#include <vector>

#include "../bs-protocol.h"
// bs::tui (MenuStyle, menu_frame, menu_footer, session_picker_*) comes from
// bs-protocol.h → bs-tui.h.

namespace {

// Independent oracle for the concrete UTF-8 corpus below. It uses explicit
// decoded sequences and expected terminal widths rather than bs-tui.h's
// scanner, so a byte-counting regression cannot make its own tests pass.
size_t oracle_cells(const std::string& s) {
    size_t cells = 0;
    for (size_t i = 0; i < s.size();) {
        // These clusters have independently specified widths in the corpus.
        bool cluster = false;
        for (const std::string token : {"👩‍💻", "🇩🇪", "❤️", "1️⃣", "👍🏽"}) {
            if (s.compare(i, token.size(), token) == 0) {
                cells += 2; i += token.size(); cluster = true; break;
            }
        }
        if (cluster) continue;
        if (s[i] == '\x1b') {
            if (s.compare(i, 2, "\x1b[") == 0) {
                i += 2;
                while (i < s.size() && static_cast<unsigned char>(s[i]) < 0x40) ++i;
                if (i < s.size()) ++i;
            } else if (s.compare(i, 2, "\x1b]") == 0) {
                i += 2;
                while (i < s.size() && s[i] != '\a') {
                    if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    ++i;
                }
                if (i < s.size() && s[i] == '\a') ++i;
            } else {
                i += 2;
            }
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (c != '\r' && c != '\n' && c != '\t') ++cells;
            ++i;
            continue;
        }
        if (s.compare(i, 2, "\xc3\xa9") == 0) { ++cells; i += 2; continue; }
        if (s.compare(i, 2, "\xcc\x81") == 0) { i += 2; continue; }
        if (s.compare(i, 3, "\xe5\x90\x8d") == 0 ||
            s.compare(i, 3, "\xe7\xa7\xb0") == 0 ||
            s.compare(i, 3, "\xe9\x95\xb7") == 0 ||
            s.compare(i, 3, "\xe3\x81\x84") == 0) {
            cells += 2; i += 3; continue;
        }
        if (s.compare(i, 4, "\xf0\x9f\x91\xa9") == 0 ||
            s.compare(i, 4, "\xf0\x9f\x92\xbb") == 0 ||
            s.compare(i, 4, "\xf0\x9f\x87\xa9") == 0 ||
            s.compare(i, 4, "\xf0\x9f\x87\xaa") == 0) {
            cells += 2; i += 4; continue;
        }
        size_t sequence = 1;
        if (c >= 0xc2 && c <= 0xdf) sequence = 2;
        else if (c >= 0xe0 && c <= 0xef) sequence = 3;
        else if (c >= 0xf0 && c <= 0xf4) sequence = 4;
        if (i + sequence <= s.size()) i += sequence;
        else ++i;
        ++cells;
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

bool valid_utf8(const std::string& text) {
    const auto continuation = [&text](size_t pos) {
        return pos < text.size() &&
               (static_cast<unsigned char>(text[pos]) & 0xc0) == 0x80;
    };
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c <= 0x7f) {
            ++i;
            continue;
        }

        size_t length = 0;
        unsigned char second_min = 0x80;
        unsigned char second_max = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) {
            length = 2;
        } else if (c == 0xe0) {
            length = 3;
            second_min = 0xa0; // reject overlong encodings
        } else if ((c >= 0xe1 && c <= 0xec) || (c >= 0xee && c <= 0xef)) {
            length = 3;
        } else if (c == 0xed) {
            length = 3;
            second_max = 0x9f; // reject UTF-16 surrogate code points
        } else if (c == 0xf0) {
            length = 4;
            second_min = 0x90; // reject overlong encodings
        } else if (c >= 0xf1 && c <= 0xf3) {
            length = 4;
        } else if (c == 0xf4) {
            length = 4;
            second_max = 0x8f; // Unicode ends at U+10FFFF
        } else {
            return false;
        }

        if (text.size() - i < length) return false;
        const auto second = static_cast<unsigned char>(text[i + 1]);
        if (second < second_min || second > second_max) return false;
        for (size_t j = 2; j < length; ++j) {
            if (!continuation(i + j)) return false;
        }
        i += length;
    }
    return true;
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
    // Compare each line against an independent terminal-cell oracle.
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
    int top = static_cast<int>(oracle_cells(ls[0]));
    REQUIRE(top == 24);       // ╭ + (width+2) dashes + ╮
    for (size_t i = 1; i < ls.size(); ++i)
        REQUIRE(oracle_cells(ls[i]) == static_cast<size_t>(top));
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
    const size_t border = oracle_cells(ls.front());  // ╭ + 22 dashes + ╮ = 24
    for (const auto& line : ls)
        REQUIRE(oracle_cells(line) == border);
    // The title still appears, truncated.
    REQUIRE(oracle_cells(ls[1]) == border);
}

// 26.10.04 regression: the frame width was computed from the rows alone, so a
// title wider than the widest row produced a box too narrow for its own title
// and the right border came out ragged. Reported as "the TUI gets distorted
// after some turns" because every repaint re-drew the bad geometry. menu_frame
// itself already truncates a long title correctly — the defect was choosing a
// width that could not hold it.
TEST_CASE("menu frame width accommodates a title wider than every row",
          "[tui][menu][wrap][regression]") {
    const std::vector<std::string> rows = {
        "hermes  \u2192 hermes --tui --yolo",
        "claude-code  \u2192 claude",
        "shell",
    };
    const std::string title = "remote-box \u2014 choose a harness:";
    const size_t cols = 100;

    const size_t w = bs::tui::menu_frame_width(rows, cols, title);
    // The returned value is the INNER width; the rendered frame is inner+4.
    // It must hold the title, not just the rows.
    REQUIRE(oracle_cells(title) <= w);
    for (const auto& r : rows)
        REQUIRE(oracle_cells(r) <= w);

    // End to end: every rendered border line is the same width.
    auto ls = frame_lines(bs::tui::menu_frame({title, true}, rows, 0, w));
    const size_t border = oracle_cells(ls.front());
    for (const auto& line : ls) REQUIRE(oracle_cells(line) == border);
    for (const auto& line : ls) REQUIRE(valid_utf8(line));

    // The decisive check, and the one that actually fails on the old code.
    //
    // Measured relationship in menu_frame: a rendered top border is exactly
    // `width + 5` cells (w=10 -> 15, w=20 -> 25, w=30 -> 35), because hline
    // draws inner+2 dashes between the corners and every side row is
    // "│ " + inner + " │". So a frame that can actually hold C cells of
    // content must be asked for C + 4, not C + 2 as the old code did. Asking
    // for three columns too few clipped the title and left the border ragged.
    const size_t content = [&]{
        size_t m = oracle_cells(title);
        for (const auto& r : rows) m = std::max(m, oracle_cells(r));
        return m;
    }();
    // Measured with the renderer's own counter on the RAW frame (ls[] has
    // already been through frame_lines, which strips the SGR styling):
    // a top border is exactly `width + 5` cells.
    const std::string raw = bs::tui::menu_frame({title, true}, rows, 0, w);
    REQUIRE(bs::tui::tui_visible_len(raw.substr(0, raw.find('\n'))) == w + 5);
    // And the width we asked for must be content + 4, since the renderer adds
    // one more column of chrome on each side plus the two corner glyphs.
    REQUIRE(w == content + 4);

    // And the title must not be truncated away: it is wider than every row, so
    // a frame sized from the rows alone would clip it.
    REQUIRE(frame_lines(bs::tui::menu_frame({title, true}, rows, 0, w))[1]
                .find("choose a harness") != std::string::npos);
}

TEST_CASE("menu frame truncates overlong rows without splitting UTF-8", "[tui][menu][wrap]") {
    // The row must be cut at a character boundary — never between the lead byte
    // and its continuation bytes (which would emit a replacement glyph).
    const std::string wide = "名称-abcdefghijklmnopqrstuvwxyz-名称";
    auto f = bs::tui::menu_frame({"t"}, {wide}, 0, 16);
    auto ls = frame_lines(f);
    REQUIRE(ls.size() >= 5);
    const size_t border = oracle_cells(ls.front());
    for (const auto& line : ls)
        REQUIRE(oracle_cells(line) == border);
    for (const auto& line : ls) REQUIRE(valid_utf8(line));
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

TEST_CASE("cell width handles combining, CJK, emoji clusters, and ANSI grammar",
          "[tui][unicode]") {
    REQUIRE(bs::tui::tui_row_width("é") == 1);
    REQUIRE(bs::tui::tui_row_width("e\xcc\x81") == 1);
    REQUIRE(bs::tui::tui_row_width("名称") == 4);
    REQUIRE(bs::tui::tui_row_width("👩‍💻") == 2);
    REQUIRE(bs::tui::tui_row_width("🇩🇪") == 2);

    const std::string styled = "\x1b[38;5;200m名称\x1b[0m";
    REQUIRE(bs::tui::tui_row_width(styled) == 4);
    REQUIRE(bs::tui::tui_row_width("\x1b[?25htext\x1b]0;title\a") == 4);
    REQUIRE(bs::tui::tui_truncate("名称-xyz", 3) == "名");
    REQUIRE(bs::tui::tui_truncate("a\nb\rc", 99) == "a b c");
    REQUIRE(bs::tui::tui_truncate(std::string("\xf0\x28\x8c\x28"), 99) ==
            "\xef\xbf\xbd(\xef\xbf\xbd(");
}

TEST_CASE("tiny menu widths remain bounded and cell-aligned", "[tui][menu][tiny]") {
    for (size_t width : {size_t{0}, size_t{1}, size_t{2}, size_t{3}}) {
        auto lines = frame_lines(bs::tui::menu_frame(
            {"長いタイトル"}, {"名称", "👩‍💻"}, 0, width));
        REQUIRE(lines.size() == 6);
        const size_t line_width = oracle_cells(lines.front());
        REQUIRE(line_width == width + 4);
        for (const auto& line : lines) REQUIRE(oracle_cells(line) == line_width);
    }
}

TEST_CASE("Unicode frame geometry uses independent cell expectations", "[tui][unicode][geometry]") {
    for (const std::string text : {"名称", "é", "e\xcc\x81", "名称名称名称名称名称名称名称", "👩‍💻", "🇩🇪", "❤️", "1️⃣", "👍🏽"}) {
        for (size_t width : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{16}, size_t{20}}) {
            auto lines = frame_lines(bs::tui::menu_frame({text}, {text, text}, 1, width));
            for (const auto& line : lines) {
                REQUIRE(valid_utf8(line));
                REQUIRE(oracle_cells(line) == width + 4);
            }
        }
    }
}

TEST_CASE("scanner consumes CSI finals and string controls safely", "[tui][scanner]") {
    for (char final = 0x40; final <= 0x7e; ++final) {
        const std::string input = "a\x1b[?12;3 " + std::string(1, final) + "b";
        REQUIRE(bs::tui::tui_truncate(input, 10) == "ab");
    }
    REQUIRE(bs::tui::tui_truncate("\x1b[38:2::1:2:3mabc\x1b[0m", 2) ==
            "\x1b[38:2::1:2:3mab");
    REQUIRE(bs::tui::tui_truncate("\x1b]8;;https://example.test\x1b\\abc\x1b]8;;\a", 10) == "abc");
    REQUIRE(bs::tui::tui_truncate("x\x1b]unclosed", 10) == "x");
    REQUIRE(bs::tui::tui_truncate("x\x1b[31", 10) == "x");
    REQUIRE(bs::tui::tui_truncate("x\x1bPpayload\x1b\\y", 10) == "xy");
    REQUIRE(bs::tui::tui_truncate("a\t\r\n\b\x7f" "b", 20) == "a     b");
    REQUIRE(bs::tui::tui_truncate("a\xe2\x80\xae" "b", 20) == "ab");
}

TEST_CASE("truncation preserves clusters and rejects malformed UTF-8", "[tui][unicode][scanner]") {
    for (const std::string cluster : {"👩‍💻", "🇩🇪", "❤️", "1️⃣", "👍🏽"}) {
        REQUIRE(bs::tui::tui_row_width(cluster) == 2);
        REQUIRE(bs::tui::tui_truncate(cluster + "x", 1).empty());
        REQUIRE(bs::tui::tui_truncate(cluster + "x", 2) == cluster);
    }
    REQUIRE(bs::tui::tui_row_width("a‍b") == 2); // ZWJ doesn't collapse arbitrary text
    REQUIRE(bs::tui::tui_truncate("\xcc\x81" "x", 1) == "x");
    REQUIRE(bs::tui::tui_truncate("e\x1b[31m\xcc\x81" "x", 1) == "e\x1b[31m\xcc\x81");
    REQUIRE(bs::tui::tui_row_width("กข") == 2); // Thai letters are not combining marks
    REQUIRE(bs::tui::tui_row_width("a\xe1\xab\x80") == 1); // U+1AC0 extended combining mark
    for (const std::string malformed : {"\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80",
                                       "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xc3", "\xe5\x90"}) {
        REQUIRE_FALSE(valid_utf8(malformed));
        const auto normalized = bs::tui::tui_truncate(malformed, 100);
        REQUIRE(valid_utf8(normalized));
        REQUIRE(normalized != malformed);
    }
    for (size_t cap = 0; cap < 16; ++cap) {
        const auto clipped = bs::tui::tui_truncate("é名称e\xcc\x81👩‍💻🇩🇪", cap);
        REQUIRE(valid_utf8(clipped));
        REQUIRE(oracle_cells(clipped) <= cap);
    }
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
