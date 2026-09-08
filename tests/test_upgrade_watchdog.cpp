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
// Linux-only: the watchdog armer itself is POSIX/Linux (setsid + /dev/tcp).
// On other platforms this target builds an empty test binary.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <string>

#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#endif

using namespace std::chrono_literals;

#ifdef __linux__

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
std::string build_script(const std::string& bin, const std::string& oldb,
                         const std::string& start_cmd, const std::string& log,
                         const std::string& port) {
    std::string script;
    script += "sleep 1; ";
    script += "for i in 1 2; do ";
    script += "  if (exec 3<>/dev/tcp/127.0.0.1/" + port + ") 2>/dev/null; then exit 0; fi; ";
    script += "  sleep 1; ";
    script += "done; ";
    script += "echo \"watchdog: rolling back\" >> " + shell_quote(log) + "; ";
    script += "if [ -f " + shell_quote(oldb) + " ]; then ";
    script += "  mv -f " + shell_quote(oldb) + " " + shell_quote(bin) + "; ";
    script += "fi; ";
    script += "eval " + shell_quote(start_cmd) + " >> " + shell_quote(log) + " 2>&1; ";
    return script;
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
    std::string script = build_script(bin, oldb, "touch " + marker, log, dead_port);

    SECTION("rollback fires when port stays dead and old binary exists") {
        { std::ofstream(bin) << "new-binary"; }
        { std::ofstream(oldb) << "old-binary"; }
        std::system(("timeout 60 sh -c " + shell_quote(script)).c_str());
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
        std::system(("timeout 60 sh -c " + shell_quote(script)).c_str());
        std::ifstream in(bin);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        REQUIRE(content == "whatever");
        std::ifstream mk(marker);
        REQUIRE(mk.good());
    }

    SECTION("live port short-circuits to success with no side effects") {
        std::remove(marker.c_str());
        std::remove(log.c_str());
        // Track freshness via a rename: earlier sections recreate the marker,
        // so assert on a path that only THIS section's script can create.
        const std::string fresh_marker = base + "/fresh-marker";
        std::remove(fresh_marker.c_str());
        // find a free port by binding one
        std::string live_port;
        {
            FILE* p = popen(
                "python3 -c \"import socket;s=socket.socket();s.bind(('127.0.0.1',0));print(s.getsockname()[1])\"",
                "r");
            char buf[16] = {0};
            if (p && fgets(buf, sizeof(buf), p)) {
                live_port = buf;
                while (!live_port.empty() && (live_port.back() == '\n' || live_port.back() == '\r'))
                    live_port.pop_back();
            }
            if (p) pclose(p);
        }
        REQUIRE_FALSE(live_port.empty());
        // hold the port open for the duration of the probe. setsid so the
        // listener survives this std::system call's shell exit.
        std::string launch = "setsid python3 -c \"import socket;s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(('127.0.0.1'," +
                             live_port + "));s.listen(1);import time;time.sleep(15)\" >/dev/null 2>&1 < /dev/null &";
        std::system(launch.c_str());
        std::this_thread::sleep_for(1500ms);
        std::string ok_script = build_script(bin, oldb, "touch " + fresh_marker, log, live_port);
        int rc = std::system(("timeout 60 sh -c " + shell_quote(ok_script)).c_str());
        REQUIRE(rc == 0);
        REQUIRE_FALSE(std::filesystem::exists(fresh_marker)); // start command never ran
        std::ifstream lm(log);
        REQUIRE_FALSE(lm.good());    // no rollback logged
    }

    std::system(("rm -rf " + base).c_str());
}

#endif // __linux__

#ifdef __linux__
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
#endif // __linux__
