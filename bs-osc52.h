// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-osc52.h — OSC 52 clipboard scanner
// Extracted from bs-protocol.h (R6 structural refactor, 2026-09-02)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace — parent file provides it.
#pragma once

// ────────────────────────────────────────────────────────────────────
// 5. OSC 52 CLIPBOARD SCANNER
// ────────────────────────────────────────────────────────────────────
// Scans PTY output for OSC 52 sequences, extracts base64 content,
// strips them from the terminal stream so they're never rendered.
//
// OSC 52 format:  ESC ] 52 ; Pc ; <base64> ST
//  - ESC = \x1b, ST = \x1b\\ or \x07 (BEL)
//  - Pc = clipboard target (c, p, q; usually 'c' for system clipboard)

#ifdef _WIN32
using ssize_t = long long;
#endif

struct Osc52Result {
    std::string cleaned_text;              // text with OSC 52 sequences stripped
    std::optional<std::string> clipboard_text;  // decoded clipboard content, if any
};

// Scan a buffer for OSC 52 sequences. Returns cleaned text + optional clipboard.
// Thread-safe: pure function, no shared state.
[[nodiscard]] Osc52Result scan_osc52(std::string_view input);

// v1.7.1: strip ANSI/VT escape sequences from PTY output for non-interactive
// one-shot exec capture (`shell <peer> --cmd ...`). ConPTY (and POSIX PTYs)
// emit cursor/title/screen-clear control sequences as a normal side effect
// of hosting even a single non-interactive command — a one-shot caller only
// wants the plain text. Handles CSI (ESC [ ... final-byte), OSC (ESC ]
// ... BEL or ESC \), and bare two-byte ESC sequences. Thread-safe: pure
// function, no shared state (called from the background exec thread).
[[nodiscard]] inline std::string strip_ansi_escapes(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == 0x1B && i + 1 < input.size()) { // ESC
            char next = input[i + 1];
            if (next == '[') { // CSI: ESC [ params... final-byte(@-~)
                size_t j = i + 2;
                while (j < input.size() &&
                       !(input[j] >= 0x40 && input[j] <= 0x7E)) ++j;
                i = (j < input.size()) ? j : input.size() - 1;
                continue;
            } else if (next == ']') { // OSC: ESC ] ... BEL or ST (ESC backslash)
                size_t j2 = i + 2;
                while (j2 < input.size() && input[j2] != '\a' &&
                       !(input[j2] == 0x1B && j2 + 1 < input.size() && input[j2 + 1] == '\\')) ++j2;
                if (j2 < input.size() && input[j2] == '\a') { i = j2; continue; }
                if (j2 + 1 < input.size()) { i = j2 + 1; continue; }
                i = input.size() - 1;
                continue;
            } else {
                // Bare 2-byte escape (e.g. ESC = , ESC > )
                i += 1;
                continue;
            }
        }
        // Drop other C0 control chars that don't carry text meaning, but
        // keep \n, \r, \t.
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') continue;
        out.push_back(input[i]);
    }
    return out;
}

// v26.09.18 (TODO item 5): strip ONLY terminal mode-setting sequences from
// scrollback replayed to a reattaching client. Unlike strip_ansi_escapes
// (which removes everything for plain-text capture), this preserves the
// visual record — colors, cursor moves, text — but removes sequences that
// would reconfigure the NEW client's terminal to match stale state:
//   CSI ? 1000-1016 h/l   mouse tracking (all protocols — the garbage)
//   CSI ? 2004 h/l        bracketed paste
//   CSI ? 1049/47/1047    alt-screen switches (replay must stay on main screen)
//   CSI ? 25 h/l          cursor visibility replay is meaningless
//   CSI ? 6/7 h/l         origin / wraparound (DECOM/DECAWM)
//   CSI ? 1 h/l           DECCKM app cursor keys — replaying this makes the
//                         reattached terminal send ESC O A-style arrows that
//                         a plain remote shell renders as escape garbage
// Pure function; thread-safe; keeps every other byte including SGR colors.
[[nodiscard]] inline std::string strip_mode_sequences(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == 0x1B && i + 2 < input.size() &&
            input[i + 1] == '[' && input[i + 2] == '?') {
            // Private-mode CSI: scan params to final byte
            size_t j = i + 3;
            size_t pstart = j;
            while (j < input.size() && input[j] >= 0x30 && input[j] <= 0x3F) ++j; // params ; digits ? < = >
            size_t pend = j;
            while (j < input.size() && !(input[j] >= 0x40 && input[j] <= 0x7E)) ++j;
            if (j < input.size() && (input[j] == 'h' || input[j] == 'l')) {
                // Parse semicolon-separated numeric params
                std::string_view params(input.data() + pstart, pend - pstart);
                bool strip = false;
                size_t pos = 0;
                while (pos <= params.size()) {
                    size_t semi = params.find(';', pos);
                    std::string_view tok = params.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
                    int val = 0; bool num = !tok.empty();
                    for (char ch : tok) {
                        if (ch < '0' || ch > '9') { num = false; break; }
                        val = val * 10 + (ch - '0');
                    }
                    if (num && (val == 1   || val == 6   || val == 7   ||
                                val == 25  || val == 47  || val == 1000 ||
                                val == 1002 || val == 1003 || val == 1004 ||
                                val == 1005 || val == 1006 || val == 1015 ||
                                val == 1016 || val == 1047 || val == 1049 ||
                                val == 2004)) {
                        strip = true; break;
                    }
                    if (semi == std::string_view::npos) break;
                    pos = semi + 1;
                }
                if (strip) { i = j; continue; }
            }
            // not a stripped private mode — copy through verbatim
            out.push_back(input[i]);
            continue;
        }
        out.push_back(input[i]);
    }
    return out;
}

// ── Internal helpers ────────────────────────────────────────────

namespace detail {

// Find the end of an OSC sequence starting at pos (after ESC ] 52 ;)
inline std::ptrdiff_t find_osc_end(std::string_view s, size_t start) {
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] == '\x07') return (std::ptrdiff_t)i;           // BEL terminator
        if (s[i] == '\x1b' && i+1 < s.size() && s[i+1] == '\\')
            return (std::ptrdiff_t)i;                            // ST terminator (ESC \)
    }
    return -1; // incomplete sequence — leave in stream
}

// Extract and decode base64 content from OSC 52 body.
// Body is "Pc;<base64>" — the "52;" prefix is already stripped.
inline std::optional<std::string> decode_osc52_body(std::string_view body) {
    auto semicolon = body.find(';');
    if (semicolon == std::string_view::npos) return std::nullopt;

    std::string_view b64 = body.substr(semicolon + 1);
    if (b64.empty()) return std::nullopt;  // empty clipboard = clear

    // Base64 decode (hand-rolled to avoid OpenSSL dependency for this)
    // P2 audit fix: 256-byte reverse lookup table instead of strchr (O(n*64)).
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static int8_t kRev[256] = {};
    static bool kRevInit = false;
    if (!kRevInit) {
        for (int i = 0; i < 256; ++i) kRev[i] = -1;
        for (size_t i = 0; i < 64; ++i) kRev[(uint8_t)kTable[i]] = (int8_t)i;
        kRevInit = true;
    }

    std::string result;
    result.reserve(b64.size() * 3 / 4);

    int acc = 0, bits = 0;
    for (char c : b64) {
        if (c == '=') break;
        int8_t v = kRev[(uint8_t)c];
        if (v < 0) continue;  // skip whitespace / invalid chars
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back((char)(acc >> bits));
            acc &= (1 << bits) - 1;
        }
    }
    return result;
}

} // namespace detail

inline Osc52Result scan_osc52(std::string_view input) {
    Osc52Result result;
    result.cleaned_text.reserve(input.size());

    size_t pos = 0;
    while (pos < input.size()) {
        // Look for ESC ] (OSC introducer)
        if (input[pos] == '\x1b' && pos + 1 < input.size() && input[pos + 1] == ']') {
            // Check for "52;" after ESC ]
            size_t after_esc = pos + 2;
            if (after_esc + 2 < input.size() &&
                input[after_esc] == '5' && input[after_esc+1] == '2' && input[after_esc+2] == ';')
            {
                // OSC 52 sequence found — find the end
                auto end = detail::find_osc_end(input, after_esc + 3);
                if (end >= 0) {
                    // Extract the body (between "52;" and ST/BEL)
                    std::string_view body = input.substr(
                        after_esc + 3, (size_t)end - (after_esc + 3));
                    auto decoded = detail::decode_osc52_body(body);
                    if (decoded && !decoded->empty()) {
                        result.clipboard_text = std::move(*decoded);
                    }
                    // Skip past the terminator
                    pos = (size_t)end;
                    if (input[pos] == '\x1b' && pos + 1 < input.size() && input[pos+1] == '\\')
                        pos += 2;
                    else if (input[pos] == '\x07')
                        pos += 1;
                    continue;
                }
                // Incomplete — leave in stream
            }
        }
        // Regular character — pass through
        result.cleaned_text.push_back(input[pos]);
        ++pos;
    }

    return result;
}

