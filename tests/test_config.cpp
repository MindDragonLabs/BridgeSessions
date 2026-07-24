#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

#include <cstdio>
#include <fstream>
#include <filesystem>

using namespace bs::mesh;
namespace fs = std::filesystem;

// Helper: write a temp config file, return path
static std::string write_temp_config(const std::string& content) {
    auto tmp = fs::temp_directory_path() / "bs_test_config";
    fs::create_directories(tmp);
    auto cfg_path = (tmp / "config").string();
    std::ofstream f(cfg_path);
    f << content;
    f.close();
    return cfg_path;
}

// ── Test 1: load_config with node.name and one seed ─────────────────

TEST_CASE("load_config populates node_name and seeds", "[config]") {
    auto cfg_path = write_temp_config(
        "node.name shadow\n"
        "seed test-pc1 203.0.113.10:9948\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.node_name == "shadow");
    REQUIRE(cfg.seeds.size() == 1);
    REQUIRE(cfg.seeds[0].name == "test-pc1");
    REQUIRE(cfg.seeds[0].addr == "203.0.113.10:9948");

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 2: defaults are set when keys are missing ──────────────────

TEST_CASE("load_config fills defaults for missing keys", "[config]") {
    auto cfg_path = write_temp_config(
        "# empty config — just a comment\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.node_name == "unnamed");
    REQUIRE(cfg.listen_addr == "0.0.0.0");
    REQUIRE(cfg.listen_port == 19949);
    REQUIRE(cfg.max_peers == 50);
    REQUIRE(cfg.gossip_interval_secs == 30);
    REQUIRE(cfg.reconnect_backoff_max_secs == 30);
    REQUIRE(cfg.ping_interval_secs == 5);
    REQUIRE(cfg.pong_timeout_secs == 30);
    REQUIRE(cfg.scrollback_lines == 2000);
    REQUIRE(cfg.idle_timeout_hours == 168);
    REQUIRE(cfg.terminal == "xterm-256color");
#ifdef _WIN32
    REQUIRE((cfg.default_shell == "pwsh.exe -NoLogo" ||
             cfg.default_shell.find("powershell.exe -NoLogo") != std::string::npos));
#elif defined(__APPLE__)
    REQUIRE(cfg.default_shell == "/bin/zsh -il");
#else
    REQUIRE(cfg.default_shell == "/bin/bash -l");
#endif
    REQUIRE(cfg.seeds.empty());
    REQUIRE(cfg.discovered.empty());

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 3: ~ expansion ────────────────────────────────────────────

TEST_CASE("expand_home expands tilde to USERPROFILE or HOME", "[config]") {
    std::string home;
#ifdef _WIN32
    const char* env = std::getenv("USERPROFILE");
#else
    const char* env = std::getenv("HOME");
#endif
    REQUIRE(env != nullptr);
    home = env;

    std::string result = expand_home("~/.bridgesessions/config");
    REQUIRE(result == home + "/.bridgesessions/config");

    // No tilde: no change
    REQUIRE(expand_home("/etc/hosts") == "/etc/hosts");
    REQUIRE(expand_home("C:\\Users\\Example\\test") == "C:\\Users\\Example\\test");
}

// ── Test 4: missing file returns defaults ──────────────────────────

TEST_CASE("shell command uses terminal mode only when stdin is interactive", "[cli][shell]") {
    REQUIRE(shell_command_uses_interactive_mode("", false));
    REQUIRE(shell_command_uses_interactive_mode("hermes --tui --yolo", true));
    REQUIRE_FALSE(shell_command_uses_interactive_mode("uname -a", false));
}

TEST_CASE("load_config missing file returns defaults", "[config]") {
    MeshConfig cfg = load_config("/nonexistent/path/config_ghost_12345");
    REQUIRE(cfg.node_name == "unnamed");
    REQUIRE(cfg.seeds.empty());
    REQUIRE(cfg.discovered.empty());
}

// ── Test 5: duplicate seed lines — last one wins ────────────────────

TEST_CASE("load_config duplicate seed name overwrites", "[config]") {
    auto cfg_path = write_temp_config(
        "seed test-pc1 203.0.113.10:9948\n"
        "seed test-pc1 10.0.0.1:9999\n"
    );

    MeshConfig cfg = load_config(cfg_path);
    // Both are added; the user spec says seeds are a vector, so both should be present
    // Actually re-reading: "duplicate seed lines → last one wins"
    // This means the LAST entry with the same name replaces the previous.
    // But seeds is a vector of PeerEntry — we should deduplicate by name.
    REQUIRE(cfg.seeds.size() == 1);
    REQUIRE(cfg.seeds[0].name == "test-pc1");
    REQUIRE(cfg.seeds[0].addr == "10.0.0.1:9999");

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 6: round-trip — discovered peers are runtime state only ────

TEST_CASE("round-trip: save does not persist discovered peers", "[config][security]") {
    auto cfg_path = write_temp_config(
        "node.name testnode\n"
        "seed test-pc1 203.0.113.10:9948\n"
    );

    // Load
    MeshConfig cfg = load_config(cfg_path);
    REQUIRE(cfg.node_name == "testnode");
    REQUIRE(cfg.seeds.size() == 1);

    // Add discovered peers (as would happen from mDNS/gossip)
    PeerEntry p1;
    p1.name = "corp-net";
    p1.addr = "10.0.0.5:19948";
    p1.pubkey_hex = "abc123";
    p1.last_seen = 1234567890;
    cfg.discovered.push_back(p1);

    // Save
    bool ok = save_config(cfg_path, cfg);
    REQUIRE(ok);

    // Reload: discovered peers must not be persisted (runtime state only).
    MeshConfig cfg2 = load_config(cfg_path);
    REQUIRE(cfg2.node_name == "testnode");
    REQUIRE(cfg2.seeds.size() == 1);
    REQUIRE(cfg2.seeds[0].name == "test-pc1");
    REQUIRE(cfg2.discovered.empty());

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 7: comments and blank lines ignored ────────────────────────

TEST_CASE("load_config ignores comments and blank lines", "[config]") {
    auto cfg_path = write_temp_config(
        "# This is a comment\n"
        "\n"
        "node.name mynode\n"
        "   # indented comment? line with only whitespace and comment\n"
        "  \n"
        "mesh.max_peers 100\n"
        "# another comment\n"
        "seed home 127.0.0.1:19948\n"
        "\n"
        "sessions.scrollback_lines 5000\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.node_name == "mynode");
    REQUIRE(cfg.max_peers == 100);
    REQUIRE(cfg.seeds.size() == 1);
    REQUIRE(cfg.seeds[0].name == "home");
    REQUIRE(cfg.seeds[0].addr == "127.0.0.1:19948");
    REQUIRE(cfg.scrollback_lines == 5000);

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 8: multiple seeds ─────────────────────────────────────────

TEST_CASE("load_config handles multiple seeds", "[config]") {
    auto cfg_path = write_temp_config(
        "node.name multi\n"
        "seed test-pc1 203.0.113.10:9948\n"
        "seed test-pc2 203.0.113.20:19948\n"
        "seed home 127.0.0.1:9948\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.node_name == "multi");
    REQUIRE(cfg.seeds.size() == 3);
    REQUIRE(cfg.seeds[0].name == "test-pc1");
    REQUIRE(cfg.seeds[0].addr == "203.0.113.10:9948");
    REQUIRE(cfg.seeds[1].name == "test-pc2");
    REQUIRE(cfg.seeds[1].addr == "203.0.113.20:19948");
    REQUIRE(cfg.seeds[2].name == "home");
    REQUIRE(cfg.seeds[2].addr == "127.0.0.1:9948");

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 9: mesh config keys parsed correctly ───────────────────────

TEST_CASE("load_config parses mesh settings", "[config]") {
    auto cfg_path = write_temp_config(
        "node.name test\n"
        "node.listen :8080\n"
        "mesh.max_peers 25\n"
        "mesh.gossip_interval_secs 60\n"
        "mesh.reconnect_backoff_max_secs 45\n"
        "mesh.ping_interval_secs 10\n"
        "mesh.pong_timeout_secs 20\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.node_name == "test");
    REQUIRE(cfg.listen_port == 8080);
    REQUIRE(cfg.max_peers == 25);
    REQUIRE(cfg.gossip_interval_secs == 60);
    REQUIRE(cfg.reconnect_backoff_max_secs == 45);
    REQUIRE(cfg.ping_interval_secs == 10);
    REQUIRE(cfg.pong_timeout_secs == 20);

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 10: node.listen with full address ─────────────────────────

TEST_CASE("node.listen parses host:port", "[config]") {
    auto cfg_path = write_temp_config(
        "node.listen 127.0.0.1:9999\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.listen_addr == "127.0.0.1");
    REQUIRE(cfg.listen_port == 9999);

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 11: node.listen with port only ────────────────────────────

TEST_CASE("node.listen parses port-only", "[config]") {
    auto cfg_path = write_temp_config(
        "node.listen :7777\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.listen_addr == "0.0.0.0");
    REQUIRE(cfg.listen_port == 7777);

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 12: sessions settings parsed correctly ─────────────────────

TEST_CASE("load_config parses sessions settings", "[config]") {
    auto cfg_path = write_temp_config(
        "sessions.scrollback_lines 4096\n"
        "sessions.idle_timeout_hours 720\n"
        "sessions.default_shell /bin/zsh\n"
        "sessions.terminal screen-256color\n"
        "sessions.persistence_path /var/sessions.json\n"
        "sessions.authorized_keys_path /etc/bs/authorized_keys\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.scrollback_lines == 4096);
    REQUIRE(cfg.idle_timeout_hours == 720);
    REQUIRE(cfg.default_shell == "/bin/zsh");
    REQUIRE(cfg.terminal == "screen-256color");
    REQUIRE(cfg.persistence_path == "/var/sessions.json");
    REQUIRE(cfg.authorized_keys_path == "/etc/bs/authorized_keys");

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 13: unknown keys silently ignored ─────────────────────────

TEST_CASE("unknown config keys are silently ignored", "[config]") {
    auto cfg_path = write_temp_config(
        "node.name test\n"
        "node.future_feature enabled\n"
        "mesh.unknown_option 42\n"
        "future.section value\n"
        "seed test-pc1 203.0.113.10:9948\n"
    );

    MeshConfig cfg = load_config(cfg_path);
    REQUIRE(cfg.node_name == "test");
    REQUIRE(cfg.seeds.size() == 1);

    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── Test 14: malformed lines don't crash ────────────────────────────

TEST_CASE("malformed config lines don't crash", "[config]") {
    auto cfg_path = write_temp_config(
        "nodename\n"           // no space
        "node.name\n"          // missing value
        "  \n"                 // whitespace only
        "\t\t\n"               // tabs only
        "node.name test\n"    // valid line after junk
    );

    MeshConfig cfg = load_config(cfg_path);
    REQUIRE(cfg.node_name == "test");

    fs::remove_all(fs::path(cfg_path).parent_path());
}

TEST_CASE("load_config parses server-owned session commands", "[config][session_profile]") {
    auto cfg_path = write_temp_config(
        "sessions.default_shell /bin/bash -l\n"
        "session.hermes.command bash -lc 'cd /srv/hermes && exec hermes --tui --yolo'\n"
        "session.logs.command journalctl --user -f\n"
    );

    MeshConfig cfg = load_config(cfg_path);

    REQUIRE(cfg.session_commands.size() == 2);
    REQUIRE(cfg.session_commands.at("hermes") ==
            "bash -lc 'cd /srv/hermes && exec hermes --tui --yolo'");
    REQUIRE(cfg.session_commands.at("logs") == "journalctl --user -f");

    fs::remove_all(fs::path(cfg_path).parent_path());
}

TEST_CASE("save_config preserves server-owned session commands", "[config][session_profile]") {
    auto cfg_path = write_temp_config("node.name profile-save\n");
    MeshConfig cfg = load_config(cfg_path);
    cfg.session_commands["hermes"] =
        "bash -lc 'cd /srv/hermes && exec hermes --tui --yolo'";

    REQUIRE(save_config(cfg_path, cfg));
    MeshConfig reloaded = load_config(cfg_path);
    REQUIRE(reloaded.session_commands.at("hermes") ==
            cfg.session_commands.at("hermes"));

    fs::remove_all(fs::path(cfg_path).parent_path());
}

TEST_CASE("platform shell defaults prefer PowerShell and login zsh",
          "[config][session_profile]") {
    REQUIRE(default_shell_for_platform(HostPlatform::Windows, true) ==
            "pwsh.exe -NoLogo");
    REQUIRE(default_shell_for_platform(HostPlatform::Windows, false).find(
                "powershell.exe -NoLogo") != std::string::npos);
    REQUIRE(default_shell_for_platform(HostPlatform::MacOS, false) ==
            "/bin/zsh -il");
    REQUIRE(default_shell_for_platform(HostPlatform::Posix, false) ==
            "/bin/bash -l");
}

TEST_CASE("session command resolution preserves command provenance",
          "[config][session_profile]") {
    MeshConfig cfg;
    cfg.default_shell = "/bin/bash -l";
    cfg.session_commands["hermes"] = "exec hermes --tui --yolo";

    const auto override_cmd = resolve_session_command(cfg, "hermes", "echo override");
    REQUIRE(override_cmd.command == "echo override");
    REQUIRE(override_cmd.source == SessionCommandSource::ClientOverride);

    const auto profile_cmd = resolve_session_command(cfg, "hermes", "");
    REQUIRE(profile_cmd.command == "exec hermes --tui --yolo");
    REQUIRE(profile_cmd.source == SessionCommandSource::NamedProfile);

    const auto default_cmd = resolve_session_command(cfg, "work", "");
    REQUIRE(default_cmd.command == "/bin/bash -l");
    REQUIRE(default_cmd.source == SessionCommandSource::ConfigDefault);
}

TEST_CASE("Windows command builder wraps client overrides exactly once",
          "[config][session_profile][windows]") {
    MeshConfig cfg;
    cfg.default_shell = "pwsh.exe -NoLogo";
    cfg.session_commands["powershell"] = "pwsh.exe -NoLogo";

    const auto override_cmd = resolve_session_command(cfg, "one-shot", "dir");
    const auto wrapped = build_windows_command_line(
        override_cmd, "C:\\Windows\\System32\\cmd.exe");
    REQUIRE(wrapped ==
            "\"C:\\Windows\\System32\\cmd.exe\" /d /s /c \"dir\"");
    REQUIRE(wrapped.find("cmd.exe /c cmd.exe /c") == std::string::npos);
    REQUIRE(wrapped.find(" /d /s /c ") == wrapped.rfind(" /d /s /c "));

    const auto profile_cmd = resolve_session_command(cfg, "powershell", "");
    REQUIRE(build_windows_command_line(profile_cmd,
                                       "C:\\Windows\\System32\\cmd.exe") ==
            "pwsh.exe -NoLogo");

    cfg.session_commands["legacy-built-in"] = "dir C:\\\\";
    const auto legacy_cmd = resolve_session_command(cfg, "legacy-built-in", "");
    REQUIRE(build_windows_command_line(
                legacy_cmd, "C:\\Windows\\System32\\cmd.exe", false) ==
            "\"C:\\Windows\\System32\\cmd.exe\" /d /s /c \"dir C:\\\\\"");
}


TEST_CASE("verify_outbound_peer_identity enforces pin/cert/Hello binding",
          "[config][security][p0]") {
    // Happy path
    auto ok = verify_outbound_peer_identity(
        "abc", "abc", "abc", "test-pc1", "test-pc1", true);
    REQUIRE(ok.ok);

    // Missing pin when required
    auto miss = verify_outbound_peer_identity(
        "", "abc", "abc", "", "n", true);
    REQUIRE_FALSE(miss.ok);
    REQUIRE(miss.reason.find("pinned") != std::string::npos);

    // Pin mismatch
    auto badpin = verify_outbound_peer_identity(
        "abc", "zzz", "zzz", "", "", true);
    REQUIRE_FALSE(badpin.ok);

    // Hello != cert
    auto badhello = verify_outbound_peer_identity(
        "abc", "abc", "fff", "", "", true);
    REQUIRE_FALSE(badhello.ok);

    // Name mismatch
    auto badname = verify_outbound_peer_identity(
        "abc", "abc", "abc", "test-pc1", "attacker", true);
    REQUIRE_FALSE(badname.ok);

    // require_pin=false allows empty pin if cert present
    auto loose = verify_outbound_peer_identity(
        "", "abc", "abc", "", "peer-a", false);
    REQUIRE(loose.ok);
}

TEST_CASE("verify_outbound_peer_identity requires complete Hello identity",
          "[config][security][p0][alpha2]") {
    auto missing_key = verify_outbound_peer_identity(
        "abc", "abc", "", "peer-a", "peer-a", true);
    REQUIRE_FALSE(missing_key.ok);
    REQUIRE(missing_key.reason.find("Hello pubkey") != std::string::npos);

    auto missing_name = verify_outbound_peer_identity(
        "abc", "abc", "abc", "peer-a", "", true);
    REQUIRE_FALSE(missing_name.ok);
    REQUIRE(missing_name.reason.find("Hello node name") != std::string::npos);
}

TEST_CASE("verify_inbound_peer_identity binds configured name key and Hello",
          "[config][security][p0][alpha2]") {
    MeshConfig cfg;
    cfg.seeds.push_back(PeerEntry{
        .name = "peer-a", .addr = "203.0.113.10:19949", .pubkey_hex = "abc"});

    REQUIRE(verify_inbound_peer_identity(cfg, "abc", "abc", "peer-a").ok);
    REQUIRE_FALSE(verify_inbound_peer_identity(cfg, "abc", "", "peer-a").ok);
    REQUIRE_FALSE(verify_inbound_peer_identity(cfg, "abc", "zzz", "peer-a").ok);
    REQUIRE_FALSE(verify_inbound_peer_identity(cfg, "abc", "abc", "peer-b").ok);
    REQUIRE_FALSE(verify_inbound_peer_identity(cfg, "zzz", "zzz", "peer-a").ok);

    // A separately authorized key may introduce a new non-colliding name.
    REQUIRE(verify_inbound_peer_identity(cfg, "xyz", "xyz", "peer-new").ok);
}

TEST_CASE("sanitize_transfer_filename rejects traversal and device names",
          "[config][security][p0]") {
    REQUIRE(sanitize_transfer_filename("ok.txt").value() == "ok.txt");
    REQUIRE(sanitize_transfer_filename("a/b/c.txt").value() == "c.txt");
    // Basename of a relative traversal is allowed; containment is enforced by path_is_inside_directory.
    REQUIRE(sanitize_transfer_filename("../etc/passwd").value() == "passwd");
    REQUIRE(sanitize_transfer_filename("foo/bar.txt").value() == "bar.txt");
    REQUIRE_FALSE(sanitize_transfer_filename("/etc/passwd").has_value());
    REQUIRE_FALSE(sanitize_transfer_filename("C:\\Windows\\x").has_value());
    REQUIRE_FALSE(sanitize_transfer_filename("").has_value());
    REQUIRE_FALSE(sanitize_transfer_filename("..").has_value());
    REQUIRE_FALSE(sanitize_transfer_filename("CON").has_value());
    REQUIRE_FALSE(sanitize_transfer_filename("nul.txt").has_value());
    REQUIRE_FALSE(sanitize_transfer_filename(std::string("a\0b", 3)).has_value());
}

TEST_CASE("path_is_inside_directory contains paths under receive root",
          "[config][security][p0]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "bs_recv_test";
    fs::create_directories(tmp);
    auto good = tmp / "file.bin";
    REQUIRE(path_is_inside_directory(good, tmp));
    auto escape = tmp / ".." / "escape.bin";
    // weakly_canonical may resolve outside
    REQUIRE_FALSE(path_is_inside_directory(escape, tmp));
    fs::remove_all(tmp);
}

TEST_CASE("transfer metadata binds declared size to canonical chunk count",
          "[config][security][transfer][alpha2]") {
    REQUIRE(validate_transfer_metadata(0, 1, 1024).ok);
    REQUIRE(validate_transfer_metadata(1, 1, 1024).ok);
    REQUIRE(validate_transfer_metadata(kTransferChunkRawSize, 1, 1 << 20).ok);
    REQUIRE(validate_transfer_metadata(kTransferChunkRawSize + 1, 2, 1 << 20).ok);

    REQUIRE_FALSE(validate_transfer_metadata(1, 0, 1024).ok);
    REQUIRE_FALSE(validate_transfer_metadata(1, 2, 1024).ok);
    REQUIRE_FALSE(validate_transfer_metadata(1025, 1, 1024).ok);
    REQUIRE_FALSE(validate_transfer_metadata(
        1, std::numeric_limits<uint32_t>::max(), 1024).ok);
}

TEST_CASE("incoming file receive state is isolated per connection",
          "[config][security][transfer][alpha2]") {
    MeshController::Conn first;
    MeshController::Conn second;
    first.file_receive.active = true;
    first.file_receive.filename = "first.bin";

    REQUIRE_FALSE(second.file_receive.active);
    REQUIRE(second.file_receive.filename.empty());
}

TEST_CASE("incoming chunks cannot exceed declared file shape",
          "[config][security][transfer][alpha2]") {
    REQUIRE(validate_transfer_chunk(1, 0, 0, 1, 0, 1, 1).ok);
    REQUIRE_FALSE(validate_transfer_chunk(1, 0, 0, 1, 0, 1, 2).ok);
    REQUIRE_FALSE(validate_transfer_chunk(1, 0, 0, 1, 0, 2, 1).ok);
    REQUIRE_FALSE(validate_transfer_chunk(1, 0, 0, 1, 1, 1, 1).ok);

    REQUIRE(validate_transfer_chunk(
        kTransferChunkRawSize + 1, 0, 0, 2,
        0, 2, kTransferChunkRawSize).ok);
    REQUIRE(validate_transfer_chunk(
        kTransferChunkRawSize + 1, kTransferChunkRawSize, 1, 2,
        1, 2, 1).ok);
    REQUIRE_FALSE(validate_transfer_chunk(
        kTransferChunkRawSize + 1, kTransferChunkRawSize, 1, 2,
        1, 2, 0).ok);
    REQUIRE(validate_transfer_chunk(0, 0, 0, 1, 0, 1, 0).ok);
}

TEST_CASE("MeshController watches the explicitly loaded config path",
          "[config][config-dir][alpha2]") {
    auto root = fs::temp_directory_path() / "bs_explicit_config_test";
    fs::remove_all(root);
    fs::create_directories(root);
    auto explicit_config = (root / "operator.conf").string();
    std::ofstream(explicit_config) << "node.name explicit-config-test\n";

    MeshConfig cfg = load_config(explicit_config);
    {
        MeshController controller(cfg, root.string(), explicit_config);
        REQUIRE(controller.config_file_path_for_test() == explicit_config);
    }
    fs::remove_all(root);
}

TEST_CASE("direct peer commands reject an unpinned peer before TCP connect",
          "[config][security][pin][alpha2]") {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() /
                ("bs_direct_pin_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    MeshConfig cfg;
    cfg.node_name = "pin-test";
    MeshController controller(cfg, root.string());
    REQUIRE(controller.direct_connect_rejects_missing_pin_for_test());

    fs::remove_all(root);
}

TEST_CASE("hex_decode rejects malformed authorized keys",
          "[config][security][authorized_keys][alpha2]") {
    REQUIRE(hex_decode(std::string(64, 'z')).empty());
    REQUIRE(hex_decode("abc").empty());
    REQUIRE(hex_decode(std::string(64, '0')).size() == 32);
    REQUIRE(hex_decode(std::string(64, 'A')).size() == 32);
}

TEST_CASE("private text files are created owner-only",
          "[config][security][permissions][alpha2]") {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() /
                ("bs_private_file_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(ensure_private_directory(root.string()));
    auto path = root / "identity.pem";

#ifndef _WIN32
    const mode_t old_mask = ::umask(0022);
#endif
    REQUIRE(write_private_text_file(path.string(), "secret\n"));
    REQUIRE(append_private_text_file(path.string(), "next\n"));
#ifndef _WIN32
    ::umask(old_mask);
    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == 0600);
    REQUIRE(::stat(root.c_str(), &st) == 0);
    REQUIRE((st.st_mode & 0777) == 0700);
#endif
    std::ifstream in(path);
    std::string content((std::istreambuf_iterator<char>(in)), {});
    REQUIRE(content == "secret\nnext\n");
    in.close();

    fs::remove_all(root);
}

TEST_CASE("editor launcher passes hostile filenames as one argv element",
          "[config][security][editor][alpha2]") {
#ifdef _WIN32
    SUCCEED("covered by _spawnlp implementation on Windows");
#else
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() /
                ("bs_editor_argv_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    auto script = root / "editor";
    auto captured = root / "captured";
    auto injected = root / "injected";
    const std::string script_body =
        "#!/bin/sh\nprintf '%s' \"$1\" > \"$BS_CAPTURED\"\n";
    {
        std::ofstream out(script);
        out << script_body;
    }
    fs::permissions(script, fs::perms::owner_all,
                    fs::perm_options::replace);
    ::setenv("BS_CAPTURED", captured.c_str(), 1);

    const std::string hostile =
        (root / ("note;touch " + injected.string())).string();
    REQUIRE(run_editor_process(script.string(), hostile) == 0);
    std::ifstream in(captured);
    std::string received((std::istreambuf_iterator<char>(in)), {});
    REQUIRE(received == hostile);
    REQUIRE_FALSE(fs::exists(injected));

    ::unsetenv("BS_CAPTURED");
    fs::remove_all(root);
#endif
}

TEST_CASE("structured logger honors configured app home",
          "[config][isolation][logging][alpha2]") {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() /
                ("bs_log_home_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    configure_logger_home(root.string());
    log_event("alpha2_logger_isolation");
    REQUIRE(fs::exists(root / "bs-mesh.log"));

    reset_logger_for_test();
    fs::remove_all(root);
}

TEST_CASE("bounded TCP connect does not exceed its deadline",
          "[config][network][timeout][alpha2]") {
#ifdef _WIN32
    SUCCEED("covered by Windows integration build");
#else
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd != INVALID_SOCKET);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9);
    REQUIRE(inet_pton(AF_INET, "203.0.113.1", &sa.sin_addr) == 1);

    auto started = std::chrono::steady_clock::now();
    auto result = connect_socket_with_timeout(
        fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa), 100);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    REQUIRE_FALSE(result.connected);
    REQUIRE(elapsed < std::chrono::seconds(1));
    CLOSESOCK(fd);
#endif
}

TEST_CASE("release and mesh Hello share the canonical version",
          "[config][release][version]") {
    REQUIRE(std::string(kBridgeSessionsVersion) == "2.0.14-alpha6");

    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() /
                ("bs_version_" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    MeshConfig cfg;
    cfg.node_name = "version-test";
    MeshController controller(cfg, root.string());
    REQUIRE(controller.hello_version_for_test() == kBridgeSessionsVersion);
    fs::remove_all(root);
}

TEST_CASE("local IPC port supports a strict per-process override",
          "[config][ipc][multi-instance][alpha2]") {
    REQUIRE(resolve_mesh_cli_port(nullptr) == 19980);
    REQUIRE(resolve_mesh_cli_port("") == 19980);
    REQUIRE(resolve_mesh_cli_port("20081") == 20081);
    REQUIRE(resolve_mesh_cli_port("0") == 19980);
    REQUIRE(resolve_mesh_cli_port("65536") == 19980);
    REQUIRE(resolve_mesh_cli_port("20081junk") == 19980);
}

TEST_CASE("transfer IPC progress lines are emitted once across recv chunks",
          "[config][ipc][transfer][alpha2]") {
    std::string pending;
    std::vector<std::string> progress;
    auto emit = [&](const std::string& line) { progress.push_back(line); };

    REQUIRE_FALSE(consume_transfer_ipc_chunk(
        pending, "PROGRESS phase=send pct=1.0\nPRO", emit).has_value());
    auto terminal = consume_transfer_ipc_chunk(
        pending, "GRESS phase=send pct=2.0\nOK sent file.bin\n", emit);

    REQUIRE(progress == std::vector<std::string>{
        "PROGRESS phase=send pct=1.0",
        "PROGRESS phase=send pct=2.0"});
    REQUIRE(terminal == std::optional<std::string>{"OK sent file.bin"});
    REQUIRE(pending.empty());
}

TEST_CASE("PowerShell client overrides skip cmd /c so $_ survives",
          "[config][windows_cmd]") {
    // powershell is a real exe token → no cmd /c wrap (cmd wrap breaks pipes/$_).
    const std::string ps_body =
        "powershell -NoProfile -Command \"1..3 | ForEach-Object { $_ }\"";
    const auto override_ps =
        resolve_session_command(MeshConfig{}, "one-shot", ps_body);
    REQUIRE(override_ps.source == SessionCommandSource::ClientOverride);
    REQUIRE(command_has_direct_windows_exe_token(ps_body));
    REQUIRE(is_windows_cli_oneshot_command(ps_body));

    const auto direct = build_windows_command_line(
        override_ps, "C:\\Windows\\System32\\cmd.exe", true);
    REQUIRE(direct == ps_body);
    REQUIRE(direct.find("/c ") == std::string::npos);

    // Builtins still wrap, with doubled quotes if needed.
    const auto dir_cmd = resolve_session_command(MeshConfig{}, "one-shot", "dir");
    const auto wrapped = build_windows_command_line(
        dir_cmd, "C:\\Windows\\System32\\cmd.exe", true);
    REQUIRE(wrapped ==
            "\"C:\\Windows\\System32\\cmd.exe\" /d /s /c \"dir\"");

    REQUIRE(escape_for_cmd_slash_c("a\"b\"c") == "a\"\"b\"\"c");
    REQUIRE(escape_for_cmd_slash_c("noquotes") == "noquotes");
    REQUIRE(first_windows_cli_token(
                "powershell -NoProfile -Command x") == "powershell");
}

TEST_CASE("SSH config expansion yields a BridgeSessions hostname",
          "[config][ssh_alias]") {
    const std::string expanded =
        "host test-pc1\n"
        "user agent\n"
        "hostname 203.0.113.10\n"
        "port 22\n";

    REQUIRE(parse_ssh_g_hostname(expanded) == "203.0.113.10");
    REQUIRE(parse_ssh_g_hostname("host test-pc1\nuser agent\n").empty());
}

TEST_CASE("SSH alias imports as a transient BridgeSessions peer",
          "[config][ssh_alias]") {
    MeshConfig cfg;
    REQUIRE(import_ssh_alias_peer(
        cfg, "test-pc1", "hostname 203.0.113.10\nuser agent\n"));
    REQUIRE(cfg.seeds.size() == 1);
    REQUIRE(cfg.seeds[0].name == "test-pc1");
    REQUIRE(cfg.seeds[0].addr == "203.0.113.10:19949");

    cfg.seeds[0].addr = "test-pc1.internal:29949";
    cfg.seeds[0].pubkey_hex = "trusted-fingerprint";
    REQUIRE(import_ssh_alias_peer(cfg, "test-pc1", "hostname refreshed.example\n"));
    REQUIRE(cfg.seeds.size() == 1);
    REQUIRE(cfg.seeds[0].addr == "refreshed.example:19949");
    REQUIRE(cfg.seeds[0].pubkey_hex == "trusted-fingerprint");
    REQUIRE(trusted_peer_pubkey(cfg, "test-pc1") == "trusted-fingerprint");
    REQUIRE(trusted_peer_pubkey(cfg, "test-pc1") == "trusted-fingerprint");
    REQUIRE(peer_identity_matches("trusted-fingerprint", "trusted-fingerprint"));
    REQUIRE_FALSE(peer_identity_matches("trusted-fingerprint", "attacker-fingerprint"));
    REQUIRE_FALSE(peer_identity_matches("", "untrusted-first-contact"));

    REQUIRE_FALSE(import_ssh_alias_peer(cfg, "missing", "user agent\n"));
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

TEST_CASE("make_app_paths isolates under custom root", "[config][security][config-dir]") {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "bs-isolated-home";
    auto p = make_app_paths(root.string());
    REQUIRE(p.root == root.string());
    REQUIRE(p.config == (root / "config").string());
    REQUIRE(p.received == (root / "received").string());
    REQUIRE(p.authorized_keys == (root / "authorized_keys").string());
    REQUIRE(p.key_pem == (root / "id_ed25519.pem").string());
    REQUIRE(resolve_under_app_home("~/.bridgesessions/authorized_keys", p.root) ==
            p.authorized_keys);
    const auto absolute_other = fs::temp_directory_path() / "bs-absolute-other";
    REQUIRE(resolve_under_app_home(absolute_other.string(), p.root) ==
            absolute_other.string());
    MeshConfig cfg;
    apply_app_home_defaults(cfg, p.root);
    REQUIRE(cfg.authorized_keys_path == p.authorized_keys);
    REQUIRE(cfg.persistence_path == p.sessions);
}

TEST_CASE("attacker forged Hello pubkey rejected by verify_outbound",
          "[config][security][attacker]") {
    // Victim pin and cert key match; attacker forges Hello with different pubkey.
    auto r = verify_outbound_peer_identity(
        "pin_victim_key",
        "pin_victim_key",
        "attacker_forged_hello_key",
        "windows-peer",
        "windows-peer",
        true);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.reason.find("Hello") != std::string::npos);
}

TEST_CASE("attacker MITM cert fails pin check",
          "[config][security][attacker]") {
    auto r = verify_outbound_peer_identity(
        "expected_seed_pin",
        "mitm_cert_key",
        "mitm_cert_key",
        "test-pc1",
        "test-pc1",
        true);
    REQUIRE_FALSE(r.ok);
    REQUIRE(r.reason.find("pin") != std::string::npos);
}

TEST_CASE("attacker empty pin refused when require_pin",
          "[config][security][attacker]") {
    auto r = verify_outbound_peer_identity(
        "", "some_cert", "some_cert", "", "x", true);
    REQUIRE_FALSE(r.ok);
}

TEST_CASE("load_config defaults mdns_enabled to false", "[config][mdns][security]") {
    auto cfg_path = write_temp_config("# empty\n");
    MeshConfig cfg = load_config(cfg_path);
    REQUIRE_FALSE(cfg.mdns_enabled);
    fs::remove_all(fs::path(cfg_path).parent_path());
}

TEST_CASE("load_config parses mesh.mdns_enabled", "[config][mdns][security]") {
    auto cfg_path = write_temp_config("mesh.mdns_enabled true\n");
    MeshConfig cfg = load_config(cfg_path);
    REQUIRE(cfg.mdns_enabled);
    fs::remove_all(fs::path(cfg_path).parent_path());
}

TEST_CASE("save_config round-trips mdns_enabled", "[config][mdns][security]") {
    auto cfg_path = write_temp_config("node.name x\n");
    MeshConfig cfg = load_config(cfg_path);
    cfg.mdns_enabled = true;
    REQUIRE(save_config(cfg_path, cfg));
    MeshConfig reloaded = load_config(cfg_path);
    REQUIRE(reloaded.mdns_enabled);
    fs::remove_all(fs::path(cfg_path).parent_path());
}

TEST_CASE("save_config does not persist untrusted discovered peers",
          "[config][security][mdns]") {
    auto cfg_path = write_temp_config("node.name x\n");
    MeshConfig cfg = load_config(cfg_path);
    PeerEntry p;
    p.name = "untrusted";
    p.addr = "10.0.0.1:19949";
    p.pubkey_hex = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    cfg.discovered.push_back(p);
    REQUIRE(save_config(cfg_path, cfg));
    MeshConfig reloaded = load_config(cfg_path);
    REQUIRE(reloaded.discovered.empty());
    fs::remove_all(fs::path(cfg_path).parent_path());
}

// ── 2.0.8-alpha3 P0: codec round-trip for all new/extended wire types ──

static Message round_trip(const Message& in, uint16_t stream_id = 0) {
    auto frame = encode(in, stream_id);
    return decode(frame);
}

TEST_CASE("P0 codec round-trip: all 2.0.8-alpha3 message types",
          "[codec][alpha3][p0]") {
    // AttachAck
    AttachAckMsg ack; ack.attach_id = 42; ack.session_name = "hms";
    ack.cols = 120; ack.rows = 40;
    auto out = std::get<AttachAckMsg>(round_trip(Message{ack}));
    REQUIRE(out.attach_id == 42); REQUIRE(out.session_name == "hms");
    REQUIRE(out.cols == 120); REQUIRE(out.rows == 40);

    // OutputGap
    OutputGapMsg gap; gap.dropped_bytes = 123456789012ULL;
    auto gout = std::get<OutputGapMsg>(round_trip(Message{gap}));
    REQUIRE(gout.dropped_bytes == 123456789012ULL);

    // ConversationAppend
    ConversationAppendMsg ca; ca.conv_id = "c1"; ca.seq = 7; ca.ts = 1700000000123ULL;
    ca.agent_id = "agent-pubkey"; ca.role = 2; ca.body = "hello world";
    auto cout = std::get<ConversationAppendMsg>(round_trip(Message{ca}));
    REQUIRE(cout.conv_id == "c1"); REQUIRE(cout.seq == 7);
    REQUIRE(cout.ts == 1700000000123ULL); REQUIRE(cout.agent_id == "agent-pubkey");
    REQUIRE(cout.role == 2); REQUIRE(cout.body == "hello world");

    // ConversationQuery
    ConversationQueryMsg cq; cq.conv_id = "c1"; cq.since_seq = 3;
    auto qout = std::get<ConversationQueryMsg>(round_trip(Message{cq}));
    REQUIRE(qout.conv_id == "c1"); REQUIRE(qout.since_seq == 3);

    // ConversationBatch (run of appends)
    ConversationBatchMsg batch; batch.conv_id = "c1";
    ConversationAppendMsg a1; a1.conv_id = "c1"; a1.seq = 1; a1.body = "one";
    ConversationAppendMsg a2; a2.conv_id = "c1"; a2.seq = 2; a2.body = "two";
    batch.messages = {a1, a2};
    auto bout = std::get<ConversationBatchMsg>(round_trip(Message{batch}));
    REQUIRE(bout.conv_id == "c1");
    REQUIRE(bout.messages.size() == 2);
    REQUIRE(bout.messages[0].seq == 1); REQUIRE(bout.messages[0].body == "one");
    REQUIRE(bout.messages[1].seq == 2); REQUIRE(bout.messages[1].body == "two");

    // CuaRequest (HID usage IDs on wire)
    CuaRequestMsg req; req.request_id = 99; req.action = 1; req.x = 640; req.y = 480;
    req.button = 1; req.hid_key = 0x04; req.modifiers = 0x02; req.text = "a";
    auto rout = std::get<CuaRequestMsg>(round_trip(Message{req}));
    REQUIRE(rout.request_id == 99); REQUIRE(rout.action == 1);
    REQUIRE(rout.x == 640); REQUIRE(rout.y == 480);
    REQUIRE(rout.hid_key == 0x04); REQUIRE(rout.modifiers == 0x02);

    // CuaResponse
    CuaResponseMsg resp; resp.request_id = 99; resp.status = 0; resp.screen_w = 1920;
    resp.screen_h = 1080; resp.format = 1;
    auto esout = std::get<CuaResponseMsg>(round_trip(Message{resp}));
    REQUIRE(esout.request_id == 99); REQUIRE(esout.status == 0);
    REQUIRE(esout.screen_w == 1920); REQUIRE(esout.screen_h == 1080);
    REQUIRE(esout.format == 1);
}

TEST_CASE("P0 backward-compat: AttachMsg trailing client_instance_id tolerant decode",
          "[codec][alpha3][p0][backward-compat]") {
    // New-format attach with client_instance_id set
    AttachMsg a; a.session_name = "hms"; a.cols = 80; a.rows = 24;
    a.client_instance_id = 0xDEADBEEF;
    auto aout = std::get<AttachMsg>(round_trip(Message{a}));
    REQUIRE(aout.session_name == "hms");
    REQUIRE(aout.client_instance_id == 0xDEADBEEF);

    // Old-format attach frame (no client_instance_id) must still decode,
    // defaulting client_instance_id to 0 (backward compat with 2.0.7 clients).
    AttachMsg old; old.session_name = "legacy"; old.cols = 100; old.rows = 30;
    auto old_frame = encode(Message{old}, 0);
    // Manually strip nothing — encode already omits trailing field when 0;
    // verify decode tolerates a frame one u32 shorter than the new max:
    auto old_out = std::get<AttachMsg>(decode(old_frame));
    REQUIRE(old_out.session_name == "legacy");
    REQUIRE(old_out.cols == 100); REQUIRE(old_out.rows == 30);
    REQUIRE(old_out.client_instance_id == 0);  // defaulted, not garbage
}
