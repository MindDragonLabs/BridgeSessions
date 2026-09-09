// main.cpp — BridgeSessions CLI entrypoint + daemon launcher
// Extracted from bridgesessions.cpp (R3 structural refactor, 2026-07-23)
#include "bs-protocol.h"
#include "bs-cua-helper.h"
#include "bs-sync-pair.h"
#include "bs-logging.h"

#ifndef INSTALL_DIR
#define INSTALL_DIR "~/.local/bin"
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#else
#include <process.h>
#ifndef _P_DETACH
#define _P_DETACH _P_NOWAIT
#endif
#endif

// Tag validation is defined in bs-protocol.h (shared with tests).

#ifndef BS_TESTING
// ────────────────────────────────────────────────────────────────────
// 2. MAIN — CLI + daemon (guarded for test builds)
// ────────────────────────────────────────────────────────────────────

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <random>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <unordered_set>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace {

std::string resolve_home(const std::string& path) {
    return bs::mesh::expand_home(path);
}

// ── keygen: generate ed25519 keypair ──────────────────────────────
int cmd_keygen(const std::string& app_home) {
    std::string dir = app_home.empty() ? (resolve_home("~") + "/.bridgesessions") : app_home;
    if (dir.empty()) { std::cerr << "config dir empty / HOME not set\n"; return 1; }

    if (!bs::mesh::ensure_private_directory(dir)) {
        std::cerr << "cannot create private config directory: " << dir << "\n";
        return 1;
    }

    std::string key_path  = dir + "/id_ed25519.pem";
    std::string cert_path = dir + "/id_ed25519-cert.pem";
    std::string pub_path  = dir + "/id_ed25519.pub";
    if (std::filesystem::exists(key_path) || std::filesystem::exists(cert_path) ||
        std::filesystem::exists(pub_path)) {
        std::cerr << "Refusing to overwrite existing identity in " << dir << "\n"
                  << "Back up and move all id_ed25519 files before deliberate rotation.\n";
        return 1;
    }

    auto [cert, key] = bs::mesh::generate_cert_key_pair("bridgesessions");
    auto pubkey = bs::mesh::pubkey_hex_from_pem(key);

    if (!bs::mesh::write_private_text_file(key_path, key) ||
        !bs::mesh::write_private_text_file(cert_path, cert) ||
        !bs::mesh::write_private_text_file(pub_path, pubkey + "\n")) {
        std::cerr << "cannot securely write generated identity\n";
        return 1;
    }

    std::cout << "Generated ed25519 keypair:\n"
              << "  Private key: " << key_path << "\n"
              << "  Certificate: " << cert_path << "\n"
              << "  Public key:  " << pub_path << "\n"
              << "  Pubkey hex:  " << pubkey << "\n";

    return 0;
}

// Free-function wrappers for IPC (used by main dispatch)
static std::string daemon_simple_ipc(const std::string& cmd, int wait_ms,
                                     const std::string& app_home) {
    std::string token = bs::mesh::load_ipc_token(app_home);
    if (token.empty()) return "";
    SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == INVALID_SOCKET) return "";
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(bs::mesh::mesh_cli_port());
    // set_socket_timeouts inline
    int ms = wait_ms > 0 ? wait_ms : 5000;
#ifdef _WIN32
    DWORD to = ms;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
#else
    timeval tv{}; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) { CLOSESOCK(sfd); return ""; }
    std::string full = token + " " + cmd + "\n";
    send(sfd, full.data(), (int)full.size(), 0);
    // Accumulate into std::string — large FLEET/SESSIONS/TELEMETRY responses
    // can exceed the old 4096-byte fixed buffer. Cap at 1 MB to avoid abuse.
    std::string acc;
    constexpr size_t kMaxIpcResponse = 1 << 20; // 1 MB
    char chunk[4096];
    while (acc.size() < kMaxIpcResponse) {
        int n = recv(sfd, chunk, (int)sizeof(chunk), 0);
        if (n > 0) {
            acc.append(chunk, (size_t)n);
            if (acc.find('\n') != std::string::npos) break;
        } else {
            break;
        }
    }
    CLOSESOCK(sfd);
    while (!acc.empty() && (acc.back() == '\r' || acc.back() == '\n')) acc.pop_back();
    return acc;
}

std::string current_exe_path(const char* argv0) {
    std::string self = argv0 ? argv0 : "bridgesessions";
#ifdef __APPLE__
    char exe_path[4096] = {};
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) == 0) self = exe_path;
#elif defined(__linux__)
    char rp[4096];
    ssize_t n = ::readlink("/proc/self/exe", rp, sizeof(rp) - 1);
    if (n > 0) { rp[n] = 0; self = rp; }
#endif
    return self;
}

// Pause the platform service so the binary can be swapped. Never persist-disable
// the systemd unit: disable + a failed resume left a peer refusing inbound
// sessions (TCP errno 61) after the 2026-08-25 upgrade.
void pause_mesh_daemon() {
#ifdef __APPLE__
    std::system("launchctl bootout gui/$(id -u)/com.bridgesessions.mesh 2>/dev/null");
    std::system("launchctl bootout gui/$(id -u)/com.bridgesessions.cua-helper 2>/dev/null");
#elif defined(__linux__)
    std::system("systemctl --user mask --runtime bridgesessions.service 2>/dev/null");
    std::system("systemctl --user stop bridgesessions.service 2>/dev/null");
#elif defined(_WIN32)
    std::system("schtasks /end /tn BridgeSessions 2>nul");
#endif
}

bool start_detached_daemon(const std::string& exe, const std::string& cfg_path) {
    if (exe.empty() || cfg_path.empty()) return false;
#ifdef _WIN32
    // argv spawn — do not interpolate paths into cmd.exe / std::system.
    const intptr_t rc = _spawnl(_P_DETACH, exe.c_str(), exe.c_str(),
                                "--daemon", "--config", cfg_path.c_str(),
                                nullptr);
    return rc >= 0;
#else
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > 2) ::close(devnull);
        }
        ::setsid();
        ::execl(exe.c_str(), exe.c_str(), "--daemon", "--config",
                cfg_path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    return true;
#endif
}

bool resume_mesh_daemon(const std::string& exe, const std::string& cfg_path) {
#ifdef __APPLE__
    int rc = std::system(
        "launchctl kickstart -k gui/$(id -u)/com.bridgesessions.mesh 2>/dev/null || "
        "launchctl bootstrap gui/$(id -u) \"$HOME/Library/LaunchAgents/com.bridgesessions.mesh.plist\" 2>/dev/null");
    std::system(
        "launchctl bootstrap gui/$(id -u) \"$HOME/Library/LaunchAgents/com.bridgesessions.cua-helper.plist\" 2>/dev/null");
    if (rc == 0) return true;
#elif defined(__linux__)
    std::system("systemctl --user unmask bridgesessions.service 2>/dev/null");
    std::system("systemctl --user daemon-reload 2>/dev/null");
    std::system("systemctl --user enable --now bridgesessions.service 2>/dev/null");
    if (std::system("systemctl --user is-active bridgesessions.service >/dev/null 2>&1") == 0)
        return true;
#elif defined(_WIN32)
    std::system("schtasks /change /tn BridgeSessions /enable >nul 2>nul");
    if (std::system("schtasks /run /tn BridgeSessions") == 0) return true;
#endif
    return start_detached_daemon(exe, cfg_path);
}

// ── authorize: register a hex-encoded ed25519 public key ──────────
int cmd_authorize(const char* hex_pubkey, const std::string& app_home) {
    if (!hex_pubkey || !*hex_pubkey) {
        std::cerr << "usage: bridgesessions authorize <hex-pubkey>\n";
        return 1;
    }

    std::string normalized(hex_pubkey);
    const auto decoded = bs::mesh::hex_decode(normalized);
    if (normalized.size() != 64 || decoded.size() != 32) {
        std::cerr << "invalid ed25519 public key: expected 64 hexadecimal characters\n";
        return 1;
    }
    for (char& c : normalized) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }

    std::string dir = app_home.empty() ? (resolve_home("~") + "/.bridgesessions") : app_home;
    if (dir.empty()) { std::cerr << "config dir empty / HOME not set\n"; return 1; }

    if (!bs::mesh::ensure_private_directory(dir)) {
        std::cerr << "cannot create private config directory: " << dir << "\n";
        return 1;
    }
    std::string path = dir + "/authorized_keys";

    // Check for duplicates
    {
        std::ifstream existing(path);
        std::string line;
        while (std::getline(existing, line)) {
            if (line == normalized) {
                std::cout << "Key already authorized: " << normalized << "\n";
                return 0;
            }
        }
    }

    if (!bs::mesh::append_private_text_file(path, normalized + "\n")) {
        std::cerr << "cannot securely update " << path << "\n";
        return 1;
    }

    std::cout << "Authorized key: " << normalized << "\n";
    std::cout << "Written to: " << path << "\n";
    return 0;
}

int cmd_doctor(const std::string& config_path, const std::string& app_home) {
    namespace fs = std::filesystem;
    std::string dir = app_home.empty() ? resolve_home("~/.bridgesessions") : app_home;
    int failures = 0;
    auto pass = [](const std::string& label, const std::string& detail = "") {
        std::cout << "[PASS] " << label;
        if (!detail.empty()) std::cout << ": " << detail;
        std::cout << "\n";
    };
    auto warn = [](const std::string& label, const std::string& detail = "") {
        std::cout << "[WARN] " << label;
        if (!detail.empty()) std::cout << ": " << detail;
        std::cout << "\n";
    };
    auto fail = [&](const std::string& label, const std::string& detail = "") {
        std::cout << "[FAIL] " << label;
        if (!detail.empty()) std::cout << ": " << detail;
        std::cout << "\n";
        ++failures;
    };

    std::cout << "bridgesessions doctor v" << bs::mesh::kBridgeSessionsVersion << "\n";
    if (fs::exists(dir) && fs::is_directory(dir)) pass("dir config", dir);
    else fail("dir config", dir);

    // Identity files live directly under ~/.bridgesessions. Older doctor builds
    // incorrectly checked ~/.bridgesessions/keys and sent operators chasing a
    // false cert failure while the daemon was using the correct files.
    for (const auto& name : {"id_ed25519.pem", "id_ed25519-cert.pem", "id_ed25519.pub"}) {
        fs::path p = fs::path(dir) / name;
        if (fs::exists(p) && fs::is_regular_file(p)) pass(name, p.string());
        else fail(name, p.string());
    }

    fs::path cfg(config_path);
    if (fs::exists(cfg) && fs::is_regular_file(cfg)) pass("config", cfg.string());
    else warn("config", cfg.string());

    for (const auto& name : {"logs", "state"}) {
        fs::path p = fs::path(dir) / name;
        if (fs::exists(p) && fs::is_directory(p)) pass(std::string("dir ") + name, p.string());
        else warn(std::string("dir ") + name, p.string());
    }

#if defined(__APPLE__)
    {
        fs::path plist = fs::path(resolve_home("~/Library/LaunchAgents")) /
                         "com.bridgesessions.mesh.plist";
        if (fs::exists(plist) && fs::is_regular_file(plist))
            pass("launchd agent", plist.string());
        else
            warn("launchd agent", plist.string() + " (missing — mesh may not auto-start)");
        // Loaded?
        int lc = std::system("launchctl print gui/$(id -u)/com.bridgesessions.mesh >/dev/null 2>&1");
        if (lc == 0) pass("launchd loaded", "com.bridgesessions.mesh");
        else warn("launchd loaded", "com.bridgesessions.mesh not loaded");
    }
#elif defined(_WIN32)
    // Optional: scheduled task presence is environment-specific — skip hard fail.
    warn("windows service", "check Task Scheduler / service for bridgesessions mesh");
#else
    {
        fs::path unit = fs::path(resolve_home("~/.config/systemd/user")) /
                        "bridgesessions.service";
        if (fs::exists(unit) && fs::is_regular_file(unit))
            pass("systemd unit", unit.string());
        else
            warn("systemd unit", unit.string());
    }
#endif

    std::string ipc = daemon_simple_ipc("HEALTH __doctor_nonexistent__", 1500, app_home);
    if (!ipc.empty()) pass("daemon IPC", "port 19980 answered");
    else warn("daemon IPC", "port 19980 did not answer");

    // ── Supervisor deep checks (26.09.09) ───────────────────────
    // A masked/failed unit, a disabled LaunchAgent, or a stale scheduled task
    // leaves the daemon down after reboot/upgrade — surface it here instead of
    // as a mystery outage.
#if defined(__linux__)
    {
        int masked = std::system(
            "systemctl --user is-enabled bridgesessions.service 2>/dev/null | grep -q masked");
        if (masked == 0) {
            fail("systemd unit masked",
                 "unit was masked (usually a past upgrade) — fix: systemctl --user unmask "
                 "bridgesessions && systemctl --user enable --now bridgesessions");
        } else {
            pass("systemd unit not masked", "");
        }
        int failed = std::system(
            "systemctl --user is-failed bridgesessions.service 2>/dev/null | grep -q failed");
        if (failed == 0) {
            warn("systemd unit failed",
                 "unit in failed state — inspect: journalctl --user -u bridgesessions");
        }
    }
#elif defined(_WIN32)
    {
        int task = std::system("schtasks /query /tn \"BridgeSessions\" >nul 2>&1");
        if (task == 0) pass("scheduled task", "BridgeSessions present");
        else warn("scheduled task", "BridgeSessions task not found (daemon will not auto-start)");
    }
#endif

    // ── Duplicate binary check (26.09.09) ───────────────────────
    // macOS nodes historically drift: LaunchAgent runs the .app copy while the
    // operator upgrades ~/.local/bin. Two different bridgesessions versions on
    // one node = confusing "upgraded but still old" reports.
    {
        namespace fs2 = std::filesystem;
        std::vector<fs2::path> candidates;
#ifdef __APPLE__
        candidates.push_back(fs2::path(resolve_home("~/Applications/BridgeSessions.app/"
            "Contents/MacOS/bridgesessions")));
        candidates.push_back(fs2::path(resolve_home("/Applications/BridgeSessions.app/"
            "Contents/MacOS/bridgesessions")));
#endif
        candidates.push_back(fs2::path(resolve_home("~/.local/bin/bridgesessions")));
        std::string self_path = current_exe_path(nullptr);
        for (const auto& c : candidates) {
            std::error_code ec;
            if (!fs2::exists(c, ec) || ec) continue;
            auto self_canon = fs2::weakly_canonical(fs2::path(self_path), ec);
            auto cand_canon = fs2::weakly_canonical(c, ec);
            if (!ec && self_canon == cand_canon) continue;  // this binary
            // Different file present: report its version if runnable.
            std::string cmd = "\"" + c.string() + "\" --version 2>/dev/null";
            std::string v;
#ifdef _WIN32
            FILE* p = _popen(cmd.c_str(), "r");
#else
            FILE* p = popen(cmd.c_str(), "r");
#endif
            if (p) {
                char buf[64]{};
                if (fgets(buf, sizeof(buf), p)) v = buf;
#ifdef _WIN32
                _pclose(p);
#else
                pclose(p);
#endif
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
            }
            if (!v.empty() && v != std::string(bs::mesh::kBridgeSessionsVersion)) {
                warn("duplicate binary",
                    c.string() + " is " + v + " (this binary is " +
                    std::string(bs::mesh::kBridgeSessionsVersion) + ")");
            }
        }
    }

    // CUA helper (user-session input/capture)
    {
        std::string home = app_home.empty() ? resolve_home("~/.bridgesessions") : app_home;
        if (bs::mesh::cua_helper_reachable(home))
            pass("cua helper", "reachable");
        else
            warn("cua helper", "not running (bs cua needs --cua-helper in user session)");
    }

    // ── Display self-check (2.0.8 P2) ──────────────────────────────────
    {
#ifdef _WIN32
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
                std::ostringstream oss;
                oss << csbi.dwSize.X << "x" << csbi.dwSize.Y
                    << " (window " << (csbi.srWindow.Right - csbi.srWindow.Left + 1)
                    << "x" << (csbi.srWindow.Bottom - csbi.srWindow.Top + 1) << ")";
                pass("display size", oss.str());
            } else {
                warn("display size", "GetConsoleScreenBufferInfo failed");
            }
            // Glyph sample — print known characters to verify rendering
            std::cout << "  display glyphs: CJK(日本語) emoji(🦀✓) box(┌─┐)\n";
        } else {
            warn("display size", "no console handle");
        }
#else
        // POSIX: probe TIOCGWINSZ on stdout
        struct winsize wsz{};
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz) == 0 && wsz.ws_col > 0) {
            std::ostringstream oss;
            oss << wsz.ws_col << "x" << wsz.ws_row;
            pass("display size", oss.str());
        } else {
            // Try stderr as fallback
            if (::ioctl(STDERR_FILENO, TIOCGWINSZ, &wsz) == 0 && wsz.ws_col > 0) {
                std::ostringstream oss;
                oss << wsz.ws_col << "x" << wsz.ws_row << " (stderr)";
                pass("display size", oss.str());
            } else {
                warn("display size", "no TTY — no terminal size available");
            }
        }
        // Glyph sample
        std::cout << "  display glyphs: CJK(日本語) emoji(🦀✓) box(┌─┐)\n";
#endif
    }

    return failures == 0 ? 0 : 1;
}

// ── doctor --gather (26.09.09) ────────────────────────────────────
// One-command diagnostics bundle for bug reports: versions, config (secrets
// stripped), fleet snapshot, recent events, and log tails — all redacted of
// fleet hostnames/IPs is NOT attempted at gather time (operators redact when
// sharing); instead only local files are included and the output path is
// printed. Returns a tar.gz path on success.
int cmd_doctor_gather(const std::string& config_path, const std::string& app_home) {
    namespace fs = std::filesystem;
    std::string dir = app_home.empty() ? resolve_home("~/.bridgesessions") : app_home;

    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char ts[32]{};
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    fs::path out(fs::path(dir) / ("diagnostics-" + std::string(ts) + ".txt"));

    std::ofstream f(out);
    if (!f) {
        std::cerr << "error: cannot write " << out.string() << "\n";
        return 1;
    }
    auto section = [&](const std::string& title) {
        f << "\n==== " << title << " ====\n";
    };

    section("versions");
    f << "cli: " << bs::mesh::kBridgeSessionsVersion << "\n";
    std::string daemon_info = daemon_simple_ipc("API_DAEMON", 1500, app_home);
    if (!daemon_info.empty() && daemon_info.rfind("ERROR", 0) != 0)
        f << "daemon: " << daemon_info << "\n";
    else
        f << "daemon: not reachable\n";

    section("fleet");
    std::string fleet = daemon_simple_ipc("FLEET", 3000, app_home);
    f << (fleet.empty() ? "(daemon not reachable)" : fleet) << "\n";

    section("events (recent, from daemon ring)");
    std::string events = daemon_simple_ipc("API_EVENTS 0", 1500, app_home);
    f << (events.empty() ? "(daemon not reachable)" : events) << "\n";

    section("config (secrets redacted)");
    {
        std::ifstream cf(config_path);
        std::string line;
        while (std::getline(cf, line)) {
            // Redact seed/discovered pubkeys and any token-looking values.
            if (line.rfind("seed ", 0) == 0 || line.rfind("discovered ", 0) == 0) {
                auto pk = line.find("pubkey=");
                if (pk != std::string::npos) line = line.substr(0, pk) + "pubkey=<redacted>";
            }
            f << line << "\n";
        }
    }

    section("log tails (last 200 lines each)");
    for (const auto& name : {"bs-mesh.log", "daemon.log"}) {
        fs::path lp = fs::path(dir) / name;
        f << "-- " << name << " --\n";
        std::ifstream lf(lp, std::ios::binary);
        if (!lf) { f << "(missing)\n"; continue; }
        std::deque<std::string> tail;
        std::string line;
        while (std::getline(lf, line)) {
            tail.push_back(line);
            if (tail.size() > 200) tail.pop_front();
        }
        for (auto& t : tail) f << t << "\n";
    }

    f.close();
    std::cout << "diagnostics bundle written: " << out.string() << "\n"
              << "Review for private data (hostnames, IPs, paths) before sharing.\n";
    return 0;
}

// ── connect: interactive server → harness selector ────────────────
// `bs connect` (and bare `bs` from a terminal) prompts for a peer,
// then for an agent harness, then opens an interactive shell on that
// peer running the harness launch command (e.g. `hermes --tui --yolo`).
// Harness commands come from `harness.<name> <cmd>` config lines,
// falling back to built-in defaults for known harnesses.
struct ConnectHarness { std::string name; std::string cmd; };

std::vector<ConnectHarness> default_harness_table() {
    return {
        {"hermes",      "hermes --tui --yolo"},
        {"claude-code", "claude"},
        {"codex",       "codex"},
        {"opencode",    "opencode"},
        {"grok",        "grok"},
        {"copilot",     "copilot"},
        {"cursor",      "cursor"},
        {"shell",       ""},
    };
}

std::string connect_harness_command(
    const bs::mesh::MeshConfig& cfg, const std::string& name) {
    auto it = cfg.harness_commands.find(name);
    if (it != cfg.harness_commands.end()) return it->second;
    for (auto& h : default_harness_table())
        if (h.name == name) return h.cmd;
    return "";
}

std::vector<std::string> connect_harness_names(
    const bs::mesh::MeshConfig& cfg) {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    for (auto& h : default_harness_table()) {
        names.push_back(h.name);
        seen.insert(h.name);
    }
    // Config-defined harnesses append (and may shadow via command lookup).
    for (auto& [name, _cmd] : cfg.harness_commands) {
        if (!seen.count(name)) { names.push_back(name); seen.insert(name); }
    }
    return names;
}

// Build the peer menu rows (name + status) like `bs peers list` does:
// FLEET IPC snapshot preferred, config seeds/discovered as fallback.
struct ConnectPeerRow { std::string name; std::string status; std::string addr; };

std::vector<ConnectPeerRow> connect_peer_rows(
    const bs::mesh::MeshConfig& cfg, const std::string& home_dir) {
    std::vector<ConnectPeerRow> rows;
    std::unordered_set<std::string> seen;
    std::string fleet_ipc = daemon_simple_ipc("FLEET", 3000, home_dir);
    if (!fleet_ipc.empty() && fleet_ipc.rfind("ERROR", 0) != 0) {
        try {
            auto j = nlohmann::json::parse(fleet_ipc);
            for (auto& [key, val] : j.items()) {
                ConnectPeerRow r;
                r.name = key;
                r.addr = val.value("addr", "");
                r.status = val.value("status", "");
                rows.push_back(std::move(r));
                seen.insert(key);
            }
        } catch (...) { rows.clear(); seen.clear(); }
    }
    for (auto& s : cfg.seeds) {
        if (seen.count(s.name)) continue;
        rows.push_back({s.name, "offline (config)", s.addr});
        seen.insert(s.name);
    }
    for (auto& d : cfg.discovered) {
        if (seen.count(d.name)) continue;
        rows.push_back({d.name, "discovered", d.addr});
        seen.insert(d.name);
    }
    std::sort(rows.begin(), rows.end(),
              [](const ConnectPeerRow& a, const ConnectPeerRow& b) {
                  return a.name < b.name;
              });
    return rows;
}

// Read a menu choice (1..count) from stdin. Returns 0 on invalid/EOF.
int connect_menu_choice(size_t count) {
    std::string line;
    if (!std::getline(std::cin, line)) return 0;
    try {
        int n = std::stoi(line);
        if (n >= 1 && static_cast<size_t>(n) <= count) return n;
    } catch (...) {}
    return 0;
}

// ── Arrow-key menu (interactive TTY) ──────────────────────────────
// With raw/VT console modes active, arrow keys arrive as ESC [ A / B byte
// sequences on both POSIX termios and Windows (ENABLE_VIRTUAL_TERMINAL_INPUT),
// so one parser serves both.

bool stdout_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDOUT_FILENO) != 0;
#endif
}

// Read one input byte. Returns -1 on EOF/error.
int menu_read_byte() {
#ifdef _WIN32
    char c = 0;
    DWORD n = 0;
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), &c, 1, &n, nullptr) || n == 0)
        return -1;
    return static_cast<unsigned char>(c);
#else
    unsigned char c = 0;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    return n == 1 ? static_cast<int>(c) : -1;
#endif
}

// Read one byte with a deadline (ESC-sequence disambiguation). -2 on timeout.
int menu_read_byte_timeout(int ms) {
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (WaitForSingleObject(hIn, static_cast<DWORD>(ms)) != WAIT_OBJECT_0) return -2;
    return menu_read_byte();
#else
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    if (select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv) <= 0) return -2;
    return menu_read_byte();
#endif
}

// RAII: raw input mode + hidden cursor while the menu is up.
struct MenuTermGuard {
    bs::mesh::SavedConsole saved;
    explicit MenuTermGuard() : saved(bs::mesh::enable_raw_mode(true)) {
        std::cout << "\x1b[?25l" << std::flush;
    }
    ~MenuTermGuard() {
        std::cout << "\x1b[?25h";
        std::cout.flush();
        bs::mesh::restore_terminal(saved);
    }
};

// Interactive ↑/↓ + Enter selector with charm-style frame rendering.
// Returns 1-based choice, 0 on cancel (Esc alone, q, Ctrl-C) or EOF.
size_t arrow_menu_select(const std::vector<std::string>& rows,
                         const std::string& title = {}) {
    if (rows.empty()) return 0;
    MenuTermGuard guard;
    const size_t n = rows.size();
    size_t sel = 0;
    // Frame geometry: box lines + one footer line. Rows are padded/truncated
    // to a fixed width by bs::tui::menu_frame so redraws never reflow.
    size_t width = 0;
    for (auto& r : rows)
        width = std::max(width, bs::tui::tui_row_width(r));
    width = std::clamp(width + 2, size_t{24}, size_t{72});
    const size_t frame_lines = n + 4;  // top, title, separator, rows, bottom
    const std::string footer =
        bs::tui::menu_footer("  ↑/↓ or j/k move · Enter select · q quit");
    auto draw = [&](bool first) {
        // Raw mode is on (OPOST off): every newline must be \r\n or the cursor
        // keeps the previous line's column and the frame shreds diagonally.
        if (first)
            std::cout << bs::tui::menu_frame({title.empty() ? " " : title, true},
                                             rows, sel, width) << "\r\n"
                      << footer << std::flush;
        else {
            // Rewind to the top of the frame and repaint it in place.
            // The \r matters: the previous frame ended on the footer with no
            // trailing newline, so the cursor sits at the footer's end column.
            // Cursor-up alone preserves that column and frame 2 would print
            // mid-line, wrap, and trash the whole menu (observed in a real
            // PTY capture 2026-09-08).
            std::cout << "\x1b[" << frame_lines << "A\r"
                      << bs::tui::menu_frame({title.empty() ? " " : title, true},
                                             rows, sel, width) << "\r\n"
                      << footer << std::flush;
        }
    };
    draw(true);
    for (;;) {
        int c = menu_read_byte();
        if (c < 0) return 0;                              // EOF
        if (c == 3 || c == 'q' || c == 'Q') return 0;     // Ctrl-C / q = cancel
        if (c == '\r' || c == '\n') return sel + 1;       // Enter
        if (c == 'k' && sel > 0) { --sel; draw(false); continue; }
        if (c == 'j' && sel + 1 < n) { ++sel; draw(false); continue; }
        if (c == 0x1b) {                                  // ESC: arrow or cancel
            int c2 = menu_read_byte_timeout(80);
            if (c2 == -2 || c2 == 0x1b) return 0;         // bare Esc = cancel
            if (c2 != '[' && c2 != 'O') continue;
            int c3 = menu_read_byte_timeout(80);
            if (c3 == 'A' && sel > 0) { --sel; draw(false); }
            else if (c3 == 'B' && sel + 1 < n) { ++sel; draw(false); }
            else if (c3 == 'H') { sel = 0; draw(false); }
            else if (c3 == 'F') { sel = n - 1; draw(false); }
        }
    }
}

// Unified menu entry: arrow keys on a real terminal, numbered prompt when
// stdin/stdout is piped (scripts, e2e). Returns 1-based choice, 0 cancel.
int connect_menu_pick(const std::string& title,
                      const std::vector<std::string>& row_labels) {
    if (bs::mesh::stdin_is_terminal() && stdout_is_terminal()) {
        std::cout << "\n" << title << "  (↑/↓ move, Enter select, q cancel)\n";
        return static_cast<int>(arrow_menu_select(row_labels));
    }
    std::cout << "\n" << title << "\n";
    for (size_t i = 0; i < row_labels.size(); ++i)
        std::cout << "  " << (i + 1) << ") " << row_labels[i] << "\n";
    std::cout << "> " << std::flush;
    return connect_menu_choice(row_labels.size());
}

int cmd_connect_selector(const std::string& config_path,
                         const std::string& home_dir,
                         const std::string& peer_hint,
                         const std::string& harness_hint) {
    bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
    bs::mesh::bootstrap_identity(home_dir);

    auto peers = connect_peer_rows(cfg, home_dir);
    // Never offer this node as its own connect target.
    peers.erase(std::remove_if(peers.begin(), peers.end(),
        [&](const ConnectPeerRow& r) { return bs::mesh::is_self_target(cfg, r.name); }),
        peers.end());
    if (peers.empty()) {
        std::cerr << "No peers configured. Add a seed with `bs peers add` or `bs join`.\n";
        return 2;
    }

    // ── 1. Server selection ─────────────────────────────────────
    std::string peer = peer_hint;
    if (peer.empty()) {
        std::vector<std::string> rows;
        rows.reserve(peers.size());
        for (auto& p : peers)
            rows.push_back(p.name + (p.status.empty() ? "" : "  [" + p.status + "]"));
        int choice = connect_menu_pick("BridgeSessions — choose a server:", rows);
        if (choice <= 0) { std::cerr << "Cancelled.\n"; return 2; }
        peer = peers[static_cast<size_t>(choice - 1)].name;
    } else {
        if (bs::mesh::is_self_target(cfg, peer)) {
            std::cerr << "Cannot connect to yourself. This is "
                      << bs::mesh::self_display_name(cfg) << ".\n";
            return 2;
        }
        bool found = false;
        for (auto& p : peers)
            if (p.name == peer) { found = true; break; }
        if (!found) {
            std::cerr << "Unknown peer: " << peer << "\n";
            return 2;
        }
    }

    // ── 2. Harness selection ────────────────────────────────────
    auto harnesses = connect_harness_names(cfg);
    std::string harness = harness_hint;
    if (harness.empty()) {
        std::vector<std::string> rows;
        rows.reserve(harnesses.size());
        for (auto& h : harnesses) {
            std::string hc = connect_harness_command(cfg, h);
            rows.push_back(h + (hc.empty() ? "" : "  → " + hc));
        }
        int choice = connect_menu_pick(peer + " — choose a harness:", rows);
        if (choice <= 0) { std::cerr << "Cancelled.\n"; return 2; }
        harness = harnesses[static_cast<size_t>(choice - 1)];
    }

    std::string cmd = connect_harness_command(cfg, harness);
    if (cmd.empty() && harness != "shell") {
        std::cerr << "No launch command for harness '" << harness
                  << "'. Add `harness." << harness << " <command>` to the config.\n";
        return 2;
    }

    // ── 3. Session selection (best-effort) ──────────────────────
    // Offer attaching to an existing session when the peer has live ones.
    // Never hard-fails: an unqueryable peer keeps the harness-name flow.
    bs::mesh::MeshConfig cfg_for_connect = cfg;
    bs::mesh::MeshController mc_probe(cfg_for_connect, home_dir);
    std::string session = harness;  // default: reuse harness name for reattach
    {
        auto listed = mc_probe.fetch_peer_sessions(peer);
        if (listed && !listed->sessions.empty()) {
            int choice = connect_menu_pick(peer + " — attach or start:",
                                           bs::tui::session_picker_rows(*listed));
            if (choice > 0) {
                std::string picked = bs::tui::session_picker_choice(
                    *listed, static_cast<size_t>(choice));
                if (!picked.empty()) session = picked;
            }
        }
    }

    // ── 4. Open the shell ───────────────────────────────────────
    auto [cols, rows] = bs::mesh::get_winsize();
    std::cout << "\nConnecting to " << peer << " → " << harness
              << (cmd.empty() ? "" : " (" + cmd + ")") << "\n\n";
    bs::mesh::MeshController mc(cfg, home_dir);
    // Reuse the harness name as the session name for easy reattach:
    // `bs shell <peer> -n <harness>`.
    // force_interactive: harness commands are full-screen TUIs — they need the
    // raw-terminal path; the plain -x path strips ANSI and scrambles them.
    return mc.shell_peer(peer, session, cmd, cols, rows, "xterm-256color",
                         true, "", /*force_interactive=*/true);
}

} // anonymous namespace

// 26.09.09: no uncaught exceptions escape to the user as raw aborts. A peer
// name that fails DNS used to kill the CLI with a std::runtime_error and a
// spilled stack instead of a one-line message + exit code.
int bridgesessions_main(int argc, char** argv);

int main(int argc, char** argv) {
    try {
        return bridgesessions_main(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n"
                  << "  → run `bs doctor` to check local configuration.\n";
        return 1;
    } catch (...) {
        std::cerr << "error: unexpected internal failure\n"
                  << "  → run `bs doctor` to check local configuration.\n";
        return 1;
    }
}

int bridgesessions_main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    // P3: seed PRNG from /dev/urandom (or random_device) — used by jitter, temp names
    {
        std::random_device rd;
        srand(rd());
    }

    CLI::App app{"Bridge Sessions — mesh terminal relay"};
    app.set_version_flag("--version,-V", std::string(bs::mesh::kBridgeSessionsVersion));

    // Global options
    std::string config_path = "";
    std::string config_dir = "";
    bool daemon_flag = false;
    bool cua_helper_flag = false;
    app.add_option("--config", config_path, "Config file path (default: ~/.bridgesessions/config)");
    app.add_option("--config-dir", config_dir, "Config directory (default: ~/.bridgesessions)");
    app.add_flag("--daemon", daemon_flag, "Detach from terminal (daemonize)");
    app.add_flag("--cua-helper", cua_helper_flag, "Run CUA helper server (screen capture + input injection in user session)");

#ifndef _WIN32
    // Internal worker mode: `bridgesessions session-worker --socket …` is
    // exec'd by the daemon per hosted session (see bs-session-worker.h).
    // Hidden from help — not a user-facing command.
    std::string worker_socket, worker_name, worker_command, worker_term = "xterm-256color", worker_app_home;
    int worker_cols = 80, worker_rows = 24;
    auto* session_worker_app = app.add_subcommand("session-worker", "internal: per-session PTY worker");
    session_worker_app->add_option("--socket", worker_socket)->required();
    session_worker_app->add_option("--name", worker_name)->required();
    session_worker_app->add_option("--command", worker_command);
    session_worker_app->add_option("--cols", worker_cols);
    session_worker_app->add_option("--rows", worker_rows);
    session_worker_app->add_option("--term", worker_term);
    session_worker_app->add_option("--app-home", worker_app_home);
#endif

    // Fast path: `bs dev hermes` (the `bs` executable is a symlink to this binary).
    // Unknown peer names are resolved through `ssh -G` for address discovery only;
    // terminal data still travels exclusively over the BridgeSessions protocol.
    std::string quick_peer, quick_session;
    bool quick_select = false;
    bool quick_list = false;
    app.add_option("PEER", quick_peer, "Peer name or SSH Host alias");
    app.add_option("SESSION", quick_session,
                   "Session name (omit to start a new session; give a name to reattach)");
    app.add_flag("-s,--select", quick_select,
                 "Interactively pick a session to attach (or start a new one)");
    app.add_flag("-l,--list", quick_list,
                 "List sessions on PEER (or local sessions when PEER is omitted) and exit");

    // Subcommand: shell
    std::string shell_peer, shell_session = "default", shell_cmd;
    uint16_t shell_cols = 80, shell_rows = 24;
    bool shell_detach = false, shell_wait = false;
    bool shell_interactive = false;
    bool shell_select = false;
    bool shell_record = false, shell_signal_forward = true;
    std::string shell_signal_on_detach;
    auto* shell_cmd_app = app.add_subcommand("shell", "Open shell on a peer");
    shell_cmd_app->add_option("peer", shell_peer, "Peer name")->required();
    shell_cmd_app->add_option("-n,--name", shell_session, "Session name");
    shell_cmd_app->add_flag("-s,--select", shell_select,
                            "Interactively pick a session to attach");
    shell_cmd_app->add_option("-x,--cmd", shell_cmd, "Command override");
    auto* shell_cols_opt = shell_cmd_app->add_option("--cols", shell_cols, "Terminal columns");
    auto* shell_rows_opt = shell_cmd_app->add_option("--rows", shell_rows, "Terminal rows");
    shell_cmd_app->add_flag("-r,--record", shell_record, "Record session output to file");
    shell_cmd_app->add_flag("--signal-forward{true}", shell_signal_forward, "Forward Ctrl-C to remote child (default: on)")->default_str("true");
    shell_cmd_app->add_option("--signal-on-detach", shell_signal_on_detach, "Send HUP/TERM/INT/QUIT/KILL to the child when the last peer detaches (default: none)")->check(CLI::IsMember({"HUP","TERM","INT","QUIT","KILL"}));
    shell_cmd_app->add_flag("--detach", shell_detach, "Send command and return immediately (session runs on peer)");
    shell_cmd_app->add_flag("--wait", shell_wait, "Block until the named session exits, then return its exit code");
    shell_cmd_app->add_flag("-i,--interactive", shell_interactive,
        "Run the -x command as a full interactive terminal session (raw mode, "
        "no ANSI stripping) — required for TUI apps like hermes --tui");

    // Subcommand: connect — interactive server → harness selector
    std::string connect_peer, connect_harness;
    auto* connect_cmd_app = app.add_subcommand(
        "connect", "Pick a server, then a harness, and open a shell on it");
    connect_cmd_app->add_option("--peer", connect_peer, "Skip the server menu (name)");
    connect_cmd_app->add_option("--harness", connect_harness, "Skip the harness menu (name)");

    // Subcommand: sessions
    std::string sessions_peer;
    bool sessions_all = false;
    bool sessions_json = false;
    auto* sessions_cmd_app = app.add_subcommand("sessions", "List sessions");
    sessions_cmd_app->add_option("peer", sessions_peer, "Peer name (omit for local)");
    sessions_cmd_app->add_flag("--all", sessions_all, "All peers");
    sessions_cmd_app->add_flag("--json", sessions_json, "Output as JSON");

    // Subcommand: keygen
    auto* keygen_cmd_app = app.add_subcommand("keygen", "Generate ed25519 keypair");

    // Subcommand: authorize
    std::string auth_pubkey;
    auto* auth_cmd_app = app.add_subcommand("authorize", "Authorize a peer public key");
    auth_cmd_app->add_option("pubkey", auth_pubkey, "Hex pubkey")->required();

    // Subcommand: doctor
    auto* doctor_cmd_app = app.add_subcommand("doctor", "Check local bridgesessions configuration");
    bool doctor_gather = false;
    doctor_cmd_app->add_flag("--gather", doctor_gather,
                             "Collect a redacted diagnostics bundle (logs, versions, fleet, config sans secrets)");

    // 26.09.09: identity rotation without hand-editing configs.
    bool rotate_identity_yes = false;
    auto* rotate_id_cmd = app.add_subcommand("rotate-identity",
        "Regenerate this node's identity keys (peers must re-pin the new pubkey)");
    rotate_id_cmd->add_flag("--yes", rotate_identity_yes, "Confirm without prompt");

    // Subcommand: peers
    auto* peers_cmd = app.add_subcommand("peers", "Manage peers");
    peers_cmd->require_subcommand(1);

    auto* peers_list = peers_cmd->add_subcommand("list", "List peers");
    std::string peer_add_name, peer_add_addr, peer_add_pubkey;
    auto* peers_add = peers_cmd->add_subcommand("add", "Add a seed peer");
    peers_add->add_option("name", peer_add_name)->required();
    peers_add->add_option("addr", peer_add_addr)->required();
    peers_add->add_option("--pubkey", peer_add_pubkey,
                          "Peer ed25519 pubkey (required when mesh.require_seed_pins=true)");
    std::string peer_remove_name;
    auto* peers_remove = peers_cmd->add_subcommand("remove", "Remove a peer");
    peers_remove->add_option("name", peer_remove_name)->required();
    // 26.09.09: rotate-pin — update a peer's pinned pubkey in the config
    // (after that peer ran rotate-identity). Replaces YAML/awk hand-editing.
    std::string rotate_pin_peer, rotate_pin_pubkey;
    auto* peers_rotate_pin = peers_cmd->add_subcommand("rotate-pin",
        "Update a peer's pinned pubkey (after the peer rotated its identity)");
    peers_rotate_pin->add_option("name", rotate_pin_peer, "Peer name")->required();
    peers_rotate_pin->add_option("pubkey", rotate_pin_pubkey,
                                 "New ed25519 pubkey (64 hex)")->required();
    // health
    std::string health_peer;
    bool health_latency = false;
    auto* health_cmd_app = app.add_subcommand("health", "Ping/pong health check against a peer");
    health_cmd_app->add_option("peer", health_peer, "Peer name")->required();
    health_cmd_app->add_flag("--latency", health_latency,
                             "Report TCP connect RTT + data-plane probe RTT");
    // reconnect
    std::string reconnect_peer;
    auto* reconnect_cmd_app = app.add_subcommand("reconnect", "Tear down and re-handshake one peer via the running daemon");
    reconnect_cmd_app->add_option("peer", reconnect_peer, "Peer name")->required();
    // invite
    auto* invite_cmd_app = app.add_subcommand("invite", "Generate an invite token for new nodes");
    // enroll — bootstrap a new member's key mesh-wide via a signed directory entry
    std::string enroll_name, enroll_pubkey, enroll_addr;
    auto* enroll_cmd_app = app.add_subcommand(
        "enroll", "Vouch for a new mesh member (sign + gossip its key to all peers)");
    enroll_cmd_app->add_option("name", enroll_name, "New member node name")->required();
    enroll_cmd_app->add_option("pubkey", enroll_pubkey, "New member ed25519 pubkey (64 hex)")->required();
    enroll_cmd_app->add_option("addr", enroll_addr, "New member addr host:port")->required();
    // join
    std::string join_addr;
    std::string join_token;
    std::string join_token_file;
    bool join_start = false;
    std::string join_node_name;
    auto* join_cmd_app = app.add_subcommand("join", "Join a mesh via invite token");
    join_cmd_app->add_option("addr", join_addr, "Host:port")->required();
    join_cmd_app->add_option("token", join_token,
                             "Invite token ('-' reads stdin; argv form is less private)");
    join_cmd_app->add_option("--token-file", join_token_file,
                             "Read invite token from file (recommended)");
    join_cmd_app->add_flag("--start", join_start, "Start daemon after joining");
    join_cmd_app->add_option("--node-name", join_node_name, "Node name (default: assigned by host)");
    // image
    std::string image_file;
    auto* image_cmd_app = app.add_subcommand("image", "Preview an image in the terminal");
    image_cmd_app->add_option("file", image_file, "Image file path")->required();
    // anim
    std::string anim_file;
    auto* anim_cmd_app = app.add_subcommand("anim", "Preview an animated GIF in the terminal");
    anim_cmd_app->add_option("file", anim_file, "GIF file path")->required();
    // stats
    auto* stats_cmd_app = app.add_subcommand("stats", "Show connection and session statistics");

    // fleet
    bool fleet_json = false;
    auto* fleet_cmd_app = app.add_subcommand(
        "fleet", "Show live fleet directory (name, addr, version, status, cpu/mem/disk)");
    fleet_cmd_app->add_flag("--json", fleet_json, "Output raw JSON from daemon");

    // upgrade
    bool upgrade_all = false;
    std::string upgrade_tag;
    auto* upgrade_cmd_app = app.add_subcommand("upgrade", "Self-update to latest (or specified) release from GitHub");
    bool allow_downgrade = false;
    upgrade_cmd_app->add_flag("--allow-downgrade", allow_downgrade, "Permit moving to an OLDER version than the running one");
    upgrade_cmd_app->add_flag("--all", upgrade_all, "Upgrade all healthy peers via mesh shell");
    upgrade_cmd_app->add_option("--tag", upgrade_tag, "Specific version tag (default: latest)");

    // telemetry
    bool telemetry_json = false;
    auto* telemetry_cmd_app = app.add_subcommand("telemetry", "Show transfer telemetry");
    telemetry_cmd_app->add_flag("--json", telemetry_json, "Output as JSON");

    // file
    auto* file_cmd = app.add_subcommand("file", "File transfer operations");
    file_cmd->require_subcommand(1);
    std::string file_send_peer, file_send_path, file_send_dest;
    bool file_send_wait = false;
    auto* file_send_app = file_cmd->add_subcommand(
        "send", "Send file to a peer (optional remote dest — scp-style)");
    file_send_app->add_option("peer", file_send_peer, "Peer name")->required();
    file_send_app->add_option("local", file_send_path, "Local file path")->required();
    file_send_app->add_option("remote", file_send_dest,
                              "Optional remote dest (relative under receive_dir). "
                              "Home/tmp dest requires peer file.dest_allow_home");
    file_send_app->add_option("--dest", file_send_dest,
                              "Remote destination path (alias of positional remote)");
    file_send_app->add_flag("--wait", file_send_wait, "Block until the peer acknowledges transfer completion");
    std::string file_recv_peer, file_recv_remote, file_recv_local, file_recv_to;
    bool file_recv_wait = false;
    auto* file_recv_app = file_cmd->add_subcommand("recv", "Receive file from a peer (run on target node)");
    file_recv_app->add_option("peer", file_recv_peer, "Peer name")->required();
    file_recv_app->add_option("remote", file_recv_remote, "Remote file path")->required();
    file_recv_app->add_option("local", file_recv_local, "Local directory (default: .)");
    file_recv_app->add_option("--to", file_recv_to, "Local destination path or directory");
    file_recv_app->add_flag("--wait", file_recv_wait, "Block until the transfer completes or fails");

    // 2.0.12: video capture
    std::string capvid_peer;
    int capvid_fps = 2, capvid_dur = 15, capvid_quality = 70, capvid_maxw = 1280;
    auto* capvid_cmd = app.add_subcommand("capture-video", "Record remote screen to video, transfer back via file recv");
    capvid_cmd->add_option("peer", capvid_peer, "Peer name")->required();
    capvid_cmd->add_option("--fps", capvid_fps, "Frames per second (default 2)");
    capvid_cmd->add_option("--duration", capvid_dur, "Duration in seconds (default 15)");
    capvid_cmd->add_option("--quality", capvid_quality, "Quality 1-100 (default 70)");
    capvid_cmd->add_option("--max-width", capvid_maxw, "Max width, 0=native (default 1280)");

    // 2.0.20: bs cua — computer-use automation
    std::string cua_peer;
    int cua_x = 0, cua_y = 0;
    std::string cua_button = "left", cua_text, cua_direction = "up", cua_modifiers, cua_output;
    int cua_code = 0, cua_amount = 3, cua_format = 1, cua_quality = 80;
    auto* cua_cmd = app.add_subcommand("cua", "Computer-use automation on a remote peer");
    cua_cmd->require_subcommand(1);
    auto* cua_screen = cua_cmd->add_subcommand("screen", "Get remote screen dimensions");
    cua_screen->add_option("peer", cua_peer, "Peer name")->required();
    auto* cua_capture = cua_cmd->add_subcommand("capture", "Capture screenshot from peer");
    cua_capture->add_option("peer", cua_peer, "Peer name")->required();
    cua_capture->add_option("--format", cua_format, "Image format: 1=png (default), 2=jpeg");
    cua_capture->add_option("--quality", cua_quality, "JPEG quality 1-100 (default 80)");
    cua_capture->add_option("--output,-o", cua_output, "Output file (default: stdout)");
    auto* cua_click = cua_cmd->add_subcommand("click", "Click mouse at coordinates");
    cua_click->add_option("peer", cua_peer, "Peer name")->required();
    cua_click->add_option("--x", cua_x, "X coordinate")->required();
    cua_click->add_option("--y", cua_y, "Y coordinate")->required();
    cua_click->add_option("--button", cua_button, "left (default), right, middle");
    auto* cua_move = cua_cmd->add_subcommand("move", "Move mouse to coordinates");
    cua_move->add_option("peer", cua_peer, "Peer name")->required();
    cua_move->add_option("--x", cua_x, "X coordinate")->required();
    cua_move->add_option("--y", cua_y, "Y coordinate")->required();
    auto* cua_type = cua_cmd->add_subcommand("type", "Type text");
    cua_type->add_option("peer", cua_peer, "Peer name")->required();
    cua_type->add_option("--text", cua_text, "Text to type")->required();
    auto* cua_key = cua_cmd->add_subcommand("key", "Press a HID key code");
    cua_key->add_option("peer", cua_peer, "Peer name")->required();
    cua_key->add_option("--code", cua_code, "USB HID usage ID")->required();
    cua_key->add_option("--modifiers", cua_modifiers, "ctrl,shift,alt,meta (comma-separated)");
    auto* cua_scroll = cua_cmd->add_subcommand("scroll", "Scroll mouse wheel");
    cua_scroll->add_option("peer", cua_peer, "Peer name")->required();
    cua_scroll->add_option("--direction", cua_direction, "up (default) or down");
    cua_scroll->add_option("--amount", cua_amount, "Scroll ticks (default 3)");

    // vfolder
    auto* vfolder_cmd = app.add_subcommand("vfolder", "Manage virtual folder sync");
    vfolder_cmd->require_subcommand(1);
    std::string vfolder_name, vfolder_local, vfolder_peer, vfolder_remote, vfolder_dir;
    int vfolder_interval = 30;
    auto* vfolder_add = vfolder_cmd->add_subcommand("add", "Add a virtual folder mapping");
    vfolder_add->add_option("name", vfolder_name, "Mapping name")->required();
    vfolder_add->add_option("local", vfolder_local, "Local path")->required();
    vfolder_add->add_option("peer", vfolder_peer, "Remote peer")->required();
    vfolder_add->add_option("remote", vfolder_remote, "Remote path")->required();
    vfolder_add->add_option("--interval", vfolder_interval, "Sync interval (seconds)");
    vfolder_add->add_option("--dir", vfolder_dir, "Sync direction (push/pull/bidirectional)");
    auto* vfolder_sync = vfolder_cmd->add_subcommand("sync", "Sync a specific folder now");
    vfolder_sync->add_option("name", vfolder_name, "Mapping name")->required();
    auto* vfolder_list = vfolder_cmd->add_subcommand("list", "List active folder mappings");

    // sync pair (roadmap phase 2 — cross-machine folder mirroring)
    auto* sync_cmd = app.add_subcommand("sync", "Cross-machine folder mirroring (bs sync pair …)");
    sync_cmd->require_subcommand(1);
    auto* sync_pair_cmd = sync_cmd->add_subcommand("pair", "Manage explicit sync pairs");
    sync_pair_cmd->require_subcommand(1);
    std::string sp_local, sp_peer_spec, sp_id;
    bool sp_approve = false, sp_via_run_script = false;
    auto* sp_init = sync_pair_cmd->add_subcommand(
        "init", "Create a pair, scan a dry-run manifest (nothing transfers until approved)");
    sp_init->add_option("local-dir", sp_local, "Local directory to mirror")->required();
    sp_init->add_option("peer-dir", sp_peer_spec,
        "Peer and remote dir as peer:/remote/dir")->required();
    sp_init->add_flag("--approve", sp_approve,
        "Approve the dry-run manifest in the same step (first transfer gate)");
    sp_init->add_flag("--via-run-script", sp_via_run_script,
        "Required for Windows peers (files pushed via run-script; no PowerShell bodies)");
    auto* sp_status = sync_pair_cmd->add_subcommand("status", "Show pairs, pending changes, logical clock");
    sp_status->add_option("id", sp_id, "Pair id (default: all pairs)");
    auto* sp_approve_cmd = sync_pair_cmd->add_subcommand("approve", "Approve a pair's pending dry-run manifest");
    sp_approve_cmd->add_option("id", sp_id, "Pair id")->required();
    auto* sp_run = sync_pair_cmd->add_subcommand("run", "Apply the approved manifest (one-shot, resumable)");
    sp_run->add_option("id", sp_id, "Pair id")->required();

    // edit
    std::string edit_target;
    auto* edit_cmd_app = app.add_subcommand("edit", "Edit a file on a remote peer");
    edit_cmd_app->add_option("target", edit_target, "Peer:path (e.g. dev:/etc/nginx.conf)")->required();

    // run-script
    std::string rscript_peer, rscript_file, rscript_interpreter = "auto";
    auto* rscript_cmd_app = app.add_subcommand("run-script", "Send a script file to a peer and execute it");
    rscript_cmd_app->add_option("peer", rscript_peer, "Peer name")->required();
    rscript_cmd_app->add_option("file", rscript_file, "Local script file (use - for stdin)")->required();
    rscript_cmd_app->add_option("--interpreter", rscript_interpreter, "Interpreter: auto|bash|powershell|python")
        ->check(CLI::IsMember({"auto", "bash", "powershell", "pwsh", "python", "python3", "cmd"}));

    // job — multi-step JSON (avoids fragile cmd1 && cmd2 && cmd3 stacks)
    std::string job_peer, job_file;
    bool job_stop_on_error = false;
    auto* job_cmd = app.add_subcommand("job", "Run a multi-step JSON job on a peer (per-step results)");
    job_cmd->require_subcommand(1);
    auto* job_run_app = job_cmd->add_subcommand("run", "Execute job JSON on peer");
    job_run_app->add_option("peer", job_peer, "Peer name")->required();
    job_run_app->add_option("file", job_file, "Job JSON file (use - for stdin)")->required();
    job_run_app->add_flag("--stop-on-error", job_stop_on_error,
                          "Abort remaining steps after the first non-zero exit (default: continue)");

    // script — content-addressed script library
    auto* script_cmd = app.add_subcommand("script", "Manage content-addressed script cache");
    script_cmd->require_subcommand(1);
    // script add <file> [--name alias]
    std::string script_add_file, script_add_name;
    auto* script_add_app = script_cmd->add_subcommand("add", "Add a script to the local cache");
    script_add_app->add_option("file", script_add_file, "Script file path")->required();
    script_add_app->add_option("--name", script_add_name, "Alias name for the script");
    // script list
    auto* script_list_app = script_cmd->add_subcommand("list", "List cached scripts");
    // script push <name> --peer <peer>
    std::string script_push_name, script_push_peer;
    auto* script_push_app = script_cmd->add_subcommand("push", "Push a script to a peer");
    script_push_app->add_option("name", script_push_name, "Script name or hash")->required();
    script_push_app->add_option("--peer", script_push_peer, "Peer name")->required();
    // script run <name> --peer <peer> [-- args...]
    std::string script_run_name, script_run_peer;
    std::vector<std::string> script_run_args;
    auto* script_run_app = script_cmd->add_subcommand("run", "Run a cached script on a peer");
    script_run_app->add_option("name", script_run_name, "Script name or hash")->required();
    script_run_app->add_option("--peer", script_run_peer, "Peer name")->required();
    script_run_app->add_option("args", script_run_args, "Arguments to pass to the script");
    // script remove <name>
    std::string script_remove_name;
    auto* script_remove_app = script_cmd->add_subcommand("remove", "Remove a script from local cache");
    script_remove_app->add_option("name", script_remove_name, "Script name or hash")->required();

    // pane (BridgePanel publish from the mesh CLI)
    std::string pane_session = "default", pane_type = "documents", pane_title, pane_file;
    auto* pane_cmd_app = app.add_subcommand("pane", "Publish a file to the BridgePanel surface");
    pane_cmd_app->require_subcommand(1);
    auto* pane_publish = pane_cmd_app->add_subcommand("publish", "Copy a local file into a BridgePanel session");
    pane_publish->add_option("--session", pane_session, "Session name (default: default)");
    pane_publish->add_option("--type", pane_type, "documents | comms");
    pane_publish->add_option("--title", pane_title, "Display title / filename override");
    pane_publish->add_option("file", pane_file, "Local markdown file to publish")->required();

    // 26.09.09: JSON API passthrough for GUI/mobile clients — talk to the
    // local daemon's machine API without a raw socket client.
    std::string api_verb, api_arg;
    auto* api_cmd = app.add_subcommand("api",
        "Query the daemon JSON API (sessions|peers|daemon|events [since_id])");
    api_cmd->add_option("verb", api_verb, "sessions | peers | daemon | events")->required();
    api_cmd->add_option("arg", api_arg, "Optional argument (events: since_id)");

    // ── Detailed help ─────────────────────────────────────────────
    // Top-level display: richer description + example footer. Per-command:
    // `bs <command> --help` shows a detailed description plus EXAMPLES footer.
    // Descriptions here are the single source of truth for each command's
    // behavior, args, and gotchas (kept in sync with the dispatch code above).
    app.description(
        "Bridge Sessions — persistent mesh terminal relay.\n"
        "\n"
        "Hosts live terminal sessions on a daemon so they survive disconnects,\n"
        "and relays them over mTLS between peers. Attach from anywhere with\n"
        "`bs <peer>`; sessions keep running while you are away.\n"
        "\n"
        "Quick start:\n"
        "  bs --daemon                 Start the local relay daemon\n"
        "  bs <peer>                   Attach to (or start) a session on a peer\n"
        "  bs <peer> -s                Pick a session on a peer interactively\n"
        "  bs <peer> --list            List that peer's sessions\n"
        "  bs connect                  Browse the fleet and pick a harness");
    app.footer(
        "Run `bs <command> --help` for detailed help on any command.\n"
        "Docs: https://github.com/MindDragonLabs/BridgeSessions");
    app.set_help_flag("-h,--help", "Print this help message and exit");

    {
        shell_cmd_app->description(
            "Open an interactive shell (or run a command) on a peer.\n"
            "\n"
            "Sessions live on the PEER's daemon: on transport loss the remote\n"
            "process keeps running and you can reattach later with the same\n"
            "-n/--name. Use -x to run a one-shot command instead of a shell.");
        shell_cmd_app->footer(
            "Examples:\n"
            "  bs shell dev                          Interactive shell on peer 'dev'\n"
            "  bs shell dev -n build                 Named session (reattach later)\n"
            "  bs shell dev -x 'docker ps'           Run a one-shot command\n"
            "  bs shell dev -x htop -i               Interactive TUI command (raw mode)\n"
            "  bs shell dev -x ./long-job --detach   Fire-and-forget; runs on peer\n"
            "  bs shell dev -n build --wait          Block until session exits");
    }
    {
        connect_cmd_app->description(
            "Interactive two-step selector: pick a fleet peer, then pick a\n"
            "launch harness (a command the peer runs to start a session, e.g.\n"
            "a shell or `hermes --tui`). Lists live sessions on the chosen peer\n"
            "and offers attaching to one. Never hard-fails: cancel at any menu\n"
            "exits cleanly with no session started.");
        connect_cmd_app->footer(
            "Examples:\n"
            "  bs connect                    Full menu: peer, then harness\n"
            "  bs connect --peer dev         Skip the peer menu\n"
            "  bs connect --peer dev --harness bash   Skip both menus");
    }
    {
        sessions_cmd_app->description(
            "List sessions on this node (omit PEER) or on a remote PEER.\n"
            "Shows session name, state, uptime, and traffic. Equivalent to the\n"
            "shortcut `bs --list` / `bs <peer> --list`.");
        sessions_cmd_app->footer(
            "Examples:\n"
            "  bs sessions                   Local sessions\n"
            "  bs sessions dev               Sessions on peer 'dev'\n"
            "  bs sessions --json            Machine-readable output");
    }
    {
        keygen_cmd_app->description(
            "Generate this node's ed25519 identity keypair. Done automatically\n"
            "on first daemon start; refuses to overwrite an existing identity\n"
            "(delete the key files first to rotate deliberately).");
    }
    {
        auth_cmd_app->description(
            "Authorize a peer's ed25519 public key for direct connections.\n"
            "Adds the key to the local trust store so the peer can attach and\n"
            "transfer files. Prefer `peers add --pubkey` for seed peers.");
        auth_cmd_app->footer("Example:\n  bs authorize 9a1b...64hex");
    }
    {
        doctor_cmd_app->description(
            "Check local configuration: identity keys, config parse, daemon\n"
            "reachability, listen port, and app-home layout. Run this first\n"
            "when a node behaves oddly.");
    }
    {
        peers_cmd->description("Manage seed peers: list, add, remove.");
        peers_list->description(
            "List configured peers with address, pubkey pin, and last status.");
        peers_add->description(
            "Add a seed peer (address + optional pinned pubkey).\n"
            "The pubkey is required when mesh.require_seed_pins=true (recommended).");
        peers_add->footer(
            "Examples:\n"
            "  bs peers add dev 100.x.y.z:19949 --pubkey 9a1b...\n"
            "  bs peers add dev dev.example.com:19949");
        peers_remove->description("Remove a seed peer from the config.");
        peers_remove->footer("Example:\n  bs peers remove dev");
    }
    {
        health_cmd_app->description(
            "Ping/pong health check against a peer: verifies TLS handshake,\n"
            "identity pins, and the data plane. Use --latency for RTT numbers.\n"
            "Exit code 0 = healthy; non-zero = unreachable/untrusted.");
        health_cmd_app->footer("Examples:\n  bs health dev\n  bs health dev --latency");
    }
    {
        reconnect_cmd_app->description(
            "Ask the local daemon to tear down and re-handshake one peer\n"
            "(fresh TLS + gossip). Use after a peer's identity or address\n"
            "changed, or when a connection is wedged but health still passes.");
        reconnect_cmd_app->footer("Example:\n  bs reconnect dev");
    }
    {
        invite_cmd_app->description(
            "Generate a single-use invite token a new node can `bs join` with.\n"
            "Tokens are time-limited; share them over a private channel.");
        invite_cmd_app->footer("Example:\n  bs invite > token.txt   # send to the new node");
    }
    {
        enroll_cmd_app->description(
            "Vouch for a new mesh member: sign its node entry and gossip the\n"
            "key to all peers, so the member is trusted mesh-wide without\n"
            "per-peer `authorize` calls.");
        enroll_cmd_app->footer(
            "Example:\n  bs enroll dev 9a1b...64hex 100.x.y.z:19949");
    }
    {
        join_cmd_app->description(
            "Join an existing mesh using an invite token from a member.\n"
            "Fetches the mesh directory, pins the inviting peer's key, and\n"
            "(with --start) starts the local daemon.");
        join_cmd_app->footer(
            "Examples:\n"
            "  bs join --token-file token.txt --start\n"
            "  cat token.txt | bs join 100.x.y.z:19949 -");
    }
    {
        image_cmd_app->description(
            "Render an image file inline in the terminal (Sixel/Block art,\n"
            "depending on terminal support).");
        image_cmd_app->footer("Example:\n  bs image screenshot.png");
    }
    {
        anim_cmd_app->description(
            "Play an animated GIF inline in the terminal (frame-by-frame\n"
            "rendering; Ctrl-C stops).");
        anim_cmd_app->footer("Example:\n  bs anim capture.gif");
    }
    {
        stats_cmd_app->description(
            "Show local daemon statistics: connections, sessions, transfer\n"
            "totals, and per-peer counters.");
    }
    {
        fleet_cmd_app->description(
            "Show the live fleet directory gathered by the daemon: every known\n"
            "peer with address, version, health status, and resource usage\n"
            "(cpu/mem/disk where reported). --json gives the raw daemon view.");
        fleet_cmd_app->footer("Examples:\n  bs fleet\n  bs fleet --json");
    }
    {
        upgrade_cmd_app->description(
            "Self-update this node's binary from GitHub releases (or another\n"
            "node with --all). Downloads, SHA256-verifies against the published\n"
            "checksums, atomically swaps the binary, restarts the daemon, and\n"
            "auto-rolls-back if the new daemon fails to bind. In-mesh upgrades\n"
            "detach first so the carrying session survives (override:\n"
            "BS_UPGRADE_IN_MESH=1).");
        upgrade_cmd_app->footer(
            "Examples:\n"
            "  bs upgrade                    Latest release\n"
            "  bs upgrade --tag 26.09.08-r2  Specific version\n"
            "  bs upgrade --all              All healthy peers (mesh shell)");
    }
    {
        telemetry_cmd_app->description(
            "Show transfer telemetry: file-transfer and stream byte counters\n"
            "per peer. --json for machine-readable output.");
    }
    {
        file_cmd->description("File transfer between peers over the mesh (mTLS, chunked).");
        file_send_app->description(
            "Send a file to a peer. Lands under the peer's receive_dir unless\n"
            "an explicit remote dest is given (home/tmp dest requires the\n"
            "peer's file.dest_allow_home). Use --wait to block until the peer\n"
            "acknowledges completion.");
        file_send_app->footer(
            "Examples:\n"
            "  bs file send dev report.pdf\n"
            "  bs file send dev report.pdf docs/report.pdf\n"
            "  bs file send dev big.iso --wait");
        file_recv_app->description(
            "Receive a file from a peer (run on the target node). Pulls the\n"
            "remote path into a local directory; --wait blocks until the\n"
            "transfer completes or fails.");
        file_recv_app->footer(
            "Example:\n  bs file recv dev ~/logs/bs-mesh.log ./pull/");
    }
    {
        capvid_cmd->description(
            "Record the peer's screen to a video (via its CUA helper), transfer\n"
            "it back with file recv, and print the local path.");
        capvid_cmd->footer(
            "Example:\n  bs capture-video dev --duration 30 --fps 4");
    }
    {
        cua_cmd->description(
            "Computer-use automation on a remote peer: screen size, screenshots,\n"
            "and synthetic mouse/keyboard input. Requires the peer's CUA helper\n"
            "(--cua-helper) running in its user session.");
        cua_screen->description("Get remote screen dimensions (width x height).");
        cua_capture->description(
            "Capture a screenshot from the peer (PNG default; --output writes a\n"
            "file instead of stdout).");
        cua_capture->footer("Example:\n  bs cua capture dev -o shot.png");
        cua_click->description(
            "Click the mouse at coordinates (--button left|right|middle).");
        cua_click->footer("Example:\n  bs cua click dev --x 640 --y 400");
        cua_move->description("Move the mouse to coordinates.");
        cua_type->description("Type text into the peer's focused window.");
        cua_type->footer("Example:\n  bs cua type dev --text 'hello'");
        cua_key->description("Press a HID key code (e.g. 40=Enter, 44=Q; see USB HID usage ids).");
        cua_key->footer("Example:\n  bs cua key dev --code 40");
        cua_scroll->description("Scroll the mouse wheel (--direction up|down, --amount ticks).");
    }
    {
        vfolder_cmd->description(
            "Virtual folder sync: keep a local directory mirrored to a peer\n"
            "path (push, pull, or bidirectional) on an interval.");
        vfolder_add->description(
            "Add a mapping. --dir push|pull|bidirectional; --interval seconds.");
        vfolder_add->footer(
            "Example:\n  bs vfolder add code ~/src dev ~/src --dir bidirectional");
        vfolder_sync->description("Trigger an immediate sync of one mapping.");
        vfolder_list->description("List active folder mappings.");
    }
    {
        edit_cmd_app->description(
            "Edit a remote file locally: copies PEER:PATH to a temp file, opens\n"
            "$EDITOR, and writes it back on save.");
        edit_cmd_app->footer("Example:\n  bs edit dev:/etc/nginx.conf");
    }
    {
        rscript_cmd_app->description(
            "Send a local script to a peer and execute it there. Interpreter\n"
            "'auto' picks bash/powershell/python from the shebang and platform.");
        rscript_cmd_app->footer(
            "Examples:\n"
            "  bs run-script dev ./deploy.sh\n"
            "  cat job.sh | bs run-script dev - --interpreter bash");
    }
    {
        job_cmd->description(
            "Multi-step JSON jobs: run an ordered list of commands on a peer\n"
            "with per-step results (more robust than long && chains).");
        job_run_app->description(
            "Execute a job JSON file on a peer. --stop-on-error aborts remaining\n"
            "steps after the first failure (default: run all steps).");
        job_run_app->footer("Example:\n  bs job run dev job.json --stop-on-error");
    }
    {
        script_cmd->description(
            "Content-addressed script cache: store scripts once (hash-named),\n"
            "then push/run them on peers by name or hash.");
        script_add_app->description("Add a script file to the local cache (optionally with an alias).");
        script_add_app->footer("Example:\n  bs script add ./deploy.sh --name deploy");
        script_list_app->description("List cached scripts (name, hash, size).");
        script_push_app->description("Push a cached script to a peer's cache.");
        script_push_app->footer("Example:\n  bs script push deploy --peer dev");
        script_run_app->description(
            "Run a cached script on a peer; extra args after -- pass to it.");
        script_run_app->footer("Example:\n  bs script run deploy --peer dev -- --prod");
        script_remove_app->description("Remove a script from the local cache.");
    }
    {
        pane_cmd_app->description(
            "Publish content to the BridgePanel surface (web dashboard) of a\n"
            "session on this node.");
        pane_publish->description(
            "Copy a local markdown file into a BridgePanel session surface\n"
            "(--type documents|comms).");
        pane_publish->footer("Example:\n  bs pane publish --title Notes ./notes.md");
    }

    CLI11_PARSE(app, argc, argv);

#ifndef _WIN32
    // --daemon: fork BEFORE any logging/thread startup. The async log pool's
    // thread transiently holds glibc malloc locks; forking after it starts can
    // wedge the child's first allocation on a lock owned by a thread that does
    // not exist in the child (observed: daemon hung in log_event_at before it
    // ever logged). Parent prints the child pid and exits, as before.
    if (daemon_flag && app.get_subcommands().empty() && quick_peer.empty()) {
        pid_t pid = fork();
        if (pid < 0) { std::cerr << "fork failed\n"; return 1; }
        if (pid > 0) { std::cout << pid << std::endl; return 0; }
        setsid();
        if (!freopen("/dev/null", "r", stdin) ||
            !freopen("/dev/null", "w", stdout) ||
            !freopen("/dev/null", "w", stderr)) {
            _exit(1);  // stdio detach failed; nothing reliable left to report on
        }
    }
#endif

    // Resolve config path
    std::string home_dir;
    if (!config_dir.empty()) { home_dir = config_dir; }
    else if (!config_path.empty()) {
        // Derive config dir from explicit --config path
        // (needed on Windows when daemon runs as SYSTEM via schtasks —
        //  USERPROFILE is the SYSTEM profile, not the user's home).
        home_dir = config_path;
        auto slash = home_dir.rfind('/');
        if (slash == std::string::npos) slash = home_dir.rfind('\\');
        if (slash != std::string::npos) home_dir = home_dir.substr(0, slash);
    }
    else { home_dir = resolve_home("~/.bridgesessions"); }
    if (config_path.empty()) { config_path = home_dir + "/config"; }
    // Ensure isolated app root exists for --config-dir runs
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(home_dir, ec);
        auto paths = bs::mesh::make_app_paths(home_dir);
        fs::create_directories(paths.received, ec);
        fs::create_directories(paths.logs, ec);
        fs::create_directories(paths.state, ec);
    }

    // Initialize structured operational logging only for daemon/cua-helper modes.
    // One-shot CLI commands (shell, health, file transfer) should NOT produce
    // startup logging noise — they write their own output to stdout/stderr.
    const bool is_long_running = daemon_flag || cua_helper_flag;
    if (is_long_running) {
        bs::log::init(home_dir, !daemon_flag);
        auto op_log = bs::log::get("main");
        op_log->info("BridgeSessions v{} starting", bs::mesh::kBridgeSessionsVersion);
        op_log->debug("app_home={}, config={}, daemon={}, cua_helper={}",
                      home_dir, config_path, daemon_flag, cua_helper_flag);
    }
    // --list without a peer: list LOCAL sessions via the daemon IPC and exit
    // (same SESSIONS IPC the `sessions` subcommand uses).
    if (quick_list && quick_peer.empty()) {
        std::string ipc = daemon_simple_ipc("SESSIONS", 3000, home_dir);
        if (!ipc.empty() && ipc.rfind("ERROR", 0) != 0) {
            std::cout << ipc << "\n";
            return 0;
        }
        std::cerr << "daemon not reachable — is `bridgesessions --daemon` running?\n";
        return 1;
    }
    if (!quick_peer.empty()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        // Self-connect must be caught before SSH-alias import / trust checks —
        // otherwise `bs <self>` dies with a misleading "untrusted" refusal.
        if (bs::mesh::is_self_target(cfg, quick_peer)) {
            std::cerr << "Cannot connect to yourself. This is "
                      << bs::mesh::self_display_name(cfg) << ".\n";
            return 2;
        }
        if (!bs::mesh::import_ssh_alias_peer(cfg, quick_peer)) {
            std::cerr << quick_peer
                      << " is not a configured BridgeSessions peer or valid SSH alias\n";
            return 2;
        }
        if (bs::mesh::trusted_peer_pubkey(cfg, quick_peer).empty()) {
            std::cerr << "Refusing untrusted first contact to " << quick_peer
                      << ": pair it once with a pinned BridgeSessions pubkey\n";
            return 2;
        }
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        // --list: print the peer's session table and exit — no attach.
        if (quick_list) {
            mc.list_sessions(quick_peer, false);
            return 0;
        }
        auto [cols, rows] = bs::mesh::get_winsize();
        const bool unnamed_session = quick_session.empty();
        // -s/--select: interactively pick an existing session (or new). Never
        // hard-fails the connect — a peer that cannot be queried falls back
        // to the default flow with a one-line warning.
        if (quick_select && bs::mesh::stdin_is_terminal()) {
            auto listed = mc.fetch_peer_sessions(quick_peer);
            if (listed) {
                if (bs::mesh::stdin_is_terminal() && stdout_is_terminal()) {
                    int choice = connect_menu_pick(
                        quick_peer + " — choose a session:",
                        bs::tui::session_picker_rows(*listed));
                    if (choice > 0) {
                        std::string picked =
                            bs::tui::session_picker_choice(*listed,
                                static_cast<size_t>(choice));
                        if (!picked.empty()) quick_session = picked;
                    }
                    // choice 0 (cancel) or "" (new session): keep default flow
                } else {
                    // Non-TTY with -s: number the rows and read a choice.
                    auto rows = bs::tui::session_picker_rows(*listed);
                    for (size_t i = 0; i < rows.size(); ++i)
                        std::cout << "  " << (i + 1) << ") " << rows[i] << "\n";
                    std::cout << "> " << std::flush;
                    int n = connect_menu_choice(rows.size());
                    if (n > 0) {
                        std::string picked = bs::tui::session_picker_choice(
                            *listed, static_cast<size_t>(n));
                        if (!picked.empty()) quick_session = picked;
                    }
                }
            } else {
                std::cerr << "note: could not list sessions on " << quick_peer
                          << "; starting a new session\n";
            }
        }
        quick_session = bs::mesh::resolve_quick_connect_session_name(quick_session);
        if (unnamed_session) {
            std::cerr << "session " << quick_session << "\n";
        }
        return mc.shell_peer(quick_peer, quick_session, {}, cols, rows,
                             "xterm-256color");
    }
    if (shell_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);

        // --detach: fire-and-forget (works with -x too)
        if (shell_detach) {
            return mc.shell_peer_detach(shell_peer, shell_session, shell_cmd,
                                        shell_cols, shell_rows, "xterm-256color");
        }
        if (shell_interactive && !bs::mesh::stdin_is_terminal()) {
            std::cerr << "shell -i requires an interactive terminal on stdin\n";
            return 2;
        }

        // Commands launched from a real terminal keep full PTY input/output. Piped or
        // automated commands use daemon IPC and capture a finite result.
        if (!shell_cmd.empty() && !bs::mesh::stdin_is_terminal()) {
            std::string output;
            int ec = mc.daemon_shell_via_ipc(shell_peer, shell_session, shell_cmd, &output);
            if (ec == -1) {
                // Shell execution intentionally uses an isolated direct TLS
                // connection; older/unavailable daemons take the same path.
                std::cerr << "Using direct TLS shell transport.\n";
                return mc.shell_peer(shell_peer, shell_session, shell_cmd,
                                     shell_cols, shell_rows, "xterm-256color");
            }
            if (ec < 0) {
                // timeout (-2) or other error — report, don't double-exec
                std::cerr << "Shell IPC error (code " << ec << ").\n";
                return 1;
            }
            if (!output.empty()) std::cout << output;
            return ec;
        }
        // Interactive shell: direct TLS (needs full terminal passthrough).
        // `--detach`: send attach and return immediately (session runs on peer).
        // `--wait`: block on daemon IPC until session completes, return exit code.
        auto [detected_cols, detected_rows] = bs::mesh::get_winsize();
        if (shell_cols_opt->count() == 0) shell_cols = detected_cols;
        if (shell_rows_opt->count() == 0) shell_rows = detected_rows;
        if (shell_detach) {
            return mc.shell_peer_detach(shell_peer, shell_session, shell_cmd,
                                        shell_cols, shell_rows, "xterm-256color");
        }
        if (shell_wait) {
            std::string output;
            int ec = mc.daemon_shell_via_ipc(shell_peer, shell_session, shell_cmd, &output);
            if (ec >= 0) { std::cout << output; return ec; }
            // Fallthrough: session not local, do direct wait
            return mc.shell_peer(shell_peer, shell_session, shell_cmd,
                                 shell_cols, shell_rows, "xterm-256color",
                                 shell_signal_forward, shell_signal_on_detach);
        }
        // -s/--select: interactively pick an existing session (or new). Never
        // hard-fails: an unqueryable peer falls back to the default flow.
        if (shell_select) {
            auto listed = mc.fetch_peer_sessions(shell_peer);
            if (listed) {
                if (bs::mesh::stdin_is_terminal() && stdout_is_terminal()) {
                    int choice = connect_menu_pick(
                        shell_peer + " — choose a session:",
                        bs::tui::session_picker_rows(*listed));
                    if (choice > 0) {
                        std::string picked = bs::tui::session_picker_choice(
                            *listed, static_cast<size_t>(choice));
                        if (!picked.empty()) shell_session = picked;
                    }
                } else {
                    auto rows = bs::tui::session_picker_rows(*listed);
                    for (size_t i = 0; i < rows.size(); ++i)
                        std::cout << "  " << (i + 1) << ") " << rows[i] << "\n";
                    std::cout << "> " << std::flush;
                    int n = connect_menu_choice(rows.size());
                    if (n > 0) {
                        std::string picked = bs::tui::session_picker_choice(
                            *listed, static_cast<size_t>(n));
                        if (!picked.empty()) shell_session = picked;
                    }
                }
            } else {
                std::cerr << "note: could not list sessions on " << shell_peer
                          << "; using default session\n";
            }
        }
        return mc.shell_peer(shell_peer, shell_session, shell_cmd, shell_cols, shell_rows, "xterm-256color", shell_signal_forward, shell_signal_on_detach, shell_interactive);
    }
    if (connect_cmd_app->parsed()) {
        return cmd_connect_selector(config_path, home_dir, connect_peer, connect_harness);
    }
    if (sessions_cmd_app->parsed()) {
        if (sessions_peer.empty()) {
            std::string ipc = daemon_simple_ipc("SESSIONS", 3000, home_dir);
            if (!ipc.empty() && ipc.rfind("ERROR", 0) != 0) {
                if (sessions_json) {
                    // Convert pipe-separated SESSIONS output to JSON
                    std::cout << sess_text_to_json(ipc) << "\n";
                } else {
                    std::cout << ipc << "\n";
                }
                return 0;
            }
        }
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::MeshController mc(cfg, home_dir);
        mc.list_sessions(sessions_peer, sessions_all);
        return 0;
    }
    if (keygen_cmd_app->parsed()) {
        return cmd_keygen(home_dir);
    }
    if (auth_cmd_app->parsed()) {
        return cmd_authorize(auth_pubkey.c_str(), home_dir);
    }
    if (doctor_cmd_app->parsed()) {
        if (doctor_gather) return cmd_doctor_gather(config_path, home_dir);
        return cmd_doctor(config_path, home_dir);
    }
    // 26.09.09: `bs api <verb>` — machine JSON for desktop/mobile clients.
    if (api_cmd->parsed()) {
        std::string verb = api_verb;
        for (char& c : verb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string ipc_verb;
        if (verb == "sessions") ipc_verb = "API_SESSIONS";
        else if (verb == "peers") ipc_verb = "API_PEERS";
        else if (verb == "daemon") ipc_verb = "API_DAEMON";
        else if (verb == "events") {
            ipc_verb = "API_EVENTS";
            if (!api_arg.empty()) ipc_verb += " " + api_arg;
        } else {
            std::cerr << "error: unknown api verb '" << api_verb
                      << "' (known: sessions, peers, daemon, events)\n";
            return 2;
        }
        std::string out = daemon_simple_ipc(ipc_verb, 3000, home_dir);
        if (out.empty()) {
            std::cerr << "daemon not reachable — is `bridgesessions --daemon` running?\n";
            return 1;
        }
        std::cout << out << "\n";
        return 0;
    }
    if (peers_list->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        // Prefer live fleet snapshot (status + metrics flag) when daemon is up.
        std::string fleet_ipc = daemon_simple_ipc("FLEET", 3000, home_dir);
        if (!fleet_ipc.empty() && fleet_ipc.rfind("ERROR", 0) != 0) {
            try {
                auto j = nlohmann::json::parse(fleet_ipc);
                struct Row { std::string name, kind, addr, status, ver; };
                std::vector<Row> rows;
                std::unordered_set<std::string> seen;
                auto lower = [](std::string s) {
                    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return s;
                };
                for (auto& [key, val] : j.items()) {
                    Row r;
                    r.name = key;
                    r.addr = val.value("addr", "");
                    r.status = val.value("status", "");
                    r.ver = val.value("version", "");
                    if (r.ver.empty()) r.ver = "-";
                    r.kind = (r.status == "self") ? "self" : "seed";
                    for (auto& s : cfg.seeds)
                        if (bs::mesh::config_peer_name_eq(s.name, key)) { r.kind = "seed"; break; }
                    for (auto& d : cfg.discovered)
                        if (bs::mesh::config_peer_name_eq(d.name, key) && r.kind != "self")
                            r.kind = "disc";
                    rows.push_back(r);
                    seen.insert(lower(key));
                }
                for (auto& s : cfg.seeds) {
                    if (seen.count(lower(s.name))) continue;
                    rows.push_back({s.name, "seed", s.addr, "offline", "-"});
                }
                std::sort(rows.begin(), rows.end(),
                          [](const Row& a, const Row& b) { return a.name < b.name; });
                size_t wn = 4, wk = 4, wa = 7, ws = 6, wv = 7;
                for (auto& r : rows) {
                    wn = std::max(wn, r.name.size());
                    wk = std::max(wk, r.kind.size());
                    wa = std::max(wa, r.addr.size());
                    ws = std::max(ws, r.status.size());
                    wv = std::max(wv, r.ver.size());
                }
                auto pad = [](const std::string& s, size_t w) {
                    return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
                };
                std::cout << pad("NAME", wn) << "  " << pad("KIND", wk) << "  "
                          << pad("ADDRESS", wa) << "  " << pad("STATUS", ws) << "  "
                          << pad("VERSION", wv) << "\n";
                std::cout << std::string(wn + wk + wa + ws + wv + 8, '-') << "\n";
                for (auto& r : rows) {
                    std::cout << pad(r.name, wn) << "  " << pad(r.kind, wk) << "  "
                              << pad(r.addr, wa) << "  " << pad(r.status, ws) << "  "
                              << pad(r.ver, wv) << "\n";
                }
                std::cout << rows.size() << " peer(s)  ·  see also: bs fleet\n";
                return 0;
            } catch (...) {
                // fall through to config dump
            }
        }
        std::cout << "=== Known peers (daemon offline — config only) ===\n";
        for (auto& p : cfg.seeds)
            std::cout << "  [seed] " << p.name << " " << p.addr << "\n";
        for (auto& p : cfg.discovered)
            std::cout << "  [discovered] " << p.name << " " << p.addr << "\n";
        return 0;
    }
    if (peers_add->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::PeerEntry pe;
        pe.name = peer_add_name;
        pe.addr = peer_add_addr;
        pe.pubkey_hex = peer_add_pubkey;
        cfg.seeds.push_back(std::move(pe));
        (void)bs::mesh::save_config(config_path, cfg);
        std::cout << "added seed " << peer_add_name << " -> " << peer_add_addr;
        if (!peer_add_pubkey.empty())
            std::cout << " pubkey=" << peer_add_pubkey;
        std::cout << std::endl;
        if (peer_add_pubkey.empty() && cfg.require_seed_pins)
            std::cout << "warning: mesh.require_seed_pins=true but no pubkey given; "
                         "this seed will be skipped on dial. Re-run with --pubkey "
                         "or add pubkey= to the config line." << std::endl;
        return 0;
    }
    if (peers_remove->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        cfg.seeds.erase(std::remove_if(cfg.seeds.begin(), cfg.seeds.end(),
            [&](auto& p){ return p.name == peer_remove_name; }), cfg.seeds.end());
        (void)bs::mesh::save_config(config_path, cfg);
        std::cout << "removed seed " << peer_remove_name << std::endl;
        return 0;
    }
    // 26.09.09: rotate-pin — update the pinned pubkey for a peer in-place.
    if (peers_rotate_pin->parsed()) {
        std::string pk = rotate_pin_pubkey;
        // Normalize: strip 0x prefix, lowercase, validate hex length.
        if (pk.rfind("0x", 0) == 0 || pk.rfind("0X", 0) == 0) pk = pk.substr(2);
        if (pk.size() != 64) {
            std::cerr << "error: pubkey must be 64 hex chars (got " << pk.size() << ")\n";
            return 2;
        }
        for (char& c : pk) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                std::cerr << "error: pubkey contains non-hex character\n";
                return 2;
            }
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bool found = false;
        for (auto& p : cfg.seeds) {
            if (bs::mesh::config_peer_name_eq(p.name, rotate_pin_peer)) {
                p.pubkey_hex = pk;
                found = true;
                break;
            }
        }
        if (!found) {
            for (auto& p : cfg.discovered) {
                if (bs::mesh::config_peer_name_eq(p.name, rotate_pin_peer)) {
                    // Discovered peers are not persisted; promote to a seed so
                    // the new pin survives restarts.
                    p.pubkey_hex = pk;
                    cfg.seeds.push_back(p);
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            std::cerr << "error: unknown peer: " << rotate_pin_peer << "\n";
            return 2;
        }
        if (!bs::mesh::save_config(config_path, cfg)) {
            std::cerr << "error: failed to write config: " << config_path << "\n";
            return 1;
        }
        std::cout << "rotated pin for " << rotate_pin_peer << " → " << pk << "\n"
                  << "Restart the local daemon to pick up the new pin "
                  << "(or run: bs reconnect " << rotate_pin_peer << ").\n";
        return 0;
    }

    // 26.09.09: rotate-identity — regenerate this node's keys with backup.
    if (rotate_id_cmd->parsed()) {
        if (!rotate_identity_yes) {
            std::cerr << "Rotating the identity changes this node's public key.\n"
                      << "Every peer pinning this node must run "
                      << "`bs peers rotate-pin <this-node> <new-key>` afterwards.\n"
                      << "Run again with --yes to proceed.\n";
            return 2;
        }
        namespace fs = std::filesystem;
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char ts[32]{};
        std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
        std::string suffix = std::string(".bak-") + ts;
        bool had_identity = false;
        for (const char* name : {"id_ed25519.pem", "id_ed25519-cert.pem", "id_ed25519.pub"}) {
            fs::path p = fs::path(home_dir) / name;
            std::error_code ec;
            if (fs::exists(p, ec)) {
                had_identity = true;
                fs::rename(p, fs::path(p.string() + suffix), ec);
                if (ec) {
                    std::cerr << "error: cannot back up " << p.string() << ": "
                              << ec.message() << "\n";
                    return 1;
                }
            }
        }
        if (!had_identity) {
            std::cerr << "error: no existing identity in " << home_dir << "\n";
            return 2;
        }
        try {
            bs::mesh::bootstrap_identity(home_dir);
        } catch (const std::exception& e) {
            std::cerr << "error: identity regeneration failed: " << e.what() << "\n"
                      << "  → restore from " << suffix << " backups\n";
            return 1;
        }
        std::ifstream pubf(fs::path(home_dir) / "id_ed25519.pub");
        std::string new_pub;
        std::getline(pubf, new_pub);
        bs::mesh::MeshConfig cfg_now = bs::mesh::load_config(config_path);
        std::cout << "identity rotated. new pubkey: " << new_pub << "\n"
                  << "old keys backed up with suffix " << suffix << "\n"
                  << "NEXT: on every peer that pins this node run:\n"
                  << "  bs peers rotate-pin "
                  << bs::mesh::self_display_name(cfg_now) << " " << new_pub << "\n";
        return 0;
    }

    if (health_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string status;
        bool ok = mc.health_check(health_peer, &status);
        std::cout << health_peer << " " << status;
        if (health_latency) {
            std::string latency;
            mc.health_latency_report(health_peer, &latency);
            if (!latency.empty()) std::cout << "  " << latency;
        }
        std::cout << std::endl;
        return ok ? 0 : 1;
    }
    if (reconnect_cmd_app->parsed()) {
        std::string result = daemon_simple_ipc("RECONNECT " + reconnect_peer, 25000, home_dir);
        if (result.empty()) {
            std::cerr << "ERROR no daemon running\n";
            return 1;
        }
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (enroll_cmd_app->parsed()) {
        // Normalize pubkey: strip whitespace; accept optional "pubkey " prefix.
        std::string pk = enroll_pubkey;
        while (!pk.empty() && (pk.back() == '\n' || pk.back() == '\r' || pk.back() == ' ')) pk.pop_back();
        if (pk.rfind("pubkey ", 0) == 0) pk = pk.substr(7);
        if (pk.size() != 64 || !std::all_of(pk.begin(), pk.end(),
                [](unsigned char c){ return std::isxdigit(c); })) {
            std::cerr << "ERROR pubkey must be 64 hex chars (got " << pk.size() << ")\n";
            return 1;
        }
        std::string result = daemon_simple_ipc(
            "ENROLL " + enroll_name + " " + pk + " " + enroll_addr, 3000, home_dir);
        std::cout << result;
        if (!result.empty() && result.back() != '\n') std::cout << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (invite_cmd_app->parsed()) {
        std::string token = daemon_simple_ipc("INVITE", 2000, home_dir);
        if (token.empty()) { std::cerr << "INVITE failed (daemon not running?)\n"; return 1; }
        while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        std::string addr = cfg.listen_addr;
        if (addr.empty() || addr == "0.0.0.0") {
            // Detect reachable address: prefer Tailscale, then best non-loopback
            FILE* ts = BS_POPEN("tailscale ip -4 2>/dev/null", "r");
            if (ts) {
                char buf[64] = {};
                if (fgets(buf, sizeof(buf), ts)) {
                    std::string ts_ip(buf);
                    while (!ts_ip.empty() && (ts_ip.back() == '\n' || ts_ip.back() == '\r')) ts_ip.pop_back();
                    if (!ts_ip.empty()) addr = ts_ip;
                }
                BS_PCLOSE(ts);
            }
            if (addr.empty() || addr == "0.0.0.0") addr = "127.0.0.1";
        }
        int port = cfg.listen_port > 0 ? cfg.listen_port : 19949;
        std::cout << "Invite (valid 2h):  " << token << "\n";
        std::cout << "One-liner:\n";
        std::cout << "  bridgesessions join " << addr << ":" << port << " --token-file <path> --start\n";
        std::cout << "  # Or pipe the token on stdin: printf '%s\\n' '<invite-token>' | bridgesessions join "
                  << addr << ":" << port << " - --start\n";
        std::cout << "Or with curl install:\n";
        std::cout << "  curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/v" << bs::mesh::kBridgeSessionsVersion << "/scripts/install.sh | bash\n"
                  << "  bridgesessions join " << addr << ":" << port << " --token-file <path> --start\n";
        std::cout << "Windows PowerShell:\n";
        std::cout << "  irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/v" << bs::mesh::kBridgeSessionsVersion << "/scripts/install.ps1 | iex\n  bridgesessions join " << addr << ":" << port << " --token-file <path> --start\n";
        return 0;
    }
    if (join_cmd_app->parsed()) {
        if (!join_token.empty() && join_token != "-") {
            static bool warned_positional_token = false;
            if (!warned_positional_token) {
                std::cerr << "join: warning: positional invite tokens are deprecated; "
                             "use --token-file or '-' (stdin) instead\n";
                warned_positional_token = true;
            }
        }
        if (!join_token_file.empty()) {
            if (!join_token.empty()) {
                std::cerr << "join: specify either token or --token-file, not both\n";
                return 1;
            }
            std::ifstream tf(join_token_file);
            if (!tf || !std::getline(tf, join_token)) {
                std::cerr << "join: could not read token file\n";
                return 1;
            }
        } else if (join_token == "-") {
            join_token.clear();
            if (!std::getline(std::cin, join_token)) {
                std::cerr << "join: could not read token from stdin\n";
                return 1;
            }
        }
        while (!join_token.empty() &&
               (join_token.back() == '\n' || join_token.back() == '\r'))
            join_token.pop_back();
        if (join_token.empty()) {
            std::cerr << "join: invite token required (use '-' or --token-file for private input)\n";
            return 1;
        }
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        std::string pubkey_hex;
        {
            bs::mesh::bootstrap_identity(home_dir);
            std::string pub_path = home_dir + "/id_ed25519.pub";
            std::ifstream pf(pub_path);
            if (pf.is_open()) { std::getline(pf, pubkey_hex);
                while (!pubkey_hex.empty() && (pubkey_hex.back() == '\n' || pubkey_hex.back() == '\r'))
                    pubkey_hex.pop_back(); }
        }
        if (pubkey_hex.empty()) { std::cerr << "No identity found — run keygen first\n"; return 1; }
        std::cout << "Identity: " << pubkey_hex.substr(0, 16) << "...\n";

        // Parse addr:port
        auto sep = join_addr.rfind(':');
        std::string host = join_addr.substr(0, sep);
        int port = 19949;
        if (sep != std::string::npos) {
            const std::string port_s = join_addr.substr(sep + 1);
            try {
                size_t idx = 0;
                port = std::stoi(port_s, &idx);
                if (idx != port_s.size() || port <= 0 || port > 65535)
                    throw std::out_of_range("port");
            } catch (...) {
                std::cerr << "join: invalid address '" << join_addr
                          << "' (expected host:port)\n";
                return 1;
            }
        }

        // Create MeshController for TLS connect (trust established via invite token)
        bs::mesh::MeshController mc(cfg, home_dir);
        auto conn = mc.connect_and_hello(join_addr, {}, true);
        if (!conn.ssl) {
            std::cerr << "Cannot connect to " << join_addr << ": "
                      << bs::mesh::MeshController::connect_fail_string(conn.fail) << "\n";
            return 1;
        }

        // Send JoinRequest
        bs::mesh::JoinRequestMsg jr;
        jr.token = join_token;
        jr.node_name = join_node_name;
        // Advertise our own reachable listen endpoint (Tailscale model): the
        // host auto-vouches for us mesh-wide using this addr, so peers can
        // dial back without ever seeing our ephemeral source port.
        {
            FILE* ts = BS_POPEN("tailscale ip -4 2>/dev/null", "r");
            std::string my_ip;
            if (ts) {
                char buf[64] = {};
                if (fgets(buf, sizeof(buf), ts)) {
                    my_ip.assign(buf);
                    while (!my_ip.empty() && (my_ip.back() == '\n' || my_ip.back() == '\r'))
                        my_ip.pop_back();
                }
                BS_PCLOSE(ts);
            }
            if (!my_ip.empty()) {
                jr.listen_addr = my_ip + ":19949";
            }
        }
        write_frame(conn.ssl.get(), jr, bs::mesh::CONTROL_STREAM_ID);
        std::fill(join_token.begin(), join_token.end(), '\0');
        join_token.clear();
        std::fill(jr.token.begin(), jr.token.end(), '\0');
        jr.token.clear();

        // Set a 10s receive timeout so we don't block forever if the
        // server's event loop is slow to process the JoinRequest.
        {
#ifdef _WIN32
            DWORD timeout_ms = 10000;
            setsockopt(conn.sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
            timeval tv{10, 0};
            setsockopt(conn.sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#endif
        }

        // Read JoinReply (skip Ping/Pong/Hello/Gossip noise from the event loop)
        bs::mesh::Message msg;
        for (int retry = 0; retry < 100; ++retry) {
            try {
                msg = bs::mesh::read_frame(conn.ssl.get());
            } catch (const std::exception& e) {
                std::cerr << "Failed to read JoinReply: " << e.what() << "\n";
                return 1;
            }
            if (std::holds_alternative<bs::mesh::JoinReplyMsg>(msg)) break;
            // Discard gossip/ping/pong noise — server event loop floods the
            // connection after promotion before we finish reading JoinReply.
        }
        if (!std::holds_alternative<bs::mesh::JoinReplyMsg>(msg)) {
            std::cerr << "Unexpected reply from host (message type index=" << msg.index() << ")\n";
            return 1;
        }
        auto& jrep = std::get<bs::mesh::JoinReplyMsg>(msg);
        if (!jrep.ok) {
            std::cerr << "Join rejected: " << jrep.error << "\n";
            return 1;
        }

        // Bind TLS cert to host identity pubkey (P1-1: prevent
        // MITM injecting a wrong seed pin during join handshake).
        {
            const std::string cert_pk = bs::mesh::peer_public_key_hex(conn.ssl.get());
            if (cert_pk.empty() || cert_pk != jrep.host_pubkey) {
                std::cerr << "Host identity mismatch: certificate key does not match "
                          << "JoinReply host_pubkey\n";
                return 1;
            }
        }

        // Configure: update node name and add host as seed
        cfg.node_name = join_node_name.empty() ? jrep.node_name : join_node_name;
        cfg.listen_addr = "0.0.0.0";
        cfg.listen_port = 19949;
        cfg.require_seed_pins = true;
        bs::mesh::PeerEntry host_seed;
        host_seed.name = "host";
        // Server replies with its own listen_addr, which is usually the
        // wildcard 0.0.0.0 — unreachable. Substitute the address we dialed.
        host_seed.addr = (jrep.host_addr.rfind("0.0.0.0", 0) == 0 ||
                          jrep.host_addr.empty()) ? join_addr : jrep.host_addr;
        host_seed.pubkey_hex = jrep.host_pubkey;
        cfg.seeds.push_back(std::move(host_seed));
        // 2.0.20: add all mesh seeds from the host's peer list
        if (!jrep.peer_pubkeys_json.empty()) {
            try {
                auto pks = nlohmann::json::parse(jrep.peer_pubkeys_json);
                for (auto& pk : pks) {
                    bs::mesh::PeerEntry pe;
                    pe.name = pk.value("name", "");
                    pe.addr = pk.value("addr", "");
                    pe.pubkey_hex = pk.value("pubkey_hex", "");
                    // Host records inbound peers at their EPHEMERAL source
                    // port (e.g. :27826) — dialing that fails. Fleet nodes
                    // all listen on the canonical port; normalize it.
                    auto colon = pe.addr.rfind(':');
                    if (colon != std::string::npos) {
                        std::string port = pe.addr.substr(colon + 1);
                        bool all_digit = !port.empty();
                        for (char c : port) if (!std::isdigit(static_cast<unsigned char>(c))) all_digit = false;
                        if (all_digit && port != "19949") pe.addr = pe.addr.substr(0, colon) + ":19949";
                    }
                    if (!pe.name.empty() && !pe.addr.empty() && !pe.pubkey_hex.empty() &&
                        pe.pubkey_hex != jrep.host_pubkey) {
                        cfg.seeds.push_back(std::move(pe));
                    }
                }
            } catch (const std::exception& e) {
                std::cerr << "join: malformed peer_pubkeys_json: " << e.what() << "\n";
            }
        }
        if (!save_config(config_path, cfg)) {
            std::cerr << "join failed: could not save config to " << config_path << "\n";
            return 1;
        }

        // Authorize host + all mesh seeds
        std::string auth_path = bs::mesh::resolve_under_app_home(cfg.authorized_keys_path, home_dir);
        {
            std::string dir = auth_path;
            auto slash = dir.rfind('/');
            if (slash == std::string::npos) slash = dir.rfind('\\');
            if (slash != std::string::npos) dir = dir.substr(0, slash);
            if (!bs::mesh::ensure_private_directory(dir)) {
                std::cerr << "join failed: could not create " << dir << "\n";
                return 1;
            }
            // Collect set of pubkeys we already have in authorized_keys
            std::unordered_set<std::string> existing_pks;
            {
                std::ifstream existing(auth_path);
                std::string line;
                while (std::getline(existing, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("pubkey ", 0) == 0) existing_pks.insert(line.substr(7));
                    else if (!line.empty()) existing_pks.insert(line);
                }
            }
            std::ofstream af(auth_path, std::ios::app);
            if (af.is_open()) {
                if (existing_pks.find(jrep.host_pubkey) == existing_pks.end())
                    af << "pubkey " << jrep.host_pubkey << "\n";
                // Authorize all mesh seed pubkeys too
                if (!jrep.peer_pubkeys_json.empty()) {
                    try {
                        auto pks = nlohmann::json::parse(jrep.peer_pubkeys_json);
                        for (auto& pk : pks) {
                            std::string pkh = pk.value("pubkey_hex", "");
                            if (!pkh.empty() && pkh != jrep.host_pubkey &&
                                existing_pks.find(pkh) == existing_pks.end()) {
                                af << "pubkey " << pkh << "\n";
                                existing_pks.insert(pkh);
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "join: malformed peer_pubkeys_json: " << e.what() << "\n";
                    }
                }
            }
        }
        std::cout << "Joined. Node: " << cfg.node_name << "  Config: " << config_path << "\n";
        if (join_start) {
            // Prefer launchd/systemd so the node stays inbound-reachable.
            // Always enable --now (do not require the unit to already be enabled).
            std::cout << "→ Starting daemon...\n";
            std::string self = current_exe_path(argv[0]);
            std::string cfg_path = home_dir + "/config";
            if (resume_mesh_daemon(self, cfg_path)) {
                std::cout << "→ Daemon started (via service manager). Run 'bridgesessions health <peer>' to verify.\n";
            } else {
                std::cout << "→ Could not auto-start daemon. Start it manually: bridgesessions --daemon\n";
            }
        } else {
            std::cout << "Start daemon: bridgesessions --daemon\n";
        }
        return 0;
    }
    if (image_cmd_app->parsed()) {
        bs::mesh::render_image_to_terminal(image_file);
        return 0;
    }
    if (anim_cmd_app->parsed()) {
        bs::mesh::render_image_to_terminal(anim_file);
        return 0;
    }
    if (stats_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string ipc = daemon_simple_ipc("STATS", 3000, home_dir);
        if (!ipc.empty() && ipc.rfind("ERROR", 0) != 0) {
            std::cout << ipc << "\n";
            return 0;
        }
        mc.show_stats();
        return 0;
    }
    if (fleet_cmd_app->parsed()) {
        std::string ipc = daemon_simple_ipc("FLEET", 3000, home_dir);
        if (ipc.empty() || ipc.rfind("ERROR", 0) == 0) {
            std::cerr << "fleet: daemon not running or returned error\n";
            return 1;
        }
        if (fleet_json) {
            std::cout << ipc;
            if (ipc.empty() || ipc.back() != '\n') std::cout << "\n";
            return 0;
        }
        try {
            auto j = nlohmann::json::parse(ipc);
            auto fmt_pct = [](const nlohmann::json& v, const char* key) -> std::string {
                if (!v.contains(key) || v[key].is_null()) return "-";
                try {
                    double d = v[key].get<double>();
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%.0f%%", d);
                    return buf;
                } catch (...) { return "-"; }
            };
            auto fmt_load = [](const nlohmann::json& v) -> std::string {
                // Only show load when peer/self reported real host metrics.
                // Bare load1 without metrics is treated as unknown (never print 0.0 lies).
                bool has_metrics = v.value("metrics", false);
                if (!has_metrics && !v.contains("os")) return "-";
                if (!v.contains("load1") || v["load1"].is_null()) return "-";
                try {
                    double d = v["load1"].get<double>();
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "%.1f", d);
                    return buf;
                } catch (...) { return "-"; }
            };
            auto fmt_uptime = [](const nlohmann::json& v, const std::string& status) -> std::string {
                if (status == "self" || status == "offline" || !v.contains("uptime_s")) return "-";
                uint64_t s = v.value("uptime_s", 0ULL);
                if (s >= 86400) return std::to_string(s / 86400) + "d";
                if (s >= 3600) return std::to_string(s / 3600) + "h";
                if (s >= 60) return std::to_string(s / 60) + "m";
                return std::to_string(s) + "s";
            };
            // Sort: self, healthy, no-pong, offline — then name within group.
            auto status_rank = [](const std::string& s) -> int {
                if (s == "self") return 0;
                if (s == "healthy") return 1;
                if (s == "no-pong") return 2;
                if (s == "offline") return 3;
                return 4;
            };
            struct Row {
                std::string name, addr, ver, status, up, cpu, mem, disk, load, os, cua;
                bool metrics = false;
            };
            std::vector<Row> rows;
            int n_connected = 0, n_offline = 0, n_metrics = 0;
            for (auto& [key, val] : j.items()) {
                Row r;
                r.name = key;
                r.addr = val.value("addr", "");
                r.ver = val.value("version", "");
                if (r.ver.empty()) r.ver = "-";
                r.status = val.value("status", "");
                // 26.09.09: surface flapping peers (old daemon JSON lacks the
                // field — defaults false, fully backward compatible).
                if (val.value("flapping", false))
                    r.status += " [flapping]";
                r.up = fmt_uptime(val, r.status);
                r.metrics = val.value("metrics", false) ||
                            (val.contains("os") && !val.value("os", std::string{}).empty());
                r.cpu = r.metrics ? fmt_pct(val, "cpu_pct") : "-";
                r.mem = r.metrics ? fmt_pct(val, "mem_pct") : "-";
                r.disk = r.metrics ? fmt_pct(val, "disk_pct") : "-";
                r.load = fmt_load(val);
                r.os = val.value("os", "");
                if (r.os.empty()) r.os = "-";
                if (r.metrics && val.contains("cua"))
                    r.cua = val.value("cua", false) ? "ok" : "-";
                else
                    r.cua = "-";
                if (r.status == "offline") ++n_offline;
                else if (r.status != "self") ++n_connected;
                if (r.metrics) ++n_metrics;
                rows.push_back(std::move(r));
            }
            std::sort(rows.begin(), rows.end(),
                      [&](const Row& a, const Row& b) {
                          int ra = status_rank(a.status), rb = status_rank(b.status);
                          if (ra != rb) return ra < rb;
                          return a.name < b.name;
                      });
            // Fixed-width aligned table (no markdown pipes).
            size_t w_name = 4, w_addr = 7, w_ver = 7, w_st = 6, w_up = 2,
                   w_cpu = 3, w_mem = 3, w_disk = 4, w_load = 4, w_os = 2, w_cua = 3;
            for (const auto& r : rows) {
                w_name = std::max(w_name, r.name.size());
                w_addr = std::max(w_addr, r.addr.size());
                w_ver = std::max(w_ver, r.ver.size());
                w_st = std::max(w_st, r.status.size());
                w_up = std::max(w_up, r.up.size());
                w_cpu = std::max(w_cpu, r.cpu.size());
                w_mem = std::max(w_mem, r.mem.size());
                w_disk = std::max(w_disk, r.disk.size());
                w_load = std::max(w_load, r.load.size());
                w_os = std::max(w_os, r.os.size());
                w_cua = std::max(w_cua, r.cua.size());
            }
            auto pad = [](const std::string& s, size_t w) {
                if (s.size() >= w) return s;
                return s + std::string(w - s.size(), ' ');
            };
            auto rpad = [](const std::string& s, size_t w) {
                if (s.size() >= w) return s;
                return std::string(w - s.size(), ' ') + s;
            };
            std::cout << pad("NAME", w_name) << "  " << pad("ADDRESS", w_addr) << "  "
                      << pad("VERSION", w_ver) << "  " << pad("STATUS", w_st) << "  "
                      << rpad("UP", w_up) << "  " << rpad("CPU", w_cpu) << "  "
                      << rpad("MEM", w_mem) << "  " << rpad("DISK", w_disk) << "  "
                      << rpad("LOAD", w_load) << "  " << pad("OS", w_os) << "  "
                      << pad("CUA", w_cua) << "\n";
            size_t total = w_name + w_addr + w_ver + w_st + w_up + w_cpu + w_mem +
                           w_disk + w_load + w_os + w_cua + 2 * 10;
            std::cout << std::string(total, '-') << "\n";
            for (const auto& r : rows) {
                std::cout << pad(r.name, w_name) << "  " << pad(r.addr, w_addr) << "  "
                          << pad(r.ver, w_ver) << "  " << pad(r.status, w_st) << "  "
                          << rpad(r.up, w_up) << "  " << rpad(r.cpu, w_cpu) << "  "
                          << rpad(r.mem, w_mem) << "  " << rpad(r.disk, w_disk) << "  "
                          << rpad(r.load, w_load) << "  " << pad(r.os, w_os) << "  "
                          << pad(r.cua, w_cua) << "\n";
            }
            // Summary line: connected vs offline, how many report host metrics.
            int n_live_for_metrics = n_connected + 1; // + self
            std::cout << rows.size() << " listed  ·  " << n_connected << " connected  ·  "
                      << n_offline << " offline  ·  host metrics "
                      << n_metrics << "/" << n_live_for_metrics << "\n";
        } catch (...) {
            std::cerr << "fleet: failed to parse JSON\n";
            return 1;
        }
        return 0;
    }
    if (upgrade_cmd_app->parsed()) {
        // Fleet-context safety (2026-09-07 incident): when this upgrade runs
        // inside a hosted mesh session (BS_SESSION=1), pausing the daemon
        // kills the session-worker's IPC and the upgrade dies before
        // resume_mesh_daemon() can un-mask the systemd unit — leaving the
        // peer offline. Instead of refusing, re-exec DETACHED from the
        // session: `setsid` + full stdio redirection to a log file, so the
        // swap completes even after the carrying daemon stops. The caller's
        // channel drops (expected; same as the old behavior) but the upgrade
        // itself now survives and the daemon comes back on the new binary.
        if (bs::mesh::upgrade_in_mesh_session() && !bs::mesh::upgrade_in_mesh_override()) {
#ifdef __linux__
            const std::string upg_log = home_dir + "/upgrade.log";
            const std::string self_exe = current_exe_path(argv[0]);
            std::string reexec = "setsid sh -c \"'" + self_exe + "' upgrade\"";
            if (!upgrade_tag.empty()) reexec += " --tag '" + upgrade_tag + "'";
            if (allow_downgrade) reexec += " --allow-downgrade";
            reexec += " >>'" + upg_log + "' 2>&1 <'/dev/null'\"";
            if (std::system(reexec.c_str()) == 0) {
                std::cout << "→ Detaching upgrade from this session (daemon carrier).\n"
                          << "  Log: " << upg_log << "\n"
                          << "  This shell will drop when the daemon stops — the\n"
                          << "  upgrade continues detached and the daemon restarts\n"
                          << "  on the new version.\n";
                return 0;
            }
            // setsid unavailable or spawn failed: fall through to the hard
            // refusal below rather than repeating the 2026-09-07 failure.
#endif
            std::cerr << "upgrade: refusing to run inside a mesh shell session.\n"
                         "         The daemon you would pause is the one carrying\n"
                         "         this command's IPC — pausing it kills the upgrade\n"
                         "         before resume_mesh_daemon() can restart anything.\n"
                         "\n"
                         "Run the upgrade OUTSIDE the mesh:\n"
                         "  • local:  exit the mesh shell, then `bs upgrade --tag <v>`\n"
                         "  • remote: SSH in directly (not via `bs shell`):\n"
                         "              ssh user@host 'bridgesessions upgrade --tag <v>'\n"
                         "    On Windows: schtasks / WinRM, not a mesh shell.\n"
                         "(Override only with BS_UPGRADE_IN_MESH=1 if you really know\n"
                         "what you are doing — and you have a recovery plan.)\n";
            return 1;
        }
        // Self-update: download latest (or specified) release from GitHub,
        // verify SHA256 (mandatory), atomic swap, restart daemon.
        std::string tag = upgrade_tag.empty() ? "latest" : upgrade_tag;
        // Validate tag to prevent shell/URL injection (W4-P1): only allow
        // alnum, dots, dashes, underscores. A tag with ' or ; or $ breaks
        // out of the single-quoted curl/system commands below.
        if (!bs::mesh::bs_upgrade_tag_valid(tag)) {
            std::cerr << "upgrade: invalid tag '" << tag << "' — only [A-Za-z0-9._-] allowed\n";
            return 1;
        }
        // Normalize: the download path prepends "v", so accept both "26.09.19"
        // and "v26.09.19" by stripping a leading "v" (but never from "latest").
        tag = bs::mesh::bs_upgrade_tag_normalize(tag);

        // If --all, upgrade every healthy peer via mesh shell
        if (upgrade_all) {
            // Get fleet status from daemon
            std::string fleet_json_raw = daemon_simple_ipc("FLEET", 5000, home_dir);
            if (fleet_json_raw.empty() || fleet_json_raw.rfind("ERROR", 0) == 0) {
                std::cerr << "upgrade --all: daemon not running or fleet query failed\n";
                return 1;
            }
            try {
                auto peers = nlohmann::json::parse(fleet_json_raw);
                // FLEET IPC returns an OBJECT keyed by peer name:
                //   {"peername": {"addr":..., "version":..., "status":...}}
                // Iterate items() to get key (name) + value (peer info).
                std::cout << "Upgrading " << peers.size() << " peers...\n";
                for (auto& [name, info] : peers.items()) {
                    std::string status = info.value("status", "");
                    if (status.find("healthy") == std::string::npos) {
                        std::cout << "  " << name << ": skipped (not healthy)\n";
                        continue;
                    }
                    if (name.empty()) continue;
                    if (!bs::mesh::bs_peer_name_shell_safe(name)) {
                        std::cout << "  " << name << ": skipped (unsafe peer name)\n";
                        continue;
                    }
                    std::cout << "  " << name << ": sending upgrade command...";
                    // Direct TLS shell (daemon IPC never relays SHELL). Remote
                    // upgrade stops the peer daemon — fire-and-forget.
                    std::string remote_cmd = "bridgesessions upgrade";
                    if (!upgrade_tag.empty()) remote_cmd += " --tag " + upgrade_tag;
                    auto sq = [](const std::string& s) {
                        std::string o = "'";
                        for (char c : s) {
                            if (c == '\'') o += "'\\''";
                            else o += c;
                        }
                        o += "'";
                        return o;
                    };
                    std::string shell_cmd =
                        "bridgesessions shell " + sq(name) + " --cmd " +
                        sq(remote_cmd) + " >/dev/null 2>&1 &";
                    std::system(shell_cmd.c_str());
                    std::cout << " dispatched\n";
                }
                std::cout << "Fleet upgrade initiated.\n";
                return 0;
            } catch (...) {
                std::cerr << "upgrade --all: failed to parse fleet JSON\n";
                return 1;
            }
        }

        // Resolve the newest published release, including prereleases.
        if (tag == "latest") {
            const std::string releases_path =
                bs::mesh::create_private_temp_file("releases", ".json");
            const std::string releases_url =
                "https://api.github.com/repos/MindDragonLabs/BridgeSessions/releases?per_page=20";
            const std::string fetch = "curl -fL -s -o " +
                bs::mesh::shell_arg_quote(releases_path) + " " +
                bs::mesh::shell_arg_quote(releases_url) + " 2>/dev/null";
            if (releases_path.empty() || std::system(fetch.c_str()) != 0) {
                if (!releases_path.empty()) ::unlink(releases_path.c_str());
                std::cerr << "upgrade: failed to resolve latest GitHub release\n";
                return 1;
            }
            try {
                std::ifstream in(releases_path);
                nlohmann::json releases; in >> releases;
                for (const auto& release : releases) {
                    if (release.value("draft", true)) continue;
                    tag = release.value("tag_name", "");
                    if (!tag.empty() && tag.front() == 'v') tag.erase(tag.begin());
                    if (!tag.empty()) break;
                }
            } catch (...) { tag.clear(); }
            ::unlink(releases_path.c_str());
            if (tag.empty() || !bs::mesh::bs_upgrade_tag_valid(tag)) {
                std::cerr << "upgrade: no valid published release\n";
                return 1;
            }
        }

        // r3 fix (P2): never let a resolved release move us BACKWARD. The 2026-08-31
        // D-002 incident: GitHub "latest" was 26.08.27-r1 while peers ran 26.08.28-r2,
        // so every auto-upgrade pull DOWNGRADED r2 peers and re-broke them. A pin
        // must be monotonic; explicit --allow-downgrade is the only bypass.
        if (bs::mesh::version_is_older(tag, bs::mesh::kBridgeSessionsVersion)) {
            if (allow_downgrade) {
                std::cout << "⚠ downgrade to " << tag << " requested explicitly\n";
            } else {
                std::cout << "keeping current version: " << tag
                          << " is older than running "
                          << bs::mesh::kBridgeSessionsVersion
                          << " (use --allow-downgrade to override)\n";
                return 0;
            }
        }

        // Self-update this host
        std::cout << "→ Current version: " << bs::mesh::kBridgeSessionsVersion << "\n";

        // Determine platform-specific binary name
        std::string binary_name, app_suffix;
#if defined(__APPLE__)
        binary_name = "bridgesessions-macos-arm64";
        app_suffix = "BridgeSessions.app";
#elif defined(__linux__)
        binary_name = "bridgesessions-linux-x86_64";
#elif defined(_WIN32)
        binary_name = "bridgesessions-windows-x86_64.exe";
#else
        std::cerr << "upgrade: unsupported platform\n";
        return 1;
#endif

        // Determine current binary path
        std::string home = std::getenv("HOME") ? std::getenv("HOME") : ".";
        std::string bin_path = home + "/.local/bin/bridgesessions";
#ifdef __APPLE__
        // Check if running from .app bundle
        char exe_path[4096] = {};
        uint32_t size = sizeof(exe_path);
        if (_NSGetExecutablePath(exe_path, &size) == 0) {
            std::string ep(exe_path);
            if (ep.find(".app") != std::string::npos) {
                bin_path = ep;
            }
        }
#endif

        const std::string base_url =
            "https://github.com/MindDragonLabs/BridgeSessions/releases/download/v" + tag;
        const std::string download_url = base_url + "/" + binary_name;
        const std::string sums_url = base_url + "/SHA256SUMS";

        std::cout << "→ Downloading " << binary_name << " from " << download_url << "\n";
        bs::log::get("upgrade")->info("downloading {} from {}", binary_name, download_url);

        std::string tmp_path = bs::mesh::create_private_temp_file("upg", "");
        if (tmp_path.empty()) {
            std::cerr << "upgrade: cannot create private temp file\n";
            return 1;
        }

        // Use curl to download
        std::string curl_cmd = "curl -fL -s -o " +
            bs::mesh::shell_arg_quote(tmp_path) + " " +
            bs::mesh::shell_arg_quote(download_url) + " 2>/dev/null";
        int rc = std::system(curl_cmd.c_str());
        if (rc != 0) {
            std::cerr << "upgrade: download failed (curl exit " << rc << ")\n";
            bs::log::get("upgrade")->error("download failed (curl exit {})", rc);
            return 1;
        }

        // Make executable before version check
        chmod(tmp_path.c_str(), 0755);

        // Verify SHA256 against published SHA256SUMS — MANDATORY.
        // A tampered or corrupted download must never be swapped in.
        {
            std::string sums_path = bs::mesh::create_private_temp_file("sum", "");
            if (sums_path.empty()) {
                std::cerr << "upgrade: cannot create SHA256SUMS temp file\n";
                ::unlink(tmp_path.c_str());
                return 1;
            }
            std::string sums_cmd = "curl -fL -s -o " +
                bs::mesh::shell_arg_quote(sums_path) + " " +
                bs::mesh::shell_arg_quote(sums_url) + " 2>/dev/null";
            if (std::system(sums_cmd.c_str()) != 0) {
                std::cerr << "upgrade: FAILED to download SHA256SUMS — aborting (hash verification is mandatory)\n";
                ::unlink(tmp_path.c_str());
                return 1;
            }
            // Compare entirely in C++ (2026-09-08 RCA: the old grep|awk|sha256sum
            // shell pipeline is POSIX-only — cmd.exe has no grep/awk/sha256sum,
            // so SHA verification could never succeed on Windows). Parse
            // SHA256SUMS ourselves and hash the download with
            // sha256_file_stream() (in-tree, OpenSSL-backed, all platforms).
            std::string expected_hash;
            {
                std::ifstream sums_in(sums_path);
                std::string line;
                while (std::getline(sums_in, line)) {
                    // Format: "<hex>  <filename>" (two spaces, sha256sum style)
                    const auto sp = line.find("  ");
                    if (sp == std::string::npos) continue;
                    std::string file = line.substr(sp + 2);
                    while (!file.empty() && (file.back() == '\r' || file.back() == '\n'))
                        file.pop_back();
                    // Asset lines use the flat name; source tarballs keep the
                    // versioned name — match either against binary_name.
                    const auto base = file.find_last_of('/');
                    if (base != std::string::npos) file = file.substr(base + 1);
                    if (file == binary_name) {
                        expected_hash = line.substr(0, sp);
                        break;
                    }
                }
            }
            std::transform(expected_hash.begin(), expected_hash.end(),
                           expected_hash.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const std::string actual_hash = bs::mesh::sha256_file_stream(tmp_path);
            bool hash_ok = expected_hash.size() == 64 && actual_hash.size() == 64 &&
                           expected_hash == actual_hash;
            ::unlink(sums_path.c_str());
            if (!hash_ok) {
                std::cerr << "upgrade: SHA256 verification FAILED — download tampered or corrupt\n"
                          << "  expected: " << expected_hash << "\n"
                          << "  actual:   " << actual_hash << "\n";
                ::unlink(tmp_path.c_str());
                return 1;
            }
            std::cout << "→ SHA256 verified\n";
            bs::log::get("upgrade")->info("SHA256 verified");
        }

        // Verify the binary runs and check version
        // Capture output. Quote via shell_arg_quote — cmd.exe (Windows) does
        // not treat single quotes as quoting, so the old "'path' --version"
        // form failed with "cannot find the path specified" (2026-09-08 RCA).
        std::string reported_version;
        {
            std::string cmd = bs::mesh::shell_arg_quote(tmp_path) + " --version 2>&1";
            FILE* p = BS_POPEN(cmd.c_str(), "r");
            if (p) {
                char buf[256];
                if (fgets(buf, sizeof(buf), p)) reported_version = buf;
                BS_PCLOSE(p);
            }
        }
        // Trim
        while (!reported_version.empty() && (reported_version.back() == '\n' || reported_version.back() == '\r'))
            reported_version.pop_back();

        if (reported_version.empty()) {
            std::cerr << "upgrade: downloaded binary failed to execute\n";
            ::unlink(tmp_path.c_str());
            return 1;
        }

        std::cout << "→ Downloaded version: " << reported_version << "\n";
        if (reported_version != tag) {
            std::cerr << "upgrade: downloaded binary reports " << reported_version
                      << "; expected " << tag << "\n";
            ::unlink(tmp_path.c_str());
            return 1;
        }

        // Skip if already up to date
        if (reported_version == std::string(bs::mesh::kBridgeSessionsVersion)) {
            std::cout << "→ Already up to date.\n";
            ::unlink(tmp_path.c_str());
            return 0;
        }

        // (already chmod'd above)

        // Developer ID — from BS_DEV_ID only (do not hardcode operator name in tree).
        // On macOS we refuse silent ad-hoc fallback: an unsigned/adhoc binary in
        // ~/.local/bin is often SIGKILL'd by Gatekeeper (exit 137).
        const char* env_dev_id = std::getenv("BS_DEV_ID");
        std::string dev_id = env_dev_id ? env_dev_id : "";

#ifdef __APPLE__
        {
            // Prefer BS_DEV_ID; else first Developer ID Application identity in keychain.
            if (dev_id.empty()) {
                FILE* p = BS_POPEN(
                    "security find-identity -v -p codesigning 2>/dev/null | "
                    "grep -F 'Developer ID Application' | head -1 | "
                    "sed -E 's/.*\"(.+)\"/\\1/'",
                    "r");
                if (p) {
                    char buf[512] = {};
                    if (fgets(buf, sizeof(buf), p)) dev_id = buf;
                    BS_PCLOSE(p);
                    while (!dev_id.empty() &&
                           (dev_id.back() == '\n' || dev_id.back() == '\r'))
                        dev_id.pop_back();
                }
            }
            // Dist binaries from GitHub are already Developer ID signed — verify
            // first; only re-sign when the download is ad-hoc / unsigned.
            std::string xattr_cmd = "xattr -cr '" + tmp_path + "' 2>/dev/null";
            std::system(xattr_cmd.c_str());
            std::string verify_cmd =
                "codesign --verify --strict '" + tmp_path + "' 2>/dev/null && "
                "codesign -dvv '" + tmp_path +
                "' 2>&1 | grep -q 'TeamIdentifier='";
            if (std::system(verify_cmd.c_str()) == 0) {
                std::cout << "→ Pre-signed Developer ID binary (verified)\n";
            } else {
                if (dev_id.empty()) {
                    std::cerr << "upgrade: binary is not Developer ID signed and "
                                 "no BS_DEV_ID / keychain identity found.\n"
                              << "  Refusing ad-hoc install (macOS may SIGKILL).\n";
                    ::unlink(tmp_path.c_str());
                    return 1;
                }
                std::string sign_cmd =
                    "codesign --force --options runtime --timestamp --sign '" +
                    dev_id + "' --identifier com.minddragon.bridgesessions '" +
                    tmp_path + "' 2>&1";
                int sign_rc = std::system(sign_cmd.c_str());
                if (sign_rc != 0) {
                    std::cerr << "upgrade: Developer ID codesign FAILED for identity:\n  "
                              << dev_id << "\n"
                              << "  Refusing ad-hoc fallback.\n";
                    ::unlink(tmp_path.c_str());
                    return 1;
                }
                std::cout << "→ Signed with Developer ID\n";
            }
        }
#endif

        // Arm the self-healing rollback watchdog BEFORE stopping the daemon.
        // Detached (setsid), so it survives both this process and the daemon
        // restart. If the new daemon never binds the mesh port within ~80s,
        // it restores bin_path from old_path and starts the daemon — a peer
        // must never strand offline because an upgrade failed mid-flight
        // (operator mandate: upgrades are seamless, quick, painless,
        // non-destructive).
        {
            std::string listen_port;
            {
                // Best-effort: read the configured mesh listen port for the
                // liveness probe; empty port falls back to version-only
                // verification below (the watchdog then also checks the
                // binary version, not just the port).
                std::FILE* pf = std::fopen((home_dir + "/config").c_str(), "r");
                if (pf) {
                    char line[256];
                    while (std::fgets(line, sizeof(line), pf)) {
                        std::string l(line);
                        const std::string key = "node.listen";
                        if (l.rfind(key, 0) == 0) {
                            auto pos = l.rfind(':');
                            if (pos != std::string::npos) {
                                std::string p;
                                for (size_t k = pos + 1; k < l.size(); ++k) {
                                    if (l[k] >= '0' && l[k] <= '9') p += l[k];
                                    else if (!p.empty()) break;
                                }
                                listen_port = p;
                            }
                        }
                    }
                    std::fclose(pf);
                }
            }
            std::string start_cmd = "systemctl --user start bridgesessions.service";
#if defined(__APPLE__)
            start_cmd = "launchctl kickstart -k gui/$(id -u)/com.bridgesessions.mesh";
#elif defined(_WIN32)
            start_cmd = "schtasks /run /tn BridgeSessions";
#endif
            bs::mesh::arm_upgrade_watchdog(bin_path, bin_path + ".old", start_cmd,
                                           listen_port.empty() ? "19949" : listen_port,
                                           home_dir);
        }

        // Stop daemon before swap — SAFELY.
        //
        // 2026-09-03 fleet incident: `pkill -9 -f 'bridgesessions --config'`
        // SIGKILLed the daemon mid-flight, skipping the graceful-shutdown
        // session persist, and left hosted session-workers uncoordinated.
        // Workers are detached (systemd-run scope / setsid) and survive the
        // daemon, but a SIGKILL also races `systemctl --user stop` against
        // `Restart=on-failure`, so a mid-restart daemon could re-exec the OLD
        // binary right after the swap.
        //
        // Safe sequence:
        //   1. `systemctl --user stop` (graceful SIGTERM) — the daemon's
        //      shutdown path persists live sessions, and detached workers
        //      keep their shells running. NEVER pkill -9 the daemon, and
        //      NEVER kill session-workers: one of them may be hosting the
        //      very terminal running this upgrade (self-kill = dead shell).
        //   2. Wait for the daemon to actually exit (poll IPC port), with a
        //      bounded timeout, so the swap never races a live daemon.
        std::cout << "→ Stopping daemon (sessions keep running)...\n";
        bs::log::get("upgrade")->info("stopping daemon (graceful)");
        pause_mesh_daemon();
#ifdef _WIN32
        Sleep(2000);
#else
        {
            // pause_mesh_daemon() issued the graceful stop (and masked the
            // unit so Restart=on-failure cannot resurrect the old binary
            // mid-swap). Poll the CLI IPC port until the daemon is gone —
            // up to 10s — instead of a blind sleep(2). No pkill: the daemon
            // was started by systemd/launchd and answers to it.
            const std::string probe =
                "bridgesessions stats >/dev/null 2>&1";   // IPC connect probe
            for (int i = 0; i < 100; ++i) {
                // Daemon is down when a STATS probe reports not-running.
                if (std::system(probe.c_str()) != 0) break;
                ::usleep(100 * 1000);
            }
        }
#endif

        // Atomic swap: install via a sibling temp name + rename(2).
        //
        // NEVER cp onto the live path: cp opens the destination in place, so
        // it fails with ETXTBSY while any process (daemon, session-worker,
        // another CLI) still execs the old inode — the exact failure that
        // half-broke the 2026-09-03 fleet upgrade. rename(2) only swaps the
        // directory entry, so running processes on the old inode are untouched
        // (they show "(deleted)" in /proc/<pid>/exe until they exit) and every
        // new exec gets the new binary.
        // The rename fallback path is kept for exotic cross-device $HOME
        // setups: install to <dir>/.bridgesessions.upg then rename again.
        std::string old_path = bin_path + ".old";
        std::string stage_path = bin_path + ".upg-new";
        std::cout << "→ Swapping binary...\n";
        bs::log::get("upgrade")->info("swapping binary");
        // Windows: after schtasks /end the old process can hold the exe lock
        // for a few seconds while it exits (2026-09-08 RCA: blind Sleep(2000)
        // then a single rename raced the lock). Retry the first rename for up
        // to ~15s before giving up; rename(2)/MoveFileEx never corrupts a
        // running image, it just fails while the lock is held.
#ifdef _WIN32
        {
            bool moved = ::rename(bin_path.c_str(), old_path.c_str()) == 0;
            for (int i = 0; !moved && i < 30; ++i) {
                Sleep(500);
                moved = ::rename(bin_path.c_str(), old_path.c_str()) == 0;
            }
            if (!moved) {
                std::cerr << "upgrade: old binary still locked — rolling back\n";
                ::unlink(tmp_path.c_str());
                resume_mesh_daemon(current_exe_path(argv[0]), home_dir + "/config");
                return 1;
            }
        }
#else
        ::rename(bin_path.c_str(), old_path.c_str());  // may fail if not exists
#endif
        if (::rename(tmp_path.c_str(), bin_path.c_str()) != 0) {
            // First rename failed (cross-device tmp vs bin). Stage on the SAME
            // filesystem as bin_path, then rename — still never cp-in-place.
            std::error_code copy_ec;
            std::filesystem::copy_file(tmp_path, stage_path,
                std::filesystem::copy_options::overwrite_existing, copy_ec);
            if (copy_ec) {
                std::cerr << "upgrade: staging copy failed — rolling back\n";
                ::rename(old_path.c_str(), bin_path.c_str());
                ::unlink(tmp_path.c_str());
                resume_mesh_daemon(current_exe_path(argv[0]), home_dir + "/config");
                return 1;
            }
            ::chmod(stage_path.c_str(), 0755);
            if (::rename(stage_path.c_str(), bin_path.c_str()) != 0) {
                std::cerr << "upgrade: binary swap failed (staged rename) — rolling back\n";
                ::rename(old_path.c_str(), bin_path.c_str());
                ::unlink(tmp_path.c_str());
                ::unlink(stage_path.c_str());
                resume_mesh_daemon(current_exe_path(argv[0]), home_dir + "/config");
                return 1;
            }
        }
#ifndef _WIN32
        chmod(bin_path.c_str(), 0755);
#endif
        ::unlink(tmp_path.c_str());

#ifdef __APPLE__
        // Update .app bundle if it exists
        std::string app_bin = "/Applications/BridgeSessions.app/Contents/MacOS/bridgesessions";
        if (std::filesystem::exists("/Applications/BridgeSessions.app")) {
            std::filesystem::copy_file(bin_path, app_bin,
                std::filesystem::copy_options::overwrite_existing);
            std::string app_sign = "codesign --force --deep --sign '" + dev_id + "' /Applications/BridgeSessions.app 2>/dev/null";
            std::system(app_sign.c_str());
        }
#endif

        // Restart daemon
        std::cout << "→ Starting daemon...\n";
        bs::log::get("upgrade")->info("restarting daemon");
        resume_mesh_daemon(current_exe_path(argv[0]), home_dir + "/config");
#ifdef _WIN32
        Sleep(3000);
#else
        sleep(3);
#endif

        // Verify
        // Verify. shell_arg_quote for the path (cmd.exe single-quote pitfall).
        std::string verify_final = bs::mesh::shell_arg_quote(bin_path) + " --version 2>&1";
        FILE* p = BS_POPEN(verify_final.c_str(), "r");
        std::string final_version;
        if (p) {
            char buf[256];
            if (fgets(buf, sizeof(buf), p)) final_version = buf;
            BS_PCLOSE(p);
        }
        while (!final_version.empty() && (final_version.back() == '\n' || final_version.back() == '\r'))
            final_version.pop_back();

        if (final_version == reported_version) {
            std::cout << "✓ Upgraded to " << final_version << "\n";
            // Clean up old binary
            ::unlink(old_path.c_str());
        } else {
            std::cerr << "✗ Upgrade verification failed: expected " << reported_version
                      << " got " << final_version << "\n";
            std::cerr << "  Restoring old binary...\n";
            ::rename(old_path.c_str(), bin_path.c_str());
            // Restart daemon after rollback so system isn't left with daemon stopped
            std::cerr << "  Restarting daemon...\n";
            resume_mesh_daemon(current_exe_path(argv[0]), home_dir + "/config");
            return 1;
        }
        return 0;
    }
    if (telemetry_cmd_app->parsed()) {
        std::string ipc = daemon_simple_ipc("TELEMETRY", 3000, home_dir);
        if (!ipc.empty() && ipc.rfind("ERROR", 0) != 0) {
            if (telemetry_json) {
                std::cout << ipc << "\n";
            } else {
                // Human-readable format
                try {
                    auto j = nlohmann::json::parse(ipc);
                    if (j.is_array() && j.empty()) {
                        std::cout << "No transfer telemetry recorded.\n";
                    } else {
                        for (auto& e : j) {
                            std::cout << "[" << e.value("dir", "") << "] "
                                      << e.value("file", "") << " -> " << e.value("peer", "") << "\n"
                                      << "  " << e.value("bytes", 0ULL) << " bytes, "
                                      << e.value("chunks", 0U) << " chunks, "
                                      << e.value("total_wall_ms", 0LL) << " ms wall\n"
                                      << "  rate=" << e.value("rate_mibs", 0.0) << " MiB/s"
                                      << " overhead=" << e.value("overhead_pct", 0.0) << "%\n"
                                      << "  select=" << e.value("select_total_ms", 0LL) << "ms"
                                      << " write=" << e.value("write_total_ms", 0LL) << "ms"
                                      << " drain=" << e.value("drain_total_ms", 0LL) << "ms\n"
                                      << "  select_mean=" << e.value("select_mean_us", 0.0) << "us"
                                      << " write_mean=" << e.value("write_mean_us", 0.0) << "us"
                                      << " drain_mean=" << e.value("drain_mean_us", 0.0) << "us\n";
                        }
                    }
                } catch (...) {
                    std::cout << ipc << "\n";
                }
            }
            return 0;
        }
        std::cout << "{}" << "\n";
        return 0;
    }
    if (file_send_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string result = mc.file_send(file_send_peer, file_send_path, file_send_wait,
                                          file_send_dest);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (file_recv_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string dest = !file_recv_to.empty() ? file_recv_to : file_recv_local;
        std::string result = mc.file_recv(file_recv_peer, file_recv_remote, dest, file_recv_wait);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (capvid_cmd->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        bs::mesh::CuaVideoCaptureMsg req;
        req.fps = static_cast<uint8_t>(capvid_fps);
        req.duration_sec = static_cast<uint16_t>(capvid_dur);
        req.quality = static_cast<uint8_t>(capvid_quality);
        req.max_width = static_cast<uint16_t>(capvid_maxw);
        std::string result = mc.capture_video(capvid_peer, req);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }

    // ── bs cua dispatch ──────────────────────────────────────────
    auto parse_mods = [](const std::string& s) -> uint8_t {
        uint8_t m = 0;
        if (s.find("ctrl") != std::string::npos) m |= 1;
        if (s.find("shift") != std::string::npos) m |= 2;
        if (s.find("alt") != std::string::npos) m |= 4;
        if (s.find("meta") != std::string::npos) m |= 8;
        return m;
    };
    auto map_button = [](const std::string& b) -> uint8_t {
        if (b == "right") return 2;
        if (b == "middle") return 1;
        return 0; // left
    };

    if (cua_screen->parsed() || cua_capture->parsed() || cua_click->parsed() ||
        cua_move->parsed() || cua_type->parsed() || cua_key->parsed() ||
        cua_scroll->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);

        if (cua_screen->parsed()) {
            auto resp = mc.send_cua_request(cua_peer, 0, 0, 0, 0, 0, 0, "");
            if (resp.status != 0) { std::cerr << "ERROR: " << resp.error << "\n"; return 1; }
            std::cout << resp.screen_w << "x" << resp.screen_h << "\n";
            return 0;
        }
        if (cua_capture->parsed()) {
            // action 6=capture; pass format+quality in text field as "fmt:quality"
            std::string fmt_param = std::to_string(cua_format) + ":" + std::to_string(cua_quality);
            auto resp = mc.send_cua_request(cua_peer, 6, 0, 0, 0, 0, 0, fmt_param);
            if (resp.status != 0) { std::cerr << "ERROR: " << resp.error << "\n"; return 1; }
            if (resp.data.empty()) { std::cerr << "ERROR: no capture data returned\n"; return 1; }
            // Sniff magic so BMP (helper) / JPEG / PNG all save correctly regardless
            // of format field history (0=PNG, 1=legacy, 2=JPEG, 3=BMP).
            auto sniff_kind = [](const std::vector<uint8_t>& d, uint8_t fmt) -> const char* {
                if (d.size() >= 2 && d[0] == 'B' && d[1] == 'M') return "bmp";
                if (d.size() >= 2 && d[0] == 0xFF && d[1] == 0xD8) return "jpeg";
                if (d.size() >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
                    return "png";
                if (fmt == 2) return "jpeg";
                if (fmt == 3) return "bmp";
                return "png";
            };
            const char* kind = sniff_kind(resp.data, resp.format);
            if (!cua_output.empty()) {
                std::ofstream f(cua_output, std::ios::binary);
                f.write(reinterpret_cast<const char*>(resp.data.data()),
                        static_cast<std::streamsize>(resp.data.size()));
                std::cout << "Saved " << resp.data.size() << " bytes (" << kind
                          << ") to " << cua_output << "\n";
            } else {
                // Binary to stdout for piping
                std::fwrite(resp.data.data(), 1, resp.data.size(), stdout);
                std::cerr << resp.data.size() << " bytes (" << kind << ")\n";
            }
            return 0;
        }
        if (cua_click->parsed()) {
            auto resp = mc.send_cua_request(cua_peer, 4,
                static_cast<int16_t>(cua_x), static_cast<int16_t>(cua_y),
                map_button(cua_button), 0, 0, "");
            if (resp.status != 0) { std::cerr << "ERROR: " << resp.error << "\n"; return 1; }
            std::cout << "Clicked at (" << cua_x << "," << cua_y << ") button=" << cua_button << "\n";
            return 0;
        }
        if (cua_move->parsed()) {
            auto resp = mc.send_cua_request(cua_peer, 3,
                static_cast<int16_t>(cua_x), static_cast<int16_t>(cua_y),
                0, 0, 0, "");
            if (resp.status != 0) { std::cerr << "ERROR: " << resp.error << "\n"; return 1; }
            std::cout << "Moved to (" << cua_x << "," << cua_y << ")\n";
            return 0;
        }
        if (cua_type->parsed()) {
            auto resp = mc.send_cua_request(cua_peer, 2, 0, 0, 0, 0, 0, cua_text);
            if (resp.status != 0) { std::cerr << "ERROR: " << resp.error << "\n"; return 1; }
            std::cout << "Typed " << cua_text.size() << " chars\n";
            return 0;
        }
        if (cua_key->parsed()) {
            auto resp = mc.send_cua_request(cua_peer, 1, 0, 0, 0,
                static_cast<uint32_t>(cua_code), parse_mods(cua_modifiers), "");
            if (resp.status != 0) { std::cerr << "ERROR: " << resp.error << "\n"; return 1; }
            std::cout << "Pressed HID key 0x" << std::hex << cua_code << std::dec << "\n";
            return 0;
        }
        if (cua_scroll->parsed()) {
            // action 5=wheel; direction: down = negative y, up = positive y
            int16_t scroll_y = cua_direction == "down"
                ? static_cast<int16_t>(-cua_amount)
                : static_cast<int16_t>(cua_amount);
            auto resp = mc.send_cua_request(cua_peer, 5, 0, scroll_y, 0, 0, 0, "");
            if (resp.status != 0) { std::cerr << "ERROR: " << resp.error << "\n"; return 1; }
            std::cout << "Scrolled " << cua_direction << " " << cua_amount << " ticks\n";
            return 0;
        }
    }

    if (edit_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        mc.edit_peer(edit_target);
        return 0;
    }
    if (rscript_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        return mc.run_script(rscript_peer, rscript_file, rscript_interpreter);
    }
    if (job_run_app->parsed()) {
        // Job JSON shape:
        // {
        //   "job_id": "optional",
        //   "stop_on_error": false,
        //   "steps": [
        //     {"id": "1", "shell": "bash", "cmd": "uname -a"},
        //     {"id": "2", "argv": ["ls", "-la", "/tmp"]}
        //   ]
        // }
        // Runs as a remote bash runner that emits one JSON line per step, then a summary.
        namespace fs = std::filesystem;
        std::string raw;
        if (job_file == "-") {
            raw.assign(std::istreambuf_iterator<char>(std::cin),
                       std::istreambuf_iterator<char>());
        } else {
            std::ifstream jf(job_file);
            if (!jf) {
                std::cerr << "ERROR cannot read job file: " << job_file << "\n";
                return 1;
            }
            raw.assign(std::istreambuf_iterator<char>(jf),
                       std::istreambuf_iterator<char>());
        }
        nlohmann::json job;
        try {
            job = nlohmann::json::parse(raw.empty() ? "{}" : raw);
        } catch (const std::exception& e) {
            std::cerr << "ERROR invalid job JSON: " << e.what() << "\n";
            return 1;
        }
        if (!job.contains("steps") || !job["steps"].is_array() || job["steps"].empty()) {
            std::cerr << "ERROR job JSON requires non-empty \"steps\" array\n";
            return 1;
        }
        const bool stop = job_stop_on_error ||
                          (job.value("stop_on_error", false));
        std::string job_id = job.value("job_id", "bs-job");
        // Build a bash runner that never aborts the whole stack on one failure
        // unless stop_on_error is set.
        std::ostringstream runner;
        runner << "#!/usr/bin/env bash\n"
               << "# generated by bs job run — per-step results, optional continue\n"
               << "set +e\n"
               << "JOB_ID=" << std::quoted(job_id) << "\n"
               << "STOP_ON_ERROR=" << (stop ? "1" : "0") << "\n"
               << "echo \"{\\\"job_id\\\":\\\"$JOB_ID\\\",\\\"event\\\":\\\"start\\\"}\"\n"
               << "FAILS=0\n";
        size_t idx = 0;
        for (const auto& step : job["steps"]) {
            ++idx;
            std::string sid = step.value("id", std::to_string(idx));
            bool cont = step.value("continue_on_error", !stop);
            std::string cmd;
            if (step.contains("argv") && step["argv"].is_array() && !step["argv"].empty()) {
                // Prefer argv form — no shell metacharacter surprises
                std::ostringstream parts;
                for (const auto& a : step["argv"]) {
                    if (!a.is_string()) continue;
                    parts << std::quoted(a.get<std::string>()) << " ";
                }
                cmd = parts.str();
            } else if (step.contains("cmd") && step["cmd"].is_string()) {
                cmd = step["cmd"].get<std::string>();
            } else if (step.contains("shell") && step["shell"].is_string() &&
                       step.contains("script") && step["script"].is_string()) {
                // shell + script: e.g. powershell -NoProfile -Command '…'
                std::string sh = step["shell"].get<std::string>();
                std::string sc = step["script"].get<std::string>();
                std::ostringstream cq;
                if (sh == "powershell" || sh == "pwsh") {
                    cq << "powershell -NoProfile -Command " << std::quoted(sc);
                } else if (sh == "python" || sh == "python3") {
                    cq << "python3 -c " << std::quoted(sc);
                } else {
                    cq << "bash -lc " << std::quoted(sc);
                }
                cmd = cq.str();
            } else {
                std::cerr << "ERROR step " << sid << " needs argv, cmd, or shell+script\n";
                return 1;
            }
            // Base64-embed the command so remote eval never fights quoting.
            std::string b64;
            {
                static const char* B =
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                const unsigned char* data =
                    reinterpret_cast<const unsigned char*>(cmd.data());
                const size_t len = cmd.size();
                for (size_t j = 0; j < len; j += 3) {
                    unsigned a = data[j];
                    unsigned b = (j + 1 < len) ? data[j + 1] : 0;
                    unsigned c = (j + 2 < len) ? data[j + 2] : 0;
                    unsigned n = (a << 16) | (b << 8) | c;
                    b64.push_back(B[(n >> 18) & 63]);
                    b64.push_back(B[(n >> 12) & 63]);
                    b64.push_back((j + 1 < len) ? B[(n >> 6) & 63] : '=');
                    b64.push_back((j + 2 < len) ? B[n & 63] : '=');
                }
            }
            runner << "{\n"
                   << "  SID=" << std::quoted(sid) << "\n"
                   << "  STEP_B64='" << b64 << "'\n"
                   << "  STEP_CMD=$(printf '%s' \"$STEP_B64\" | base64 -d 2>/dev/null || printf '%s' \"$STEP_B64\" | base64 -D 2>/dev/null)\n"
                   << "  OUTF=$(mktemp 2>/dev/null || echo /tmp/bs-job-$$-$SID.out)\n"
                   << "  ERRF=$(mktemp 2>/dev/null || echo /tmp/bs-job-$$-$SID.err)\n"
                   << "  set +e\n"
                   << "  eval \"$STEP_CMD\" >\"$OUTF\" 2>\"$ERRF\"\n"
                   << "  EC=$?\n"
                   << "  set -e\n"
                   << "  # JSON-escape stdout/stderr (truncate 8k each)\n"
                   << "  py_escape() { python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()[:8192]))' 2>/dev/null || "
                      "python -c 'import json,sys; print(json.dumps(sys.stdin.read()[:8192]))' 2>/dev/null || echo '\"\"'; }\n"
                   << "  SOUT=$(cat \"$OUTF\" | py_escape)\n"
                   << "  SERR=$(cat \"$ERRF\" | py_escape)\n"
                   << "  rm -f \"$OUTF\" \"$ERRF\"\n"
                   << "  echo \"{\\\"job_id\\\":\\\"$JOB_ID\\\",\\\"step\\\":\\\"$SID\\\",\\\"exit\\\":$EC,"
                      "\\\"stdout\\\":$SOUT,\\\"stderr\\\":$SERR}\"\n"
                   << "  if [ \"$EC\" -ne 0 ]; then\n"
                   << "    FAILS=$((FAILS+1))\n"
                   << "    if [ \"$STOP_ON_ERROR\" = \"1\" ] || [ \"" << (cont ? "0" : "1") << "\" = \"1\" ]; then\n"
                   << "      echo \"{\\\"job_id\\\":\\\"$JOB_ID\\\",\\\"event\\\":\\\"aborted\\\",\\\"at\\\":\\\"$SID\\\",\\\"fails\\\":$FAILS}\"\n"
                   << "      exit $EC\n"
                   << "    fi\n"
                   << "  fi\n"
                   << "}\n";
        }
        runner << "echo \"{\\\"job_id\\\":\\\"$JOB_ID\\\",\\\"event\\\":\\\"done\\\",\\\"fails\\\":$FAILS}\"\n"
               << "exit $FAILS\n";

        // Write runner to temp and run-script it
        std::string tmp_runner;
        tmp_runner = bs::mesh::create_private_temp_file("job", ".sh");
        if (tmp_runner.empty()) {
            std::cerr << "ERROR cannot create temp job runner\n";
            return 1;
        }
        {
            std::ofstream of(tmp_runner, std::ios::trunc);
            if (!of) {
                std::cerr << "ERROR cannot write temp job runner\n";
                return 1;
            }
            of << runner.str();
        }
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        int rc = mc.run_script(job_peer, tmp_runner, "bash");
        fs::remove(tmp_runner);
        return rc;
    }
    // ── bs script dispatch ───────────────────────────────────────
    if (script_add_app->parsed()) {
        bs::mesh::MeshController mc(bs::mesh::load_config(config_path), home_dir);
        std::string result = mc.script_add(script_add_file, script_add_name);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (script_list_app->parsed()) {
        bs::mesh::MeshController mc(bs::mesh::load_config(config_path), home_dir);
        mc.script_list();
        return 0;
    }
    if (script_push_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string result = mc.script_push(script_push_name, script_push_peer);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (script_run_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        return mc.script_run(script_run_name, script_run_peer, script_run_args);
    }
    if (script_remove_app->parsed()) {
        bs::mesh::MeshController mc(bs::mesh::load_config(config_path), home_dir);
        std::string result = mc.script_remove(script_remove_name);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (pane_publish->parsed()) {
        // type whitelist
        if (pane_type != "comms" && pane_type != "documents") {
            std::cerr << "ERROR --type must be comms or documents\n";
            return 2;
        }
        // session / title are used as directory + file names -> restrict to a safe charset
        auto safe_name = [](const std::string& s) {
            return std::all_of(s.begin(), s.end(), [](unsigned char c) {
                return std::isalnum(c) || c == ' ' || c == '.' || c == '_' || c == '-';
            });
        };
        if (!safe_name(pane_session) || !safe_name(pane_title)) {
            std::cerr << "ERROR invalid --session or --title (use alnum, space, . _ -)\n";
            return 2;
        }
        // locate the bridgepanel helper on PATH (installed to ~/.local/bin)
        std::string bin = "bridgepanel";
        FILE* which = BS_POPEN("command -v bridgepanel 2>/dev/null", "r");
        if (which) {
            char buf[512];
            std::string found;
            while (std::fgets(buf, sizeof(buf), which)) found += buf;
            BS_PCLOSE(which);
            auto nl = found.find('\n');
            if (nl != std::string::npos) found = found.substr(0, nl);
            if (!found.empty()) bin = found;
        }
        // single-quote escape for safe shell passing of the file path
        auto sq = [](const std::string& s) {
            std::string o = "'";
            for (char c : s) {
                if (c == '\'') o += "'\\''";
                else o += c;
            }
            return o + "'";
        };
        std::string cmd = bin + " publish --session " + sq(pane_session)
                          + " --type " + sq(pane_type)
                          + (pane_title.empty() ? std::string() : " --title " + sq(pane_title))
                          + " " + sq(pane_file);
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "ERROR bridgepanel publish failed (rc=" << rc << ")\n";
            return 1;
        }
        return 0;
    }
    // ── bs sync pair dispatch ─────────────────────────────────────
    if (sp_init->parsed()) {
        using namespace bs::sync;
        std::string peer, remote_dir;
        if (!sync_parse_peer_dir(sp_peer_spec, peer, remote_dir)) {
            std::cerr << "ERROR peer-dir must be peer:/remote/dir (POSIX remote path)\n";
            return 2;
        }
        std::error_code lec;
        if (!fs::exists(sp_local, lec) || !fs::is_directory(sp_local, lec)) {
            std::cerr << "ERROR local dir not found: " << sp_local << "\n";
            return 2;
        }
        auto pairs = sync_pairs_load(home_dir);
        SyncPairSpec p;
        p.id = sync_make_pair_id(pairs, sp_local, peer);
        p.local_dir = sp_local;
        p.peer = peer;
        p.remote_dir = remote_dir;
        p.via_run_script = sp_via_run_script;
        p.created_at = "created";
        // Dry-run manifest scan. mtime is never consulted: classification is
        // content-hash only (cross-host skew cannot regress data).
        SyncIndex current = sync_scan_dir(p.local_dir);
        p.clock = sync_clock_tick(p); // every manifest takes one Lamport tick
        SyncManifest m = sync_diff(p.id, {}, current, p.clock);
        p.approved = sp_approve;
        if (!sp_approve) {
            if (!sync_manifest_save(home_dir, m)) {
                std::cerr << "ERROR cannot persist manifest under " << home_dir << "/state\n";
                return 1;
            }
        } else {
            fs::remove(sync_manifest_dir(home_dir) / (p.id + ".json"), lec);
        }
        pairs.push_back(p);
        if (!sync_pairs_save(home_dir, pairs)) {
            std::cerr << "ERROR cannot write " << sync_pairs_path(home_dir) << "\n";
            return 1;
        }
        std::cout << "pair " << p.id << ": " << p.local_dir << " <-> "
                  << p.peer << ":" << p.remote_dir << "\n"
                  << sync_manifest_text(m);
        if (sp_approve) {
            std::cout << "APPROVED — run `bs sync pair run " << p.id
                      << "` to transfer (" << m.ops.size() << " ops)\n";
        } else {
            std::cout << "DRY RUN — nothing transferred. Re-run with --approve or\n"
                      << "`bs sync pair approve " << p.id << "`, then `bs sync pair run "
                      << p.id << "`.\n";
        }
        return 0;
    }
    if (sp_approve_cmd->parsed() || sp_status->parsed() || sp_run->parsed()) {
        using namespace bs::sync;
        auto pairs = sync_pairs_load(home_dir);
        if (pairs.empty()) {
            std::cout << "no sync pairs (create one with `bs sync pair init <dir> peer:/dir`)\n";
            return 0;
        }
        auto find_pair = [&](const std::string& id) -> SyncPairSpec* {
            for (auto& p : pairs)
                if (p.id == id) return &p;
            return nullptr;
        };
        if (sp_status->parsed()) {
            for (auto& p : pairs) {
                if (!sp_id.empty() && p.id != sp_id) continue;
                SyncIndex current = sync_scan_dir(p.local_dir);
                SyncManifest prev;
                bool have_prev = sync_manifest_load(home_dir, p.id, prev);
                SyncIndex applied = have_prev ? sync_index_from_manifest(prev) : SyncIndex{};
                SyncManifest m = sync_diff(p.id, applied, current, p.clock);
                std::cout << p.id << ": " << p.local_dir << " <-> " << p.peer
                          << ":" << p.remote_dir
                          << (p.via_run_script ? " (via run-script)" : "")
                          << " | " << current.size() << " files"
                          << " | pending changes: " << m.ops.size()
                          << " | logical clock: " << p.clock
                          << " | " << (p.approved ? "approved" : "awaiting approval")
                          << " (ordering: logical clock + sha256; mtime never used)\n";
            }
            return 0;
        }
        SyncPairSpec* p = find_pair(sp_id);
        if (!p) {
            std::cerr << "ERROR no such pair: " << sp_id << "\n";
            return 1;
        }
        if (sp_approve_cmd->parsed()) {
            if (p->approved) {
                std::cout << "pair " << p->id << " already approved\n";
                return 0;
            }
            SyncManifest m;
            if (!sync_manifest_load(home_dir, p->id, m)) {
                std::cerr << "ERROR no pending manifest for " << p->id
                          << " (run `bs sync pair init` first)\n";
                return 1;
            }
            p->approved = true;
            if (!sync_pairs_save(home_dir, pairs)) {
                std::cerr << "ERROR cannot write " << sync_pairs_path(home_dir) << "\n";
                return 1;
            }
            std::cout << "approved " << p->id << " (" << m.ops.size()
                      << " ops at logical clock " << m.clock << ")\n";
            return 0;
        }
        // sp_run: one-shot apply of the approved manifest via existing
        // file-transfer verbs (resumable, hash-verified). No daemon-embedded
        // loop in this increment. POSIX push for Unix peers; Windows peers
        // go through run-script with no PowerShell bodies pushed.
        if (!p->approved) {
            std::cerr << "ERROR pair " << p->id
                      << " is not approved — run `bs sync pair approve " << p->id
                      << "` first (dry-run manifest gate)\n";
            return 1;
        }
        SyncManifest m;
        if (!sync_manifest_load(home_dir, p->id, m)) {
            std::cerr << "ERROR no manifest for " << p->id << "\n";
            return 1;
        }
        std::error_code lec;
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        size_t ok = 0, fail = 0;
        for (const auto& op : m.ops) {
            if (op.kind == "delete") {
                // Deletes are applied remotely via a shell verb only after the
                // manifest gate; a failed delete is reported, not retried.
                std::cout << "delete " << op.relpath << " (remote) — skipped in one-shot run;"
                             " apply remotely with `bs " << p->peer << " rm` equivalent\n";
                continue;
            }
            const std::string local = (fs::path(p->local_dir) / op.relpath).string();
            const std::string dest =
                p->remote_dir + (p->remote_dir.back() == '/' ? "" : "/") + op.relpath;
            std::string res = mc.file_send(p->peer, local, true, dest);
            if (res.rfind("ERROR", 0) == 0) {
                std::cerr << res << "\n";
                fail++;
            } else {
                std::cout << op.kind << " " << op.relpath << " -> " << p->peer
                          << ":" << dest << " OK\n";
                ok++;
            }
        }
        // Persist the applied manifest as the new baseline and drop pending.
        fs::remove(sync_manifest_dir(home_dir) / (p->id + ".json"), lec);
        for (auto& pp : pairs)
            if (pp.id == p->id) pp.approved = true;
        sync_pairs_save(home_dir, pairs);
        std::cout << "pair " << p->id << ": " << ok << " transferred, " << fail
                  << " failed, clock " << m.clock << "\n";
        return fail == 0 ? 0 : 1;
    }
    if (vfolder_sync->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        const std::string result = mc.sync_vfolder(vfolder_name);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (vfolder_list->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        std::cout << "=== Virtual folders ===\n";
        for (auto& v : cfg.vfolders) {
            std::cout << v.name << ": " << v.local_path << " <-> " << v.remote_peer << ":" << v.remote_path
                      << " (" << v.direction << ", every " << v.sync_interval_secs << "s)\n";
        }
        return 0;
    }
    if (vfolder_add->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::MeshConfig::VFolderEntry ve;
        ve.name = vfolder_name; ve.local_path = vfolder_local;
        ve.remote_peer = vfolder_peer; ve.remote_path = vfolder_remote;
        ve.sync_interval_secs = vfolder_interval;
        if (!vfolder_dir.empty()) ve.direction = vfolder_dir;
        cfg.vfolders.push_back(ve);
        if (!bs::mesh::save_config(config_path, cfg)) {
            std::cerr << "failed to write config: " << config_path << "\n";
            return 1;
        }
        std::cout << "added vfolder " << vfolder_name << "\n";
        return 0;
    }
    // CUA helper mode: run in user session for screen capture + input injection
    if (cua_helper_flag) {
        bs::log::get("cua-helper")->info("CUA helper starting (app_home={})", home_dir);
        return bs::mesh::run_cua_helper(home_dir);
    }

#ifndef _WIN32
    // Internal: per-session PTY worker (spawned by the daemon; not user-facing).
    // Hosts the PTY + shell outside the daemon process so sessions survive
    // daemon restarts and upgrades.
    if (session_worker_app->parsed()) {
        bs::mesh::worker::WorkerConfig wc;
        wc.socket_path = worker_socket;
        wc.session_name = worker_name;
        wc.command = worker_command;
        wc.cols = static_cast<uint16_t>(worker_cols);
        wc.rows = static_cast<uint16_t>(worker_rows);
        wc.term = worker_term;
        wc.app_home = worker_app_home.empty() ? home_dir : worker_app_home;
        auto res = bs::mesh::worker::run_session_worker_posix(wc);
        return res.exit_code;
    }
#endif

    // Bare `bs` from an interactive terminal: open the server → harness
    // selector instead of defaulting to daemon mode. The launchd/systemd
    // daemon runs with stdin not a terminal, so it still falls through to
    // daemon mode below (never blocks on a menu). `--daemon` always wins.
    if (app.get_subcommands().empty() && quick_peer.empty() &&
        !daemon_flag && bs::mesh::stdin_is_terminal()) {
        return cmd_connect_selector(config_path, home_dir, "", "");
    }

    // Default: daemon mode
    bs::log::get("daemon")->info("Daemon mode: loading config from {}", config_path);
    bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
    bs::log::get("daemon")->info("Config loaded: listen={}:{}, {} seed peers",
        cfg.listen_addr, cfg.listen_port, cfg.seeds.size());
    bs::mesh::bootstrap_identity(home_dir);
#ifdef _WIN32
    if (daemon_flag) {
        FreeConsole();
        FILE* nul = fopen("nul", "w");
        if (nul) { fclose(stdout); _dup2(_fileno(nul), _fileno(stdout)); fclose(nul); }
    }
#endif
    // POSIX --daemon forked right after CLI parse (before the log pool starts).
    bs::mesh::MeshController mc(cfg, home_dir);
#ifndef _WIN32
    mc.enable_session_hosting();   // workers keep shells alive across upgrades
#endif
    mc.run();
    return 0;
}

#endif
