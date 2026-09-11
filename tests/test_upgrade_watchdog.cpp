// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon
//
// Watchdog contract tests (operator mandate: upgrades must be seamless,
// quick, painless, non-destructive).
//
// arm_upgrade_watchdog() fires a detached shell one-liner. We can't observe
// the detached process in a unit test, but we CAN observe its effects:
//   1. on a dead port with old_path present, the helper restores it over
//      bin_path, runs the start command, and appends to the log;
//   2. no old binary: log + start command only, bin untouched;
//   3. a live listener satisfies the probe → exit 0, no side effects.
// The test reconstructs the helper script with the same rules as the armer
// and executes it with sh (running the real armer would detach and race).
//
// Linux-only: the watchdog armer is POSIX/Linux (setsid + bash /dev/tcp).
// On other platforms this file compiles to a stub main() so the CMake
// GLOB-produced target still links (an empty Catch binary is fine; it
// discovers zero tests).

#include <catch2/catch_session.hpp>

#ifdef __linux__

#include <catch2/catch_test_macros.hpp>
#include "../bs-protocol.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>

using namespace std::chrono_literals;

namespace {

std::string shell_quote(const std::string& s) {
    std::string out{"'"};
    for (const char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += '\'';
    return out;
}

// Same script rules as bs-upgrade-watchdog.h, parameterized for tests.
// Probe mirrors the product's arm-time selection: bash /dev/tcp when bash
// exists, curl otherwise — the test must never emit a probe the shell
// running it cannot execute (dash has no /dev/tcp).
std::string build_script(const std::string& bin, const std::string& oldb,
                         const std::string& start_cmd, const std::string& log,
                         const std::string& port, bool have_bash) {
    std::string script;
    script += "sleep 1; ";
    script += "for i in 1 2; do ";
    if (have_bash)
        script += "  if (exec 3<>/dev/tcp/127.0.0.1/" + port + ") 2>/dev/null; then exit 0; fi; ";
    else
        script += "  curl -m 2 -sk https://127.0.0.1:" + port +
                  "/ >/dev/null 2>&1; if [ $? -ne 7 ]; then exit 0; fi; ";
    script += "  sleep 1; ";
    script += "done; ";
    script += "echo \"watchdog: rolling back\" >> " + shell_quote(log) + "; ";
    script += "if [ -f " + shell_quote(oldb) + " ]; then ";
    script += "  mv -f " + shell_quote(oldb) + " " + shell_quote(bin) + "; ";
    script += "fi; ";
    script += "eval " + shell_quote(start_cmd) + " >> " + shell_quote(log) + " 2>&1; ";
    return script;
}

// The product launches the helper with the interpreter that matches the probe
// it selected (bash for the /dev/tcp probe, sh for curl). The test must do the
// same: running a /dev/tcp probe under dash fails on every round, so the test
// would assert against a probe the shell cannot execute.
std::string shell_for(const bool have_bash) {
    return have_bash ? "bash" : "sh";
}

} // namespace

TEST_CASE("upgrade watchdog restores old binary when new daemon never binds", "[watchdog]") {
    char dir[] = "/tmp/bs-watchdog-test-XXXXXX";
    REQUIRE(mkdtemp(dir) != nullptr);
    std::string base = dir;
    std::string bin = base + "/bridgesessions";
    std::string oldb = base + "/bridgesessions.old";
    std::string log = base + "/watchdog.log";
    std::string marker = base + "/start-marker";
    const std::string dead_port = "59999";
    // The production armer probes with bash /dev/tcp when bash exists,
    // curl otherwise. Reproduce that selection so the test runs the same
    // probe the product would arm on this host (dash containers exercise
    // the curl path; bash hosts exercise /dev/tcp).
    const bool have_bash = std::system("command -v bash >/dev/null 2>&1") == 0;
    const std::string shell = shell_for(have_bash);
    auto run = [&](const std::string& s) {
        return std::system(("timeout 60 " + shell + " -c " + shell_quote(s)).c_str());
    };
    std::string script = build_script(bin, oldb, "touch " + marker, log, dead_port, have_bash);

    SECTION("rollback fires when port stays dead and old binary exists") {
        { std::ofstream(bin) << "new-binary"; }
        { std::ofstream(oldb) << "old-binary"; }
        run(script);
        std::ifstream in(bin);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        REQUIRE(content == "old-binary");
        std::ifstream lm(log);
        REQUIRE(lm.good());          // rollback logged
        std::ifstream mk(marker);
        REQUIRE(mk.good());          // start command ran
    }

    SECTION("no old binary: log + start only, installed bin untouched") {
        std::remove(oldb.c_str());
        { std::ofstream(bin) << "whatever"; }
        run(script);
        std::ifstream in(bin);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        REQUIRE(content == "whatever");
        std::ifstream lm(log);
        REQUIRE(lm.good());          // failure logged
        std::ifstream mk(marker);
        REQUIRE(mk.good());          // start command still ran
    }

    SECTION("live port short-circuits to success with no side effects") {
        std::remove(marker.c_str());
        std::remove(log.c_str());
        // Track freshness via a unique path: earlier sections recreate the
        // marker, so assert on a file only THIS section's script can create.
        const std::string fresh_marker = base + "/fresh-marker";
        std::remove(fresh_marker.c_str());
        // Hold a live port with a listening socket owned by THIS process:
        // bind :0, read the assigned port back, listen(1). The watchdog's
        // bash /dev/tcp probe completes against the kernel accept queue, so
        // the script must see the port as live. No external interpreter
        // needed (python3 is absent in minimal builder containers).
        int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listener >= 0);
        int reuse = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in laddr{};
        laddr.sin_family = AF_INET;
        laddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        laddr.sin_port = 0;
        REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&laddr), sizeof(laddr)) == 0);
        sockaddr_in got{};
        socklen_t got_len = sizeof(got);
        REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&got), &got_len) == 0);
        const std::string live_port = std::to_string(ntohs(got.sin_port));
        REQUIRE(::listen(listener, 1) == 0);
        std::string ok_script = build_script(bin, oldb, "touch " + fresh_marker, log, live_port, have_bash);
        int rc = run(ok_script);
        ::close(listener);
        REQUIRE(rc == 0);
        REQUIRE_FALSE(std::filesystem::exists(fresh_marker)); // start command never ran
        std::ifstream lm(log);
        REQUIRE_FALSE(lm.good());    // no rollback logged
    }

    std::system(("rm -rf " + base).c_str());
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

#else // !__linux__

// Stub main so the GLOB-generated target links on macOS/Windows.
int main() { return 0; }

#endif // __linux__
