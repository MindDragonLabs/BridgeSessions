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
#include "bs-logging.h"

#ifndef _WIN32
#include <sys/un.h>    // AF_UNIX, sockaddr_un — CUA helper Unix socket (P2)
#include <sys/stat.h>   // chmod
#endif

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#include <CoreGraphics/CGEvent.h>
#include <unistd.h>
extern "C" int bs_macos_capture_png(const char*, unsigned, char*, size_t);
#endif

namespace bs::mesh {

// ── platform backends (run in the USER session) ─────────────────

#ifdef _WIN32

#include <gdiplus.h>
#include <objidl.h>
// GDI+ linkage: gdiplus.lib — ensure build system links it
#pragma comment(lib, "gdiplus.lib")

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
        case 6: {  // capture — native GDI BitBlt + GDI+ JPEG (user session → real pixels).
            // 2.0.20: replaced PowerShell Add-Type (1-3s latency) with in-process GDI.
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            if (vw <= 0 || vh <= 0) {
                resp.status = 1; resp.error = "capture: invalid screen metrics";
                return resp;
            }
            HDC screen_dc = GetDC(NULL);
            if (!screen_dc) { resp.status = 1; resp.error = "GetDC failed"; return resp; }
            HDC mem_dc = CreateCompatibleDC(screen_dc);
            HBITMAP bmp = CreateCompatibleBitmap(screen_dc, vw, vh);
            if (!mem_dc || !bmp) {
                if (mem_dc) DeleteDC(mem_dc);
                if (bmp) DeleteObject(bmp);
                ReleaseDC(NULL, screen_dc);
                resp.status = 1; resp.error = "capture: CreateCompatible failed";
                return resp;
            }
            HBITMAP old = (HBITMAP)SelectObject(mem_dc, bmp);
            BitBlt(mem_dc, 0, 0, vw, vh, screen_dc, vx, vy, SRCCOPY);
            // Extract raw pixels via GetDIBits
            BITMAPINFOHEADER bih{};
            bih.biSize = sizeof(BITMAPINFOHEADER);
            bih.biWidth = vw;
            bih.biHeight = -vh;  // top-down
            bih.biPlanes = 1;
            bih.biBitCount = 32;
            bih.biCompression = BI_RGB;
            std::vector<uint8_t> pixels((size_t)vw * vh * 4);
            int rows = GetDIBits(mem_dc, bmp, 0, vh, pixels.data(),
                                 (BITMAPINFO*)&bih, DIB_RGB_COLORS);
            SelectObject(mem_dc, old);
            DeleteObject(bmp);
            DeleteDC(mem_dc);
            ReleaseDC(NULL, screen_dc);
            if (rows <= 0) {
                resp.status = 1; resp.error = "capture: GetDIBits failed";
                return resp;
            }
            // Encode as JPEG via GDI+
            ULONG gdiplus_token = 0;
            Gdiplus::GdiplusStartupInput gdiplus_input;
            if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, NULL) != Gdiplus::Ok) {
                resp.status = 1; resp.error = "capture: GDI+ init failed";
                return resp;
            }
            Gdiplus::Bitmap bitmap(vw, vh, (size_t)vw * 4, PixelFormat32bppRGB, pixels.data());
            // quality 70 — ~30-50KB per screenshot, fits mesh frame limit
            CLSID jpg_clsid;
            UINT num = 0, size = 0;
            Gdiplus::GetImageEncodersSize(&num, &size);
            bool got_encoder = false;
            if (size > 0) {
                std::vector<uint8_t> enc_buf(size);
                auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(enc_buf.data());
                if (Gdiplus::GetImageEncoders(num, size, encoders) == Gdiplus::Ok) {
                    for (UINT i = 0; i < num; ++i) {
                        if (encoders[i].FormatID == Gdiplus::ImageFormatJPEG) {
                            jpg_clsid = encoders[i].Clsid;
                            got_encoder = true;
                            break;
                        }
                    }
                }
            }
            if (!got_encoder) {
                Gdiplus::GdiplusShutdown(gdiplus_token);
                resp.status = 1; resp.error = "capture: JPEG encoder not found";
                return resp;
            }
            IStream* stream = nullptr;
            if (CreateStreamOnHGlobal(NULL, TRUE, &stream) != S_OK || !stream) {
                Gdiplus::GdiplusShutdown(gdiplus_token);
                resp.status = 1; resp.error = "capture: CreateStream failed";
                return resp;
            }
            Gdiplus::EncoderParameters params;
            params.Count = 1;
            params.Parameter[0].Guid = Gdiplus::EncoderQuality;
            params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            ULONG quality = 70;
            params.Parameter[0].Value = &quality;
            auto save_status = bitmap.Save(stream, &jpg_clsid, &params);
            if (save_status != Gdiplus::Ok) {
                stream->Release();
                Gdiplus::GdiplusShutdown(gdiplus_token);
                resp.status = 1; resp.error = "capture: GdipSaveImageToStream failed";
                return resp;
            }
            LARGE seek_pos{};
            ULARGE large_pos{};
            stream->Seek(seek_pos, STREAM_SEEK_SET, &large_pos);
            STATSTG stat{};
            stream->Stat(&stat, STATFLAG_NONAME);
            size_t jpeg_size = (size_t)stat.cbSize.QuadPart;
            if (jpeg_size == 0 || jpeg_size > MAX_IMAGE_BYTES) {
                stream->Release();
                Gdiplus::GdiplusShutdown(gdiplus_token);
                resp.status = 1; resp.error = "capture: JPEG empty/oversize";
                return resp;
            }
            resp.data.resize(jpeg_size);
            ULONG bytes_read = 0;
            stream->Read(resp.data.data(), (ULONG)jpeg_size, &bytes_read);
            stream->Release();
            Gdiplus::GdiplusShutdown(gdiplus_token);
            if (bytes_read != jpeg_size) {
                resp.status = 1; resp.error = "capture: JPEG stream read short";
                return resp;
            }
            resp.format = 2; resp.status = 0;  // JPEG
            resp.screen_w = (int16_t)vw;
            resp.screen_h = (int16_t)vh;
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
        case 6: {  // capture — ScreenCaptureKit → JPEG (downscaled to fit frame limit)
            char tmp[] = "/tmp/bs-cua-helper-XXXXXX.png";
            int fd = mkstemps(tmp, 4);
            if (fd < 0) {
                resp.status = 1;
                resp.error = "capture failed: mkstemps failed — cannot create temp file";
                return resp;
            }
            close(fd);
            char err[1024] = {};
            if (!bs_macos_capture_png(tmp, 0, err, sizeof(err))) {
                resp.status = 1;
                resp.error = std::string("capture failed — grant Screen Recording to the helper (TCC): ") + err;
                ::unlink(tmp);
                return resp;
            }
            // Convert PNG to JPEG, downscale to 1280px max, quality 70%.
            // PNG screenshots are 2-4MB; JPEG at these settings ≈ 30-50KB
            // which fits within the 65535-byte mesh frame limit.
            char jpg[] = "/tmp/bs-cua-helper-XXXXXX.jpg";
            int jfd = mkstemps(jpg, 4);
            if (jfd < 0) {
                ::unlink(tmp);
                resp.status = 1;
                resp.error = "capture failed: mkstemps failed — cannot create JPEG temp file";
                return resp;
            }
            close(jfd);
            std::string cmd = "/usr/bin/sips -s format jpeg -s formatOptions 40 -Z 800 '";
            cmd += tmp; cmd += "' --out '"; cmd += jpg; cmd += "' 2>/dev/null";
            int rc = std::system(cmd.c_str());
            ::unlink(tmp);
            if (rc != 0) {
                ::unlink(jpg);
                resp.status = 1;
                resp.error = "JPEG conversion failed (sips rc=" + std::to_string(rc) + ")";
                return resp;
            }
            std::ifstream cap(jpg, std::ios::binary | std::ios::ate);
            if (!cap) { resp.status = 1; resp.error = "open JPEG failed"; ::unlink(jpg); return resp; }
            auto sz = cap.tellg();
            if (sz <= 0 || (size_t)sz > MAX_IMAGE_BYTES) {
                resp.status = 1; resp.error = "capture empty/oversize";
                ::unlink(jpg); return resp;
            }
            resp.data.resize((size_t)sz);
            cap.seekg(0); cap.read((char*)resp.data.data(), sz);
            ::unlink(jpg);
            resp.format = 2; resp.status = 0;  // JPEG after sips conversion
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
    // P2 security: POSIX uses a Unix domain socket (filesystem perms protect the
    // token from loopback sniffing). Windows keeps TCP loopback (named pipes are
    // the alternative but need a separate code path).
#ifdef _WIN32
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
    bs::log::get("cua-helper")->info("Listening on 127.0.0.1:{}", kCuaHelperPort);
#else
    const std::string sock_path = cua_helper_socket_path(app_home);
    // Remove a stale socket left by a previously killed helper — a live socket
    // file from a dead process would otherwise block bind().
    ::unlink(sock_path.c_str());
    SOCKET lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd == INVALID_SOCKET) { std::cerr << "cua-helper: unix socket failed\n"; return 1; }
    sockaddr_un su{};
    su.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof(su.sun_path)) {
        std::cerr << "cua-helper: socket path too long\n";
        CLOSESOCK(lfd); return 1;
    }
    std::strncpy(su.sun_path, sock_path.c_str(), sizeof(su.sun_path) - 1);
    if (bind(lfd, (sockaddr*)&su, sizeof(su)) == SOCKET_ERROR || listen(lfd, 4) == SOCKET_ERROR) {
        std::cerr << "cua-helper: bind/listen " << sock_path << " failed\n";
        CLOSESOCK(lfd); return 1;
    }
    // Restrict the socket file to the owner — the token is now protected by
    // filesystem permissions instead of loopback anonymity.
    ::chmod(sock_path.c_str(), 0600);
    std::cout << "cua-helper: listening on " << sock_path
              << " (token " << cua_helper_token_path(app_home) << ")\n" << std::flush;
    bs::log::get("cua-helper")->info("Listening on {}", sock_path);
#endif

    for (;;) {
        SOCKET cfd = accept(lfd, nullptr, nullptr);
        if (cfd == INVALID_SOCKET) continue;
        set_socket_timeouts(cfd, 60000);
        std::string line;
        char buf[65536];
        while (line.size() < 256 * 1024) {  // 256KB cap — close oversized requests
            int n = recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            line.append(buf, (size_t)n);
            if (line.find('\n') != std::string::npos) break;
        }
        if (line.empty()) { CLOSESOCK(cfd); continue; }
        if (line.find('\n') == std::string::npos) {
            // No newline within cap — reject and close to prevent memory DoS
            const char* deny = "{\"status\":1,\"error\":\"request too large\"}\n";
            send(cfd, deny, (int)strlen(deny), 0);
            CLOSESOCK(cfd); continue;
        }

        // "TOKEN {json}\n"
        auto sp = line.find(' ');
        std::string got = sp == std::string::npos ? line : line.substr(0, sp);
        while (!got.empty() && (got.back() == '\r' || got.back() == '\n')) got.pop_back();
        // Constant-time comparison to avoid timing side-channel on token
        bool token_ok = (got.size() == token.size());
        if (token_ok) {
            volatile unsigned char acc = 0;
            for (size_t i = 0; i < token.size(); ++i)
                acc |= (unsigned char)(got[i] ^ token[i]);
            token_ok = (acc == 0);
        }
        if (!token_ok || sp == std::string::npos) {
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
