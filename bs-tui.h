// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-tui.h — Charm-style TUI rendering primitives (menu frames, footers,
// session pickers). Pure functions over strings/protocol structs so they are
// unit-testable without a terminal or a mesh (tests/test_tui_menu.cpp).
// Designed for inclusion inside `namespace bs::mesh { ... }` like the other
// bs-*.h headers; the bs::tui namespace is nested inside bs::mesh then.
#pragma once

#include <cctype>
#include <string>
#include <vector>

#include "bs-codec.h"  // SessionListMsg

namespace bs::tui {

struct MenuStyle { std::string title; bool rounded = true; };

namespace {
// Visible cell count (ANSI escape sequences stripped) for width math.
inline size_t tui_visible_len(const std::string& s) {
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

// Truncate s to at most max_cells visible cells, cutting at a UTF-8 character
// boundary (never mid-glyph). ANSI escape sequences are skipped (not counted)
// so a coloured label truncates by its rendered width, not its byte length.
// Returns s unchanged when it already fits.
inline std::string tui_truncate(std::string s, size_t max_cells) {
    size_t cells = 0;
    size_t cut = s.size();
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
        if ((c & 0xC0) != 0x80) {  // start of a new character (not a continuation byte)
            if (cells == max_cells) { cut = i; break; }
            ++cells;
        }
        ++i;
    }
    if (cut < s.size()) s.resize(cut);
    return s;
}
} // anonymous namespace

inline std::string menu_footer(const std::string& hint) {
    return std::string("\x1b[2m") + hint + "\x1b[0m";
}

// Visible cell width of a row label (so callers can size the frame).
inline size_t tui_row_width(const std::string& s) { return tui_visible_len(s); }

// Rounded charm-style box: dim borders, a title row, one reverse-video
// `❯`-highlighted row at `selected`, rows padded to a fixed visible width.
inline std::string menu_frame(const MenuStyle& style, const std::vector<std::string>& rows,
                              size_t selected, size_t width) {
    const size_t inner = width;  // visible columns between the border padding
    // Box glyphs as string literals — multi-byte CHAR literals ('─') get
    // truncated by GCC to one byte and would render as garbage.
    const std::string kH = "─", kTL = "╭", kTR = "╮", kBL = "╰", kBR = "╯",
                      kV = "│", kML = "├", kMR = "┤";
    auto hline = [&](const std::string& l, const std::string& r) {
        std::string s = "\x1b[2m" + l;
        for (size_t i = 0; i < inner + 2; ++i) s += kH;
        // \r\n, not \n: the caller runs in raw mode (OPOST off), so a bare \n
        // moves down WITHOUT resetting the column — frame lines would drift
        // right one after another and shred the menu (real-PTY bug 2026-09-08).
        return s + r + "\x1b[0m\r\n";
    };
    auto pad = [&](const std::string& s) {
        std::string r = s;
        size_t vis = tui_visible_len(s);
        if (vis < inner) r.append(inner - vis, ' ');
        return r;
    };
    std::string out = hline(kTL, kTR);
    out += "\x1b[2m" + kV + "\x1b[0m " + pad(tui_truncate(style.title, inner))
           + " \x1b[2m" + kV + "\x1b[0m\r\n";
    out += "\x1b[2m" + kML;
    for (size_t i = 0; i < inner + 2; ++i) out += kH;
    out += kMR + "\x1b[0m\r\n";
    for (size_t i = 0; i < rows.size(); ++i) {
        std::string marker = (i == selected) ? "\x1b[7m❯ \x1b[0m" : "  ";
        // Marker occupies 2 visible cells, so rows fit in inner-2 cells to keep
        // every line the same rendered width as the borders. Overlong rows are
        // truncated to inner-2 cells at a UTF-8 character boundary.
        std::string row = tui_truncate(rows[i], inner - 2);
        const size_t vis = tui_visible_len(row);
        if (vis < inner - 2) row.append(inner - 2 - vis, ' ');
        out += "\x1b[2m" + kV + "\x1b[0m " + marker + row + " \x1b[2m" + kV + "\x1b[0m\r\n";
    }
    out += "\x1b[2m" + kBL;
    for (size_t i = 0; i < inner + 2; ++i) out += kH;
    out += kBR + "\x1b[0m";
    return out;
}

// Rows for the interactive session picker: "new session" first, live sessions
// after (died sessions filtered out), each with state + uptime.
inline std::vector<std::string> session_picker_rows(const bs::mesh::SessionListMsg& list) {
    std::vector<std::string> rows;
    rows.push_back("✦ New session");
    for (auto& si : list.sessions) {
        if (si.state == "died") continue;
        rows.push_back(si.name + "  [" + si.state
                       + " · up " + bs::mesh::human_duration(static_cast<uint64_t>(si.uptime_seconds)) + "]");
    }
    return rows;
}

// Map a 1-based picker choice (0 = cancelled) to a session name.
// Returns "" for "new session" (and for cancelled/out-of-range, which callers
// treat the same way: fall through to the ephemeral-session flow).
inline std::string session_picker_choice(const bs::mesh::SessionListMsg& list, size_t choice) {
    if (choice == 0) return "";
    std::vector<std::string> live;
    for (auto& si : list.sessions) if (si.state != "died") live.push_back(si.name);
    if (choice == 1) return "";                       // the "New session" row
    if (choice - 2 < live.size()) return live[choice - 2];
    return "";
}

} // namespace bs::tui
