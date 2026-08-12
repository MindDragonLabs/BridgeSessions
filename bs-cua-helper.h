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
#include <memory>
#include <nlohmann/json.hpp>
#include "bs-logging.h"

#ifndef _WIN32
#include <sys/un.h>    // AF_UNIX, sockaddr_un — CUA helper Unix socket (P2)
#include <sys/stat.h>   // chmod
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreGraphics/CGEvent.h>
#include <unistd.h>
extern "C" int bs_macos_capture_png(const char*, unsigned, char*, size_t);
#endif

namespace bs::mesh {

#if defined(__APPLE__)
// TCC bootstrap API — forward-declared instead of including
// <ApplicationServices/ApplicationServices.h>, whose Carbon umbrella
// headers ('Rect', etc.) fail to compile as C++. Linked via
// -framework ApplicationServices (see CMakeLists.txt).
// (bs-protocol.h pulls CoreFoundation into this namespace, so the CF
// types resolve here.)
extern "C" {
    extern CFStringRef kAXTrustedCheckOptionPrompt;
    bool AXIsProcessTrusted(void);
    bool AXIsProcessTrustedWithOptions(CFDictionaryRef options);
    // ScreenCaptureKit / CG TCC preflight + request (macOS 10.15+)
    bool CGPreflightScreenCaptureAccess(void);
    bool CGRequestScreenCaptureAccess(void);
}
#endif

// ── platform backends (run in the USER session) ─────────────────

#ifdef _WIN32

// GDI only (no GDI+): BitBlt/StretchBlt + hand-rolled BMP. GDI+ FromHBITMAP /
// Save AVs (0xc0000005 in gdiplus.dll) on recent Win11 builds during Session-1
// helper capture (see RCA 2026-08-12 win11-vm1).
#include <windows.h>
#include <vector>
#include <cstdint>
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
        case 6: {  // capture — GDI StretchBlt + hand-rolled BMP (no GDI+).
            // Session 0 has no interactive desktop — fail closed.
            DWORD sid = 0;
            if (ProcessIdToSessionId(GetCurrentProcessId(), &sid) && sid == 0) {
                resp.status = 1;
                resp.error = "capture: helper is in Session 0 (no interactive desktop) — "
                             "run bridgesessions --cua-helper in a logged-on user session";
                return resp;
            }
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            if (vw <= 0 || vh <= 0) {
                vw = GetSystemMetrics(SM_CXSCREEN);
                vh = GetSystemMetrics(SM_CYSCREEN);
                vx = 0; vy = 0;
            }
            if (vw <= 0 || vh <= 0) {
                resp.status = 1; resp.error = "capture: invalid screen metrics";
                return resp;
            }
            // Soft cap ~1280x720 so hand-rolled BMP fits mesh frame capacity
            // (full 1920x1080 24bpp BMP is ~6MB and can exceed frame limits).
            const int max_w = 1280, max_h = 720;
            int cap_w = vw, cap_h = vh;
            if (cap_w > max_w || cap_h > max_h) {
                double sx = (double)max_w / cap_w;
                double sy = (double)max_h / cap_h;
                double s = sx < sy ? sx : sy;
                cap_w = (int)(cap_w * s);
                cap_h = (int)(cap_h * s);
                if (cap_w < 1) cap_w = 1;
                if (cap_h < 1) cap_h = 1;
            }
            // 24bpp DIB, top-down (negative biHeight) so bits[] starts at top row.
            const int row_stride = (cap_w * 3 + 3) & ~3;
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = cap_w;
            bmi.bmiHeader.biHeight = -cap_h;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HDC screen_dc = GetDC(NULL);
            if (!screen_dc) { resp.status = 1; resp.error = "GetDC failed"; return resp; }
            HDC mem_dc = CreateCompatibleDC(screen_dc);
            HBITMAP dib = CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (!mem_dc || !dib || !bits) {
                if (dib) DeleteObject(dib);
                if (mem_dc) DeleteDC(mem_dc);
                ReleaseDC(NULL, screen_dc);
                resp.status = 1; resp.error = "capture: CreateDIBSection failed";
                return resp;
            }
            HBITMAP old = (HBITMAP)SelectObject(mem_dc, dib);
            SetStretchBltMode(mem_dc, COLORONCOLOR);
            BOOL ok = StretchBlt(mem_dc, 0, 0, cap_w, cap_h, screen_dc, vx, vy, vw, vh, SRCCOPY);
            SelectObject(mem_dc, old);
            DeleteDC(mem_dc);
            ReleaseDC(NULL, screen_dc);
            if (!ok) {
                DeleteObject(dib);
                resp.status = 1; resp.error = "capture: StretchBlt failed";
                return resp;
            }
            // Build a standard bottom-up BMP (positive biHeight) without GDI+.
            const size_t pixel_bytes = (size_t)row_stride * (size_t)cap_h;
            const size_t header_bytes = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
            const size_t bmp_size = header_bytes + pixel_bytes;
            if (bmp_size == 0 || bmp_size > MAX_IMAGE_BYTES) {
                DeleteObject(dib);
                resp.status = 1; resp.error = "capture: BMP empty/oversize";
                return resp;
            }
            resp.data.resize(bmp_size);
            auto* out = resp.data.data();
            BITMAPFILEHEADER fh{};
            fh.bfType = 0x4D42;  // 'BM'
            fh.bfSize = (DWORD)bmp_size;
            fh.bfOffBits = (DWORD)header_bytes;
            BITMAPINFOHEADER ih{};
            ih.biSize = sizeof(BITMAPINFOHEADER);
            ih.biWidth = cap_w;
            ih.biHeight = cap_h;  // positive = bottom-up in file
            ih.biPlanes = 1;
            ih.biBitCount = 24;
            ih.biCompression = BI_RGB;
            ih.biSizeImage = (DWORD)pixel_bytes;
            std::memcpy(out, &fh, sizeof(fh));
            std::memcpy(out + sizeof(fh), &ih, sizeof(ih));
            // Flip top-down DIB rows into bottom-up BMP order.
            auto* src = static_cast<const uint8_t*>(bits);
            auto* dst = out + header_bytes;
            for (int y = 0; y < cap_h; ++y) {
                const uint8_t* src_row = src + (size_t)y * (size_t)row_stride;
                uint8_t* dst_row = dst + (size_t)(cap_h - 1 - y) * (size_t)row_stride;
                std::memcpy(dst_row, src_row, (size_t)row_stride);
            }
            DeleteObject(dib);
            resp.format = 3;  // BMP (CLI sniffs magic; 0=PNG 1=legacy 2=JPEG 3=BMP)
            resp.status = 0;
            resp.screen_w = (int16_t)cap_w;
            resp.screen_h = (int16_t)cap_h;
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
        case 6: {  // capture — ScreenCaptureKit, with screencapture(1) fallback
            char tmp[] = "/tmp/bs-cua-helper-XXXXXX.png";
            int fd = mkstemps(tmp, 4);
            if (fd < 0) {
                resp.status = 1;
                resp.error = "capture failed: mkstemps failed — cannot create temp file";
                return resp;
            }
            close(fd);
            char err[1024] = {};
            bool got_png = bs_macos_capture_png(tmp, 0, err, sizeof(err));
            if (!got_png) {
                // Fallback: macOS screencapture CLI (often already TCC-authorized
                // for the user session when SCK fails with "no shareable displays").
                std::string sc = "/usr/sbin/screencapture -x -t png '";
                sc += tmp;
                sc += "' 2>/dev/null";
                if (std::system(sc.c_str()) == 0 && std::filesystem::exists(tmp) &&
                    std::filesystem::file_size(tmp) > 0) {
                    got_png = true;
                    err[0] = '\0';
                }
            }
            if (!got_png) {
                resp.status = 1;
                resp.error = std::string("capture failed — grant Screen Recording to "
                                         "BridgeSessions.app (TCC): ") +
                             (err[0] ? err : "no shareable displays / screencapture failed");
                ::unlink(tmp);
                return resp;
            }
            // Convert PNG to JPEG, downscale to fit mesh frames (~30-50KB).
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
    (void)make_app_paths(app_home);  // ensure root exists

#ifdef __APPLE__
    // ── TCC permission bootstrap ────────────────────────────────────
    // TCC (Accessibility + Screen Recording):
    // - Never re-prompt on every launchd KeepAlive / kickstart — that stacks
    //   dozens of System Settings dialogs when the helper restarts.
    // - Preflight first; only request when not already decided.
    // - Interactive request at most once per app_home (marker file). Force a
    //   capture probe only on that first interactive attempt (or when
    //   BS_TCC_FORCE_PROMPT=1). Later starts log status only.
    {
        const bool force_prompt = [] {
            const char* e = std::getenv("BS_TCC_FORCE_PROMPT");
            return e && e[0] == '1' && e[1] == '\0';
        }();
        const std::string marker = app_home + "/tcc-prompted-v1";
        const bool already_prompted = !force_prompt &&
            (access(marker.c_str(), F_OK) == 0);

        // Accessibility: silent check first.
        bool ax_ok = AXIsProcessTrusted();
        if (!ax_ok && !already_prompted) {
            const void* ax_keys[] = { kAXTrustedCheckOptionPrompt };
            const void* ax_vals[] = { kCFBooleanTrue };
            CFDictionaryRef ax_opts = CFDictionaryCreate(
                nullptr, ax_keys, ax_vals, 1,
                &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            ax_ok = AXIsProcessTrustedWithOptions(ax_opts);
            if (ax_opts) CFRelease(ax_opts);
        }

        // Screen Recording: preflight; request only if needed and not yet asked.
        bool sr_ok = CGPreflightScreenCaptureAccess();
        if (!sr_ok && !already_prompted) {
            sr_ok = CGRequestScreenCaptureAccess();
            if (!sr_ok) {
                // One-shot SCKit probe — only on first interactive attempt.
                // (CGRequest alone is often silent for launchd on macOS 15+/26.)
                std::string probe = "/tmp/.bs-tcc-probe.png";
                char err[256] = {};
                bs_macos_capture_png(probe.c_str(), 0, err, sizeof(err));
                ::unlink(probe.c_str());
                sr_ok = CGPreflightScreenCaptureAccess();
            }
        }

        if (!already_prompted) {
            // Mark so kickstart/KeepAlive cannot stack more dialogs.
            FILE* mf = std::fopen(marker.c_str(), "w");
            if (mf) {
                std::fprintf(mf, "prompted=1 ax=%d sr=%d\n", ax_ok ? 1 : 0, sr_ok ? 1 : 0);
                std::fclose(mf);
            }
        }

        bs::log::get("cua-helper")->info(
            "tcc_status accessibility={} screen_recording={} prompted_before={} force={}",
            ax_ok, sr_ok, already_prompted, force_prompt);
    }
#endif

    // P3 order is critical: claim exclusive listen FIRST, then rotate the
    // IPC token. A second helper that writes a new token and then fails to
    // bind leaves the primary helper with a stale in-memory token while the
    // daemon reads the overwritten file → "ERROR: auth" on every RPC.
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    // Local\ mutex is per-session; exclusive bind is the cross-session lock.
    HANDLE single = CreateMutexW(nullptr, FALSE, L"Local\\BridgeSessions-CuaHelper-v1");
    if (!single) {
        std::cerr << "cua-helper: CreateMutex failed\n";
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cerr << "cua-helper: another instance is already running "
                     "(only one --cua-helper per user session)\n";
        CloseHandle(single);
        return 1;
    }
    SOCKET lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == INVALID_SOCKET) {
        std::cerr << "cua-helper: socket failed\n";
        CloseHandle(single);
        return 1;
    }
    // No SO_REUSEADDR: exclusive bind so a second helper cannot steal the port.
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(kCuaHelperPort);
    if (bind(lfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR || listen(lfd, 4) == SOCKET_ERROR) {
        std::cerr << "cua-helper: bind/listen 127.0.0.1:" << kCuaHelperPort
                  << " failed (another helper already owns the port?)\n";
        CLOSESOCK(lfd);
        CloseHandle(single);
        return 1;
    }
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
        std::cerr << "cua-helper: bind/listen " << sock_path
                  << " failed (another helper already owns the socket?)\n";
        CLOSESOCK(lfd); return 1;
    }
    // Restrict the socket file to the owner — the token is now protected by
    // filesystem permissions instead of loopback anonymity.
    ::chmod(sock_path.c_str(), 0600);
#endif

    // Token only after exclusive listen is held (see order note above).
    std::string token;
    try { token = generate_ipc_token(); }
    catch (const std::exception& e) {
        std::cerr << "cua-helper: token generation failed: " << e.what() << "\n";
        CLOSESOCK(lfd);
#ifdef _WIN32
        CloseHandle(single);
#endif
        return 1;
    }
    // SYSTEM-readable on Windows so Session-0 mesh daemon can auth to Session-1 helper
    if (!write_cua_helper_token(app_home, token)) {
        std::cerr << "cua-helper: cannot write " << cua_helper_token_path(app_home) << "\n";
        CLOSESOCK(lfd);
#ifdef _WIN32
        CloseHandle(single);
#endif
        return 1;
    }

    std::cout << "cua-helper: listening (token " << cua_helper_token_path(app_home) << ")\n"
              << std::flush;
#ifdef _WIN32
    bs::log::get("cua-helper")->info("Listening on 127.0.0.1:{}", kCuaHelperPort);
#else
    bs::log::get("cua-helper")->info("Listening on {}", cua_helper_socket_path(app_home));
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
