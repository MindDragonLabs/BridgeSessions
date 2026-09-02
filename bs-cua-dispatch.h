// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-cua-dispatch.h — Computer-use dispatch and video capture
// Extracted from bs-protocol.h (R6 structural refactor, 2026-09-02)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace — parent file provides it.
#pragma once

// ── 2.0.8 P5: Cross-platform Computer Use dispatch ─────────────────────
// Dispatches CuaRequestMsg to the appropriate OS backend.

#ifndef _WIN32
[[nodiscard]] std::optional<std::string> find_binary(std::string_view name) {
    if (name.find('/') != std::string_view::npos) {
        std::filesystem::path p{name};
        if (::access(p.c_str(), X_OK) == 0) return p.string();
        return std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return std::nullopt;
    std::string path_str(path_env);
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(':', start);
        std::string_view part = end == std::string::npos
            ? std::string_view(path_str).substr(start)
            : std::string_view(path_str).substr(start, end - start);
        std::filesystem::path dir = part.empty() ? std::filesystem::path(".") : std::filesystem::path(part);
        std::filesystem::path candidate = dir / name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate.string();
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}
#endif // _WIN32

// CUA helper constants and helpers (used by bs-cua-helper.h)
inline constexpr uint16_t kCuaHelperPort = 19986; // Windows-only (POSIX uses Unix socket)
[[nodiscard]] inline std::string cua_helper_token_path(const std::string& app_home) {
    return (std::filesystem::path(app_home) / "cua-helper-token").string();
}

// Windows: Session-1 helper writes the token as the interactive user; the mesh
// daemon often runs as SYSTEM (Session 0). Default creator-owner ACLs can block
// the daemon from reading the token → "ERROR: auth". Grant SYSTEM + Admins read.
[[nodiscard]] inline bool grant_local_system_read(const std::string& path) {
#ifdef _WIN32
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD st = GetNamedSecurityInfoA(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &old_dacl, nullptr, &sd);
    if (st != ERROR_SUCCESS) return false;
    PSID system_sid = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt_auth, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &system_sid)) {
        if (sd) LocalFree(sd);
        return false;
    }
    EXPLICIT_ACCESSA ea{};
    ea.grfAccessPermissions = FILE_GENERIC_READ;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPSTR>(system_sid);
    PACL new_dacl = nullptr;
    st = SetEntriesInAclA(1, &ea, old_dacl, &new_dacl);
    bool ok = false;
    if (st == ERROR_SUCCESS && new_dacl) {
        st = SetNamedSecurityInfoA(const_cast<char*>(path.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
        ok = (st == ERROR_SUCCESS);
        LocalFree(new_dacl);
    }
    FreeSid(system_sid);
    if (sd) LocalFree(sd);
    return ok;
#else
    (void)path;
    return true;
#endif
}

// Write CUA helper IPC token (owner-private + SYSTEM read on Windows).
[[nodiscard]] inline bool write_cua_helper_token(const std::string& app_home,
                                                 std::string_view token) {
    const std::string path = cua_helper_token_path(app_home);
    if (!write_private_text_file(path, token)) return false;
#ifdef _WIN32
    (void)grant_local_system_read(path);
#endif
    return true;
}

// P2: Unix domain socket path for CUA helper (POSIX). Windows keeps TCP loopback.
[[nodiscard]] inline std::string cua_helper_socket_path(const std::string& app_home) {
    return (std::filesystem::path(app_home) / "cua-helper.sock").string();
}

// Cross-platform socket close helper for cua_helper_rpc
inline void close_socket(int fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

// CUA helper RPC: call the user-session helper at localhost:19986
// The helper runs in the interactive desktop session (where capture/input work).
// Returns {status=1, error} if helper not running — caller falls back.
[[nodiscard]] inline CuaResponseMsg cua_helper_rpc(const CuaRequestMsg& req, const std::string& app_home) {
    CuaResponseMsg resp;
    resp.status = 1;

    // Load auth token from app home (path helper; SYSTEM must be able to read)
    std::string token_path = cua_helper_token_path(app_home);
    std::ifstream tf(token_path);
    if (!tf) { resp.error = "no cua-helper token"; return resp; }
    std::string token((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
    if (token.empty()) { resp.error = "empty cua-helper token"; return resp; }

    // P2 security: POSIX uses Unix domain socket (filesystem perms protect token).
    // Windows: TCP loopback (named pipes are the alternative but need separate code path).
#ifdef _WIN32
    int sfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { resp.error = "socket failed"; return resp; }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(kCuaHelperPort);
    if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) < 0) {
        close_socket(sfd);
        resp.error = "cua-helper not running (connect failed)";
        return resp;
    }
#else
    std::string sock_path = cua_helper_socket_path(app_home);
    int sfd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { resp.error = "socket failed"; return resp; }
    sockaddr_un su{};
    su.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof(su.sun_path)) {
        close_socket(sfd);
        resp.error = "cua-helper socket path too long";
        return resp;
    }
    std::strncpy(su.sun_path, sock_path.c_str(), sizeof(su.sun_path) - 1);
    if (connect(sfd, (sockaddr*)&su, sizeof(su)) < 0) {
        close_socket(sfd);
        resp.error = "cua-helper not running (connect failed)";
        return resp;
    }
#endif
    // Recv timeout: capture (action 6) may take PowerShell+GDI ~2–8s; allow 20s.
    // Input actions stay snappy with the same budget (fail closed if helper wedged).
#ifdef _WIN32
    DWORD rcv_timeout_ms = 20000;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_timeout_ms, sizeof(rcv_timeout_ms));
#else
    struct timeval rcv_tv { 20, 0 };
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
#endif

    // Send request as JSON line
    nlohmann::json j;
    j["action"] = req.action;
    j["x"] = req.x;
    j["y"] = req.y;
    j["button"] = req.button;
    j["hid_key"] = req.hid_key;
    j["modifiers"] = req.modifiers;
    j["text"] = req.text;
    // Token as space-prefix: helper expects "TOKEN {json}\n"
    std::string line = token + " " + j.dump() + "\n";
    if (send(sfd, line.data(), (int)line.size(), 0) <= 0) {
        close_socket(sfd);
        resp.error = "cua-helper send failed";
        return resp;
    }

    // Read response
    std::string acc;
    char buf[65536];
    while (true) {
        int n = (int)recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        acc.append(buf, n);
        if (acc.find('\n') != std::string::npos) break;
    }
    close_socket(sfd);

    if (acc.empty()) { resp.error = "cua-helper no response"; return resp; }
    try {
        auto r = nlohmann::json::parse(acc);
        resp.status = r.value("status", 1);
        resp.error = r.value("error", "");
        resp.screen_w = r.value("screen_w", 0);
        resp.screen_h = r.value("screen_h", 0);
        resp.format = r.value("format", 0);
        if (r.contains("data_b64") && r["data_b64"].is_string()) {
            // Base64 decode inline (helper sends base64-encoded image data as "data_b64")
            // P2 audit fix: 256-byte reverse lookup table instead of find() (O(n*64))
            // for large screenshots (up to 50MB).
            std::string b64 = r["data_b64"].get<std::string>();
            static const std::string b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            static int8_t kRev[256] = {};
            static bool kRevInit = false;
            if (!kRevInit) {
                for (int i = 0; i < 256; ++i) kRev[i] = -1;
                for (size_t i = 0; i < b64chars.size(); ++i) kRev[(uint8_t)b64chars[i]] = (int8_t)i;
                kRevInit = true;
            }
            int val = 0, valb = -8;
            for (char c : b64) {
                if (c == '=') break;
                int8_t v = kRev[(uint8_t)c];
                if (v < 0) continue;
                val = (val << 6) | v;
                valb += 6;
                if (valb >= 0) {
                    resp.data.push_back((uint8_t)((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
        }
    } catch (const std::exception& e) {
        resp.error = std::string("cua-helper parse error: ") + e.what();
    }
    return resp;
}

// Forward declarations for macOS CUA (definitions in macos-capture.mm)
#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#ifdef BS_TESTING
inline int bs_macos_capture_png(const char*, unsigned, char* error, size_t capacity) {
    if (error && capacity) std::snprintf(error, capacity, "ScreenCaptureKit test stub");
    return 0;
}
#else
extern "C" int bs_macos_capture_png(const char*, unsigned, char*, size_t);
#endif
#endif

[[nodiscard]] CuaResponseMsg cua_execute(const CuaRequestMsg& req, const std::string& app_home = "") {
    CuaResponseMsg resp;
    resp.status = 0;

#ifdef _WIN32
    // Prefer cua-helper for ALL actions (including capture). Session 0 daemons
    // cannot reliably GDI-capture the interactive desktop; the helper (user
    // session) owns BitBlt + BMP encode (no GDI+ — crashes on recent Win11).
    {
        auto helper_resp = cua_helper_rpc(req, app_home);
        if (helper_resp.status == 0) return helper_resp;
        // P1: always retain helper error for the outer message
        if (!helper_resp.error.empty()) resp.error = helper_resp.error;
    }
    // Fallback: PowerShell GDI capture for action 6 only, and only when this
    // process is already in an interactive session (Session != 0). Session 0
    // PowerShell cannot see the desktop — skipping avoids a misleading path.
    if (req.action == 6) {
        DWORD sid = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sid);
        if (sid == 0) {
            resp.status = 1;
            if (resp.error.empty()) {
                resp.error = "windows CUA capture requires --cua-helper in Session 1 "
                             "(daemon is Session 0; PowerShell GDI fallback skipped)";
            } else {
                resp.error += "; PowerShell GDI fallback skipped (Session 0)";
            }
            return resp;
        }
        std::string tmp_path = create_private_temp_file("bsc", ".png", app_home);
        if (tmp_path.empty()) {
            resp.status = 1;
            resp.error = "failed to allocate private capture temp file";
            return resp;
        }
        // P2: load System.Windows.Forms (VirtualScreen) + System.Drawing.
        // Escape single quotes in path for PowerShell single-quoted -Command.
        std::string ps_path = tmp_path;
        for (size_t i = 0; i < ps_path.size(); ++i) {
            if (ps_path[i] == '\'') { ps_path.insert(i, "'"); ++i; }
        }
        std::string ps_cmd =
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
            "$g.Dispose();$img.Save('" + ps_path + "',[System.Drawing.Imaging.ImageFormat]::Png);"
            "$img.Dispose()\" 2>nul";
        int rc = std::system(ps_cmd.c_str());
        if (rc != 0 || !std::filesystem::exists(tmp_path)) {
            resp.status = 1;
            // P1: never drop the helper error under a generic message
            if (resp.error.empty()) {
                resp.error = "windows screen capture failed (helper unreachable; "
                             "PowerShell GDI fallback also failed)";
            } else {
                resp.error += "; PowerShell GDI fallback also failed";
            }
            return resp;
        }
        std::ifstream cap(tmp_path, std::ios::binary | std::ios::ate);
        if (!cap) {
            resp.status = 1;
            resp.error = (resp.error.empty() ? "" : resp.error + "; ") +
                         "failed to open capture file";
            ::unlink(tmp_path.c_str()); return resp;
        }
        auto cap_size = cap.tellg();
        if (cap_size > 0 && static_cast<size_t>(cap_size) <= MAX_IMAGE_BYTES) {
            resp.data.resize(static_cast<size_t>(cap_size));
            cap.seekg(0);
            cap.read(reinterpret_cast<char*>(resp.data.data()), cap_size);
            resp.format = 0; resp.status = 0;  // PNG
            resp.screen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            resp.screen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            resp.error.clear();
        } else {
            resp.status = 1;
            resp.error = (resp.error.empty() ? "" : resp.error + "; ") +
                         "capture empty or exceeds 50MB";
        }
        ::unlink(tmp_path.c_str());
        return resp;
    }
    resp.status = 1;
    if (resp.error.empty())
        resp.error = "windows CUA requires cua-helper (run: bridgesessions --cua-helper in user session)";
    return resp;
#elif defined(__APPLE__)
    // macOS: route through cua-helper (runs in user session for desktop access).
    // The helper uses CGEvent for input injection and ScreenCaptureKit for capture.
    {
        auto helper_resp = cua_helper_rpc(req, app_home);
        if (helper_resp.status == 0) return helper_resp;
        // Fall through to in-process fallback
    }
    // In-process fallback (daemon running in user session)
    if (req.action == 0) {
        // Screen dimensions via CoreGraphics (no subprocess needed)
        auto r = CGDisplayBounds(CGMainDisplayID());
        resp.screen_w = static_cast<uint32_t>(std::max(0.0, r.size.width));
        resp.screen_h = static_cast<uint32_t>(std::max(0.0, r.size.height));
        resp.status = 0;
        return resp;
    }
    if (req.action == 6) {
        // Capture via ScreenCaptureKit (macos-capture.mm), with screencapture(1)
        // fallback when TCC blocks SCK ("user declined" / no shareable displays).
        // Default to failure — do not leave status=0 with empty data (CLI then
        // prints "no capture data returned" with a false success).
        resp.status = 1;
        std::string tmp_owned = create_private_temp_file("cua", ".png", app_home);
        if (tmp_owned.empty()) {
            resp.error = "macOS capture failed: cannot create private temp file";
            return resp;
        }
        const char* tmp = tmp_owned.c_str();
        char err[1024] = {};
        bool got_png = bs_macos_capture_png(tmp, 0, err, sizeof(err));
        if (!got_png) {
            std::string sc = "/usr/sbin/screencapture -x -t png '";
            sc += tmp;
            sc += "' 2>/dev/null";
            if (std::system(sc.c_str()) == 0 && std::filesystem::exists(tmp) &&
                std::filesystem::file_size(tmp) > 0) {
                got_png = true;
                err[0] = '\0';
            }
        }
        if (got_png) {
            std::ifstream cap(tmp, std::ios::binary | std::ios::ate);
            auto sz = cap.tellg();
            if (sz > 0 && (size_t)sz <= MAX_IMAGE_BYTES) {
                resp.data.resize((size_t)sz);
                cap.seekg(0);
                cap.read((char*)resp.data.data(), sz);
                resp.format = 1;
                resp.status = 0;
                resp.error.clear();
                auto r = CGDisplayBounds(CGMainDisplayID());
                resp.screen_w = static_cast<uint32_t>(std::max(0.0, r.size.width));
                resp.screen_h = static_cast<uint32_t>(std::max(0.0, r.size.height));
            } else {
                resp.error = "macOS capture empty/oversize";
            }
        } else {
            resp.error = std::string("macOS capture failed (grant Screen Recording to "
                                     "BridgeSessions.app if needed): ") +
                         (err[0] ? err : "ScreenCaptureKit / screencapture failed");
        }
        ::unlink(tmp);
        return resp;
    }
    resp.status = 1;
    resp.error = "macos CUA input requires cua-helper (run: bridgesessions --cua-helper in user session)";
    return resp;
#else
    // Linux: dispatch via xdotool (ubiquitous on X11 desktops).
    std::string cmd;
    switch (req.action) {
        case 0: { // screen info
            std::string out;
            FILE* p = popen("xdpyinfo 2>/dev/null | grep dimensions", "r");
            if (p) { char buf[256]; while (fgets(buf, sizeof(buf), p)) out += buf; pclose(p); }
            if (!out.empty()) {
                // Parse "  dimensions:    1920x1080 pixels" (or "1920 x 1080").
                // IMPORTANT: do not search for 'x' from offset 0 — that hits the
                // letter x inside the word "dimensions".
                auto dim = out.find("dimensions");
                auto px = (dim == std::string::npos) ? std::string::npos
                                                     : out.find('x', dim);
                if (px != std::string::npos && px > 0) {
                    // Walk back over spaces to the start of the width number
                    size_t w_end = px;
                    while (w_end > 0 && out[w_end - 1] == ' ') --w_end;
                    size_t w_start = w_end;
                    while (w_start > 0 && std::isdigit(static_cast<unsigned char>(out[w_start - 1])))
                        --w_start;
                    size_t h_start = px + 1;
                    while (h_start < out.size() && out[h_start] == ' ') ++h_start;
                    size_t h_end = h_start;
                    while (h_end < out.size() && std::isdigit(static_cast<unsigned char>(out[h_end])))
                        ++h_end;
                    if (w_end > w_start && h_end > h_start) {
                        resp.screen_w = static_cast<int16_t>(
                            std::stoul(out.substr(w_start, w_end - w_start)));
                        resp.screen_h = static_cast<int16_t>(
                            std::stoul(out.substr(h_start, h_end - h_start)));
                        resp.status = 0;
                        return resp;
                    }
                }
            }
            resp.status = 1; resp.error = "cannot determine screen size";
            return resp;
        }
        case 1: // key press
            cmd = "xdotool key --delay 0 " + std::to_string(req.hid_key) + " 2>/dev/null";
            break;
        case 2: { // text entry
            // POSIX single-quote escape: close, escaped-quote, reopen ('\'').
            // A backslash-prefix does NOT escape inside single quotes — the
            // old escaping allowed full shell injection via req.text
            // (2.0.8 MoA P0, two-lane consensus).
            std::string escaped = "'";
            for (char ch : req.text) {
                if (ch == '\'') escaped += "'\\''";
                else escaped += ch;
            }
            escaped += "'";
            cmd = "xdotool type --delay 0 " + escaped + " 2>/dev/null";
            break;
        }
        case 3: // mouse move
            cmd = "xdotool mousemove " + std::to_string(req.x) + " "
                + std::to_string(req.y) + " 2>/dev/null";
            break;
        case 4: // mouse button
            cmd = std::string("xdotool click ") + std::to_string(req.button) + " 2>/dev/null";
            break;
        case 5: // mouse wheel
            cmd = std::string("xdotool click ") + ((req.button == 0) ? "4" : "5") + " 2>/dev/null";
            break;
        case 6: { // screen capture — v2.0.11 P5c
            // Tool order matters: ImageMagick `import` often "succeeds" with a
            // 1-bit grayscale black PNG under XFCE/Xvfb (~263B for 1280x800) while
            // scrot returns a real RGB framebuffer. Prefer grim (Wayland) then
            // scrot, and only then import as last resort.
            // Ensure DISPLAY for X11 tools when daemon lacks a session env.
            if (const char* d = std::getenv("DISPLAY"); !d || !*d) {
                setenv("DISPLAY", ":0", 0);
                // Common agent desktop paths (bs-qa-ubuntu XFCE uses :1)
                if (std::filesystem::exists("/tmp/.X11-unix/X1"))
                    setenv("DISPLAY", ":1", 1);
                else if (std::filesystem::exists("/tmp/.X11-unix/X0"))
                    setenv("DISPLAY", ":0", 1);
            }
            std::string tmp_path;
            const char* tools[] = {"grim", "scrot", "import", nullptr};
            for (int i = 0; tools[i]; ++i) {
                auto bin = find_binary(tools[i]);
                if (!bin) continue;
                tmp_path = create_private_temp_file("cap", ".png", app_home);
                if (tmp_path.empty()) continue;
                std::string capture_cmd;
                if (std::string(tools[i]) == "grim") {
                    capture_cmd = *bin + " '" + tmp_path + "' 2>/dev/null";
                } else if (std::string(tools[i]) == "import") {
                    capture_cmd = *bin + " -window root '" + tmp_path + "' 2>/dev/null";
                } else {
                    capture_cmd = *bin + " '" + tmp_path + "' 2>/dev/null";
                }
                if (std::system(capture_cmd.c_str()) != 0) {
                    ::unlink(tmp_path.c_str());
                    tmp_path.clear();
                    continue;
                }
                // Reject tiny / 1-bit "success" captures and try next tool.
                std::error_code ec;
                auto sz = std::filesystem::file_size(tmp_path, ec);
                if (ec || sz < 1024) {
                    ::unlink(tmp_path.c_str());
                    tmp_path.clear();
                    continue;
                }
                break;
            }
            if (tmp_path.empty()) {
                resp.status = 1;
                resp.error = "no screen capture tool available (install grim, scrot, or imagemagick)";
                return resp;
            }
            // Prefer JPEG for mesh (macOS path does the same): smaller frames,
            // reliable +frm2 transport. Fall back to PNG if convert missing.
            std::string out_path = tmp_path;
            uint8_t out_format = 1;  // PNG
            {
                std::string jpg = tmp_path + ".jpg";
                std::string conv;
                if (auto c = find_binary("convert")) {
                    conv = *c + " '" + tmp_path +
                           "' -colorspace sRGB -type TrueColor -resize '1280x1280>' -quality 40 '" +
                           jpg + "' 2>/dev/null";
                } else if (auto m = find_binary("magick")) {
                    conv = *m + " '" + tmp_path +
                           "' -colorspace sRGB -type TrueColor -resize '1280x1280>' -quality 40 '" +
                           jpg + "' 2>/dev/null";
                }
                if (!conv.empty() && std::system(conv.c_str()) == 0 &&
                    std::filesystem::exists(jpg) &&
                    std::filesystem::file_size(jpg) > 0) {
                    ::unlink(tmp_path.c_str());
                    out_path = jpg;
                    out_format = 2;  // JPEG
                }
            }
            std::ifstream cap(out_path, std::ios::binary | std::ios::ate);
            if (!cap) {
                resp.status = 1;
                resp.error = "failed to open capture temp file";
                ::unlink(out_path.c_str());
                return resp;
            }
            auto cap_size = cap.tellg();
            if (cap_size > 0 && static_cast<size_t>(cap_size) <= MAX_IMAGE_BYTES) {
                resp.data.resize(static_cast<size_t>(cap_size));
                cap.seekg(0);
                cap.read(reinterpret_cast<char*>(resp.data.data()), cap_size);
                resp.format = out_format;
                resp.status = 0;
                FILE* xr = popen(
                    "xdpyinfo 2>/dev/null | grep dimensions", "r");
                if (xr) {
                    char dims[256] = {};
                    if (fgets(dims, sizeof(dims), xr)) {
                        // Parse after "dimensions" so we don't match the 'x' in the word.
                        std::string out = dims;
                        auto dim = out.find("dimensions");
                        auto px = (dim == std::string::npos) ? std::string::npos
                                                             : out.find('x', dim);
                        if (px != std::string::npos && px > 0) {
                            int w = 0, h = 0;
                            // walk back for width digits
                            size_t i = px;
                            while (i > 0 && std::isdigit(static_cast<unsigned char>(out[i - 1]))) --i;
                            w = std::atoi(out.c_str() + i);
                            h = std::atoi(out.c_str() + px + 1);
                            if (w > 0 && h > 0) {
                                resp.screen_w = (int16_t)w;
                                resp.screen_h = (int16_t)h;
                            }
                        }
                    }
                    pclose(xr);
                }
            } else {
                resp.status = 1;
                resp.error = "capture file empty or exceeds size limit";
            }
            ::unlink(out_path.c_str());
            return resp;
        }
        default:
            resp.status = 1;
            resp.error = "unknown CUA action " + std::to_string(req.action);
            return resp;
    }
    if (!cmd.empty() && std::system(cmd.c_str()) != 0) {
        resp.status = 1;
        resp.error = "xdotool failed";
    }
    return resp;
#endif
}

// ── 2.0.12: Video capture via ffmpeg ─────────────────────────────
// (bs_macos_capture_png forward declaration moved to top of file near cua_execute)
[[nodiscard]] CuaVideoCaptureResultMsg video_capture_execute(const CuaVideoCaptureMsg& req) {
    CuaVideoCaptureResultMsg result;
    result.request_id = req.request_id;
    result.status = 1;

#ifdef _WIN32
    std::string tmp_path = create_private_temp_file("bsv", ".mp4");
    if (tmp_path.empty()) {
        result.error = "failed to allocate private video temp file";
        return result;
    }
    std::string cmd =
        "ffmpeg -y -f gdigrab -framerate " + std::to_string(req.fps) +
        " -t " + std::to_string(req.duration_sec) +
        " -i desktop -c:v libx264 -preset ultrafast -crf 28 -pix_fmt yuv420p \"" +
        tmp_path + "\" 2>nul";
#elif defined(__APPLE__)
    std::string tmp_path = create_private_temp_file("bsv", ".mp4");
    if (tmp_path.empty()) {
        result.error = "failed to allocate private video temp file";
        return result;
    }
    const std::string frames_dir = tmp_path + ".frames";
    std::error_code frames_ec;
    std::filesystem::create_directories(frames_dir, frames_ec);
    if (frames_ec) {
        result.error = "failed to create macOS capture frame directory";
        return result;
    }

    const uint32_t fps = (std::max)(uint32_t{1}, static_cast<uint32_t>(req.fps));
    const uint32_t duration = (std::max)(uint32_t{1}, static_cast<uint32_t>(req.duration_sec));
    const uint32_t max_width = req.max_width == 0 ? 1920u : static_cast<uint32_t>(req.max_width);
    const uint64_t frame_count = static_cast<uint64_t>(fps) * duration;
    const auto interval = std::chrono::duration<double>(1.0 / static_cast<double>(fps));
    auto next_frame = std::chrono::steady_clock::now();
    for (uint64_t frame = 0; frame < frame_count; ++frame) {
        char filename[64]{};
        std::snprintf(filename, sizeof(filename), "frame-%06llu.png",
                      static_cast<unsigned long long>(frame));
        const std::string frame_path =
            (std::filesystem::path(frames_dir) / filename).string();
        char capture_error[1024]{};
        if (!bs_macos_capture_png(frame_path.c_str(), max_width,
                                  capture_error, sizeof(capture_error))) {
            std::filesystem::remove_all(frames_dir, frames_ec);
            result.error = "ScreenCaptureKit failed: " + std::string(capture_error);
            return result;
        }
        next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        std::this_thread::sleep_until(next_frame);
    }

    auto ffmpeg = find_binary("/opt/homebrew/bin/ffmpeg");
    if (!ffmpeg) ffmpeg = find_binary("/usr/local/bin/ffmpeg");
    if (!ffmpeg) ffmpeg = find_binary("ffmpeg");
    std::string cmd;
    if (ffmpeg) {
        cmd = *ffmpeg + " -hide_banner -loglevel error -y -framerate " +
              std::to_string(fps) + " -i '" + frames_dir +
              "/frame-%06d.png' -c:v libx264 -preset ultrafast -crf 28" +
              " -pix_fmt yuv420p '" + tmp_path + "' 2>'" +
              (private_tmp_dir() + "/bs-video-ffmpeg.log") + "'";
    }
#else
    std::string tmp_path = create_private_temp_file("bsv", ".mp4");
    if (tmp_path.empty()) {
        result.error = "failed to allocate private video temp file";
        return result;
    }
    auto ffmpeg = find_binary("ffmpeg");
    std::string cmd;
    if (ffmpeg) {
        cmd = *ffmpeg + " -y -f x11grab -framerate " + std::to_string(req.fps) +
              " -t " + std::to_string(req.duration_sec) +
              " -i :0.0 -c:v libx264 -preset ultrafast -crf 28 -pix_fmt yuv420p '" +
              tmp_path + "' 2>/dev/null";
    }
#endif
    if (cmd.empty()) {
        result.error = "ffmpeg not available for video capture";
        return result;
    }
    int rc = std::system(cmd.c_str());
    if (rc != 0 || !std::filesystem::exists(tmp_path)) {
#ifdef __APPLE__
        result.error = "macOS video capture failed (ffmpeg exit " + std::to_string(rc) +
                       "); verify Screen Recording permission for bridgesessions/ffmpeg";
#else
        result.error = "video capture failed (ffmpeg exit " + std::to_string(rc) + ")";
#endif
        return result;
    }
    result.status = 0;
    result.file_path = tmp_path;
    result.duration_sec = req.duration_sec;
    result.format = 1; // mp4
#ifdef __APPLE__
    // P2 audit fix: frames_dir (~450MB of PNGs per capture) leaked on success.
    // Remove the intermediate frame directory now that the mp4 is produced.
    std::error_code cleanup_ec;
    std::filesystem::remove_all(frames_dir, cleanup_ec);
#endif
    return result;
}

[[nodiscard]] std::string read_available_pty_output(Session& session,
                                                     size_t max_bytes = 256 * 1024) {
    std::string output;
    output.reserve((std::min)(max_bytes, size_t{16 * 1024}));
    std::array<char, 16 * 1024> buf{};

    while (output.size() < max_bytes) {
        const size_t wanted = (std::min)(buf.size(), max_bytes - output.size());
#ifdef _WIN32
        DWORD available = 0;
        if (!PeekNamedPipe(session.master_fd, nullptr, 0, nullptr, &available, nullptr) || available == 0)
            break;
        DWORD nread = 0;
        if (!ReadFile(session.master_fd, buf.data(),
                      static_cast<DWORD>((std::min)(wanted, static_cast<size_t>(available))),
                      &nread, nullptr) || nread == 0)
            break;
        output.append(buf.data(), static_cast<size_t>(nread));
#else
        const ssize_t nread = ::read(session.master_fd, buf.data(), wanted);
        if (nread > 0) {
            output.append(buf.data(), static_cast<size_t>(nread));
            continue;
        }
        if (nread < 0 && errno == EINTR) continue;
        if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        break;
#endif
    }
    return output;
}

