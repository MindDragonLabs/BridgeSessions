// bs-cua-helper.h — user-session CUA helper server (2.0.19)
//
// Included by main.cpp only. `bs --cua-helper` runs this loop: binds
// 127.0.0.1:19986, token-auth (owner-only file under the app home), one
// JSON line per request/response. Screen capture and input injection happen
// HERE — inside the interactive desktop session — because the mesh daemon
// typically runs in Session 0 (Windows schtasks/SYSTEM) or under launchd
// (macOS), where capture returns blank pixels and input targets nothing.
//
// The daemon's CUA path tries this helper first (cua_helper_rpc in
// bs-protocol.h) and falls back to its in-process backend when the helper
// is absent — so running the helper is strictly additive.
//
// Linux: not needed (daemon already runs in the user session); the helper
// refuses to start so the fallback path stays in effect.

#pragma once

#include <cstring>
#include <iostream>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>
#include <unistd.h>
#endif

namespace bs::mesh {

// ── platform backends (run in the USER session) ─────────────────

#ifdef _WIN32

// Minimal HID usage → Win32 VK table for control keys. Letters/digits are
// computed (HID 0x04..0x1D = A..Z, 0x1E..0x27 = 1..9,0). Text entry never
// uses this — it goes through KEYEVENTF_UNICODE.
inline int cua_hid_to_vk(uint8_t hid) {
    if (hid >= 0x04 && hid <= 0x1D) return 'A' + (hid - 0x04);
    if (hid >= 0x1E && hid <= 0x26) return '1' + (hid - 0x1E);
    if (hid == 0x27) return '0';
    switch (hid) {
        case 0x28: return VK_RETURN;
        case 0x29: return VK_ESCAPE;
        case 0x2A: return VK_BACK;
        case 0x2B: return VK_TAB;
        case 0x2C: return VK_SPACE;
        case 0x4F: return VK_RIGHT;
        case 0x50: return VK_LEFT;
        case 0x51: return VK_DOWN;
        case 0x52: return VK_UP;
        case 0x4A: return VK_HOME;
        case 0x4D: return VK_END;
        case 0x4B: return VK_PRIOR;   // Page Up
        case 0x4E: return VK_NEXT;    // Page Down
        case 0x49: return VK_INSERT;
        case 0x4C: return VK_DELETE;
        default: return 0;
    }
}

inline void cua_win_send_text(const std::string& utf8) {
    // UTF-8 → UTF-16, then KEYEVENTF_UNICODE per unit (surrogates included).
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (wlen <= 0) return;
    std::wstring w((size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), w.data(), wlen);
    for (wchar_t wc : w) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = wc; in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0]; in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

inline CuaResponseMsg cua_helper_execute(const CuaRequestMsg& req) {
    CuaResponseMsg resp;
    switch (req.action) {
        case 0: {  // screen_info
            resp.screen_w = (int16_t)GetSystemMetrics(SM_CXVIRTUALSCREEN);
            resp.screen_h = (int16_t)GetSystemMetrics(SM_CYVIRTUALSCREEN);
            resp.status = 0;
            return resp;
        }
        case 1: {  // key press
            int vk = cua_hid_to_vk(req.hid_key);
            if (!vk) { resp.status = 1; resp.error = "unmapped hid key"; return resp; }
            INPUT in[2]{};
            in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)vk;
            in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
            resp.status = 0;
            return resp;
        }
        case 2:  // text entry
            cua_win_send_text(req.text);
            resp.status = 0;
            return resp;
        case 3: {  // mouse move
            SetCursorPos(req.x, req.y);
            resp.status = 0;
            return resp;
        }
        case 4: {  // mouse button (1=left 2=middle 3=right)
            DWORD down, up;
            if (req.button == 2)      { down = MOUSEEVENTF_MIDDLEDOWN; up = MOUSEEVENTF_MIDDLEUP; }
            else if (req.button == 3) { down = MOUSEEVENTF_RIGHTDOWN;  up = MOUSEEVENTF_RIGHTUP; }
            else                      { down = MOUSEEVENTF_LEFTDOWN;   up = MOUSEEVENTF_LEFTUP; }
            INPUT in[2]{};
            in[0].type = INPUT_MOUSE; in[0].mi.dwFlags = down;
            in[1].type = INPUT_MOUSE; in[1].mi.dwFlags = up;
            SendInput(2, in, sizeof(INPUT));
            resp.status = 0;
            return resp;
        }
        case 5: {  // wheel (button 0 = up, else down)
            INPUT in{};
            in.type = INPUT_MOUSE;
            in.mi.dwFlags = MOUSEEVENTF_WHEEL;
            in.mi.mouseData = (req.button == 0) ? 120 : (DWORD)-120;
            SendInput(1, &in, sizeof(INPUT));
            resp.status = 0;
            return resp;
        }
        case 6: {  // capture — GDI via PowerShell (user session → real pixels).
            char tmpl[MAX_PATH], tmpdir[MAX_PATH];
            GetTempPathA(sizeof(tmpdir), tmpdir);
            GetTempFileNameA(tmpdir, "bsh", 0, tmpl);
            std::string tmp_path = std::string(tmpl) + ".png";
            ::unlink(tmpl);
            std::string ps =
                "powershell -NoProfile -Command \""
                "Add-Type -AssemblyName System.Drawing,System.Windows.Forms;"
                "$b=[System.Drawing.Rectangle]::FromLTRB("
                "[System.Windows.Forms.SystemInformation]::VirtualScreen.Left,"
                "[System.Windows.Forms.SystemInformation]::VirtualScreen.Top,"
                "[System.Windows.Forms.SystemInformation]::VirtualScreen.Right,"
                "[System.Windows.Forms.SystemInformation]::VirtualScreen.Bottom);"
                "$img=New-Object System.Drawing.Bitmap($b.Width,$b.Height);"
                "$g=[System.Drawing.Graphics]::FromImage($img);"
                "$g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size);"
                "$g.Dispose();$img.Save('" + tmp_path + "',[System.Drawing.Imaging.ImageFormat]::Png);"
                "$img.Dispose()\" 2>nul";
            int rc = std::system(ps.c_str());
            if (rc != 0 || !std::filesystem::exists(tmp_path)) {
                resp.status = 1; resp.error = "helper capture failed (PowerShell GDI)";
                return resp;
            }
            std::ifstream cap(tmp_path, std::ios::binary | std::ios::ate);
            if (!cap) { resp.status = 1; resp.error = "open capture failed"; return resp; }
            auto sz = cap.tellg();
            if (sz <= 0 || (size_t)sz > MAX_IMAGE_BYTES) {
                resp.status = 1; resp.error = "capture empty/oversize";
                ::unlink(tmp_path.c_str()); return resp;
            }
            resp.data.resize((size_t)sz);
            cap.seekg(0); cap.read((char*)resp.data.data(), sz);
            ::unlink(tmp_path.c_str());
            resp.format = 1; resp.status = 0;
            resp.screen_w = (int16_t)GetSystemMetrics(SM_CXVIRTUALSCREEN);
            resp.screen_h = (int16_t)GetSystemMetrics(SM_CYVIRTUALSCREEN);
            return resp;
        }
        default:
            resp.status = 1; resp.error = "unknown action";
            return resp;
    }
}

#elif defined(__APPLE__)

inline int cua_hid_to_cg(uint8_t hid) {  // HID usage → CGKeyCode (ANSI)
    if (hid >= 0x04 && hid <= 0x1D) {    // A..Z
        static const int m[26] = {0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
        return m[hid - 0x04];
    }
    if (hid >= 0x1E && hid <= 0x26) {    // 1..9
        static const int m[9] = {18,19,20,21,23,22,26,28,25};
        return m[hid - 0x1E];
    }
    if (hid == 0x27) return 29;          // 0
    switch (hid) {
        case 0x28: return 36;   // Return
        case 0x29: return 53;   // Esc
        case 0x2A: return 51;   // Delete
        case 0x2B: return 48;   // Tab
        case 0x2C: return 49;   // Space
        case 0x4F: return 124;  // Right
        case 0x50: return 123;  // Left
        case 0x51: return 125;  // Down
        case 0x52: return 126;  // Up
        case 0x4A: return 115;  // Home
        case 0x4D: return 119;  // End
        case 0x4B: return 116;  // PgUp
        case 0x4E: return 121;  // PgDn
        case 0x4C: return 117;  // Fwd Delete
        default: return -1;
    }
}

inline CuaResponseMsg cua_helper_execute(const CuaRequestMsg& req) {
    CuaResponseMsg resp;
    switch (req.action) {
        case 0: {  // screen_info
            auto r = CGDisplayBounds(CGMainDisplayID());
            resp.screen_w = (int16_t)r.size.width;
            resp.screen_h = (int16_t)r.size.height;
            resp.status = 0;
            return resp;
        }
        case 1: {  // key press
            int kc = cua_hid_to_cg(req.hid_key);
            if (kc < 0) { resp.status = 1; resp.error = "unmapped hid key"; return resp; }
            CGEventRef dn = CGEventCreateKeyboardEvent(nullptr, (CGKeyCode)kc, true);
            CGEventRef up = CGEventCreateKeyboardEvent(nullptr, (CGKeyCode)kc, false);
            if (dn) { CGEventPost(kCGHIDEventTap, dn); CFRelease(dn); }
            if (up) { CGEventPost(kCGHIDEventTap, up); CFRelease(up); }
            resp.status = 0;
            return resp;
        }
        case 2: {  // text entry — unicode per chunk
            CFStringRef cf = CFStringCreateWithCString(nullptr, req.text.c_str(), kCFStringEncodingUTF8);
            if (!cf) { resp.status = 1; resp.error = "utf8 conversion failed"; return resp; }
            CFIndex len = CFStringGetLength(cf);
            for (CFIndex i = 0; i < len; ++i) {
                UniChar uc = CFStringGetCharacterAtIndex(cf, i);
                CGEventRef dn = CGEventCreateKeyboardEvent(nullptr, 0, true);
                CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
                if (dn) { CGEventKeyboardSetUnicodeString(dn, 1, &uc); CGEventPost(kCGHIDEventTap, dn); CFRelease(dn); }
                if (up) { CGEventKeyboardSetUnicodeString(up, 1, &uc); CGEventPost(kCGHIDEventTap, up); CFRelease(up); }
            }
            CFRelease(cf);
            resp.status = 0;
            return resp;
        }
        case 3: {  // mouse move
            CGEventRef ev = CGEventCreateMouseEvent(nullptr, kCGEventMouseMoved,
                                                    CGPointMake(req.x, req.y), kCGMouseButtonLeft);
            if (ev) { CGEventPost(kCGHIDEventTap, ev); CFRelease(ev); }
            resp.status = 0;
            return resp;
        }
        case 4: {  // mouse button (1=left 2=middle 3=right)
            CGMouseButton b = kCGMouseButtonLeft;
            CGEventType dn = kCGEventLeftMouseDown, up = kCGEventLeftMouseUp;
            if (req.button == 2)      { b = kCGMouseButtonCenter; dn = kCGEventOtherMouseDown; up = kCGEventOtherMouseUp; }
            else if (req.button == 3) { b = kCGMouseButtonRight;  dn = kCGEventRightMouseDown; up = kCGEventRightMouseUp; }
            CGPoint p = CGPointMake(req.x, req.y);
            CGEventRef e1 = CGEventCreateMouseEvent(nullptr, dn, p, b);
            CGEventRef e2 = CGEventCreateMouseEvent(nullptr, up, p, b);
            if (e1) { CGEventPost(kCGHIDEventTap, e1); CFRelease(e1); }
            if (e2) { CGEventPost(kCGHIDEventTap, e2); CFRelease(e2); }
            resp.status = 0;
            return resp;
        }
        case 5: {  // wheel
            CGEventRef ev = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1,
                                                          (req.button == 0) ? 1 : -1);
            if (ev) { CGEventPost(kCGHIDEventTap, ev); CFRelease(ev); }
            resp.status = 0;
            return resp;
        }
        case 6: {  // capture — screencapture CLI (user session + Screen Recording perm)
            std::string tmp = "/tmp/bs-cua-helper-" + std::to_string(getpid()) + ".png";
            ::unlink(tmp.c_str());
            std::string cmd = "screencapture -x '" + tmp + "' 2>/dev/null";
            int rc = std::system(cmd.c_str());
            if (rc != 0 || !std::filesystem::exists(tmp)) {
                resp.status = 1;
                resp.error = "screencapture failed — grant Screen Recording to the helper (TCC)";
                return resp;
            }
            std::ifstream cap(tmp, std::ios::binary | std::ios::ate);
            if (!cap) { resp.status = 1; resp.error = "open capture failed"; return resp; }
            auto sz = cap.tellg();
            if (sz <= 0 || (size_t)sz > MAX_IMAGE_BYTES) {
                resp.status = 1; resp.error = "capture empty/oversize";
                ::unlink(tmp.c_str()); return resp;
            }
            resp.data.resize((size_t)sz);
            cap.seekg(0); cap.read((char*)resp.data.data(), sz);
            ::unlink(tmp.c_str());
            resp.format = 1; resp.status = 0;
            auto r = CGDisplayBounds(CGMainDisplayID());
            resp.screen_w = (int16_t)r.size.width;
            resp.screen_h = (int16_t)r.size.height;
            return resp;
        }
        default:
            resp.status = 1; resp.error = "unknown action";
            return resp;
    }
}

#endif // platform backends

// ── server loop ─────────────────────────────────────────────────

inline int run_cua_helper(const std::string& app_home_in) {
#if !defined(_WIN32) && !defined(__APPLE__)
    std::cerr << "cua-helper: not needed on Linux (daemon already runs in the user session)\n";
    return 0;
#else
    std::string app_home = app_home_in.empty()
        ? (expand_home("~") + "/.bridgesessions") : app_home_in;
    make_app_paths(app_home);  // ensure root exists

    // Token: generate fresh each start, owner-only. The daemon reads the
    // same file — rotate-on-start means a stale daemon-side cache can't
    // exist (client reads per-RPC).
    std::string token;
    try { token = generate_ipc_token(); }
    catch (const std::exception& e) {
        std::cerr << "cua-helper: token generation failed: " << e.what() << "\n";
        return 1;
    }
    if (!write_private_text_file(cua_helper_token_path(app_home), token)) {
        std::cerr << "cua-helper: cannot write " << cua_helper_token_path(app_home) << "\n";
        return 1;
    }

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SOCKET lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == INVALID_SOCKET) { std::cerr << "cua-helper: socket failed\n"; return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(kCuaHelperPort);
    if (bind(lfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR || listen(lfd, 4) == SOCKET_ERROR) {
        std::cerr << "cua-helper: bind/listen 127.0.0.1:" << kCuaHelperPort << " failed\n";
        CLOSESOCK(lfd); return 1;
    }
    std::cout << "cua-helper: listening on 127.0.0.1:" << kCuaHelperPort
              << " (token " << cua_helper_token_path(app_home) << ")\n" << std::flush;

    for (;;) {
        SOCKET cfd = accept(lfd, nullptr, nullptr);
        if (cfd == INVALID_SOCKET) continue;
        set_socket_timeouts(cfd, 60000);
        std::string line;
        char buf[65536];
        while (line.size() < 4 * 1024 * 1024) {
            int n = recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            line.append(buf, (size_t)n);
            if (line.find('\n') != std::string::npos) break;
        }
        if (line.empty()) { CLOSESOCK(cfd); continue; }

        // "TOKEN {json}\n"
        auto sp = line.find(' ');
        std::string got = sp == std::string::npos ? line : line.substr(0, sp);
        while (!got.empty() && (got.back() == '\r' || got.back() == '\n')) got.pop_back();
        if (got != token || sp == std::string::npos) {
            const char* deny = "{\"status\":1,\"error\":\"auth\"}\n";
            send(cfd, deny, (int)strlen(deny), 0);
            CLOSESOCK(cfd); continue;
        }

        CuaRequestMsg req;
        try {
            auto j = nlohmann::json::parse(line.substr(sp + 1));
            req.action  = j.value("action", 0);
            req.x       = (int16_t)j.value("x", 0);
            req.y       = (int16_t)j.value("y", 0);
            req.hid_key = (uint8_t)j.value("hid_key", 0);
            req.button  = (uint8_t)j.value("button", 0);
            req.text    = j.value("text", "");
        } catch (...) {
            const char* bad = "{\"status\":1,\"error\":\"bad json\"}\n";
            send(cfd, bad, (int)strlen(bad), 0);
            CLOSESOCK(cfd); continue;
        }

        CuaResponseMsg r = cua_helper_execute(req);
        nlohmann::json out;
        out["status"] = r.status;
        out["error"] = r.error;
        out["format"] = r.format;
        out["screen_w"] = r.screen_w;
        out["screen_h"] = r.screen_h;
        if (!r.data.empty())
            out["data_b64"] = b64enc(r.data.data(), r.data.size());
        std::string wire = out.dump() + "\n";
        size_t off = 0;
        while (off < wire.size()) {
            int n = send(cfd, wire.data() + off, (int)(wire.size() - off), 0);
            if (n <= 0) break;
            off += (size_t)n;
        }
        CLOSESOCK(cfd);
    }
#endif
}

} // namespace bs::mesh
