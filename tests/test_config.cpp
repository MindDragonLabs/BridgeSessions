#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "bridgesessions.cpp"

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
    REQUIRE(expand_home("C:\\Users\\Shadow\\test") == "C:\\Users\\Shadow\\test");
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

// ── Test 6: round-trip — save + reload preserves discovered ────────

TEST_CASE("round-trip: save and reload preserves discovered peers", "[config]") {
    auto cfg_path = write_temp_config(
        "node.name testnode\n"
        "seed test-pc1 203.0.113.10:9948\n"
    );

    // Load
    MeshConfig cfg = load_config(cfg_path);
    REQUIRE(cfg.node_name == "testnode");
    REQUIRE(cfg.seeds.size() == 1);

    // Add discovered peers
    PeerEntry p1;
    p1.name = "corp-net";
    p1.addr = "10.0.0.5:19948";
    p1.pubkey_hex = "abc123";
    p1.last_seen = 1234567890;
    cfg.discovered.push_back(p1);

    // Save
    bool ok = save_config(cfg_path, cfg);
    REQUIRE(ok);

    // Reload
    MeshConfig cfg2 = load_config(cfg_path);
    REQUIRE(cfg2.node_name == "testnode");
    REQUIRE(cfg2.seeds.size() == 1);
    REQUIRE(cfg2.seeds[0].name == "test-pc1");
    REQUIRE(cfg2.discovered.size() == 1);
    REQUIRE(cfg2.discovered[0].name == "corp-net");
    REQUIRE(cfg2.discovered[0].addr == "10.0.0.5:19948");
    REQUIRE(cfg2.discovered[0].pubkey_hex == "abc123");
    REQUIRE(cfg2.discovered[0].last_seen == 1234567890);

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
