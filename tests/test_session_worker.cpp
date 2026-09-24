#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#ifndef _WIN32
    // Worker-exit tests write to sockets the worker may have just closed
    // (linger window lapsing mid-pump). A raised SIGPIPE would kill the
    // whole test binary — treat it as EPIPE instead.
    ::signal(SIGPIPE, SIG_IGN);
#endif
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

TEST_CASE("session worker socket names are collision-free",
          "[session_worker][socket_path]") {
    const fs::path home = make_temp_home();
    const std::string colon = worker::worker_socket_path(home.string(), "build:1");
    const std::string underscore = worker::worker_socket_path(home.string(), "build_1");
    REQUIRE(colon != underscore);
    REQUIRE(worker::worker_socket_path(home.string(), "ordinary-name_1") ==
            home.string() + "/run/bs-sessions/ordinary-name_1.sock");
    REQUIRE(worker::legacy_worker_socket_path(home.string(), "build:1") ==
            worker::legacy_worker_socket_path(home.string(), "build_1"));
    fs::remove_all(home);
}

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

TEST_CASE("session worker: ClientOverride replacement does not adopt old worker",
          "[session_worker][replacement]") {
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

    Session* original = reg.attach("replace", "/bin/sh", 80, 24,
                                   "xterm-256color");
    REQUIRE(original != nullptr);
    REQUIRE(original->hosted);
    const uint64_t old_generation = original->generation;
    // Replacement is permitted once the previous transport has detached.
    REQUIRE_FALSE(reg.detach("replace"));

    uint16_t eff_cols = 0, eff_rows = 0;
    const uint32_t aid = reg.attach_connection(
        "replace",
        ResolvedSessionCommand{
            "/bin/sh -c 'echo REPLACEMENT-RAN; sleep 10'",
            SessionCommandSource::ClientOverride},
        80, 24, "xterm-256color", "", 0, false, eff_cols, eff_rows);
    REQUIRE(aid != 0);

    Session* replacement = reg.get("replace");
    REQUIRE(replacement == original);
    REQUIRE(replacement->hosted);
    REQUIRE(replacement->generation > old_generation);
    REQUIRE(replacement->command.find("REPLACEMENT-RAN") != std::string::npos);

    std::string captured;
    REQUIRE(pump_until_contains(*replacement, "REPLACEMENT-RAN", 5s,
                                &captured));

    const fs::path sock_dir = home / "run" / "bs-sessions";
    reg.kill("replace");
    REQUIRE(wait_socket_gone(sock_dir, 5s));
    fs::remove_all(home);
}

TEST_CASE("session worker: READY identity detects legacy name-only payload by path",
          "[session_worker][adoption][legacy]") {
    const std::string home = "/tmp/bs-ready-decode-test";
    const std::string name = "legacy:name";
    const std::string path = worker::legacy_worker_socket_path(home, name);
    std::string decoded;
    pid_t child_pid = -1;

    std::vector<uint8_t> legacy(name.begin(), name.end());
    REQUIRE(legacy.size() >= 4);
    REQUIRE(worker::decode_ready_identity(legacy, home, path, decoded, child_pid));
    REQUIRE(decoded == name);
    REQUIRE(child_pid == -1);

    std::vector<uint8_t> current(name.begin(), name.end());
    const uint8_t pid_bytes[] = {0, 0, 0x12, 0x34};
    current.insert(current.end(), std::begin(pid_bytes), std::end(pid_bytes));
    REQUIRE(worker::decode_ready_identity(current, home, path, decoded, child_pid));
    REQUIRE(decoded == name);
    REQUIRE(child_pid == 0x1234);

    REQUIRE_FALSE(worker::decode_ready_identity(legacy, home,
        worker::worker_socket_path(home, "different"), decoded, child_pid));
}

TEST_CASE("session worker: replacing adopted legacy-path worker waits for retirement",
          "[session_worker][replacement][legacy]") {
    const std::string exe = worker_exe_from_env();
    if (exe.empty()) {
        WARN("BS_TEST_BS_BINARY not set — skipping session-worker tests");
        SUCCEED("skipped: BS_TEST_BS_BINARY unset");
        return;
    }
    const fs::path home = make_temp_home();
    const std::string name = "legacy:replace";
    const std::string old_path = worker::legacy_worker_socket_path(home.string(), name);
    worker::WorkerConfig cfg;
    cfg.socket_path = old_path;
    cfg.session_name = name;
    cfg.command = "sleep 60";
    cfg.app_home = home.string();
    const pid_t spawned = worker::spawn_session_worker(cfg, exe);
    REQUIRE(spawned >= 0);

    const auto deadline = std::chrono::steady_clock::now() + 8s;
    int probe = -1;
    while (std::chrono::steady_clock::now() < deadline && probe < 0) {
        probe = worker::connect_to_worker(old_path, 100);
        if (probe < 0) std::this_thread::sleep_for(20ms);
    }
    REQUIRE(probe >= 0);
    ::close(probe);

    pid_t expected_worker_pid = spawned;
    if (expected_worker_pid == 0) {
        std::ifstream pid_file(old_path + ".pid");
        long value = -1;
        REQUIRE(pid_file >> value);
        expected_worker_pid = static_cast<pid_t>(value);
    }

    SessionRegistry reg;
    reg.set_app_home(home.string());
    reg.set_worker_exe(exe);
    reg.adopt_workers();
    Session* adopted = reg.get(name);
    REQUIRE(adopted != nullptr);
    REQUIRE(adopted->hosted);
    REQUIRE(adopted->state == SessionState::Detached);
    REQUIRE(adopted->worker_pid == expected_worker_pid);

    uint16_t cols = 0, rows = 0;
    const uint32_t aid = reg.attach_connection(
        name, {"echo NEW-WORKER; sleep 10", SessionCommandSource::ClientOverride},
        80, 24, "xterm-256color", "", 0, false, cols, rows);
    REQUIRE(aid != 0);
    REQUIRE(adopted->generation > 0);
    REQUIRE_FALSE(fs::exists(old_path));
    std::string captured;
    REQUIRE(pump_until_contains(*adopted, "NEW-WORKER", 5s, &captured));
    reg.kill(name);
    REQUIRE(wait_socket_gone(home / "run" / "bs-sessions", 5s));
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

// 26.09.18 regression: a one-shot command whose child exits before any
// controller connects used to make the worker vanish instantly — the spawn
// wait (systemd-run path has no waitpid evidence) burned its whole 12s
// budget, then forkpty re-ran the command (13.5s one-shot latency,
// 2026-09-18 fleet incident). The worker now lingers briefly: spawn must
// connect fast, and the session must still deliver output + a real exit.
TEST_CASE("session worker: one-shot fast-exit spawn does not stall",
          "[session_worker][oneshot]") {
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

    const auto t0 = std::chrono::steady_clock::now();
    Session* s = reg.attach("quick", "/bin/sh -c 'echo FAST-EXIT-OK'",
                            80, 24, "xterm-256color");
    const auto spawn_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(s != nullptr);
    // Pre-fix this took ~12s (full budget) before the fallback. Healthy
    // connect (linger keeps the socket up) is well under that.
    REQUIRE(spawn_ms < 8000);

    std::string captured;
    REQUIRE(pump_until_contains(*s, "FAST-EXIT-OK", 5s, &captured));

    const fs::path sock_dir = home / "run" / "bs-sessions";
    reg.kill("quick");
    REQUIRE(wait_socket_gone(sock_dir, 5s));
    fs::remove_all(home);
}

#endif // !_WIN32
