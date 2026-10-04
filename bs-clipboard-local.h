#ifndef BS_CLIPBOARD_LOCAL_H
#define BS_CLIPBOARD_LOCAL_H
// bs-clipboard-local.h — put text on the LOCAL machine's clipboard.
//
// 26.10.04. The OSC 52 relay is split across two halves and only one was
// wired up:
//
//   server  bs-mesh-transfer.h scans PTY output with scan_osc52() and fans a
//           ClipboardMsg to attached peers. This half worked.
//   client  the interactive attach loop (process_shell_response) handled
//           Output, Scrollback, SessionDied, ExitCode, Detach and Ping — but
//           NOT ClipboardMsg, so the frame was read and dropped on the floor.
//           That is why remote copy/paste silently did nothing.
//
// The server's own copy of this logic only has a #ifdef _WIN32 branch, so on
// Linux and macOS a ClipboardMsg received by the daemon was also discarded.
// This header is the portable version, used by both.
//
// Backends, tried in order, best-effort and silent on failure:
//   Windows   Win32 OpenClipboard/CF_UNICODETEXT
//   macOS     pbcopy
//   Wayland   wl-copy
//   X11       xclip, then xsel
//   other     OSC 52 passthrough to stdout, so a terminal that does support it
//             still gets the text
//
// A failure here is never fatal: a failed clipboard write must not tear down a
// live session.

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

// Included from inside `namespace bs::mesh { ... }` (same convention as
// bs-osc52.h and bs-mesh-support.h), so this file deliberately opens no
// namespace of its own.
namespace detail {

// Run `argv`'s binary with `text` on stdin. Returns true only if the backend is
// installed and exited 0. Never throws, never blocks on a missing tool: the
// binary is resolved with `command -v` first, so a missing clipboard tool is
// reported as "not available" instead of as a failure of pclose().
[[nodiscard]] inline bool run_clipboard_helper(const char* binary,
                                               std::initializer_list<const char*> args,
                                               std::string_view text) {
#if defined(_WIN32)
    (void)binary; (void)args; (void)text;
    return false;
#else
    // Resolve the binary before invoking it.
    std::string probe = "command -v ";
    probe += binary;
    probe += " 2>/dev/null";
    std::FILE* which = ::popen(probe.c_str(), "r");
    if (!which) return false;
    char found[512] = {};
    const bool got = std::fgets(found, sizeof(found), which) != nullptr;
    ::pclose(which);
    if (!got) return false;
    std::string path(found);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
        path.pop_back();
    if (path.empty()) return false;

    std::string cmd = "'" + path + "'";
    for (const char* a : args) { cmd += ' '; cmd += a; }
    std::FILE* in = ::popen(cmd.c_str(), "w");
    if (!in) return false;
    if (!text.empty()) std::fwrite(text.data(), 1, text.size(), in);
    std::fputc('\n', in);
    return ::pclose(in) == 0;
#endif
}

[[nodiscard]] inline bool set_clipboard_win32(std::string_view text) {
#if defined(_WIN32)
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    const int wsize = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr, 0);
    if (wsize <= 0) { CloseClipboard(); return false; }
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (wsize + 1) * sizeof(WCHAR));
    if (!hMem) { CloseClipboard(); return false; }
    auto* wstr = static_cast<WCHAR*>(GlobalLock(hMem));
    if (!wstr) { GlobalFree(hMem); CloseClipboard(); return false; }
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wstr, wsize);
    wstr[wsize] = L'\0';
    GlobalUnlock(hMem);
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return true;
#else
    (void)text;
    return false;
#endif
}

// Base64-encode `text` for an OSC 52 payload. Exposed (rather than kept inside
// osc52_passthrough) so the encoder can be tested directly against an
// independent implementation, including the padding cases.
[[nodiscard]] inline std::string osc52_b64(std::string_view text) {
    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((text.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < text.size()) {
        const unsigned v = (static_cast<unsigned char>(text[i]) << 16) |
                           (static_cast<unsigned char>(text[i + 1]) << 8) |
                           static_cast<unsigned char>(text[i + 2]);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
        i += 3;
    }
    if (i < text.size()) {
        const size_t rem = text.size() - i;
        const unsigned v = (static_cast<unsigned char>(text[i]) << 16) |
                           (rem == 2 ? static_cast<unsigned char>(text[i + 1]) << 8 : 0);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(rem == 2 ? kB64[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

[[nodiscard]] bool osc52_passthrough(std::string_view text);

}  // namespace detail

[[nodiscard]] inline bool set_local_clipboard(std::string_view text,
                                              std::string* method) {
    if (text.empty()) {
        if (method) method->clear();
        return false;
    }
    auto& m = method;
#if defined(_WIN32)
    if (detail::set_clipboard_win32(text)) { if (m) *m = "win32"; return true; }
#elif defined(__APPLE__)
    if (detail::run_clipboard_helper("pbcopy", {}, text)) {
        if (m) *m = "pbcopy";
        return true;
    }
#else
    if (const char* wayland = std::getenv("WAYLAND_DISPLAY"); wayland && *wayland) {
        if (detail::run_clipboard_helper("wl-copy", {}, text)) {
            if (m) *m = "wl-copy";
            return true;
        }
    }
    if (detail::run_clipboard_helper("xclip", {"-selection", "clipboard"}, text)) {
        if (m) *m = "xclip";
        return true;
    }
    if (detail::run_clipboard_helper("xsel", {"--clipboard", "--input"}, text)) {
        if (m) *m = "xsel";
        return true;
    }
#endif
    if (detail::osc52_passthrough(text)) {
        if (m) *m = "osc52";
        return true;
    }
    if (m) m->clear();
    return false;
}

namespace detail {

[[nodiscard]] inline bool osc52_passthrough(std::string_view text) {
    // Reuse the shared encoder so the bytes written to stdout and the bytes the
    // test asserts on can never diverge.
    const std::string b64 = osc52_b64(text);
    std::fprintf(stdout, "\033]52;c;%s\a", b64.c_str());
    std::fflush(stdout);
    return true;
}

}  // namespace detail

#endif  // BS_CLIPBOARD_LOCAL_H
