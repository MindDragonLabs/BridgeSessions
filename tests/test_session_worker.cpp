#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace bs::mesh;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

namespace {

#ifndef _WIN32

std::string worker_exe_from_env() {
    const char* p = std::getenv("BS_TEST_BS_BINARY");
    return (p && *p) ? std::string(p) : std::string{};
}

fs::path make_temp_home() {
    // macOS $TMPDIR (/var/folders/<30 chars>/T/) makes the worker unix-socket
    // path exceed the 104-byte sun_path limit — hosted spawn refuses and the
    // test would exercise the forkpty fallback instead. Keep the base short.
#ifdef __APPLE__
    const fs::path base = "/tmp";
#else
    const fs::path base = fs::temp_directory_path();
#endif
    auto tmp = base / ("bs_sw_" + std::to_string(::getpid()) + "_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch().count()));
    fs::create_directories(tmp);
    return tmp;
}

// Pump the hosted session in a loop until the accumulated
// output+scrollback contains `marker`, or `timeout` elapses.
bool pump_until_contains(Session& s, const std::string& marker,
                         std::chrono::milliseconds timeout,
                         std::string* captured = nullptr) {
    std::string acc;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto pump = pump_hosted_session(s);
        acc += pump.output;
        acc += pump.scrollback;
        if (acc.find(marker) != std::string::npos) {
            if (captured) *captured = acc;
            return true;
        }
        if (s.worker_died) break;
        std::this_thread::sleep_for(20ms);
    }
    if (captured) *captured = acc;
    return false;
}

// Wait until the given directory contains no *.sock files (worker exited
// and unlinked its socket), or timeout.
bool wait_socket_gone(const fs::path& sock_dir,
                      std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        size_t socks = 0;
        std::error_code ec;
        if (fs::exists(sock_dir, ec)) {
            for (auto& e : fs::directory_iterator(sock_dir, ec))
                if (e.path().extension() == ".sock") ++socks;
        }
        if (socks == 0) return true;
        std::this_thread::sleep_for(20ms);
    }
    return false;
}

void send_shell_line(Session& s, const std::string& line) {
    worker_queue_frame(s, worker::WMSG_INPUT, line.data(), line.size());
}

#endif

} // namespace

#ifndef _WIN32

TEST_CASE("session worker: spawn, IO, and clean kill", "[session_worker]") {
    const std::string exe = worker_exe_from_env();
    if (exe.empty()) {
        WARN("BS_TEST_BS_BINARY not set — skipping session-worker tests");
        SUCCEED("skipped: BS_TEST_BS_BINARY unset");
        return;
    }

    const fs::path home = make_temp_home();

    SessionRegistry reg;
    reg.set_app_home(home.string());
    reg.set_worker_exe(exe);

    Session* s = reg.attach("w1", "/bin/sh", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->hosted);
    REQUIRE(s->worker_pid > 0);
    REQUIRE(s->is_pollable());

    send_shell_line(*s, "echo MARKER-$((3*4))\n");

    std::string captured;
    REQUIRE(pump_until_contains(*s, "MARKER-12", 5s, &captured));

    const fs::path sock_dir = home / "run" / "bs-sessions";
    reg.kill("w1");
    REQUIRE(wait_socket_gone(sock_dir, 5s));

    fs::remove_all(home);
}

TEST_CASE("session worker: survives daemon death and is re-adopted",
          "[session_worker]") {
    const std::string exe = worker_exe_from_env();
    if (exe.empty()) {
        WARN("BS_TEST_BS_BINARY not set — skipping session-worker tests");
        SUCCEED("skipped: BS_TEST_BS_BINARY unset");
        return;
    }

    const fs::path home = make_temp_home();

    // Child process plays the role of a daemon that spawns a hosted
    // session and then dies abruptly (SIGKILL simulation: _exit with no
    // destructors, so SessionRegistry cleanup never runs).
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        SessionRegistry reg;
        reg.set_app_home(home.string());
        reg.set_worker_exe(exe);
        Session* s = reg.attach("persist", "/bin/sh", 80, 24,
                                "xterm-256color");
        const bool ok = s && s->hosted && s->worker_pid > 0
                        && s->is_pollable();
        _exit(ok ? 0 : 42);
    }

    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    INFO("daemon-sim child exit code: " << WEXITSTATUS(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    // New "daemon" instance on the same app home adopts the live worker.
    SessionRegistry reg;
    reg.set_app_home(home.string());
    reg.set_worker_exe(exe);
    reg.adopt_workers();

    Session* s = reg.get("persist");
    REQUIRE(s != nullptr);
    REQUIRE(s->hosted);
    REQUIRE(s->state == SessionState::Detached);
    REQUIRE(s->is_pollable());

    send_shell_line(*s, "echo ALIVE-$((5*6))\n");
    std::string captured;
    REQUIRE(pump_until_contains(*s, "ALIVE-30", 5s, &captured));

    const fs::path sock_dir = home / "run" / "bs-sessions";
    reg.kill("persist");
    REQUIRE(wait_socket_gone(sock_dir, 5s));

    fs::remove_all(home);
}

TEST_CASE("session worker: forkpty fallback when worker exe is missing",
          "[session_worker]") {
    const fs::path home = make_temp_home();

    SessionRegistry reg;
    reg.set_app_home(home.string());
    reg.set_worker_exe("/nonexistent/bs");

    Session* s = reg.attach("fb", "/bin/sh", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE_FALSE(s->hosted);   // fell back to direct forkpty
    REQUIRE(s->is_pollable());

    reg.kill("fb");
    fs::remove_all(home);
}

#endif // !_WIN32
