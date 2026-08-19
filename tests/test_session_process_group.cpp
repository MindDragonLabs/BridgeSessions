// test_session_process_group.cpp — verifies item-1 process-group cleanup.
//
// When a BS session's shell exits or is killed, every process in the shell's
// process group (background jobs the user started) must die with it — not
// outlive the session as orphans holding the PTY open.
//
// POSIX-only: relies on forkpty session-leader semantics + kill(-pgid).
#ifdef _WIN32
// Windows ConPTY does not expose process groups the same way; covered by E2E.
int main() { return 0; }
#else

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "../bs-protocol.h"
#include "../bs-session.h"

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fstream>
#include <thread>
#include <chrono>

using namespace bs::mesh;

static bool pid_alive(pid_t pid) {
    if (pid <= 0) return false;
    return ::kill(pid, 0) == 0;
}

// Spawn a session whose shell launches a background `sleep`, capture the
// background job's PID, then destroy the Session (the path `exit` takes).
// The background job must be gone afterward.
TEST_CASE("destroying a session kills the whole process group (background jobs)",
          "[session][process-group][posix-only]") {
    // The shell writes the background job PID to a file so the test can read it.
    std::string pidfile = "/tmp/bs_pg_" + std::to_string(getpid()) + ".pid";
    std::remove(pidfile.c_str());

    // `sleep 999 &` then `echo $!` — $! is the bg job's PID (a member of the
    // forkpty session-leader's process group).
    std::string cmd =
        "sleep 999 & echo $! > " + pidfile + "; wait";

    auto sess = create_session("pg-test", cmd, 80, 24, "xterm-256color");
    REQUIRE(sess.has_value());
    Session s = std::move(*sess);
    REQUIRE(s.child_pid > 0);
    REQUIRE(s.master_fd >= 0);

    // Wait for the shell to write the bg PID.
    pid_t bg_pid = -1;
    for (int i = 0; i < 100; ++i) {
        std::ifstream f(pidfile);
        if (f >> bg_pid && bg_pid > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(bg_pid > 0);
    REQUIRE(pid_alive(bg_pid));

    // Destroy the session (what happens on `exit` / session teardown).
    // ~Session must kill the whole process group, not just the shell leader.
    pid_t leader = s.child_pid;
    {
        // Scope a second Session and let RAII destroy it — do NOT use
        // move-assignment (Session::operator=(Session&&) is known-buggy and
        // would corrupt the heap; see SessionRegistry::install_spawned_runtime
        // for the supported hand-off path).
        Session doomed = std::move(s);
        (void)doomed;
        // doomed destructs here -> ~Session -> kill(-pgid)
    }

    // Give SIGTERM a moment to propagate.
    for (int i = 0; i < 60 && pid_alive(bg_pid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Background job must be dead.
    REQUIRE_FALSE(pid_alive(bg_pid));
    REQUIRE_FALSE(pid_alive(leader));

    std::remove(pidfile.c_str());
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
#endif
