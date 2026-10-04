// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-tui.h — Charm-style TUI rendering primitives (menu frames, footers,
// session pickers). Pure functions over strings/protocol structs so they are
// unit-testable without a terminal or a mesh (tests/test_tui_menu.cpp).
// Designed for inclusion inside `namespace bs::mesh { ... }` like the other
// bs-*.h headers; the bs::tui namespace is nested inside bs::mesh then.
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "bs-codec.h"  // SessionListMsg

namespace bs::tui {

struct MenuStyle { std::string title; bool rounded = true; };

namespace {
// Cell-width and truncation helpers live here so the TUI has no locale or
// dependency-family requirement.
struct TuiDecoded {
    uint32_t cp = 0xfffd;
    size_t next = 0;
    bool valid = false;
};

struct TuiPiece {
    std::string bytes;
    size_t cells = 0;
    bool ansi = false;
};

inline bool tui_is_continuation(unsigned char c) { return (c & 0xc0) == 0x80; }

inline TuiDecoded tui_decode(const std::string& s, size_t at) {
    const size_t n = s.size();
    const unsigned char a = static_cast<unsigned char>(s[at]);
    if (a < 0x80) return {a, at + 1, true};
    if (a >= 0xc2 && a <= 0xdf && at + 1 < n) {
        const unsigned char b = static_cast<unsigned char>(s[at + 1]);
        if (tui_is_continuation(b))
            return {static_cast<uint32_t>(a & 0x1f) << 6 | (b & 0x3f), at + 2, true};
    }
    if (a >= 0xe0 && a <= 0xef && at + 2 < n) {
        const unsigned char b = static_cast<unsigned char>(s[at + 1]);
        const unsigned char c = static_cast<unsigned char>(s[at + 2]);
        const bool second_ok = (a != 0xe0 || b >= 0xa0) &&
                               (a != 0xed || b <= 0x9f);
        if (second_ok && tui_is_continuation(b) && tui_is_continuation(c))
            return {static_cast<uint32_t>(a & 0x0f) << 12 |
                        static_cast<uint32_t>(b & 0x3f) << 6 | (c & 0x3f),
                    at + 3, true};
    }
    if (a >= 0xf0 && a <= 0xf4 && at + 3 < n) {
        const unsigned char b = static_cast<unsigned char>(s[at + 1]);
        const unsigned char c = static_cast<unsigned char>(s[at + 2]);
        const unsigned char d = static_cast<unsigned char>(s[at + 3]);
        const bool second_ok = (a != 0xf0 || b >= 0x90) &&
                               (a != 0xf4 || b <= 0x8f);
        if (second_ok && tui_is_continuation(b) && tui_is_continuation(c) &&
            tui_is_continuation(d))
            return {static_cast<uint32_t>(a & 0x07) << 18 |
                        static_cast<uint32_t>(b & 0x3f) << 12 |
                        static_cast<uint32_t>(c & 0x3f) << 6 | (d & 0x3f),
                    at + 4, true};
    }
    return {0xfffd, at + 1, false};
}

inline bool tui_csi_final(unsigned char c) { return c >= 0x40 && c <= 0x7e; }

// CSI uses parameter bytes 0x30-0x3f, intermediate bytes 0x20-0x2f, then
// exactly one final byte 0x40-0x7e. OSC is consumed through BEL or ST. Only
// SGR is retained; cursor/control sequences from labels are discarded.
inline size_t tui_escape_end(const std::string& s, size_t at, bool& safe_sgr) {
    safe_sgr = false;
    if (at + 1 >= s.size()) return s.size();
    const unsigned char kind = static_cast<unsigned char>(s[at + 1]);
    if (kind == '[') {
        size_t p = at + 2;
        while (p < s.size() && static_cast<unsigned char>(s[p]) >= 0x30 &&
               static_cast<unsigned char>(s[p]) <= 0x3f) ++p;
        const size_t parameters_end = p;
        while (p < s.size() && static_cast<unsigned char>(s[p]) >= 0x20 &&
               static_cast<unsigned char>(s[p]) <= 0x2f) ++p;
        if (p < s.size() && tui_csi_final(static_cast<unsigned char>(s[p]))) {
            safe_sgr = s[p] == 'm' && p == parameters_end &&
                s.find_first_not_of("0123456789;:", at + 2) >= parameters_end;
            return p + 1;
        }
        if (p == s.size()) return p; // incomplete CSI is not label text
        return at + 1;  // malformed CSI: discard only ESC, inspect the rest
    }
    if (kind == ']') {
        for (size_t p = at + 2; p < s.size(); ++p) {
            const unsigned char c = static_cast<unsigned char>(s[p]);
            if (c == 0x07 || c == 0x9c) return p + 1;
            if (c == 0x1b && p + 1 < s.size() && s[p + 1] == '\\') return p + 2;
        }
        return s.size();  // unterminated OSC cannot print text
    }
    if (kind == 'P' || kind == '^' || kind == '_') {
        const size_t end = s.find("\x1b\\", at + 2);
        return end == std::string::npos ? s.size() : end + 2;
    }
    return at + 2;  // unsupported 2-byte ESC sequence: never emit it
}

inline bool tui_is_combining(uint32_t c) {
    // Unicode 16.0.0 Mn/Me intervals; no process locale dependency.
    static constexpr uint32_t ranges[][2] = {
        {0x300, 0x36f},
        {0x483, 0x489},
        {0x591, 0x5bd},
        {0x5bf, 0x5bf},
        {0x5c1, 0x5c2},
        {0x5c4, 0x5c5},
        {0x5c7, 0x5c7},
        {0x610, 0x61a},
        {0x64b, 0x65f},
        {0x670, 0x670},
        {0x6d6, 0x6dc},
        {0x6df, 0x6e4},
        {0x6e7, 0x6e8},
        {0x6ea, 0x6ed},
        {0x711, 0x711},
        {0x730, 0x74a},
        {0x7a6, 0x7b0},
        {0x7eb, 0x7f3},
        {0x7fd, 0x7fd},
        {0x816, 0x819},
        {0x81b, 0x823},
        {0x825, 0x827},
        {0x829, 0x82d},
        {0x859, 0x85b},
        {0x897, 0x89f},
        {0x8ca, 0x8e1},
        {0x8e3, 0x902},
        {0x93a, 0x93a},
        {0x93c, 0x93c},
        {0x941, 0x948},
        {0x94d, 0x94d},
        {0x951, 0x957},
        {0x962, 0x963},
        {0x981, 0x981},
        {0x9bc, 0x9bc},
        {0x9c1, 0x9c4},
        {0x9cd, 0x9cd},
        {0x9e2, 0x9e3},
        {0x9fe, 0x9fe},
        {0xa01, 0xa02},
        {0xa3c, 0xa3c},
        {0xa41, 0xa42},
        {0xa47, 0xa48},
        {0xa4b, 0xa4d},
        {0xa51, 0xa51},
        {0xa70, 0xa71},
        {0xa75, 0xa75},
        {0xa81, 0xa82},
        {0xabc, 0xabc},
        {0xac1, 0xac5},
        {0xac7, 0xac8},
        {0xacd, 0xacd},
        {0xae2, 0xae3},
        {0xafa, 0xaff},
        {0xb01, 0xb01},
        {0xb3c, 0xb3c},
        {0xb3f, 0xb3f},
        {0xb41, 0xb44},
        {0xb4d, 0xb4d},
        {0xb55, 0xb56},
        {0xb62, 0xb63},
        {0xb82, 0xb82},
        {0xbc0, 0xbc0},
        {0xbcd, 0xbcd},
        {0xc00, 0xc00},
        {0xc04, 0xc04},
        {0xc3c, 0xc3c},
        {0xc3e, 0xc40},
        {0xc46, 0xc48},
        {0xc4a, 0xc4d},
        {0xc55, 0xc56},
        {0xc62, 0xc63},
        {0xc81, 0xc81},
        {0xcbc, 0xcbc},
        {0xcbf, 0xcbf},
        {0xcc6, 0xcc6},
        {0xccc, 0xccd},
        {0xce2, 0xce3},
        {0xd00, 0xd01},
        {0xd3b, 0xd3c},
        {0xd41, 0xd44},
        {0xd4d, 0xd4d},
        {0xd62, 0xd63},
        {0xd81, 0xd81},
        {0xdca, 0xdca},
        {0xdd2, 0xdd4},
        {0xdd6, 0xdd6},
        {0xe31, 0xe31},
        {0xe34, 0xe3a},
        {0xe47, 0xe4e},
        {0xeb1, 0xeb1},
        {0xeb4, 0xebc},
        {0xec8, 0xece},
        {0xf18, 0xf19},
        {0xf35, 0xf35},
        {0xf37, 0xf37},
        {0xf39, 0xf39},
        {0xf71, 0xf7e},
        {0xf80, 0xf84},
        {0xf86, 0xf87},
        {0xf8d, 0xf97},
        {0xf99, 0xfbc},
        {0xfc6, 0xfc6},
        {0x102d, 0x1030},
        {0x1032, 0x1037},
        {0x1039, 0x103a},
        {0x103d, 0x103e},
        {0x1058, 0x1059},
        {0x105e, 0x1060},
        {0x1071, 0x1074},
        {0x1082, 0x1082},
        {0x1085, 0x1086},
        {0x108d, 0x108d},
        {0x109d, 0x109d},
        {0x135d, 0x135f},
        {0x1712, 0x1714},
        {0x1732, 0x1733},
        {0x1752, 0x1753},
        {0x1772, 0x1773},
        {0x17b4, 0x17b5},
        {0x17b7, 0x17bd},
        {0x17c6, 0x17c6},
        {0x17c9, 0x17d3},
        {0x17dd, 0x17dd},
        {0x180b, 0x180d},
        {0x180f, 0x180f},
        {0x1885, 0x1886},
        {0x18a9, 0x18a9},
        {0x1920, 0x1922},
        {0x1927, 0x1928},
        {0x1932, 0x1932},
        {0x1939, 0x193b},
        {0x1a17, 0x1a18},
        {0x1a1b, 0x1a1b},
        {0x1a56, 0x1a56},
        {0x1a58, 0x1a5e},
        {0x1a60, 0x1a60},
        {0x1a62, 0x1a62},
        {0x1a65, 0x1a6c},
        {0x1a73, 0x1a7c},
        {0x1a7f, 0x1a7f},
        {0x1ab0, 0x1ace},
        {0x1b00, 0x1b03},
        {0x1b34, 0x1b34},
        {0x1b36, 0x1b3a},
        {0x1b3c, 0x1b3c},
        {0x1b42, 0x1b42},
        {0x1b6b, 0x1b73},
        {0x1b80, 0x1b81},
        {0x1ba2, 0x1ba5},
        {0x1ba8, 0x1ba9},
        {0x1bab, 0x1bad},
        {0x1be6, 0x1be6},
        {0x1be8, 0x1be9},
        {0x1bed, 0x1bed},
        {0x1bef, 0x1bf1},
        {0x1c2c, 0x1c33},
        {0x1c36, 0x1c37},
        {0x1cd0, 0x1cd2},
        {0x1cd4, 0x1ce0},
        {0x1ce2, 0x1ce8},
        {0x1ced, 0x1ced},
        {0x1cf4, 0x1cf4},
        {0x1cf8, 0x1cf9},
        {0x1dc0, 0x1dff},
        {0x20d0, 0x20f0},
        {0x2cef, 0x2cf1},
        {0x2d7f, 0x2d7f},
        {0x2de0, 0x2dff},
        {0x302a, 0x302d},
        {0x3099, 0x309a},
        {0xa66f, 0xa672},
        {0xa674, 0xa67d},
        {0xa69e, 0xa69f},
        {0xa6f0, 0xa6f1},
        {0xa802, 0xa802},
        {0xa806, 0xa806},
        {0xa80b, 0xa80b},
        {0xa825, 0xa826},
        {0xa82c, 0xa82c},
        {0xa8c4, 0xa8c5},
        {0xa8e0, 0xa8f1},
        {0xa8ff, 0xa8ff},
        {0xa926, 0xa92d},
        {0xa947, 0xa951},
        {0xa980, 0xa982},
        {0xa9b3, 0xa9b3},
        {0xa9b6, 0xa9b9},
        {0xa9bc, 0xa9bd},
        {0xa9e5, 0xa9e5},
        {0xaa29, 0xaa2e},
        {0xaa31, 0xaa32},
        {0xaa35, 0xaa36},
        {0xaa43, 0xaa43},
        {0xaa4c, 0xaa4c},
        {0xaa7c, 0xaa7c},
        {0xaab0, 0xaab0},
        {0xaab2, 0xaab4},
        {0xaab7, 0xaab8},
        {0xaabe, 0xaabf},
        {0xaac1, 0xaac1},
        {0xaaec, 0xaaed},
        {0xaaf6, 0xaaf6},
        {0xabe5, 0xabe5},
        {0xabe8, 0xabe8},
        {0xabed, 0xabed},
        {0xfb1e, 0xfb1e},
        {0xfe00, 0xfe0f},
        {0xfe20, 0xfe2f},
        {0x101fd, 0x101fd},
        {0x102e0, 0x102e0},
        {0x10376, 0x1037a},
        {0x10a01, 0x10a03},
        {0x10a05, 0x10a06},
        {0x10a0c, 0x10a0f},
        {0x10a38, 0x10a3a},
        {0x10a3f, 0x10a3f},
        {0x10ae5, 0x10ae6},
        {0x10d24, 0x10d27},
        {0x10d69, 0x10d6d},
        {0x10eab, 0x10eac},
        {0x10efc, 0x10eff},
        {0x10f46, 0x10f50},
        {0x10f82, 0x10f85},
        {0x11001, 0x11001},
        {0x11038, 0x11046},
        {0x11070, 0x11070},
        {0x11073, 0x11074},
        {0x1107f, 0x11081},
        {0x110b3, 0x110b6},
        {0x110b9, 0x110ba},
        {0x110c2, 0x110c2},
        {0x11100, 0x11102},
        {0x11127, 0x1112b},
        {0x1112d, 0x11134},
        {0x11173, 0x11173},
        {0x11180, 0x11181},
        {0x111b6, 0x111be},
        {0x111c9, 0x111cc},
        {0x111cf, 0x111cf},
        {0x1122f, 0x11231},
        {0x11234, 0x11234},
        {0x11236, 0x11237},
        {0x1123e, 0x1123e},
        {0x11241, 0x11241},
        {0x112df, 0x112df},
        {0x112e3, 0x112ea},
        {0x11300, 0x11301},
        {0x1133b, 0x1133c},
        {0x11340, 0x11340},
        {0x11366, 0x1136c},
        {0x11370, 0x11374},
        {0x113bb, 0x113c0},
        {0x113ce, 0x113ce},
        {0x113d0, 0x113d0},
        {0x113d2, 0x113d2},
        {0x113e1, 0x113e2},
        {0x11438, 0x1143f},
        {0x11442, 0x11444},
        {0x11446, 0x11446},
        {0x1145e, 0x1145e},
        {0x114b3, 0x114b8},
        {0x114ba, 0x114ba},
        {0x114bf, 0x114c0},
        {0x114c2, 0x114c3},
        {0x115b2, 0x115b5},
        {0x115bc, 0x115bd},
        {0x115bf, 0x115c0},
        {0x115dc, 0x115dd},
        {0x11633, 0x1163a},
        {0x1163d, 0x1163d},
        {0x1163f, 0x11640},
        {0x116ab, 0x116ab},
        {0x116ad, 0x116ad},
        {0x116b0, 0x116b5},
        {0x116b7, 0x116b7},
        {0x1171d, 0x1171d},
        {0x1171f, 0x1171f},
        {0x11722, 0x11725},
        {0x11727, 0x1172b},
        {0x1182f, 0x11837},
        {0x11839, 0x1183a},
        {0x1193b, 0x1193c},
        {0x1193e, 0x1193e},
        {0x11943, 0x11943},
        {0x119d4, 0x119d7},
        {0x119da, 0x119db},
        {0x119e0, 0x119e0},
        {0x11a01, 0x11a0a},
        {0x11a33, 0x11a38},
        {0x11a3b, 0x11a3e},
        {0x11a47, 0x11a47},
        {0x11a51, 0x11a56},
        {0x11a59, 0x11a5b},
        {0x11a8a, 0x11a96},
        {0x11a98, 0x11a99},
        {0x11c30, 0x11c36},
        {0x11c38, 0x11c3d},
        {0x11c3f, 0x11c3f},
        {0x11c92, 0x11ca7},
        {0x11caa, 0x11cb0},
        {0x11cb2, 0x11cb3},
        {0x11cb5, 0x11cb6},
        {0x11d31, 0x11d36},
        {0x11d3a, 0x11d3a},
        {0x11d3c, 0x11d3d},
        {0x11d3f, 0x11d45},
        {0x11d47, 0x11d47},
        {0x11d90, 0x11d91},
        {0x11d95, 0x11d95},
        {0x11d97, 0x11d97},
        {0x11ef3, 0x11ef4},
        {0x11f00, 0x11f01},
        {0x11f36, 0x11f3a},
        {0x11f40, 0x11f40},
        {0x11f42, 0x11f42},
        {0x11f5a, 0x11f5a},
        {0x13440, 0x13440},
        {0x13447, 0x13455},
        {0x1611e, 0x16129},
        {0x1612d, 0x1612f},
        {0x16af0, 0x16af4},
        {0x16b30, 0x16b36},
        {0x16f4f, 0x16f4f},
        {0x16f8f, 0x16f92},
        {0x16fe4, 0x16fe4},
        {0x1bc9d, 0x1bc9e},
        {0x1cf00, 0x1cf2d},
        {0x1cf30, 0x1cf46},
        {0x1d167, 0x1d169},
        {0x1d17b, 0x1d182},
        {0x1d185, 0x1d18b},
        {0x1d1aa, 0x1d1ad},
        {0x1d242, 0x1d244},
        {0x1da00, 0x1da36},
        {0x1da3b, 0x1da6c},
        {0x1da75, 0x1da75},
        {0x1da84, 0x1da84},
        {0x1da9b, 0x1da9f},
        {0x1daa1, 0x1daaf},
        {0x1e000, 0x1e006},
        {0x1e008, 0x1e018},
        {0x1e01b, 0x1e021},
        {0x1e023, 0x1e024},
        {0x1e026, 0x1e02a},
        {0x1e08f, 0x1e08f},
        {0x1e130, 0x1e136},
        {0x1e2ae, 0x1e2ae},
        {0x1e2ec, 0x1e2ef},
        {0x1e4ec, 0x1e4ef},
        {0x1e5ee, 0x1e5ef},
        {0x1e8d0, 0x1e8d6},
        {0x1e944, 0x1e94a},
        {0xe0100, 0xe01ef},
    };
    size_t lo = 0, hi = sizeof(ranges) / sizeof(ranges[0]);
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (c < ranges[mid][0]) hi = mid;
        else if (c > ranges[mid][1]) lo = mid + 1;
        else return true;
    }
    return (c >= 0x1160 && c <= 0x11ff); // Hangul medial/final jamo
}

inline bool tui_is_zero_width_format(uint32_t c) {
    return c == 0x200b || c == 0x200c || c == 0x200d ||
           (c >= 0x2060 && c <= 0x2064) || c == 0xfeff ||
           (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0x1f3fb && c <= 0x1f3ff) ||
           (c >= 0xe0020 && c <= 0xe007f);
}

inline bool tui_is_emoji_presentation(uint32_t c) {
    return (c >= 0x1f000 && c <= 0x1faff) || c == 0x231a || c == 0x231b ||
           (c >= 0x23e9 && c <= 0x23f3) || (c >= 0x23f8 && c <= 0x23fa) ||
           c == 0x24c2 || (c >= 0x25fd && c <= 0x25fe) ||
           c == 0x2614 || c == 0x2615 || (c >= 0x2648 && c <= 0x2653) ||
           c == 0x267f || c == 0x2693 || c == 0x26a1 ||
           (c >= 0x26aa && c <= 0x26ab) || (c >= 0x26bd && c <= 0x26be) ||
           (c >= 0x26c4 && c <= 0x26c5) || c == 0x26ce || c == 0x26d4 ||
           c == 0x26ea || (c >= 0x26f2 && c <= 0x26f3) || c == 0x26f5 ||
           c == 0x26fa || c == 0x26fd || c == 0x2705 ||
           (c >= 0x270a && c <= 0x270b) || c == 0x2728 ||
           c == 0x274c || c == 0x274e || (c >= 0x2753 && c <= 0x2755) ||
           c == 0x2757 || (c >= 0x2795 && c <= 0x2797) || c == 0x27b0 ||
           c == 0x27bf;
}

inline bool tui_is_emoji_base(uint32_t c) {
    return tui_is_emoji_presentation(c) || (c >= 0x2600 && c <= 0x27bf) ||
           c == 0x00a9 || c == 0x00ae || c == 0x203c || c == 0x2049 ||
           c == 0x2122 || c == 0x2139 || (c >= 0x2194 && c <= 0x21ff) ||
           (c >= 0x2300 && c <= 0x23ff);
}

inline bool tui_is_wide(uint32_t c) {
    return (c >= 0x1100 && c <= 0x115f) || c == 0x2329 || c == 0x232a ||
           (c >= 0x2e80 && c <= 0x303e) || (c >= 0x3040 && c <= 0x3247) ||
           (c >= 0x3250 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0xa4cf) ||
           (c >= 0xa960 && c <= 0xa97c) || (c >= 0xac00 && c <= 0xd7a3) ||
           (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe10 && c <= 0xfe19) ||
           (c >= 0xfe30 && c <= 0xfe6b) || (c >= 0xff01 && c <= 0xff60) ||
           (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x20000 && c <= 0x3fffd);
}

inline size_t tui_codepoint_width(uint32_t c) {
    if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || tui_is_combining(c) ||
        tui_is_zero_width_format(c)) return 0;
    if (tui_is_wide(c) || tui_is_emoji_presentation(c)) return 2;
    return 1;
}

struct TuiUnit { uint32_t cp; std::string bytes; };

inline TuiUnit tui_unit(const std::string& s, size_t& at) {
    const size_t begin = at;
    const TuiDecoded d = tui_decode(s, at);
    at = d.next;
    if (d.cp < 0x20 || (d.cp >= 0x7f && d.cp <= 0x9f) ||
        d.cp == 0x2028 || d.cp == 0x2029)
        return {0x20, " "};
    if (!d.valid) return {0xfffd, "\xef\xbf\xbd"};
    // Bidi overrides/isolation can reorder borders; soft hyphens can change
    // width at wrap. Strip them rather than trusting terminal implementation.
    if (d.cp == 0x00ad || d.cp == 0x061c || d.cp == 0x200e || d.cp == 0x200f ||
        (d.cp >= 0x202a && d.cp <= 0x202e) ||
        (d.cp >= 0x2066 && d.cp <= 0x206f)) return {0, ""};
    return {d.cp, s.substr(begin, d.next - begin)};
}

inline bool tui_is_cluster_tail(uint32_t c) {
    return tui_is_combining(c) || tui_is_zero_width_format(c);
}

inline std::vector<TuiPiece> tui_pieces(const std::string& s) {
    std::vector<TuiPiece> pieces;
    for (size_t at = 0; at < s.size();) {
        if (static_cast<unsigned char>(s[at]) == 0x1b) {
            bool safe_sgr = false;
            const size_t end = tui_escape_end(s, at, safe_sgr);
            if (safe_sgr) pieces.push_back({s.substr(at, end - at), 0, true});
            at = end > at ? end : at + 1;
            continue;
        }

        std::vector<TuiUnit> units;
        size_t next = at;
        units.push_back(tui_unit(s, next));
        if (tui_is_cluster_tail(units.front().cp) || units.front().cp == 0) {
            // SGR may sit between a base and its accent. Keep that accent with
            // the base so truncation cannot separate them or touch a border.
            if (tui_is_combining(units.front().cp)) {
                size_t base = pieces.size();
                while (base > 0 && pieces[base - 1].ansi) --base;
                if (base > 0) {
                    for (size_t i = base; i < pieces.size(); ++i)
                        pieces[base - 1].bytes += pieces[i].bytes;
                    pieces.resize(base);
                    pieces.back().bytes += units.front().bytes;
                }
            }
            at = next; // orphan marks must not modify the menu's preceding border
            continue;
        }
        // Regional-indicator pairs are one flag (two cells), not four.
        if (units.front().cp >= 0x1f1e6 && units.front().cp <= 0x1f1ff &&
            next < s.size() && static_cast<unsigned char>(s[next]) != 0x1b) {
            size_t probe = next;
            TuiUnit second = tui_unit(s, probe);
            if (second.cp >= 0x1f1e6 && second.cp <= 0x1f1ff) {
                units.push_back(std::move(second));
                next = probe;
            }
        }
        // Combining marks, variation selectors, modifiers, keycap marks, and
        // ZWJ-linked emoji belong to the preceding terminal cluster.
        for (;;) {
            if (next >= s.size() || static_cast<unsigned char>(s[next]) == 0x1b) break;
            size_t probe = next;
            TuiUnit tail = tui_unit(s, probe);
            if (tail.cp == 0x200d) {
                if (probe >= s.size() || static_cast<unsigned char>(s[probe]) == 0x1b ||
                    !tui_is_emoji_base(units.front().cp)) break;
                size_t joined_end = probe;
                TuiUnit joined = tui_unit(s, joined_end);
                if (!tui_is_emoji_base(joined.cp)) break;
                units.push_back(std::move(tail));
                units.push_back(std::move(joined));
                next = joined_end;
                continue;
            }
            if (tui_is_cluster_tail(tail.cp) || tail.cp == 0x20e3) {
                units.push_back(std::move(tail));
                next = probe;
                continue;
            }
            // A keycap is digit/#/* + optional VS16 + U+20e3.
            const uint32_t first = units.front().cp;
            if ((first == '#' || first == '*' || (first >= '0' && first <= '9')) &&
                tail.cp >= 0xfe00 && tail.cp <= 0xfe0f) {
                units.push_back(std::move(tail));
                next = probe;
                continue;
            }
            break;
        }
        size_t cells = 0;
        for (const auto& unit : units)
            cells = std::max(cells, tui_codepoint_width(unit.cp));
        for (const auto& unit : units) {
            if ((unit.cp == 0xfe0f && tui_is_emoji_base(units.front().cp)) ||
                (unit.cp == 0x20e3 && (units.front().cp == '#' ||
                 units.front().cp == '*' || (units.front().cp >= '0' &&
                                            units.front().cp <= '9')))) cells = 2;
        }
        std::string bytes;
        for (const auto& unit : units) bytes += unit.bytes;
        pieces.push_back({std::move(bytes), cells, false});
        at = next;
    }
    return pieces;
}

inline size_t tui_visible_len(const std::string& s) {
    size_t cells = 0;
    for (const auto& piece : tui_pieces(s)) {
        if (piece.cells > std::numeric_limits<size_t>::max() - cells)
            return std::numeric_limits<size_t>::max();
        cells += piece.cells;
    }
    return cells;
}

// Normalize and truncate by terminal cells. Unsafe CSI/OSC and invalid/control
// bytes are removed or replaced, so the result can safely be placed in a menu.
inline std::string tui_truncate(const std::string& s, size_t max_cells) {
    if (max_cells == 0) return {};
    std::string out;
    size_t cells = 0;
    for (const auto& piece : tui_pieces(s)) {
        if (!piece.ansi) {
            if (piece.cells > max_cells - cells) break;
            cells += piece.cells;
        }
        out += piece.bytes;
    }
    return out;
}
} // anonymous namespace

inline std::string menu_footer(const std::string& hint) {
    return std::string("\x1b[2m") +
           tui_truncate(hint, std::numeric_limits<size_t>::max()) +
           "\x1b[0m";
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
        const size_t marker_cells = (inner == 0) ? 0 : (inner == 1 ? 1 : 2);
        std::string marker;
        if (i == selected && marker_cells == 2) marker = "\x1b[7m❯ \x1b[0m";
        else if (i == selected && marker_cells == 1) marker = "\x1b[7m>\x1b[0m";
        else marker.assign(marker_cells, ' ');
        const size_t row_cells = inner - marker_cells;
        std::string row = tui_truncate(rows[i], row_cells);
        const size_t vis = tui_visible_len(row);
        if (vis < row_cells) row.append(row_cells - vis, ' ');
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
