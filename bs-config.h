// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-config.h — Config, paths, persistence, protocol logging
// Extracted from bs-protocol.h (R6 structural refactor, 2026-09-02)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace — parent file provides it.
#pragma once

// ────────────────────────────────────────────────────────────────────
// CONFIG PARSER — key=value config file parser
// ────────────────────────────────────────────────────────────────────

struct PeerEntry {
    std::string name;
    std::string addr;       // "host:port"
    std::string pubkey_hex; // learned via Hello, empty until then
    uint64_t last_seen = 0;
};

enum class HostPlatform : uint8_t { Windows, MacOS, Posix };

[[nodiscard]] std::string default_shell_for_platform(
    HostPlatform platform, bool pwsh_available) {
    switch (platform) {
        case HostPlatform::Windows:
            return pwsh_available ? "pwsh.exe -NoLogo"
                                  : "powershell.exe -NoLogo";
        case HostPlatform::MacOS:
            return "/bin/zsh -il";
        case HostPlatform::Posix:
            return "/bin/bash -l";
    }
    return "/bin/bash -l";
}

#ifdef _WIN32
[[nodiscard]] bool windows_executable_available(const wchar_t* executable) {
    DWORD needed = SearchPathW(nullptr, executable, nullptr, 0, nullptr, nullptr);
    return needed > 0;
}
#endif

[[nodiscard]] std::string platform_default_shell() {
#ifdef _WIN32
    return default_shell_for_platform(
        HostPlatform::Windows, windows_executable_available(L"pwsh.exe"));
#elif defined(__APPLE__)
    return default_shell_for_platform(HostPlatform::MacOS, false);
#else
    return default_shell_for_platform(HostPlatform::Posix, false);
#endif
}

struct MeshConfig {
    // Runtime provenance only; never serialized. The reload watcher must follow
    // the file actually loaded by --config rather than assuming root/config.
    std::string source_path;
    std::string node_name = "unnamed";
    std::string listen_addr = "0.0.0.0";
    uint16_t listen_port = 19949;
    size_t max_peers = 50;
    int gossip_interval_secs = 30;
    int reconnect_backoff_max_secs = 300;  // was 30 → dead-seed TLS-retry storm (RSS 3GB, 48% CPU)
    int join_window_max_secs = 300;
    int startup_wait_secs = 30;  // boot-time network readiness gate (0 = skip)
    std::string receive_dir_override;  // override default received/ path (for SYSTEM daemons)
    int ping_interval_secs = 5;
    int pong_timeout_secs = 30;
    // When true, offer `bridgesessions upgrade` to peers that reconnect with an
    // older Hello.version than this node (cooldown-limited). Peers that were
    // offline during a fleet upgrade catch up automatically when they return.
    bool auto_upgrade = true;
    // Minimum seconds between auto-upgrade attempts for the same peer.
    int auto_upgrade_cooldown_secs = 3600;
    // When true (default), outbound seed/discovered dials require pubkey= pin and
    // post-handshake cert/Hello identity binding. TLS fingerprint TOFU alone is
    // not sufficient for mesh trust (independent review 2026-07-16 P0-1).
    bool require_seed_pins = true;
    // Seconds a discovered (runtime-learned) peer may stay silent before it is
    // pruned. Durable `seed` peers are exempt — they persist offline and are
    // expected to return. 0 = keep discovered peers indefinitely (no pruning).
    int discovered_ttl_secs = 900;
    // Hard cap on inbound file transfer size (bytes). 0 = unlimited.
    // Default 8 GiB so 500MB+ agent artifacts work; override with transfer.max_bytes.
    uint64_t transfer_max_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    // Serve/overwrite identity, authorized_keys, tokens, *.pem/*.key.
    bool allow_sensitive_paths = false;
    // scp --dest may target $HOME / temp. Default is receive_dir only.
    bool dest_allow_home = false;
    std::vector<PeerEntry> seeds;
    std::vector<PeerEntry> discovered;
    std::string authorized_keys_path = "~/.bridgesessions/authorized_keys";
    std::string persistence_path = "~/.bridgesessions/sessions.json";
    int scrollback_lines = 2000;
    int idle_timeout_hours = 168;
    std::string default_shell;
    std::string terminal = "xterm-256color";
    std::string render_hint = "auto";  // "auto", "markdown", "raw"
    std::unordered_map<std::string, std::string> session_commands;

    // Named agent harness launch commands for `bs connect` / bare `bs`
    // interactive selector. Key = harness name (e.g. "hermes"), value =
    // command run on the selected peer. Configured via `harness.<name> <cmd>`
    // lines in the config file; built-in defaults exist for common harnesses.
    std::unordered_map<std::string, std::string> harness_commands;

    // P5: Virtual folder mappings
    struct VFolderEntry {
        std::string name;
        std::string local_path;
        std::string remote_peer;
        std::string remote_path;
        std::string direction = "bidirectional";  // push, pull, bidirectional
        int sync_interval_secs = 30;
    };
    std::vector<VFolderEntry> vfolders;

    // Last sync times for vfolders
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> vfolder_last_sync_;

    // D15: WebRTC transport
    bool webrtc_enabled = false;

    // D16: DHT
    bool dht_enabled = false;

    // D17: NAT traversal via UPnP
    bool upnp_enabled = false;

    // mDNS LAN discovery: disabled by default. Gossip/mDNS announcements are
    // only merged when the announced pubkey is explicitly trusted.
    bool mdns_enabled = false;

    MeshConfig() : default_shell(platform_default_shell()) {}
};

// ── expand_home — resolve ~ to HOME/USERPROFILE ─────────────────────

[[nodiscard]] std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;

    std::string home;
#ifdef _WIN32
    const char* env = std::getenv("USERPROFILE");
    if (env) home = env;
#else
    const char* env = std::getenv("HOME");
    if (env) home = env;
#endif
    if (home.empty()) return path; // fallback: return as-is

    // strip leading ~  (and optional /)
    std::string rest = path.substr(1);
    // Ensure single separator
    if (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) {
        // If home ends with separator and rest starts with one, skip the first char of rest
        if (!home.empty() && (home.back() == '/' || home.back() == '\\')) {
            rest = rest.substr(1);
        }
    }
    return home + rest;
}

// ── Helpers ─────────────────────────────────────────────────────────

namespace {

// Trim leading whitespace
std::string_view ltrim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    return s;
}

// Trim trailing whitespace
std::string_view rtrim(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// Trim both sides
std::string_view trim(std::string_view s) {
    return ltrim(rtrim(s));
}

// Parse an integer from a string_view
std::optional<int> parse_int(std::string_view s) {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    try {
        int sign = 1;
        size_t pos = 0;
        if (s[pos] == '-') { sign = -1; ++pos; }
        if (pos >= s.size()) return std::nullopt;
        int val = 0;
        for (; pos < s.size(); ++pos) {
            if (s[pos] < '0' || s[pos] > '9') return std::nullopt;
            val = val * 10 + (s[pos] - '0');
        }
        return val * sign;
    } catch (...) {
        return std::nullopt;
    }
}

// Parse host:port string → fills addr and port
void parse_listen_addr(const std::string& raw, std::string& out_addr, uint16_t& out_port) {
    auto colon = raw.rfind(':');
    if (colon == std::string::npos) {
        // No colon — treat as address only, keep default port
        out_addr = raw;
        return;
    }
    std::string addr_part = raw.substr(0, colon);
    std::string port_part = raw.substr(colon + 1);

    if (addr_part.empty()) {
        out_addr = "0.0.0.0";
    } else {
        out_addr = addr_part;
    }

    auto port_opt = parse_int(port_part);
    if (port_opt.has_value() && *port_opt > 0 && *port_opt <= 65535) {
        out_port = static_cast<uint16_t>(*port_opt);
    }
}

// Write a seed/discovered line to output
void write_peer_line(std::ostream& os, const std::string& prefix, const PeerEntry& p) {
    os << prefix << " " << p.name << " " << p.addr;
    if (!p.pubkey_hex.empty()) {
        os << " pubkey=" << p.pubkey_hex;
    }
    if (p.last_seen > 0) {
        os << " last_seen=" << p.last_seen;
    }
    os << "\n";
}

} // anonymous namespace

// ── load_config — parse key=value config file ────────────────────────

[[nodiscard]] MeshConfig load_config(const std::string& path) {
    MeshConfig cfg;
    std::string resolved = expand_home(path);
    cfg.source_path = resolved;

    std::ifstream f(resolved);
    if (!f.is_open()) return cfg; // missing file = all defaults

    std::string line;
    while (std::getline(f, line)) {
        // Strip a trailing CR so CRLF (Windows-authored) configs parse on POSIX.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Trim the line
        std::string_view sv(line);
        sv = trim(sv);

        // Skip blank lines and comments
        if (sv.empty() || sv[0] == '#') continue;

        // Find the first space or tab delimiter
        size_t space_pos = std::string::npos;
        for (size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == ' ' || sv[i] == '\t') {
                space_pos = i;
                break;
            }
        }
        if (space_pos == std::string::npos) continue; // no delimiter, malformed

        std::string_view key = trim(sv.substr(0, space_pos));
        std::string_view val = trim(sv.substr(space_pos + 1));
        if (key.empty()) continue;

        std::string key_str(key);

        // ── node.<key> ───────────────────────────────────────
        if (key_str == "node.name") {
            cfg.node_name = std::string(val);
        } else if (key_str == "node.listen") {
            parse_listen_addr(std::string(val), cfg.listen_addr, cfg.listen_port);
        }
        // ── mesh.<key> ───────────────────────────────────────
        else if (key_str == "mesh.max_peers") {
            auto v = parse_int(val);
            if (v.has_value() && *v >= 0) cfg.max_peers = static_cast<size_t>(*v);
        } else if (key_str == "mesh.gossip_interval_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.gossip_interval_secs = *v;
        } else if (key_str == "mesh.reconnect_backoff_max_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.reconnect_backoff_max_secs = *v;
        } else if (key_str == "mesh.join_window_max_secs") {
            auto v = parse_int(val);
            if (v.has_value() && *v > 0) cfg.join_window_max_secs = *v;
        } else if (key_str == "mesh.startup_wait_secs") {
            auto v = parse_int(val);
            if (v.has_value() && *v >= 0) cfg.startup_wait_secs = *v;
        } else if (key_str == "receive_dir") {
            cfg.receive_dir_override = val;
        } else if (key_str == "mesh.ping_interval_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.ping_interval_secs = *v;
        } else if (key_str == "mesh.pong_timeout_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.pong_timeout_secs = *v;
        } else if (key_str == "mesh.auto_upgrade") {
            std::string s(val);
            for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            cfg.auto_upgrade = !(s == "false" || s == "0" || s == "no" || s == "off");
        } else if (key_str == "mesh.auto_upgrade_cooldown_secs") {
            auto v = parse_int(val);
            if (v.has_value() && *v >= 60) cfg.auto_upgrade_cooldown_secs = *v;
        } else if (key_str == "mesh.require_seed_pins") {
            std::string s(val);
            for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            cfg.require_seed_pins = !(s == "false" || s == "0" || s == "no" || s == "off");
        } else if (key_str == "mesh.discovered_ttl_secs") {
            auto v = parse_int(val);
            if (v.has_value() && *v >= 0) cfg.discovered_ttl_secs = *v;
        } else if (key_str == "mesh.mdns_enabled") {
            std::string_view t = trim(val);
            cfg.mdns_enabled = (t == "true" || t == "1" || t == "yes");
        } else if (key_str == "transfer.max_bytes") {
            try {
                cfg.transfer_max_bytes = static_cast<uint64_t>(std::stoull(std::string(val)));
            } catch (...) {}
        } else if (key_str == "transfer.allow_sensitive_paths") {
            std::string_view t = trim(val);
            cfg.allow_sensitive_paths =
                (t == "true" || t == "1" || t == "yes" || t == "on");
        } else if (key_str == "file.dest_allow_home") {
            std::string_view t = trim(val);
            cfg.dest_allow_home =
                (t == "true" || t == "1" || t == "yes" || t == "on");
        }
        // ── transport.<key> (D15 WebRTC) ─────────────────────
        else if (key_str == "transport.webrtc_enabled") {
            std::string_view t = trim(val);
            cfg.webrtc_enabled = (t == "true" || t == "1" || t == "yes");
        }
        // ── dht.<key> (D16) ───────────────────────────────────
        else if (key_str == "dht.enabled") {
            std::string_view t = trim(val);
            cfg.dht_enabled = (t == "true" || t == "1" || t == "yes");
        }
        // ── vfolder.<name>.<key> (P5) ────────────────────────
        else if (key_str.rfind("vfolder.", 0) == 0) {
            auto rest = key_str.substr(8);  // "vfolder." = 8 chars
            // P3 audit fix: split from the RIGHT so vfolder names containing
            // dots (e.g. 'vfolder.my.app.local_path') parse correctly.
            // The key is the segment after the last dot.
            auto dot = rest.rfind('.');
            if (dot != std::string::npos) {
                std::string vname = std::string(rest.substr(0, dot));
                std::string vkey = std::string(rest.substr(dot + 1));
                bool found = false;
                for (auto& v : cfg.vfolders) {
                    if (v.name == vname) { found = true;
                        if (vkey == "local") v.local_path = std::string(trim(val));
                        else if (vkey == "peer") v.remote_peer = std::string(trim(val));
                        else if (vkey == "remote") v.remote_path = std::string(trim(val));
                        else if (vkey == "direction") v.direction = std::string(trim(val));
                        else if (vkey == "interval") { auto iv = parse_int(val); if (iv.has_value()) v.sync_interval_secs = *iv; }
                        break;
                    }
                }
                if (!found) {
                    MeshConfig::VFolderEntry ve;
                    ve.name = vname;
                    if (vkey == "local") ve.local_path = std::string(trim(val));
                    else if (vkey == "peer") ve.remote_peer = std::string(trim(val));
                    else if (vkey == "remote") ve.remote_path = std::string(trim(val));
                    else if (vkey == "direction") ve.direction = std::string(trim(val));
                    else if (vkey == "interval") { auto iv = parse_int(val); if (iv.has_value()) ve.sync_interval_secs = *iv; }
                    cfg.vfolders.push_back(std::move(ve));
                }
            }
        }
        // ── upnp.<key> (D17) ──────────────────────────────────
        else if (key_str == "upnp.enabled") {
            std::string_view t = trim(val);
            cfg.upnp_enabled = (t == "true" || t == "1" || t == "yes");
        }
        // ── sessions.<key> ───────────────────────────────────
        else if (key_str == "sessions.scrollback_lines") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.scrollback_lines = *v;
        } else if (key_str == "sessions.idle_timeout_hours") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.idle_timeout_hours = *v;
        } else if (key_str == "sessions.default_shell") {
            cfg.default_shell = std::string(val);
        } else if (key_str == "sessions.terminal") {
            cfg.terminal = std::string(val);
        } else if (key_str == "sessions.persistence_path") {
            cfg.persistence_path = std::string(val);
        } else if (key_str == "sessions.authorized_keys_path") {
            cfg.authorized_keys_path = std::string(val);
        }
        // ── session.<name>.command ──────────────────────────────
        else if (key_str.rfind("session.", 0) == 0 &&
                 key_str.size() > 16 &&
                 key_str.ends_with(".command")) {
            constexpr size_t prefix_len = 8;  // "session."
            constexpr size_t suffix_len = 8;  // ".command"
            std::string name = key_str.substr(
                prefix_len, key_str.size() - prefix_len - suffix_len);
            if (!name.empty()) cfg.session_commands[std::move(name)] = std::string(val);
        }
        // ── harness.<name> <command> ──────────────────────────
        // Launch command for the interactive `bs connect` selector. Examples:
        //   harness.hermes hermes --tui --yolo
        //   harness.claude-code claude
        else if (key_str.rfind("harness.", 0) == 0 &&
                 key_str.size() > 8) {
            std::string name = key_str.substr(8);  // "harness." = 8 chars
            if (!name.empty()) cfg.harness_commands[std::move(name)] = std::string(val);
        }
        // ── seed <name> <addr> ───────────────────────────────
        else if (key_str == "seed") {
            // Parse: seed <name> <addr> [pubkey=<hex>]
            std::string v2(val);
            std::istringstream iss(v2);
            std::string seed_name, seed_addr;
            if (!(iss >> seed_name >> seed_addr)) continue;
            if (seed_name.empty() || seed_addr.empty()) continue;

            PeerEntry parsed;
            parsed.name = std::move(seed_name);
            parsed.addr = std::move(seed_addr);
            std::string extra;
            while (iss >> extra) {
                if (extra.starts_with("pubkey=")) {
                    parsed.pubkey_hex = extra.substr(7);
                }
            }

            // Deduplicate by name: if a seed with this name already exists, update fields.
            bool found = false;
            for (auto& s : cfg.seeds) {
                if (s.name == parsed.name) {
                    s.addr = parsed.addr;
                    s.pubkey_hex = parsed.pubkey_hex;
                    found = true;
                    break;
                }
            }
            if (!found) cfg.seeds.push_back(std::move(parsed));
        }
        // ── discovered <name> <addr> [pubkey=<hex>] [last_seen=<unix>] ──
        else if (key_str == "discovered") {
            // Parse: discovered <name> <addr> [pubkey=<hex>] [last_seen=<ts>]
            std::string v2(val);
            std::istringstream iss(v2);
            std::string d_name, d_addr;
            if (!(iss >> d_name >> d_addr)) continue; // need at least name and addr

            PeerEntry p;
            p.name = d_name;
            p.addr = d_addr;

            std::string extra;
            while (iss >> extra) {
                if (extra.starts_with("pubkey=")) {
                    p.pubkey_hex = extra.substr(7);
                } else if (extra.starts_with("last_seen=")) {
                    auto ts = parse_int(std::string_view(extra).substr(10));
                    if (ts.has_value()) p.last_seen = static_cast<uint64_t>(*ts);
                }
            }
            cfg.discovered.push_back(std::move(p));
        }
        // ── unknown keys: log a warning so config typos are not silent ──
        else {
            // P3 audit fix: surface typos (e.g. 'nodge.name') instead of
            // silently ignoring them. log_event is forward-declared above.
            static std::once_flag once;
            std::call_once(once, [&]{
                log_event("config_unknown_key", key_str);
            });
        }
    }

    return cfg;
}

[[nodiscard]] std::string parse_ssh_g_hostname(const std::string& expanded) {
    std::istringstream input(expanded);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key)) continue;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key != "hostname") continue;
        std::string hostname;
        if (fields >> hostname) return hostname;
    }
    return {};
}

// ── Peer name resolution: fuzzy matching helpers ──────────────
// NO-FALLBACK CONTRACT: bs resolves peer names, connects, and either
// succeeds or fails with diagnostics. It NEVER invokes ssh/winrm/telnet
// as a fallback transport. There is no code path that does so. This is
// a design invariant — see AUDIT-MOA-2026-08-06.md.

// Classic two-row Wagner-Fischer. Returns edit distance.
[[nodiscard]] inline size_t levenshtein(const std::string& a,
                                        const std::string& b) {
    const auto m = a.size(), n = b.size();
    if (m == 0) return n;
    if (n == 0) return m;
    std::vector<size_t> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = j;
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            size_t cost = std::tolower(static_cast<unsigned char>(a[i - 1])) !=
                          std::tolower(static_cast<unsigned char>(b[j - 1]));
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(curr);
    }
    return prev[n];
}

// Same-length names that differ by exactly one digit (node-3/node-4, host-1/host-2)
// are distinct hosts, not typos. Fuzzy resolve must not remap them.
[[nodiscard]] inline bool names_are_numeric_siblings(const std::string& a,
                                                     const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    size_t diffs = 0;
    bool digit_sub = false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(a[i])));
        const auto cb = static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(b[i])));
        if (ca == cb) continue;
        ++diffs;
        if (std::isdigit(ca) && std::isdigit(cb)) digit_sub = true;
    }
    return diffs == 1 && digit_sub;
}

// Live attach policy: Ctrl-C is always a remote keystroke. Never disconnect.
[[nodiscard]] inline bool session_ctrl_c_disconnects() { return false; }

// Detach policy (docs/usage.md): Ctrl-D (0x04) is the LOCAL detach key. It is
// intercepted client-side and must never reach the remote PTY — there it
// would read as EOF and exit the shell the user meant to keep.
[[nodiscard]] inline bool local_input_requests_detach(std::string_view input) {
    return input.find('\x04') != std::string_view::npos;
}

#ifndef _WIN32
// Hermes TUI / process-group SIGINT never appears as 0x03 on stdin.
// Convert it to one forwarded Ctrl-C keystroke instead of dying.
inline volatile sig_atomic_t g_shell_sigint_forward = 0;
inline void shell_sigint_forward_handler(int) noexcept {
    g_shell_sigint_forward = 1;
}

// Best-effort terminal restore for SIGHUP/SIGTERM while the interactive
// client holds raw mode (e.g. terminal closed, kill(1), systemd stop).
// Without this the local terminal is left in raw+mouse-tracking mode: every
// mouse move prints escape garbage and BELs. tcsetattr/write are not formally
// async-signal-safe; this is the same trade every full-screen program makes.
inline struct termios g_sig_saved_termios{};
inline volatile sig_atomic_t g_sig_have_termios = 0;
inline void shell_signal_cleanup_handler(int sig) noexcept {
    if (g_sig_have_termios) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_sig_saved_termios);
        static const char seq[] =
            "\x1b[?9l\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1004l"
            "\x1b[?1005l\x1b[?1006l\x1b[?1015l\x1b[?1016l"
            "\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l";
        const ssize_t ignored =
            ::write(STDOUT_FILENO, seq, sizeof(seq) - 1);
        (void)ignored;
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);   // die by the real signal so waiters see the true status
}
#endif

// Case-insensitive suffix/prefix check for tier-3 matching.
[[nodiscard]] inline bool name_has_segment(const std::string& name,
                                           const std::string& query) {
    if (query.empty() || name.size() < query.size()) return false;
    auto ic_eq = [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    };
    // Exact suffix: "shadow" matches "lab-shadow"
    if (name.size() > query.size() &&
        name[name.size() - query.size() - 1] == '-' &&
        std::equal(query.rbegin(), query.rend(), name.rbegin(), ic_eq))
        return true;
    // Exact prefix: "shadow" matches "shadow-worker-1"
    if (name.size() > query.size() &&
        name[query.size()] == '-' &&
        std::equal(query.begin(), query.end(), name.begin(), ic_eq))
        return true;
    return false;
}

[[nodiscard]] bool config_peer_name_eq(const std::string& a,
                                       const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

[[nodiscard]] inline bool is_local_node_name(const std::string& query,
                                             const std::string& node_name) {
    return !query.empty() && config_peer_name_eq(query, node_name);
}

// OS hostname of this machine ("" on failure). gethostname exists in both
// POSIX unistd.h and winsock2.h, so this is cross-platform.
[[nodiscard]] inline std::string local_hostname() {
    char buf[256];
    if (::gethostname(buf, sizeof(buf) - 1) != 0) return {};
    buf[sizeof(buf) - 1] = '\0';
    return std::string(buf);
}

// True when `name` refers to this node — by mesh node name or by OS hostname
// (users type either; `bs <self>` must be caught before trust/import logic).
[[nodiscard]] inline bool is_self_target(const MeshConfig& cfg,
                                         const std::string& name) {
    if (is_local_node_name(name, cfg.node_name)) return true;
    const std::string h = local_hostname();
    if (is_local_node_name(name, h)) return true;
    // Bonjour/FQDN drift: macOS reports "Jeffersons-Mini.local" while
    // `hostname -s` gives "Jeffersons-Mini". Compare first DNS labels so
    // short ↔ FQDN mismatches still catch self. A false-positive refusal is
    // the safe direction (a real peer connect falls under the trust check).
    auto first_label = [](const std::string& s) {
        auto dot = s.find('.');
        return dot == std::string::npos ? s : s.substr(0, dot);
    };
    return is_local_node_name(first_label(name), first_label(h));
}

// Human-facing name for this node in self-connect messages.
[[nodiscard]] inline std::string self_display_name(const MeshConfig& cfg) {
    if (!cfg.node_name.empty() && cfg.node_name != "unnamed") return cfg.node_name;
    std::string h = local_hostname();
    return h.empty() ? std::string{"this node"} : h;
}

[[nodiscard]] bool import_ssh_alias_peer(
    MeshConfig& cfg,
    const std::string& alias,
    const std::string& expanded_ssh_config) {
    std::string hostname = parse_ssh_g_hostname(expanded_ssh_config);
    std::string resolved_addr = hostname.empty() ? std::string{} : hostname + ":19949";

    auto refresh_existing = [&](std::vector<PeerEntry>& peers) {
        for (auto& peer : peers) {
            if (!config_peer_name_eq(peer.name, alias)) continue;
            if (!resolved_addr.empty()) peer.addr = resolved_addr;
            return true;
        }
        return false;
    };
    if (refresh_existing(cfg.seeds) || refresh_existing(cfg.discovered)) return true;
    if (resolved_addr.empty()) return false;

    // Only copy pubkey when exactly ONE peer has that addr (multi-peer shared
    // host must not blindly inherit another peer's key — name collision fix).
    auto copy_identity_for_addr = [&](const std::vector<PeerEntry>& peers) {
        const PeerEntry* match = nullptr;
        int count = 0;
        for (const auto& peer : peers) {
            if (peer.addr == resolved_addr && !peer.pubkey_hex.empty()) {
                match = &peer;
                ++count;
            }
        }
        return (count == 1) ? match->pubkey_hex : std::string{};
    };
    std::string pubkey = copy_identity_for_addr(cfg.seeds);
    if (pubkey.empty()) pubkey = copy_identity_for_addr(cfg.discovered);
    cfg.seeds.push_back(PeerEntry{alias, resolved_addr, std::move(pubkey), 0});
    return true;
}

[[nodiscard]] std::string trusted_peer_pubkey(const MeshConfig& cfg,
                                               const std::string& peer_name) {
    for (const auto& peer : cfg.seeds) {
        if (config_peer_name_eq(peer.name, peer_name)) return peer.pubkey_hex;
    }
    for (const auto& peer : cfg.discovered) {
        if (config_peer_name_eq(peer.name, peer_name)) return peer.pubkey_hex;
    }
    return {};
}

[[nodiscard]] bool peer_identity_matches(const std::string& expected,
                                         const std::string& actual) {
    return !expected.empty() && expected == actual;
}

// ── Outbound peer identity (mesh + direct) ─────────────────────────
// Independent review 2026-07-16 P0-1: mesh connector must not trust TLS alone.
struct OutboundPeerVerifyResult {
    bool ok = false;
    std::string reason;
};

[[nodiscard]] const PeerEntry* find_peer_entry_by_addr(const MeshConfig& cfg,
                                                       const std::string& addr) {
    for (const auto& peer : cfg.seeds) {
        if (peer.addr == addr) return &peer;
    }
    for (const auto& peer : cfg.discovered) {
        if (peer.addr == addr) return &peer;
    }
    return nullptr;
}

// Single verification routine for outbound links: pin ↔ cert ↔ Hello.
// require_pin: when true, empty expected_pubkey is a hard fail.
[[nodiscard]] OutboundPeerVerifyResult verify_outbound_peer_identity(
    const std::string& expected_pubkey,
    const std::string& cert_pubkey,
    const std::string& hello_pubkey,
    const std::string& expected_name,
    const std::string& hello_name,
    bool require_pin) {
    if (cert_pubkey.empty()) {
        return {false, "empty peer certificate public key"};
    }
    if (require_pin && expected_pubkey.empty()) {
        return {false, "no pinned public key (seed/discovered pubkey= required)"};
    }
    if (!expected_pubkey.empty() &&
        !peer_identity_matches(expected_pubkey, cert_pubkey)) {
        return {false, "certificate public key does not match pin"};
    }
    if (hello_pubkey.empty()) {
        return {false, "empty Hello pubkey"};
    }
    if (hello_pubkey != cert_pubkey) {
        return {false, "Hello pubkey does not match TLS certificate key"};
    }
    if (hello_name.empty()) {
        return {false, "empty Hello node name"};
    }
    // 2.0.8 MoA fix: reject control chars (log/IPC injection via node name).
    for (unsigned char ch : hello_name)
        if (ch < 0x20 || ch == 0x7f)
            return {false, "Hello node name contains control characters"};
    // Name is an operational label; identity is the pinned key. Allow rename
    // when the pin already matched (e.g. seed "windows-peer" announces as
    // "windows-peer-2"). Reject only when there is no pin to bind identity.
    if (!expected_name.empty() &&
        !config_peer_name_eq(expected_name, hello_name)) {
        if (expected_pubkey.empty() ||
            !peer_identity_matches(expected_pubkey, cert_pubkey)) {
            return {false, "Hello node name does not match expected peer name"};
        }
        // pinned key OK — accept Hello name (conn.peer_name becomes hello_name)
    }
    return {true, {}};
}

// Inbound links are already authorized by the certificate callback, but the
// application Hello still has to identify the same key and must not claim a
// configured name belonging to another key. Otherwise an authorized peer can
// impersonate a different peer in name-based command routing.
[[nodiscard]] OutboundPeerVerifyResult verify_inbound_peer_identity(
    const MeshConfig& cfg,
    const std::string& cert_pubkey,
    const std::string& hello_pubkey,
    const std::string& hello_name) {
    if (cert_pubkey.empty()) return {false, "empty peer certificate public key"};
    if (hello_pubkey.empty()) return {false, "empty Hello pubkey"};
    if (hello_pubkey != cert_pubkey) {
        return {false, "Hello pubkey does not match TLS certificate key"};
    }
    if (hello_name.empty()) return {false, "empty Hello node name"};
    // 2.0.8 MoA fix: reject control chars in node names — a \n-bearing name
    // forges log lines (log injection) and breaks line-oriented IPC replies.
    for (unsigned char ch : hello_name)
        if (ch < 0x20 || ch == 0x7f)
            return {false, "Hello node name contains control characters"};

    auto check_peer = [&](const PeerEntry& peer) -> std::optional<std::string> {
        // Name collision fix: do NOT reject when cert_pubkey matches a trusted
        // key that belongs to a differently-named peer. Multiple peers may share
        // a host/key legitimately. The name→key binding check below still guards
        // against a different key claiming an existing name.
        if (!peer.pubkey_hex.empty() &&
            config_peer_name_eq(peer.name, hello_name) &&
            peer.pubkey_hex != cert_pubkey) {
            return "Hello node name is pinned to a different certificate key";
        }
        return std::nullopt;
    };
    bool matched_authoritative_seed = false;
    for (const auto& peer : cfg.seeds) {
        if (auto reason = check_peer(peer)) return {false, *reason};
        if (!peer.pubkey_hex.empty() && peer.pubkey_hex == cert_pubkey &&
            config_peer_name_eq(peer.name, hello_name)) {
            matched_authoritative_seed = true;
        }
    }
    // Explicit seed pins are operator-controlled and authoritative. Gossip may
    // retain a peer's previous key after a legitimate rotation; once the seed
    // name/key pair matches, stale discovered entries must not poison that
    // identity indefinitely.
    if (matched_authoritative_seed) return {true, {}};
    for (const auto& peer : cfg.discovered) {
        if (auto reason = check_peer(peer)) return {false, *reason};
    }
    return {true, {}};
}

// ── File transfer path containment (P0-3) ──────────────────────────
[[nodiscard]] std::optional<std::string> sanitize_transfer_filename(
    std::string_view name) {
    if (name.empty() || name.size() > 255) return std::nullopt;
    // Reject absolute paths / drive letters before basename.
    if (name[0] == '/' || name[0] == '\\') return std::nullopt;
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) &&
        name[1] == ':') {
        return std::nullopt;
    }
    std::string s(name);
    // Basename only (reject if empty after strip).
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (s.empty() || s == "." || s == "..") return std::nullopt;
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) {
        return std::nullopt;
    }
    for (unsigned char c : s) {
        if (c < 32 || c == 127) return std::nullopt;
    }
    // Windows reserved device names (case-insensitive).
    std::string upper = s;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    static constexpr const char* kReserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    std::string stem = upper;
    const auto dot = stem.find('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    for (const char* r : kReserved) {
        if (stem == r) return std::nullopt;
    }
    return s;
}

// Resolve a remote FileRequest path to a local filesystem path.
// Relative requests are tried under receive_dir; common client mistakes that
// re-prefix `.bridgesessions/received/` or `received/` are stripped so
// meshmon-style probes do not double-nest (see test_file_path_sanitization).
[[nodiscard]] inline std::string resolve_file_request_path(
        const std::string& path, const std::string& receive_dir) {
    namespace fs = std::filesystem;
    if (path.empty()) return {};

    auto exists_file = [](const std::string& p) -> bool {
        std::error_code ec;
        return !p.empty() && fs::exists(p, ec) && !fs::is_directory(p, ec);
    };

    std::vector<std::string> candidates;
    candidates.reserve(8);

    const std::string expanded = expand_home(path);
    candidates.push_back(expanded);

    fs::path as_path(expanded);
    if (!as_path.is_absolute()) {
        // Basename-only under receive_dir (most common agent/meshmon form).
        candidates.push_back((fs::path(receive_dir) / as_path.filename()).string());
        // Full relative path under receive_dir.
        candidates.push_back((fs::path(receive_dir) / as_path).string());

        // Strip accidental receive-dir prefixes clients re-send.
        auto strip_and_join = [&](std::string_view prefix) {
            std::string rel = expanded;
            // Accept either POSIX or Windows separators in the prefix match.
            if (rel.size() >= prefix.size()) {
                bool match = true;
                for (size_t i = 0; i < prefix.size(); ++i) {
                    char a = rel[i];
                    char b = prefix[i];
                    if (a == '\\') a = '/';
                    if (b == '\\') b = '/';
                    if (a != b) { match = false; break; }
                }
                if (match) {
                    rel = rel.substr(prefix.size());
                    while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
                        rel.erase(rel.begin());
                    if (!rel.empty())
                        candidates.push_back((fs::path(receive_dir) / rel).string());
                }
            }
        };
        strip_and_join(".bridgesessions/received/");
        strip_and_join("bridgesessions/received/");
        strip_and_join("received/");
        strip_and_join(".bridgesessions\\received\\");
        strip_and_join("received\\");
    }

    for (const auto& c : candidates) {
        if (exists_file(c)) return c;
    }
    // Fallback for error messages: keep the relative tree under receive_dir
    // (basename-only is only preferred when that candidate actually exists).
    if (!as_path.is_absolute())
        return (fs::path(receive_dir) / as_path).string();
    return expanded;
}

[[nodiscard]] bool path_is_inside_directory(const std::filesystem::path& candidate,
                                            const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root_abs = fs::weakly_canonical(root, ec);
    if (ec) root_abs = fs::absolute(root, ec);
    if (ec) return false;
    fs::path cand_abs = fs::weakly_canonical(candidate, ec);
    if (ec) cand_abs = fs::absolute(candidate, ec);
    if (ec) return false;
    auto root_s = root_abs.lexically_normal().string();
    auto cand_s = cand_abs.lexically_normal().string();
#ifdef _WIN32
    for (char& c : root_s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : cand_s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : root_s) if (c == '/') c = '\\';
    for (char& c : cand_s) if (c == '/') c = '\\';
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (root_s.empty()) return false;
    if (!root_s.empty() && root_s.back() == sep) {
        // ok
    } else {
        root_s.push_back(sep);
    }
    return cand_s == root_s.substr(0, root_s.size() - 1) ||
           cand_s.rfind(root_s, 0) == 0;
}

// Resolve scp-style file-send destination on the receiver.
// - empty → nullopt (caller uses receive_dir/basename)
// - relative → receive_dir / dest
// - ~/… or absolute → expand, then require containment under receive_dir
//   (default) or, when allow_home_tmp, also $HOME / temp.
// Rejects ".." segments and escapes.
[[nodiscard]] inline std::optional<std::string> resolve_file_send_dest(
    std::string_view dest_req,
    const std::string& receive_dir,
    bool allow_home_tmp = false) {
    if (dest_req.empty()) return std::nullopt;
    if (dest_req.size() > 1024) return std::nullopt;
    for (unsigned char c : dest_req) {
        if (c < 32 || c == 127) return std::nullopt;
    }
    {
        std::string scan(dest_req);
        for (char& c : scan) if (c == '\\') c = '/';
        size_t i = 0;
        while (i < scan.size()) {
            while (i < scan.size() && scan[i] == '/') ++i;
            size_t j = i;
            while (j < scan.size() && scan[j] != '/') ++j;
            std::string_view part(scan.data() + i, j - i);
            if (part == "..") return std::nullopt;
            i = j;
        }
    }
    namespace fs = std::filesystem;
    std::string dest(dest_req);
    std::string candidate;
    const bool abs =
        (!dest.empty() && (dest[0] == '/' || dest[0] == '\\')) ||
        (dest.size() >= 2 && std::isalpha(static_cast<unsigned char>(dest[0])) &&
         dest[1] == ':');
    if (!dest.empty() && dest[0] == '~') {
        candidate = expand_home(dest);
    } else if (abs) {
        candidate = dest;
    } else {
        candidate = (fs::path(receive_dir) / dest).lexically_normal().string();
    }
    std::vector<std::string> roots;
    roots.push_back(expand_home(receive_dir));
    if (allow_home_tmp) {
        roots.push_back(expand_home("~"));
#ifdef _WIN32
        if (const char* t = std::getenv("TEMP")) roots.emplace_back(t);
        if (const char* t = std::getenv("TMP")) roots.emplace_back(t);
        roots.emplace_back("C:\\Windows\\Temp");
#else
        roots.emplace_back("/tmp");
#endif
    }
    const std::string receive_root = expand_home(receive_dir);
    if (!receive_root.empty() && path_is_inside_directory(candidate, receive_root))
        return candidate;
    for (size_t root_index = 1; root_index < roots.size(); ++root_index) {
        const auto& root = roots[root_index];
        if (root.empty()) continue;
        if (!path_is_inside_directory(candidate, root)) continue;
        std::error_code ec;
        auto rel = std::filesystem::weakly_canonical(candidate, ec).lexically_relative(
            std::filesystem::weakly_canonical(root, ec));
        if (ec || rel.empty()) return std::nullopt;
        bool hidden_component = false;
        for (const auto& component : rel) {
            const std::string part = component.string();
            if (part.size() > 1 && part[0] == '.') {
                hidden_component = true;
                break;
            }
        }
        if (!hidden_component) return candidate;
    }
    return std::nullopt;
}

// ── Host metrics (for `bs fleet` + ServerInfo gossip) ─────────────
struct HostStats {
    double load1 = -1.0;       // 1-min loadavg; -1 = N/A
    double cpu_pct = -1.0;     // 0–100; -1 until second sample
    double mem_pct = -1.0;
    double disk_pct = -1.0;
    uint64_t mem_used_mb = 0;
    uint64_t mem_total_mb = 0;
    uint64_t disk_used_gb = 0;
    uint64_t disk_total_gb = 0;
    int ncpu = 0;
    std::string os;
    std::string arch;
    bool cua_helper = false;   // local CUA helper reachable
    std::string primary_addr;  // best non-loopback advertise IP (no port)
};

// Best-effort primary IPv4 for fleet "self" row (prefer 100.x Tailscale).
[[nodiscard]] inline std::string primary_advertise_ip() {
#if defined(_WIN32)
    // Prefer GetAdaptersAddresses; fall back to empty (CLI shows listen).
    return {};
#else
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0 || !ifa) return {};
    std::string ts, other;
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN] = {};
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
        std::string ip(buf);
        if (ip.rfind("100.", 0) == 0) { ts = ip; break; }
        if (other.empty() && ip.rfind("127.", 0) != 0) other = ip;
    }
    freeifaddrs(ifa);
    return !ts.empty() ? ts : other;
#endif
}

// True when local CUA helper token+socket/port responds (quick connect probe).
[[nodiscard]] inline bool cua_helper_reachable(const std::string& app_home) {
    if (app_home.empty()) return false;
    namespace fs = std::filesystem;
#ifdef _WIN32
    // Token file present + TCP connect to helper port.
    if (!fs::exists(cua_helper_token_path(app_home))) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(kCuaHelperPort);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    bool ok = false;
    if (rc == 0) ok = true;
    else if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS) {
        WSAPOLLFD pfd{s, POLLWRNORM, 0};
        ok = WSAPoll(&pfd, 1, 200) > 0;
    }
    closesocket(s);
    return ok;
#else
    std::string sock = cua_helper_socket_path(app_home);
    if (!fs::exists(sock)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (sock.size() >= sizeof(sa.sun_path)) { close(fd); return false; }
    std::strncpy(sa.sun_path, sock.c_str(), sizeof(sa.sun_path) - 1);
    // Short connect timeout via nonblock + poll
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    bool ok = false;
    if (rc == 0) ok = true;
    else if (errno == EINPROGRESS) {
        pollfd pfd{fd, POLLOUT, 0};
        ok = poll(&pfd, 1, 200) > 0;
    }
    close(fd);
    return ok;
#endif
}

[[nodiscard]] inline std::string host_stats_platform_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unix";
#endif
}

[[nodiscard]] inline std::string host_stats_platform_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

// Compact JSON for wire + fleet table. Omits unknown (-1) pct fields as null.
[[nodiscard]] inline std::string host_stats_to_json(const HostStats& h) {
    auto num_or_null = [](double v) -> std::string {
        if (v < 0) return "null";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", v);
        return buf;
    };
    std::ostringstream o;
    o << "{\"cpu\":" << num_or_null(h.cpu_pct)
      << ",\"mem\":" << num_or_null(h.mem_pct)
      << ",\"disk\":" << num_or_null(h.disk_pct)
      << ",\"load\":" << num_or_null(h.load1)
      << ",\"os\":\"" << h.os << "\""
      << ",\"arch\":\"" << h.arch << "\""
      << ",\"ncpu\":" << h.ncpu
      << ",\"mem_mb\":" << h.mem_total_mb
      << ",\"disk_gb\":" << h.disk_total_gb
      << ",\"cua\":" << (h.cua_helper ? "true" : "false")
      << "}";
    return o.str();
}

// Sample host CPU / mem / disk. CPU uses a static previous tick so the first
// call may return cpu_pct=-1; subsequent calls (gossip ~30s) are accurate.
// app_home: optional path for CUA helper probe (default ~/.bridgesessions).
[[nodiscard]] inline HostStats collect_host_stats(const std::string& app_home = {}) {
    HostStats h;
    h.os = host_stats_platform_os();
    h.arch = host_stats_platform_arch();
    h.ncpu = static_cast<int>(std::thread::hardware_concurrency());
    h.primary_addr = primary_advertise_ip();
    {
        std::string home = app_home.empty() ? expand_home("~/.bridgesessions") : app_home;
        h.cua_helper = cua_helper_reachable(home);
    }

#if !defined(_WIN32)
    {
        double la[3] = {0, 0, 0};
        if (getloadavg(la, 3) > 0) h.load1 = la[0];
    }
    // Memory
#if defined(__APPLE__)
    {
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        vm_statistics64_data_t vm;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                              reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
            int64_t pagesize = sysconf(_SC_PAGESIZE);
            if (pagesize <= 0) pagesize = 4096;
            uint64_t total_pages = 0;
            size_t len = sizeof(total_pages);
            int mib[2] = {CTL_HW, HW_MEMSIZE};
            uint64_t memsize = 0;
            size_t mlen = sizeof(memsize);
            if (sysctl(mib, 2, &memsize, &mlen, nullptr, 0) == 0 && memsize > 0) {
                // free + inactive + speculative ≈ available
                uint64_t free_pages = static_cast<uint64_t>(vm.free_count) +
                                      static_cast<uint64_t>(vm.inactive_count) +
                                      static_cast<uint64_t>(vm.speculative_count);
                uint64_t free_b = free_pages * static_cast<uint64_t>(pagesize);
                h.mem_total_mb = memsize / (1024 * 1024);
                h.mem_used_mb = (memsize > free_b)
                    ? (memsize - free_b) / (1024 * 1024) : 0;
                if (h.mem_total_mb > 0)
                    h.mem_pct = 100.0 * static_cast<double>(h.mem_used_mb) /
                                static_cast<double>(h.mem_total_mb);
            }
            (void)total_pages;
        }
        // CPU via host_statistics. Tick delta needs two samples; a zero-delta
        // call (FLEET + gossip racing, or two back-to-back FLEET seeds) must
        // not wipe a previously good reading.
        {
            host_cpu_load_info_data_t cpuinfo{};
            mach_msg_type_number_t ccount = HOST_CPU_LOAD_INFO_COUNT;
            if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                                reinterpret_cast<host_info_t>(&cpuinfo),
                                &ccount) == KERN_SUCCESS) {
                static std::mutex cpu_mu;
                static uint64_t prev_user = 0, prev_system = 0, prev_idle = 0, prev_nice = 0;
                uint64_t user = cpuinfo.cpu_ticks[CPU_STATE_USER];
                uint64_t system = cpuinfo.cpu_ticks[CPU_STATE_SYSTEM];
                uint64_t idle = cpuinfo.cpu_ticks[CPU_STATE_IDLE];
                uint64_t nice = cpuinfo.cpu_ticks[CPU_STATE_NICE];
                uint64_t total = user + system + idle + nice;
                std::lock_guard<std::mutex> lk(cpu_mu);
                uint64_t prev_total = prev_user + prev_system + prev_idle + prev_nice;
                if (prev_total > 0 && total > prev_total) {
                    uint64_t d_total = total - prev_total;
                    uint64_t d_idle = idle - prev_idle;
                    if (d_total > 0)
                        h.cpu_pct = 100.0 * (1.0 - static_cast<double>(d_idle) /
                                                     static_cast<double>(d_total));
                }
                prev_user = user; prev_system = system; prev_idle = idle; prev_nice = nice;
            }
        }
    }
#elif defined(__linux__)
    {
        std::ifstream mf("/proc/meminfo");
        uint64_t mem_total_kb = 0, mem_avail_kb = 0, mem_free_kb = 0;
        std::string key;
        uint64_t val = 0;
        std::string unit;
        while (mf >> key >> val >> unit) {
            if (key == "MemTotal:") mem_total_kb = val;
            else if (key == "MemAvailable:") mem_avail_kb = val;
            else if (key == "MemFree:") mem_free_kb = val;
        }
        if (mem_avail_kb == 0) mem_avail_kb = mem_free_kb;
        if (mem_total_kb > 0) {
            h.mem_total_mb = mem_total_kb / 1024;
            h.mem_used_mb = (mem_total_kb > mem_avail_kb)
                ? (mem_total_kb - mem_avail_kb) / 1024 : 0;
            h.mem_pct = 100.0 * static_cast<double>(mem_total_kb - mem_avail_kb) /
                        static_cast<double>(mem_total_kb);
        }
        // CPU from /proc/stat
        std::ifstream sf("/proc/stat");
        std::string cpu_label;
        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
                 irq = 0, softirq = 0, steal = 0;
        if (sf >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
            static uint64_t prev_idle = 0, prev_total = 0;
            uint64_t idle_all = idle + iowait;
            uint64_t non_idle = user + nice + system + irq + softirq + steal;
            uint64_t total = idle_all + non_idle;
            if (prev_total > 0 && total > prev_total) {
                uint64_t d_total = total - prev_total;
                uint64_t d_idle = idle_all - prev_idle;
                if (d_total > 0)
                    h.cpu_pct = 100.0 * (1.0 - static_cast<double>(d_idle) /
                                                 static_cast<double>(d_total));
            }
            prev_idle = idle_all;
            prev_total = total;
        }
    }
#endif
    // Disk: root filesystem
    {
        struct statvfs st{};
        if (statvfs("/", &st) == 0 && st.f_blocks > 0) {
            uint64_t total = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
            uint64_t freeb = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
            uint64_t used = total > freeb ? total - freeb : 0;
            h.disk_total_gb = total / (1024ULL * 1024ULL * 1024ULL);
            h.disk_used_gb = used / (1024ULL * 1024ULL * 1024ULL);
            h.disk_pct = 100.0 * static_cast<double>(used) / static_cast<double>(total);
        }
    }
#else  // _WIN32
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            h.mem_total_mb = ms.ullTotalPhys / (1024ULL * 1024ULL);
            h.mem_used_mb = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024ULL * 1024ULL);
            h.mem_pct = static_cast<double>(ms.dwMemoryLoad);
        }
        ULARGE_INTEGER freeb{}, total{}, dummy{};
        if (GetDiskFreeSpaceExA("C:\\", &freeb, &total, &dummy) && total.QuadPart > 0) {
            uint64_t used = total.QuadPart > freeb.QuadPart
                ? total.QuadPart - freeb.QuadPart : 0;
            h.disk_total_gb = total.QuadPart / (1024ULL * 1024ULL * 1024ULL);
            h.disk_used_gb = used / (1024ULL * 1024ULL * 1024ULL);
            h.disk_pct = 100.0 * static_cast<double>(used) /
                         static_cast<double>(total.QuadPart);
        }
        // CPU via GetSystemTimes
        FILETIME idle_ft{}, kernel_ft{}, user_ft{};
        if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
            auto ft64 = [](const FILETIME& ft) -> uint64_t {
                return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            };
            static uint64_t prev_idle = 0, prev_kernel = 0, prev_user = 0;
            uint64_t idle = ft64(idle_ft), kernel = ft64(kernel_ft), user = ft64(user_ft);
            // kernel includes idle on Windows
            uint64_t total = kernel + user;
            uint64_t prev_total = prev_kernel + prev_user;
            if (prev_total > 0 && total > prev_total) {
                uint64_t d_total = total - prev_total;
                uint64_t d_idle = idle - prev_idle;
                if (d_total > 0)
                    h.cpu_pct = 100.0 * (1.0 - static_cast<double>(d_idle) /
                                                 static_cast<double>(d_total));
            }
            prev_idle = idle; prev_kernel = kernel; prev_user = user;
        }
        // Approximate "load" as cpu fraction so existing load field is useful
        if (h.cpu_pct >= 0) h.load1 = h.cpu_pct / 100.0 * std::max(1, h.ncpu);
    }
#endif
    if (h.cpu_pct > 100.0) h.cpu_pct = 100.0;
    if (h.mem_pct > 100.0) h.mem_pct = 100.0;
    if (h.disk_pct > 100.0) h.disk_pct = 100.0;
    if (h.cpu_pct < 0 && h.cpu_pct != -1.0) h.cpu_pct = 0;
    // Reuse last good CPU when this sample had no tick delta (common on
    // macOS: FLEET IPC and gossip both call collect_host_stats; a zero-delta
    // sample used to publish cpu_pct=null for `self` forever).
    {
        static std::mutex last_cpu_mu;
        static double last_good_cpu = -1.0;
        std::lock_guard<std::mutex> lk(last_cpu_mu);
        if (h.cpu_pct >= 0.0) last_good_cpu = h.cpu_pct;
        else if (last_good_cpu >= 0.0) h.cpu_pct = last_good_cpu;
    }
    return h;
}

// ── App home isolation (--config-dir) ─────────────────────────────
// When --config-dir is set, ALL identity/config/receive/state live under that
// directory (not under $HOME/.bridgesessions). Audit residual R1.
struct AppPaths {
    std::string root;
    std::string config;
    std::string received;
    std::string authorized_keys;
    std::string sessions;
    std::string key_pem;
    std::string cert_pem;
    std::string pub;
    std::string logs;
    std::string state;
};

[[nodiscard]] inline AppPaths make_app_paths(std::string root) {
    if (root.empty()) root = expand_home("~/.bridgesessions");
    // Strip trailing slashes
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    const std::filesystem::path root_path(root);
    AppPaths p;
    p.root = root_path.string();
    p.config = (root_path / "config").string();
    p.received = (root_path / "received").string();
    p.authorized_keys = (root_path / "authorized_keys").string();
    p.sessions = (root_path / "sessions.json").string();
    p.key_pem = (root_path / "id_ed25519.pem").string();
    p.cert_pem = (root_path / "id_ed25519-cert.pem").string();
    p.pub = (root_path / "id_ed25519.pub").string();
    p.logs = (root_path / "logs").string();
    p.state = (root_path / "state").string();
    return p;
}

[[nodiscard]] inline std::string private_tmp_dir(const std::string& app_home) {
    const std::string root = app_home.empty()
        ? expand_home("~/.bridgesessions") : app_home;
    const std::string dir =
        (std::filesystem::path(make_app_paths(root).root) / "tmp").string();
    (void)ensure_private_directory(dir);
    return dir;
}

// Unique file under ~/.bridgesessions/tmp (0700). Empty on failure.
[[nodiscard]] inline std::string create_private_temp_file(
        const std::string& prefix,
        const std::string& suffix,
        const std::string& app_home) {
    const std::string dir = private_tmp_dir(app_home);
#ifdef _WIN32
    char tmpl[MAX_PATH]{};
    std::string pfx = prefix.empty() ? "bs" : prefix.substr(0, 3);
    if (GetTempFileNameA(dir.c_str(), pfx.c_str(), 0, tmpl) == 0) return {};
    std::string created = tmpl;
    if (suffix.empty()) return created;
    std::string renamed = created + suffix;
    if (std::rename(created.c_str(), renamed.c_str()) != 0) {
        ::unlink(created.c_str());
        return {};
    }
    return renamed;
#else
    std::string tmpl = dir + "/" + (prefix.empty() ? "bs" : prefix) +
                       "XXXXXX" + suffix;
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), static_cast<int>(suffix.size()));
    if (fd < 0) return {};
    ::close(fd);
    return std::string(buf.data());
#endif
}

// Rewrite legacy ~/.bridgesessions/... defaults into an isolated app root.
[[nodiscard]] inline std::string resolve_under_app_home(const std::string& path,
                                                        const std::string& app_root) {
    if (path.empty()) return path;
    constexpr std::string_view kLegacy = "~/.bridgesessions";
    if (path == kLegacy || path.starts_with(std::string(kLegacy) + "/") ||
        path.starts_with(std::string(kLegacy) + "\\")) {
        std::string relative = path.substr(kLegacy.size());
        while (!relative.empty() &&
               (relative.front() == '/' || relative.front() == '\\')) {
            relative.erase(relative.begin());
        }
        if (relative.empty()) return std::filesystem::path(app_root).string();
        return (std::filesystem::path(app_root) / relative).string();
    }
    return expand_home(path);
}

inline void apply_app_home_defaults(MeshConfig& cfg, const std::string& app_root) {
    cfg.authorized_keys_path = resolve_under_app_home(cfg.authorized_keys_path, app_root);
    cfg.persistence_path = resolve_under_app_home(cfg.persistence_path, app_root);
}

// ── Per-daemon IPC authentication token ─────────────────────────────
// Each daemon instance generates a fresh CSPRNG token after binding its
// loopback IPC socket. The token is written owner-only under the app home;
// every CLI helper must read it and prepend it to each IPC request.
// There is no unauthenticated fallback.

[[nodiscard]] inline std::string ipc_token_path(const std::string& app_home) {
    return (std::filesystem::path(make_app_paths(app_home).root) /
            "ipc-token").string();
}

[[nodiscard]] inline std::string generate_ipc_token() {
    std::array<uint8_t, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed for IPC token");
    }
    static const char* d = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        token.push_back(d[b >> 4]);
        token.push_back(d[b & 0xF]);
    }
    return token;
}

[[nodiscard]] inline bool write_ipc_token_file(const std::string& app_home,
                                              const std::string& token) {
    return write_private_text_file(ipc_token_path(app_home), token);
}

[[nodiscard]] inline std::string load_ipc_token(const std::string& app_home) {
    std::ifstream f(ipc_token_path(app_home));
    if (!f.is_open()) return {};
    std::string token;
    if (std::getline(f, token)) {
        // Strip trailing CR in case the file was edited on Windows.
        if (!token.empty() && token.back() == '\r') token.pop_back();
    }
    return token;
}

[[nodiscard]] std::string expand_ssh_alias(const std::string& alias) {
    if (alias.empty() || !std::all_of(alias.begin(), alias.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '_' || c == '-';
        })) {
        return {};
    }
#ifdef _WIN32
    std::string command = "ssh -G " + alias + " 2>NUL";
    FILE* pipe = _popen(command.c_str(), "r");
#else
    std::string command = "ssh -G " + alias + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return {};
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

[[nodiscard]] bool import_ssh_alias_peer(MeshConfig& cfg,
                                         const std::string& alias) {
    return import_ssh_alias_peer(cfg, alias, expand_ssh_alias(alias));
}

enum class SessionCommandSource : uint8_t {
    ClientOverride,
    NamedProfile,
    ConfigDefault,
};

struct ResolvedSessionCommand {
    std::string command;
    SessionCommandSource source = SessionCommandSource::ConfigDefault;
};

// Escape a payload for cmd.exe `/S /C "..."`.
// Nested double-quotes must be doubled (`"` → `""`) or cmd terminates the
// outer `/C` string early. Prefer NOT wrapping PowerShell in cmd at all
// (see build_windows_command_line) — empirical: even with doubled quotes,
// `cmd /S /C "powershell -Command ""...| ForEach-Object { $_ }..."""` still
// breaks pipes so cmd tries to run ForEach-Object as its own command.
// `$` itself is not special to cmd; quote/pipe destruction makes `$_` look
// "mistreated". Callers still must protect `$` from *bash* expansion.
[[nodiscard]] std::string escape_for_cmd_slash_c(const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 8);
    for (unsigned char ch : payload) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

// First argv token of a Windows command line (quote-aware, best-effort).
[[nodiscard]] std::string first_windows_cli_token(const std::string& command) {
    size_t i = 0;
    while (i < command.size() && (command[i] == ' ' || command[i] == '\t')) ++i;
    if (i >= command.size()) return {};
    if (command[i] == '"') {
        const size_t end = command.find('"', i + 1);
        if (end == std::string::npos) return command.substr(i + 1);
        return command.substr(i + 1, end - (i + 1));
    }
    const size_t end = command.find_first_of(" \t", i);
    return command.substr(i, end == std::string::npos ? std::string::npos : end - i);
}

// Heuristic: command line starts with a real Windows application rather than a
// cmd builtin (`dir`, `echo`, …). Used to skip cmd /c wrapping so PowerShell
// scriptblocks with `$_` and pipes survive CreateProcess.
[[nodiscard]] bool command_has_direct_windows_exe_token(const std::string& command) {
    std::string token = first_windows_cli_token(command);
    if (token.empty()) return false;
    // basename lower
    size_t slash = token.find_last_of("\\/");
    std::string base = slash == std::string::npos ? token : token.substr(slash + 1);
    for (char& c : base) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".exe") == 0) {
        return true;
    }
    // bare names that SearchPath resolves with PATHEXT
    static constexpr const char* kKnown[] = {
        "powershell", "powershell.exe", "pwsh", "pwsh.exe",
        "cmd", "cmd.exe", "python", "python.exe", "python3", "python3.exe",
        "py", "py.exe",
    };
    for (const char* k : kKnown) {
        if (base == k) return true;
    }
    return false;
}

// True for non-interactive client-override launches that must use anonymous
// pipes (not ConPTY) so --cmd stdout is captured reliably.
[[nodiscard]] bool is_windows_cli_oneshot_command(const std::string& command) {
    if (command.find("/c ") != std::string::npos ||
        command.find("/C ") != std::string::npos ||
        command.find("/c\"") != std::string::npos ||
        command.find("/C\"") != std::string::npos) {
        return true;
    }
    std::string lower = command;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const bool is_ps =
        lower.find("powershell") != std::string::npos ||
        lower.find("pwsh") != std::string::npos;
    if (!is_ps) return false;
    return lower.find("-command") != std::string::npos ||
           lower.find("-encodedcommand") != std::string::npos ||
           lower.find("-file") != std::string::npos ||
           lower.find(" -c ") != std::string::npos ||
           lower.find(" -c\"") != std::string::npos;
}

[[nodiscard]] std::string build_windows_command_line(
    const ResolvedSessionCommand& resolved,
    const std::string& comspec,
    bool direct_executable_available = true) {
    // Named/default profile whose first token is a real application: direct.
    if (resolved.source != SessionCommandSource::ClientOverride &&
        direct_executable_available) {
        return resolved.command;
    }
    // ClientOverride that is already an executable cmdline (powershell …):
    // do NOT wrap in cmd /c. Wrapping destroys nested quotes, pipes, and $_.
    // Empirically even quote-doubling under cmd /S /C still breaks PS pipes.
    if (resolved.source == SessionCommandSource::ClientOverride &&
        direct_executable_available &&
        command_has_direct_windows_exe_token(resolved.command)) {
        return resolved.command;
    }
    // Builtins (`dir`, …) and non-resolvable commands: cmd /c + doubled quotes.
    const std::string shell = comspec.empty() ? "cmd.exe" : comspec;
    return "\"" + shell + "\" /d /s /c \"" +
           escape_for_cmd_slash_c(resolved.command) + "\"";
}

[[nodiscard]] std::string prepare_session_command(
    const ResolvedSessionCommand& resolved) {
#ifdef _WIN32
    const char* comspec = std::getenv("ComSpec");
    // Always SearchPath the first token — including ClientOverride — so
    // powershell.exe skips cmd wrapping while `dir` still gets cmd /c.
    const bool direct_executable_available =
        !resolve_windows_application(utf8_to_wide(resolved.command)).empty() ||
        command_has_direct_windows_exe_token(resolved.command);
    return build_windows_command_line(
        resolved, comspec ? comspec : "cmd.exe", direct_executable_available);
#else
    return resolved.command;
#endif
}

[[nodiscard]] ResolvedSessionCommand resolve_session_command(
    const MeshConfig& cfg,
    const std::string& session_name,
    const std::string& client_command) {
    if (!client_command.empty()) {
        return {client_command, SessionCommandSource::ClientOverride};
    }
    auto it = cfg.session_commands.find(session_name);
    if (it != cfg.session_commands.end() && !it->second.empty()) {
        return {it->second, SessionCommandSource::NamedProfile};
    }
    return {cfg.default_shell, SessionCommandSource::ConfigDefault};
}

// ── save_config — write MeshConfig back to file ──────────────────────

[[nodiscard]] bool save_config(const std::string& path, const MeshConfig& cfg) {
    std::string resolved = expand_home(path);

    std::ofstream f(resolved, std::ios::trunc);
    if (!f.is_open()) return false;

    f << "# bridgesessions mesh config\n";
    f << "# Generated — edit with care\n\n";

    // Node section
    f << "# ── Node identity ──────────────────────────────────\n";
    f << "node.name " << cfg.node_name << "\n";
    f << "node.listen " << cfg.listen_addr << ":" << cfg.listen_port << "\n";
    f << "\n";

    // Mesh section
    f << "# ── Mesh settings ──────────────────────────────────\n";
    f << "mesh.max_peers " << cfg.max_peers << "\n";
    f << "mesh.gossip_interval_secs " << cfg.gossip_interval_secs << "\n";
    f << "mesh.reconnect_backoff_max_secs " << cfg.reconnect_backoff_max_secs << "\n";
    f << "mesh.join_window_max_secs " << cfg.join_window_max_secs << "\n";
    f << "mesh.ping_interval_secs " << cfg.ping_interval_secs << "\n";
    f << "mesh.pong_timeout_secs " << cfg.pong_timeout_secs << "\n";
    f << "mesh.auto_upgrade " << (cfg.auto_upgrade ? "true" : "false") << "\n";
    f << "mesh.auto_upgrade_cooldown_secs " << cfg.auto_upgrade_cooldown_secs << "\n";
    f << "mesh.require_seed_pins " << (cfg.require_seed_pins ? "true" : "false") << "\n";
    f << "mesh.discovered_ttl_secs " << cfg.discovered_ttl_secs << "\n";
    f << "mesh.mdns_enabled " << (cfg.mdns_enabled ? "true" : "false") << "\n";
    f << "transfer.max_bytes " << cfg.transfer_max_bytes << "\n";
    f << "transfer.allow_sensitive_paths "
      << (cfg.allow_sensitive_paths ? "true" : "false") << "\n";
    f << "file.dest_allow_home " << (cfg.dest_allow_home ? "true" : "false") << "\n";
    f << "transport.webrtc_enabled " << (cfg.webrtc_enabled ? "true" : "false") << "\n";
    f << "dht.enabled " << (cfg.dht_enabled ? "true" : "false") << "\n";
    f << "upnp.enabled " << (cfg.upnp_enabled ? "true" : "false") << "\n";
    // Virtual folders
    for (auto& v : cfg.vfolders) {
        std::string prefix = "vfolder." + v.name + ".";
        f << prefix << "local " << v.local_path << "\n";
        f << prefix << "peer " << v.remote_peer << "\n";
        f << prefix << "remote " << v.remote_path << "\n";
        f << prefix << "direction " << v.direction << "\n";
        f << prefix << "interval " << v.sync_interval_secs << "\n";
    }
    f << "\n";

    // Seeds
    f << "# ── Bootstrap peers ────────────────────────────────\n";
    for (const auto& s : cfg.seeds) {
        write_peer_line(f, "seed", s);
    }
    f << "\n";

    // Discovered peers are runtime state learned via trusted mDNS/gossip.
    // They are intentionally NOT persisted so untrusted LAN announcements cannot
    // be written back to the operator's config file.
    (void)cfg.discovered;

    // Sessions
    f << "# ── Session defaults ───────────────────────────────\n";
    f << "sessions.scrollback_lines " << cfg.scrollback_lines << "\n";
    f << "sessions.idle_timeout_hours " << cfg.idle_timeout_hours << "\n";
    f << "sessions.default_shell " << cfg.default_shell << "\n";
    f << "sessions.terminal " << cfg.terminal << "\n";
    f << "sessions.persistence_path " << cfg.persistence_path << "\n";
    f << "sessions.authorized_keys_path " << cfg.authorized_keys_path << "\n";
    if (!cfg.session_commands.empty()) {
        f << "\n# ── Named persistent session commands ───────────────────\n";
        std::vector<std::pair<std::string, std::string>> profiles(
            cfg.session_commands.begin(), cfg.session_commands.end());
        std::sort(profiles.begin(), profiles.end());
        for (const auto& [name, command] : profiles) {
            f << "session." << name << ".command " << command << "\n";
        }
    }
    if (!cfg.harness_commands.empty()) {
        f << "\n# ── Harness launch commands (`bs connect` selector) ────\n";
        std::vector<std::pair<std::string, std::string>> harnesses(
            cfg.harness_commands.begin(), cfg.harness_commands.end());
        std::sort(harnesses.begin(), harnesses.end());
        for (const auto& [name, command] : harnesses) {
            f << "harness." << name << " " << command << "\n";
        }
    }

    f.close();
    return true;
}

// ────────────────────────────────────────────────────────────────────
// 8. PERSISTENCE — JSON session persistence (nlohmann/json)
// ────────────────────────────────────────────────────────────────────

struct SessionMeta {
    std::string name;
    std::string owner_id;   // unused in mesh (empty), kept for compat
    std::string command;
    std::string state;
    std::string created_at; // ISO 8601
};

// Save session list to JSON file (atomic write: temp + rename)
// v1:plain format with room for future encryption
inline bool save_sessions(const std::string& path,
                          const std::vector<SessionMeta>& sessions) {
    nlohmann::json j = nlohmann::json::array();
    for (auto& s : sessions) {
        nlohmann::json entry;
        entry["name"] = s.name;
        entry["owner_id"] = s.owner_id;
        entry["command"] = redact_secrets(s.command);
        entry["state"] = s.state;
        entry["created_at"] = s.created_at;
        j.push_back(entry);
    }
    std::string plain = j.dump(2);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << "v1:plain\n" << plain << '\n';
        f.flush();
        if (!f) { std::filesystem::remove(tmp); return false; }
    }
    if (!restrict_private_file_permissions(tmp)) {
        std::filesystem::remove(tmp);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(tmp); return false; }
    return true;
}

// Load session list (supports v1:plain and legacy raw JSON)
inline std::vector<SessionMeta> load_sessions(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string header;
    if (!std::getline(f, header)) return {};
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string plain;
    if (header == "v1:plain") {
        plain = data;
    } else {
        plain = header + "\n" + data;  // legacy raw JSON
    }
    try {
        auto j = nlohmann::json::parse(plain);
        std::vector<SessionMeta> result;
        for (auto& entry : j) {
            SessionMeta m;
            m.name = entry.value("name", "");
            m.owner_id = entry.value("owner_id", "");
            m.command = entry.value("command", "");
            m.state = entry.value("state", "detached");
            m.created_at = entry.value("created_at", "");
            result.push_back(m);
        }
        return result;
    } catch (...) { return {}; }
}

// ────────────────────────────────────────────────────────────────────
// 9. LOGGING — structured JSON logging via spdlog
// ────────────────────────────────────────────────────────────────────

struct StructuredLoggerState {
    std::mutex mutex;
    std::string app_home;
    std::shared_ptr<spdlog::logger> logger;
};

inline StructuredLoggerState& structured_logger_state() {
    static StructuredLoggerState state;
    return state;
}

inline void configure_logger_home(const std::string& app_home) {
    if (app_home.empty()) return;
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.app_home == app_home) return;
    if (state.logger) state.logger->flush();
    spdlog::drop("bs-mesh");
    state.logger.reset();
    state.app_home = app_home;
}

#ifdef BS_TESTING
inline void reset_logger_for_test() {
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.logger) state.logger->flush();
    spdlog::drop("bs-mesh");
    state.logger.reset();
    state.app_home.clear();
}
#endif

// Thread-safe JSON logger
inline std::shared_ptr<spdlog::logger> get_logger() {
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.logger) return state.logger;

    if (state.app_home.empty()) {
        const char* home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        if (!home || !*home)
            throw std::runtime_error("cannot initialize logger: home directory unavailable");
        state.app_home = (std::filesystem::path(home) / ".bridgesessions").string();
    }
    if (!ensure_private_directory(state.app_home))
        throw std::runtime_error("cannot initialize logger directory " + state.app_home);

    const std::string path =
        (std::filesystem::path(state.app_home) / "bs-mesh.log").string();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        path, 1'048'576, 3);  // 1 MB, 3 rotated files
    file_sink->set_pattern("%v");  // raw JSON lines

    state.logger = std::make_shared<spdlog::logger>("bs-mesh", file_sink);
    state.logger->set_level(spdlog::level::debug);
    state.logger->flush_on(spdlog::level::info);
    spdlog::register_logger(state.logger);
    return state.logger;
}

// Log a structured event as a single JSON line.
inline void log_event_at(spdlog::level::level_enum level,
                         const std::string& event, const std::string& detail) {
    auto l = get_logger();
    nlohmann::json j;
    j["ts"] = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    // P3 audit fix: also include wall-clock ISO time so logs correlate with
    // system/peer logs across processes (steady_clock has no defined epoch).
    {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &tt);
#else
        gmtime_r(&tt, &tm);
#endif
        char tbuf[32]{};
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        j["wall"] = tbuf;
    }
    j["event"] = event;
    if (!detail.empty()) j["detail"] = detail;
    l->log(level, j.dump());
}

inline void log_event(const std::string& event, const std::string& detail = "") {
    log_event_at(spdlog::level::info, event, detail);
}

inline void log_debug_event(const std::string& event, const std::string& detail = "") {
    log_event_at(spdlog::level::debug, event, detail);
}

