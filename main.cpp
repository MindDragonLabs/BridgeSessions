// main.cpp — BridgeSessions CLI entrypoint + daemon launcher
// Extracted from bridgesessions.cpp (R3 structural refactor, 2026-07-23)
#include "bs-protocol.h"
#include "bs-cua-helper.h"

// ────────────────────────────────────────────────────────────────────
// 2. MAIN — CLI + daemon (guarded for test builds)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_TESTING

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <random>
#include <fstream>
#include <iostream>
#include <unordered_set>

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
    sa.sin_port = htons(19980);
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
    char buf[4096] = {}; int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n > 0) { total += n; buf[total] = '\0'; if (strchr(buf, '\n')) break; }
        else break;
    }
    CLOSESOCK(sfd);
    std::string line(buf);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    return line;
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

#ifndef _WIN32
    fs::path unit = fs::path(resolve_home("~/.config/systemd/user")) / "bridgesessions.service";
    if (fs::exists(unit) && fs::is_regular_file(unit)) pass("systemd unit", unit.string());
    else warn("systemd unit", unit.string());
#endif

    std::string ipc = daemon_simple_ipc("HEALTH __doctor_nonexistent__", 1500, app_home);
    if (!ipc.empty()) pass("daemon IPC", "port 19980 answered");
    else warn("daemon IPC", "port 19980 did not answer");

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

} // anonymous namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    // P3: seed PRNG from /dev/urandom (or random_device) — used by jitter, temp names
    {
        std::random_device rd;
        srand(rd());
    }

    CLI::App app{"bridgesessions — mesh terminal relay"};
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

    // Fast path: `bs dev hermes` (the `bs` executable is a symlink to this binary).
    // Unknown peer names are resolved through `ssh -G` for address discovery only;
    // terminal data still travels exclusively over the BridgeSessions protocol.
    std::string quick_peer, quick_session = "shell";
    app.add_option("peer", quick_peer, "Peer name or SSH Host alias");
    app.add_option("session", quick_session, "Persistent server session name (default: shell)");

    // Subcommand: shell
    std::string shell_peer, shell_session = "default", shell_cmd;
    uint16_t shell_cols = 80, shell_rows = 24;
    bool shell_detach = false, shell_wait = false;
    bool shell_record = false, shell_signal_forward = true;
    std::string shell_signal_on_detach;
    auto* shell_cmd_app = app.add_subcommand("shell", "Open shell on a peer");
    shell_cmd_app->add_option("peer", shell_peer, "Peer name")->required();
    shell_cmd_app->add_option("-n,--name", shell_session, "Session name");
    shell_cmd_app->add_option("-x,--cmd", shell_cmd, "Command override");
    auto* shell_cols_opt = shell_cmd_app->add_option("--cols", shell_cols, "Terminal columns");
    auto* shell_rows_opt = shell_cmd_app->add_option("--rows", shell_rows, "Terminal rows");
    shell_cmd_app->add_flag("-r,--record", shell_record, "Record session output to file");
    shell_cmd_app->add_flag("--signal-forward{true}", shell_signal_forward, "Forward Ctrl-C to remote child (default: on)")->default_str("true");
    shell_cmd_app->add_option("--signal-on-detach", shell_signal_on_detach, "Send HUP/TERM/INT/QUIT/KILL to the child when the last peer detaches (default: none)")->check(CLI::IsMember({"HUP","TERM","INT","QUIT","KILL"}));
    shell_cmd_app->add_flag("--detach", shell_detach, "Send command and return immediately (session runs on peer)");
    shell_cmd_app->add_flag("--wait", shell_wait, "Block until the named session exits, then return its exit code");

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
    // health
    std::string health_peer;
    auto* health_cmd_app = app.add_subcommand("health", "Ping/pong health check against a peer");
    health_cmd_app->add_option("peer", health_peer, "Peer name")->required();
    // reconnect
    std::string reconnect_peer;
    auto* reconnect_cmd_app = app.add_subcommand("reconnect", "Tear down and re-handshake one peer via the running daemon");
    reconnect_cmd_app->add_option("peer", reconnect_peer, "Peer name")->required();
    // invite
    auto* invite_cmd_app = app.add_subcommand("invite", "Generate an invite token for new nodes");
    // join
    std::string join_addr;
    std::string join_token;
    bool join_start = false;
    std::string join_node_name;
    auto* join_cmd_app = app.add_subcommand("join", "Join a mesh via invite token");
    join_cmd_app->add_option("addr", join_addr, "Host:port")->required();
    join_cmd_app->add_option("token", join_token, "Invite token")->required();
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
    auto* fleet_cmd_app = app.add_subcommand("fleet", "Show live fleet directory with peer names, addresses, versions, and status");

    // telemetry
    bool telemetry_json = false;
    auto* telemetry_cmd_app = app.add_subcommand("telemetry", "Show transfer telemetry");
    telemetry_cmd_app->add_flag("--json", telemetry_json, "Output as JSON");

    // file
    auto* file_cmd = app.add_subcommand("file", "File transfer operations");
    file_cmd->require_subcommand(1);
    std::string file_send_peer, file_send_path;
    bool file_send_wait = false;
    auto* file_send_app = file_cmd->add_subcommand("send", "Send file to a peer's receive directory");
    file_send_app->add_option("peer", file_send_peer, "Peer name")->required();
    file_send_app->add_option("local", file_send_path, "Local file path")->required();
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

    // pane (BridgePanel publish from the mesh CLI)
    std::string pane_session = "default", pane_type = "documents", pane_title, pane_file;
    auto* pane_cmd_app = app.add_subcommand("pane", "Publish a file to the BridgePanel surface");
    pane_cmd_app->require_subcommand(1);
    auto* pane_publish = pane_cmd_app->add_subcommand("publish", "Copy a local file into a BridgePanel session");
    pane_publish->add_option("--session", pane_session, "Session name (default: default)");
    pane_publish->add_option("--type", pane_type, "documents | comms");
    pane_publish->add_option("--title", pane_title, "Display title / filename override");
    pane_publish->add_option("file", pane_file, "Local markdown file to publish")->required();

    CLI11_PARSE(app, argc, argv);

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

    // Dispatch
    if (!quick_peer.empty()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
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
        auto [cols, rows] = bs::mesh::get_winsize();
        bs::mesh::MeshController mc(cfg, home_dir);
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
        return mc.shell_peer(shell_peer, shell_session, shell_cmd, shell_cols, shell_rows, "xterm-256color", shell_signal_forward, shell_signal_on_detach);
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
        return cmd_doctor(config_path, home_dir);
    }
    if (peers_list->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        // Show known peers from config
        std::cout << "=== Known peers ===\n";
        for (auto& p : cfg.seeds) std::cout << "  [seed] " << p.name << " " << p.addr << std::endl;
        for (auto& p : cfg.discovered) std::cout << "  [discovered] " << p.name << " " << p.addr << std::endl;
        // Show live connection status if daemon is running
        bs::mesh::MeshController mc(cfg, home_dir);
        mc.show_peers_detail();
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

    if (health_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string status;
        bool ok = mc.health_check(health_peer, &status);
        std::cout << health_peer << " " << status << std::endl;
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
    if (invite_cmd_app->parsed()) {
        std::string token = daemon_simple_ipc("INVITE", 2000, home_dir);
        if (token.empty()) { std::cerr << "INVITE failed (daemon not running?)\n"; return 1; }
        while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        std::string addr = cfg.listen_addr;
        if (addr.empty() || addr == "0.0.0.0") {
            // Detect reachable address: prefer Tailscale, then best non-loopback
            FILE* ts = popen("tailscale ip -4 2>/dev/null", "r");
            if (ts) {
                char buf[64] = {};
                if (fgets(buf, sizeof(buf), ts)) {
                    std::string ts_ip(buf);
                    while (!ts_ip.empty() && (ts_ip.back() == '\n' || ts_ip.back() == '\r')) ts_ip.pop_back();
                    if (!ts_ip.empty()) addr = ts_ip;
                }
                pclose(ts);
            }
            if (addr.empty() || addr == "0.0.0.0") addr = "127.0.0.1";
        }
        int port = cfg.listen_port > 0 ? cfg.listen_port : 19949;
        std::cout << "Invite (valid 2h):  " << token << "\n";
        std::cout << "One-liner:\n";
        std::cout << "  bridgesessions join " << addr << ":" << port << " " << token << "\n";
        std::cout << "Or with curl install:\n";
        std::cout << "  curl -fsSL https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v" << bs::mesh::kBridgeSessionsVersion << "/scripts/install.sh | bash -s -- join " << addr << ":" << port << " " << token << "\n";
        std::cout << "Windows PowerShell:\n";
        std::cout << "  irm https://codeberg.org/Mind-Dragon/BridgeSessions/raw/tag/v" << bs::mesh::kBridgeSessionsVersion << "/scripts/install.ps1 | iex\n  bridgesessions join " << addr << ":" << port << " " << token << "\n";
        return 0;
    }
    if (join_cmd_app->parsed()) {
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
        if (sep != std::string::npos) port = std::stoi(join_addr.substr(sep + 1));

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
        write_frame(conn.ssl.get(), jr, bs::mesh::CONTROL_STREAM_ID);

        // Read JoinReply
        bs::mesh::Message msg = bs::mesh::read_frame(conn.ssl.get());
        if (!std::holds_alternative<bs::mesh::JoinReplyMsg>(msg)) {
            std::cerr << "Unexpected reply from host\n";
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
        host_seed.addr = jrep.host_addr;
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
                    if (!pe.name.empty() && !pe.addr.empty() && !pe.pubkey_hex.empty() &&
                        pe.pubkey_hex != jrep.host_pubkey) {
                        cfg.seeds.push_back(std::move(pe));
                    }
                }
            } catch (...) {}
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
                    } catch (...) {}
                }
            }
        }
        std::cout << "Joined. Node: " << cfg.node_name << "  Config: " << config_path << "\n";
        if (join_start) {
            // Start daemon in background
            std::cout << "→ Starting daemon...\n";
#ifdef _WIN32
            std::string cmd = "start /B bridgesessions --daemon";
#else
            std::string cmd = "nohup bridgesessions --daemon > /dev/null 2>&1 &";
#endif
            int rc = std::system(cmd.c_str());
            if (rc != 0) {
                std::cout << "→ Could not auto-start daemon (system rc=" << rc
                          << "). Start it manually: bridgesessions --daemon\n";
            } else {
                std::cout << "→ Daemon started. Run 'bridgesessions health <peer>' to verify.\n";
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
        try {
            auto j = nlohmann::json::parse(ipc);
            // Markdown table output
            std::cout << "| Name | Address | Version | Status | Uptime |\n";
            std::cout << "|------|---------|---------|--------|--------|\n";
            for (auto& [key, val] : j.items()) {
                std::string name = key;
                std::string addr = val.value("addr", "");
                std::string version = val.value("version", "");
                std::string status = val.value("status", "");
                std::string uptime;
                if (val.contains("uptime_s") && status != "self") {
                    uint64_t s = val.value("uptime_s", 0ULL);
                    if (s >= 86400) uptime = std::to_string(s / 86400) + "d";
                    else if (s >= 3600) uptime = std::to_string(s / 3600) + "h";
                    else if (s >= 60) uptime = std::to_string(s / 60) + "m";
                    else uptime = std::to_string(s) + "s";
                }
                std::cout << "| " << name << " | " << addr << " | " << version
                          << " | " << status << " | " << uptime << " |\n";
            }
        } catch (...) {
            std::cerr << "fleet: failed to parse JSON\\n";
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
        std::string result = mc.file_send(file_send_peer, file_send_path, file_send_wait);
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
            const char* ext = resp.format == 2 ? "jpg" : "png";
            if (!cua_output.empty()) {
                std::ofstream f(cua_output, std::ios::binary);
                f.write(reinterpret_cast<const char*>(resp.data.data()),
                        static_cast<std::streamsize>(resp.data.size()));
                std::cout << "Saved " << resp.data.size() << " bytes to " << cua_output << "\n";
            } else {
                // Binary to stdout for piping
                std::fwrite(resp.data.data(), 1, resp.data.size(), stdout);
                std::string chosen_ext = resp.format == 2 ? "jpeg" : "png";
                std::cerr << resp.data.size() << " bytes (" << chosen_ext << ")\n";
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
        FILE* which = popen("command -v bridgepanel 2>/dev/null", "r");
        if (which) {
            char buf[512];
            std::string found;
            while (std::fgets(buf, sizeof(buf), which)) found += buf;
            pclose(which);
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
    if (vfolder_sync->parsed()) {
        bs::mesh::bootstrap_identity(home_dir);
        std::string ipc = daemon_simple_ipc("VFOLDER_SYNC " + vfolder_name, 300000, home_dir);
        if (ipc.empty()) { std::cerr << "no daemon running\n"; return 1; }
        std::cout << ipc << "\n";
        return ipc.rfind("ERROR", 0) == 0 ? 1 : 0;
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
        return bs::mesh::run_cua_helper(home_dir);
    }

    // Default: daemon mode
    bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
    bs::mesh::bootstrap_identity(home_dir);
#ifdef _WIN32
    if (daemon_flag) {
        FreeConsole();
        FILE* nul = fopen("nul", "w");
        if (nul) { fclose(stdout); _dup2(_fileno(nul), _fileno(stdout)); fclose(nul); }
    }
#else
    if (daemon_flag) {
        pid_t pid = fork();
        if (pid < 0) { std::cerr << "fork failed\n"; return 1; }
        if (pid > 0) { std::cout << pid << std::endl; return 0; }
        setsid();
        if (!freopen("/dev/null", "r", stdin) ||
            !freopen("/dev/null", "w", stdout) ||
            !freopen("/dev/null", "w", stderr)) {
            return 1;  // stdio detach failed; nothing reliable left to report on
        }
    }
#endif
    bs::mesh::MeshController mc(cfg, home_dir);
    mc.run();
    return 0;
}

#endif
