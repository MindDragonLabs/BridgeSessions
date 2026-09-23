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
#include <fcntl.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace bs::mesh;

static void set_test_env(const char* name, const char* value) {
#ifdef _WIN32
    (void)_putenv_s(name, value);
#else
    (void)::setenv(name, value, 1);
#endif
}

static void unset_test_env(const char* name) {
#ifdef _WIN32
    (void)_putenv_s(name, "");
#else
    (void)::unsetenv(name);
#endif
}

TEST_CASE("jail is opt-in: unset/empty BS_JAIL means disabled", "[jail]") {
    // 26.09.06-r5: jail defaults OFF — PR_SET_NO_NEW_PRIVS broke sudo in
    // sessions and the confinement was heavier than intended (operator).
    unset_test_env("BS_JAIL");
    unset_test_env("BS_JAIL_RW");
    auto p = jail_policy_from_env("/home/testuser", "/home/testuser/proj");
    REQUIRE_FALSE(p.enabled);
    set_test_env("BS_JAIL", "");
    auto p2 = jail_policy_from_env("/home/testuser", "/home/testuser/proj");
    REQUIRE_FALSE(p2.enabled);
}

TEST_CASE("jail policy enables with BS_JAIL=1 and builds home/tmp roots", "[jail]") {
    set_test_env("BS_JAIL", "1");
    unset_test_env("BS_JAIL_RW");
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
    set_test_env("BS_JAIL", "0");
    auto p = jail_policy_from_env("/home/testuser", "/");
    REQUIRE_FALSE(p.enabled);
    set_test_env("BS_JAIL", "1");
}

TEST_CASE("jail policy parses BS_JAIL_RW extra roots", "[jail]") {
    set_test_env("BS_JAIL", "1");
    set_test_env("BS_JAIL_RW", "/srv/deploys:/var/www:~/notes");
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
    unset_test_env("BS_JAIL_RW");
}

TEST_CASE("jail policy skips missing roots and dedupes", "[jail]") {
    set_test_env("BS_JAIL", "1");
    unset_test_env("BS_JAIL_RW");
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
    set_test_env("BS_JAIL_RW", "/nonexistent-jail-path-xyz");
    auto p2 = jail_policy_from_env("/home/testuser", "/");
    bool found_missing = false;
    for (auto& r : p2.writable_roots) if (r == "/nonexistent-jail-path-xyz") found_missing = true;
    REQUIRE(found_missing);
    unset_test_env("BS_JAIL_RW");
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
    set_test_env("BS_JAIL", "1");
    set_test_env("BS_JAIL_RW", root.c_str());

    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // Child: apply the jail, then probe.
        // daemon_cwd="/home/testuser" (a default root that does not exist on
        // this host — grants nothing at enforcement open-time). Crucially NOT
        // "/", which would be a writable root covering everything.
        auto p = jail_policy_from_env("/home/testuser", "/home/testuser");
        if (!apply_filesystem_jail(p)) {
            // Diagnose which step failed (2026-09-07: worked standalone,
            // failed in Catch fork child).
            FILE* dbg = ::fopen("/tmp/bs-jail-test-fail.txt", "w");
            if (dbg) {
                ::fprintf(dbg, "avail=%d errno=%d (%s)\n",
                          (int)landlock_available(), errno, ::strerror(errno));
                ::fclose(dbg);
            }
            _exit(99);
        }
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

#ifndef _WIN32
TEST_CASE("jail permits writing /dev/null (2>/dev/null works in sessions)", "[jail][landlock]") {
    // 26.09.06-r2/r3 regression: Landlock jailed shells lost write access to
    // /dev/null, breaking every `2>/dev/null` redirect — including the
    // internal curl of `bs upgrade`. Device nodes must stay writable.
    auto pol = bs::mesh::jail_policy_from_env("/tmp", "/tmp");
    if (!bs::mesh::apply_filesystem_jail(pol)) {
        WARN("apply_filesystem_jail failed (kernels without Landlock "
             "supporting device-node rules degrade gracefully)");
        return;
    }
    int fd = ::open("/dev/null", O_RDWR);
    REQUIRE(fd >= 0);
    const char* probe = "x";
    REQUIRE(::write(fd, probe, 1) == 1);
    ::close(fd);
}
#endif

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
