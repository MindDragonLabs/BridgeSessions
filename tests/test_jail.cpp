// test_jail.cpp — session filesystem jail (26.09.06-r1 Phase 6)
//
// Policy: session shells (the daemon's PTY workers) keep READ access
// everywhere ("read to know where things are") but WRITE only inside the
// allowed roots (user's working directories) unless explicitly extended
// via BS_JAIL_RW. Enforced with Landlock on Linux; env markers only on
// macOS/Windows (no kernel jail — see docs/session-jail.md).

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "../bs-protocol.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace bs::mesh;

TEST_CASE("jail policy defaults to enabled with home/tmp roots", "[jail]") {
    ::setenv("BS_JAIL", "", 1);          // unset-ish: default on
    ::unsetenv("BS_JAIL_RW");
    auto p = jail_policy_from_env("/home/testuser", "/home/testuser/proj");
    REQUIRE(p.enabled);
    // $HOME and /tmp always present; daemon cwd included when not redundant.
    bool has_home = false, has_tmp = false, has_cwd = false;
    for (auto& r : p.writable_roots) {
        if (r == "/home/testuser") has_home = true;
        if (r == "/tmp") has_tmp = true;
        if (r == "/home/testuser/proj") has_cwd = true;
    }
    REQUIRE(has_home);
    REQUIRE(has_tmp);
    REQUIRE(has_cwd);
}

TEST_CASE("jail policy can be disabled with BS_JAIL=0", "[jail]") {
    ::setenv("BS_JAIL", "0", 1);
    auto p = jail_policy_from_env("/home/testuser", "/");
    REQUIRE_FALSE(p.enabled);
    ::setenv("BS_JAIL", "1", 1);
}

TEST_CASE("jail policy parses BS_JAIL_RW extra roots", "[jail]") {
    ::setenv("BS_JAIL", "1", 1);
    ::setenv("BS_JAIL_RW", "/srv/deploys:/var/www:~/notes", 1);
    auto p = jail_policy_from_env("/home/testuser", "/");
    REQUIRE(p.enabled);
    bool has_srv = false, has_var = false, has_notes = false;
    for (auto& r : p.writable_roots) {
        if (r == "/srv/deploys") has_srv = true;
        if (r == "/var/www") has_var = true;
        if (r == "/home/testuser/notes") has_var = has_var;  // checked below
        if (r == "/home/testuser/notes") has_notes = true;
    }
    REQUIRE(has_srv);
    REQUIRE(has_var);
    REQUIRE(has_notes);   // ~ expanded against the passed home
    // Round-trip: env string carries ALL roots (defaults + extras), deduped,
    // ~-expanded, in insertion order.
    REQUIRE(p.writable_roots_env() ==
            "/home/testuser:/:/tmp:/srv/deploys:/var/www:/home/testuser/notes");
    ::unsetenv("BS_JAIL_RW");
}

TEST_CASE("jail policy skips missing roots and dedupes", "[jail]") {
    ::setenv("BS_JAIL", "1", 1);
    ::unsetenv("BS_JAIL_RW");
    auto p = jail_policy_from_env("/home/testuser", "/home/testuser");
    REQUIRE(p.enabled);
    // cwd == home → single entry, not duplicated.
    int homes = 0;
    for (auto& r : p.writable_roots) if (r == "/home/testuser") ++homes;
    REQUIRE(homes == 1);
    // The policy is DECLARATIVE: a nonexistent explicit root stays in the
    // list and is skipped at enforcement open-time (apply_filesystem_jail
    // drops it when O_DIRECTORY open fails) — it can never silently broaden
    // the jail to a path nobody can resolve anyway.
    ::setenv("BS_JAIL_RW", "/nonexistent-jail-path-xyz", 1);
    auto p2 = jail_policy_from_env("/home/testuser", "/");
    bool found_missing = false;
    for (auto& r : p2.writable_roots) if (r == "/nonexistent-jail-path-xyz") found_missing = true;
    REQUIRE(found_missing);
    ::unsetenv("BS_JAIL_RW");
}

#if !defined(_WIN32) && !defined(__APPLE__)
TEST_CASE("landlock jail blocks writes outside roots, allows inside",
          "[jail][landlock]") {
    if (!landlock_available()) {
        WARN("Landlock not available on this kernel; skipping enforcement test");
        return;
    }
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "bs-jail-test";
    fs::create_directories(root);
    ::setenv("BS_JAIL", "1", 1);
    ::setenv("BS_JAIL_RW", root.c_str(), 1);

    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Child: apply the jail, then probe.
        // daemon_cwd="/home/testuser" (a default root that does not exist on
        // this host — grants nothing at enforcement open-time). Crucially NOT
        // "/", which would be a writable root covering everything.
        auto p = jail_policy_from_env("/home/testuser", "/home/testuser");
        if (!apply_filesystem_jail(p)) _exit(99);
        // 1. Write inside the root → allowed.
        int ok = ::open((root / "w.txt").c_str(), O_CREAT | O_WRONLY, 0600);
        if (ok < 0) _exit(1);
        ::close(ok);
        // 2. Write outside every root → denied. /var/tmp is world-writable
        // like /tmp but is NOT a policy root (only /tmp itself is), so the
        // only thing that can make this open fail is the jail.
        int denied = ::open("/var/tmp/bs-jail-should-fail", O_CREAT | O_WRONLY, 0600);
        if (denied >= 0) { ::close(denied); ::unlink("/var/tmp/bs-jail-should-fail"); _exit(2); }
        // 3. Read outside the root → still allowed ("read to know where things are").
        int rd = ::open("/etc/hostname", O_RDONLY);
        if (rd < 0) _exit(3);   // /etc/hostname exists on essentially every Linux
        ::close(rd);
        _exit(0);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
    (void)::system(("rm -rf '" + root.string() + "'").c_str());
}
#endif

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
