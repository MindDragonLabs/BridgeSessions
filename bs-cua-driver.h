// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-cua-driver.h — cua-driver-rs (trycua/cua) backend for `bs cua`.
//
// When a cua-driver binary and its user-session daemon are present, prefer it
// over the bs helper / native backends. Strictly additive: any failure falls
// through to the existing chain (helper → in-process). BS_CUA_DRIVER=0 opts
// out entirely.
//
// Interface (driver 0.30.x, verified against 0.28.2 + source):
//   cua-driver call <tool> '<json-args>' [--screenshot-out-file <path>]
//   stdout: MCP-style result { content: [...], structuredContent: {...} }
//   errors: result.isError == true, or stderr + exit!=0 (daemon down)
// Input verbs support scope:"desktop" — no window handle required.
//
// Designed for inclusion inside `namespace bs::mesh { ... }` (same contract
// as bs-cua-dispatch.h). Requires nlohmann/json.hpp already included.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// NOTE: POSIX system headers (poll.h, signal.h, sys/wait.h, unistd.h) are
// included at global scope by bs-protocol.h — this header is included inside
// namespace bs::mesh and must not pull them in here.

// ── run_with_timeout ────────────────────────────────────────────────────
// The driver hangs indefinitely against a dead display server (observed on a
// headless Linux box: get_screen_size blocks on the X connect). Every driver
// call goes through this bounded executor — never a raw popen.
struct ProcResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string out;
    std::string err;
};

#ifndef _WIN32
[[nodiscard]] inline ProcResult run_with_timeout(const std::vector<std::string>& argv,
                                                 int timeout_ms) {
    ProcResult res;
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) return res;

    pid_t pid = fork();
    if (pid == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        std::vector<char*> cargv;
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::string out, err;
    bool out_open = true, err_open = true;
    while (out_open || err_open) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            res.timed_out = true;
            kill(pid, SIGKILL);
            break;
        }
        int remaining = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        struct pollfd fds[2];
        nfds_t nfds = 0;
        if (out_open) { fds[nfds] = {out_pipe[0], POLLIN, 0}; nfds++; }
        if (err_open) { fds[nfds] = {err_pipe[0], POLLIN, 0}; nfds++; }
        int rc = poll(fds, nfds, remaining);
        if (rc <= 0) continue;
        nfds_t idx = 0;
        char buf[65536];
        if (out_open) {
            if (fds[idx].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(out_pipe[0], buf, sizeof(buf));
                if (n > 0) out.append(buf, static_cast<size_t>(n));
                else { out_open = false; close(out_pipe[0]); }
            }
            idx++;
        }
        if (err_open) {
            if (fds[idx].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(err_pipe[0], buf, sizeof(buf));
                if (n > 0) err.append(buf, static_cast<size_t>(n));
                else { err_open = false; close(err_pipe[0]); }
            }
        }
    }
    close(out_pipe[0]);
    close(err_pipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    res.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    res.out = std::move(out);
    res.err = std::move(err);
    return res;
}
#else
[[nodiscard]] inline ProcResult run_with_timeout(const std::vector<std::string>& argv,
                                                 int timeout_ms) {
    // Windows v1: _popen without a hard timeout. The driver daemon answers
    // locally so calls are short; the bs helper chain remains the fallback.
    ProcResult res;
    std::string cmd;
    for (const auto& a : argv) {
        if (!cmd.empty()) cmd += " ";
        cmd += "\"" + a + "\"";
    }
    cmd += " 2>nul";
    FILE* p = _popen(cmd.c_str(), "rb");
    if (!p) return res;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) res.out.append(buf, n);
    res.exit_code = _pclose(p);
    return res;
}
#endif

// ── driver discovery ─────────────────────────────────────────────────────
[[nodiscard]] inline std::optional<std::string> find_cua_driver() {
    // Opt-out wins over everything, including an explicit path override.
    if (const char* off = std::getenv("BS_CUA_DRIVER"); off && std::string(off) == "0")
        return std::nullopt;
    if (const char* p = std::getenv("CUA_DRIVER_PATH"); p && *p) {
        if (std::filesystem::exists(p)) return std::string(p);
        return std::nullopt;  // explicit override must exist
    }
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    const char* localapp = std::getenv("LOCALAPPDATA");
    std::vector<std::string> candidates;
    if (localapp) candidates.push_back(std::string(localapp) + "\\Programs\\cua-driver\\cua-driver.exe");
    if (home) candidates.push_back(std::string(home) + "\\.cua-driver\\packages\\current\\cua-driver.exe");
    candidates.push_back("cua-driver.exe");  // PATH
#else
    const char* home = std::getenv("HOME");
    std::vector<std::string> candidates;
    if (home) candidates.push_back(std::string(home) + "/.cua-driver/packages/current/cua-driver");
    candidates.push_back("cua-driver");  // PATH
#endif
    for (const auto& c : candidates) {
        if (c.find_first_of("/\\") != std::string::npos) {
            if (std::filesystem::exists(c)) return c;
        } else {
            // PATH lookup (local, self-contained — facade order puts this
            // header before the dispatch header that owns find_binary).
#ifndef _WIN32
            const char* path_env = std::getenv("PATH");
            if (path_env) {
                std::string p(path_env);
                size_t start = 0;
                while (start <= p.size()) {
                    size_t end = p.find(':', start);
                    std::filesystem::path dir =
                        p.substr(start, end == std::string::npos ? end - start : end);
                    std::filesystem::path cand = dir / c;
                    if (::access(cand.c_str(), X_OK) == 0) return cand.string();
                    if (end == std::string::npos) break;
                    start = end + 1;
                }
            }
#endif
        }
    }
    return std::nullopt;
}

// ── USB HID usage ID → cua-driver key name ───────────────────────────────
[[nodiscard]] inline std::optional<std::string> hid_to_driver_key(uint32_t hid) {
    if (hid >= 0x04 && hid <= 0x1D)  // a..z
        return std::string(1, static_cast<char>('a' + (hid - 0x04)));
    if (hid >= 0x1E && hid <= 0x26)  // 1..9
        return std::string(1, static_cast<char>('1' + (hid - 0x1E)));
    if (hid == 0x27) return std::string("0");
    switch (hid) {
        case 0x28: return std::string("return");
        case 0x29: return std::string("escape");
        case 0x2A: return std::string("backspace");
        case 0x2B: return std::string("tab");
        case 0x2C: return std::string("space");
        case 0x2D: return std::string("minus");
        case 0x2E: return std::string("equal");
        case 0x2F: return std::string("bracketleft");
        case 0x30: return std::string("bracketright");
        case 0x31: return std::string("backslash");
        case 0x33: return std::string("semicolon");
        case 0x34: return std::string("quote");
        case 0x35: return std::string("grave");
        case 0x36: return std::string("comma");
        case 0x37: return std::string("period");
        case 0x38: return std::string("slash");
        case 0x39: return std::string("capslock");
        case 0x4C: return std::string("delete");
        case 0x4F: return std::string("right");
        case 0x50: return std::string("left");
        case 0x51: return std::string("down");
        case 0x52: return std::string("up");
        case 0x4A: return std::string("home");
        case 0x4B: return std::string("pageup");
        case 0x4D: return std::string("end");
        case 0x4E: return std::string("pagedown");
        default: break;
    }
    if (hid >= 0x3A && hid <= 0x45)  // F1..F12
        return std::string("f") + std::to_string(hid - 0x3A + 1);
    return std::nullopt;
}

[[nodiscard]] inline nlohmann::json hid_modifiers_json(uint8_t mods) {
    auto arr = nlohmann::json::array();
    if (mods & 1) arr.push_back("ctrl");
    if (mods & 2) arr.push_back("shift");
    if (mods & 4) arr.push_back("alt");
    if (mods & 8) arr.push_back("super");
    return arr;
}

// ── the seam ─────────────────────────────────────────────────────────────
[[nodiscard]] inline std::vector<uint8_t> cua_b64_decode(const std::string& s) {
    // Local minimal decoder (facade order: bs-mesh-support.h's b64dec lands
    // after this header).
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int t[256] = {};
    for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(kAlphabet[i])] = i;
    t[static_cast<uint8_t>('=')] = 0;
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;  // padding: never emit a trailing partial byte
        acc = (acc << 6) | static_cast<uint32_t>(t[static_cast<uint8_t>(c)]);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

// Returns nullopt when the driver is unavailable/disabled (caller falls
// through). Returns a response (ok or error) when the driver answered.
[[nodiscard]] inline std::optional<CuaResponseMsg> cua_driver_execute(const CuaRequestMsg& req) {
    // Not cached: tests flip CUA_DRIVER_PATH/BS_CUA_DRIVER between cases, and
    // the lookup is a few stat() calls — cheap relative to a process spawn.
    const std::optional<std::string> driver = find_cua_driver();
    if (!driver) return std::nullopt;

    std::string tool;
    nlohmann::json args = nlohmann::json::object();
    args["scope"] = "desktop";

    switch (req.action) {
        case 0:  // screen_info
            tool = "get_screen_size";
            args.erase("scope");
            break;
        case 6:  // capture
            tool = "get_desktop_state";
            args.erase("scope");
            break;
        case 2:  // text
            tool = "type_text";
            args["text"] = req.text;
            break;
        case 1: {  // key
            auto key = hid_to_driver_key(req.hid_key);
            if (!key) return std::nullopt;  // unmapped key: use native path
            tool = "press_key";
            args["key"] = *key;
            auto mods = hid_modifiers_json(req.modifiers);
            if (!mods.empty()) args["modifiers"] = mods;
            break;
        }
        case 4: {  // mouse_button (click)
            tool = "click";
            args["x"] = req.x;
            args["y"] = req.y;
            args["button"] = req.button == 2 ? "right" : (req.button == 3 ? "middle" : "left");
            break;
        }
        case 5: {  // wheel
            tool = "scroll";
            args["x"] = req.x;
            args["y"] = req.y;
            args["direction"] = req.button == 1 ? "down" : "up";
            args["amount"] = 3;
            break;
        }
        default:
            return std::nullopt;  // mouse_move (3) and anything unknown: native path
    }

    int timeout_ms = 20000;
    if (const char* t = std::getenv("BS_CUA_DRIVER_TIMEOUT_MS"); t && *t) {
        if (int v = std::atoi(t); v > 0) timeout_ms = v;
    }
    ProcResult pr = run_with_timeout(
        {*driver, "call", tool, args.dump()}, timeout_ms);

    CuaResponseMsg resp;
    resp.request_id = req.request_id;
    if (pr.timed_out) {
        resp.status = 1;
        resp.error = "cua-driver call timed out (" + tool + ")";
        return resp;
    }
    if (pr.exit_code != 0) {
        // Daemon down or tool hard-failed: fall through to the bs chain.
        return std::nullopt;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(pr.out);
    } catch (...) {
        return std::nullopt;  // unparseable — treat as unavailable
    }
    if (j.value("isError", false)) {
        std::string msg = "cua-driver error";
        if (j.contains("content") && j["content"].is_array() && !j["content"].empty())
            msg = j["content"][0].value("text", msg);
        resp.status = 1;
        resp.error = msg;
        return resp;
    }

    auto sc = j.contains("structuredContent") ? j["structuredContent"] : j;
    switch (req.action) {
        case 0:
            resp.screen_w = sc.value("width", sc.value("screen_w", 0u));
            resp.screen_h = sc.value("height", sc.value("screen_h", 0u));
            break;
        case 6: {
            // image payload: content[].type=="image" base64, merged into
            // structuredContent as screenshot_png_b64 by the driver CLI.
            std::string b64 = sc.value("screenshot_png_b64", std::string{});
            if (b64.empty() && j.contains("content") && j["content"].is_array()) {
                for (const auto& item : j["content"]) {
                    if (item.value("type", "") == "image") {
                        b64 = item.value("data", std::string{});
                        break;
                    }
                }
            }
            if (b64.empty()) {
                resp.status = 1;
                resp.error = "cua-driver capture returned no image";
                return resp;
            }
            resp.format = 1;  // png
            resp.data = cua_b64_decode(b64);
            if (sc.contains("width")) resp.screen_w = sc.value("width", 0u);
            if (sc.contains("height")) resp.screen_h = sc.value("height", 0u);
            break;
        }
        default:
            break;
    }
    resp.status = 0;
    return resp;
}
