#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>

using namespace bs::mesh;
using namespace std::chrono_literals;

#ifdef _WIN32
#define BS_CMD(win_cmd, posix_cmd) win_cmd
#else
#define BS_CMD(win_cmd, posix_cmd) posix_cmd
#endif

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── Helpers ─────────────────────────────────────────────────────

static void kill_child_process(Session& s) {
#ifdef _WIN32
    if (s.child_pid) {
        TerminateProcess(s.child_pid, 1);
        WaitForSingleObject(s.child_pid, 5000);
        CloseHandle(s.child_pid);
        s.child_pid = nullptr;
    }
#else
    if (s.child_pid > 0) {
        kill(s.child_pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            if (waitpid(s.child_pid, &status, WNOHANG) == s.child_pid) break;
            usleep(100000);
        }
        if (waitpid(s.child_pid, &status, WNOHANG) != s.child_pid) {
            kill(s.child_pid, SIGKILL);
            waitpid(s.child_pid, &status, 0);
        }
        s.child_pid = -1;
    }
#endif
}

// Get a temp path for persistence testing
static std::string temp_persistence_path() {
    auto tmp = std::filesystem::temp_directory_path() / "bs_test_sessions.json";
    return tmp.string();
}

static void cleanup_temp_file(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(path + ".tmp", ec);
}

// ── Test 1: Create session via registry.attach ──────────────────

TEST_CASE("SessionRegistry: create session via attach", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_create", BS_CMD("cmd.exe /c echo hello", "echo hello"), 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->name == "test_create");
    REQUIRE(s->state == SessionState::Attached);
    REQUIRE(registry.count() == 1);

    auto list = registry.list();
    REQUIRE(list.size() == 1);
    REQUIRE(list[0].name == "test_create");
    REQUIRE(list[0].state == "attached");

    registry.kill("test_create");
    REQUIRE(registry.count() == 0);
}

// ── Test 2: Detach session, verify state ─────────────────────────

TEST_CASE("SessionRegistry: detach session changes state", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_detach", BS_CMD("cmd.exe /c timeout /t 10", "sleep 10"), 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Attached);

    registry.detach("test_detach");
    REQUIRE(s->state == SessionState::Detached);
    REQUIRE(registry.count() == 1);

    // Verify state in list
    auto list = registry.list();
    REQUIRE(list.size() == 1);
    REQUIRE(list[0].state == "detached");

    registry.kill("test_detach");
}

// ── Test 3: Reattach to detached session ────────────────────────

TEST_CASE("SessionRegistry: reattach to detached session", "[session_registry]") {
    SessionRegistry registry;
    auto* s1 = registry.attach("test_reattach", BS_CMD("cmd.exe /c timeout /t 10", "sleep 10"), 80, 24, "xterm-256color");
    REQUIRE(s1 != nullptr);
    registry.detach("test_reattach");
    REQUIRE(s1->state == SessionState::Detached);

    auto* s2 = registry.attach("test_reattach", "", 100, 50, "xterm");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2 == s1);  // same session object
    REQUIRE(s2->state == SessionState::Attached);
#ifndef _WIN32
    winsize ws{};
    REQUIRE(::ioctl(s2->master_fd, TIOCGWINSZ, &ws) == 0);
    REQUIRE(ws.ws_col == 100);
    REQUIRE(ws.ws_row == 50);
#endif

    registry.kill("test_reattach");
}

// ── Test 4: Kill session removes from map ────────────────────────

TEST_CASE("SessionRegistry: detached child exit is reaped", "[session_registry][reconnect]") {
    SessionRegistry registry;
    auto* s = registry.attach("detached-dead", BS_CMD("cmd.exe /c exit /b 7", "exit 7"),
                              80, 24, "xterm", "peer-a");
    REQUIRE(s != nullptr);
    registry.detach("detached-dead", "peer-a");
    REQUIRE(s->state == SessionState::Detached);
    std::this_thread::sleep_for(500ms);
    registry.reap_dead();
    REQUIRE(s->state == SessionState::Died);
    registry.kill("detached-dead");
}

TEST_CASE("SessionRegistry: mesh reaper defers attached children to PTY poller",
          "[session_registry][reconnect][pty]") {
    SessionRegistry registry;
    auto* s = registry.attach("attached-reaper-owner",
                              BS_CMD("cmd.exe /c exit /b 0", "exit 0"),
                              80, 24, "xterm", "peer-a");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Attached);

    std::this_thread::sleep_for(500ms);
    registry.reap_dead(false);

    // MeshController::pty_output_poller owns attached-child reaping because it
    // must fan out SessionDied. The registry's end-of-tick reaper must not steal
    // waitpid() first and suppress that protocol frame.
    REQUIRE(s->state == SessionState::Attached);
    REQUIRE(s->is_pollable());

    registry.reap_dead();
    REQUIRE(s->state == SessionState::Died);
    REQUIRE_FALSE(s->is_valid());
    registry.kill("attached-reaper-owner");
}

TEST_CASE("SessionRegistry: transport detach after child exit preserves resurrection eligibility",
          "[session_registry][reconnect]") {
    SessionRegistry registry;
    auto* s = registry.attach("died-before-detach", BS_CMD("cmd.exe /c exit /b 7", "exit 7"),
                              80, 24, "xterm", "peer-a");
    REQUIRE(s != nullptr);
    const auto first_generation = s->generation;

    std::this_thread::sleep_for(500ms);
    registry.reap_dead();
    REQUIRE(s->state == SessionState::Died);
    // A dead child must not retain its PTY master until a future reattach.
    // Scrollback/session metadata preserve resurrection eligibility without
    // holding an OS descriptor open.
    REQUIRE_FALSE(s->is_valid());
    REQUIRE_FALSE(s->is_pollable());

    registry.detach("died-before-detach", "peer-a");
    REQUIRE(s->state == SessionState::Died);

    auto* restarted = registry.attach(
        "died-before-detach", BS_CMD("cmd.exe /c exit /b 0", "exit 0"),
        80, 24, "xterm", "peer-a");
    REQUIRE(restarted == s);
    REQUIRE(restarted->generation == first_generation + 1);
    REQUIRE(restarted->state == SessionState::Attached);

    registry.kill("died-before-detach");
}

TEST_CASE("SessionRegistry: kill removes session", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_kill", BS_CMD("cmd.exe /c timeout /t 10", "sleep 10"), 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(registry.count() == 1);

    registry.kill("test_kill");
    REQUIRE(registry.count() == 0);
    REQUIRE(registry.get("test_kill") == nullptr);

    auto list = registry.list();
    REQUIRE(list.empty());
}

// ── Test 5: Dead session reattach resurrects session ─────────────

TEST_CASE("SessionRegistry: reattach to dead session resurrects in place", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_dead", BS_CMD("cmd.exe /c exit /b 1", "exit 1"), 80, 24, "xterm-256color", "peer-a");
    REQUIRE(s != nullptr);
    s->peer_ids.push_back("peer-b");

    // Wait for process to exit
    std::this_thread::sleep_for(500ms);
#ifdef _WIN32
    if (s->child_pid && WaitForSingleObject(s->child_pid, 2000) == WAIT_OBJECT_0) {
        CloseHandle(s->child_pid);
        s->child_pid = nullptr;
        s->state = SessionState::Died;
    }
#else
    if (s->child_pid > 0) {
        int status = 0;
        if (waitpid(s->child_pid, &status, WNOHANG) == s->child_pid) {
            s->child_pid = -1;
            s->state = SessionState::Died;
        }
    }
#endif

    // v1.7 behavior: a dead named session can be reattached and is replaced
    // with a fresh child so repeated one-shot execs do not hang forever.
    auto* s2 = registry.attach("test_dead", "", 80, 24, "xterm", "peer-a");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2 == s);
    REQUIRE(s2->state == SessionState::Attached);
    REQUIRE(std::find(s2->peer_ids.begin(), s2->peer_ids.end(), "peer-a") != s2->peer_ids.end());
    REQUIRE(std::find(s2->peer_ids.begin(), s2->peer_ids.end(), "peer-b") != s2->peer_ids.end());

    registry.kill("test_dead");
}

// ── Test 6: get() and list() access ─────────────────────────────

TEST_CASE("SessionRegistry: get returns session or nullptr", "[session_registry]") {
    SessionRegistry registry;
    REQUIRE(registry.get("nonexistent") == nullptr);

    auto* s = registry.attach("test_get", BS_CMD("cmd.exe /c timeout /t 10", "sleep 10"), 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(registry.get("test_get") == s);
    REQUIRE(registry.get("test_get")->name == "test_get");

    registry.kill("test_get");
}

// ── Test 7: Multiple sessions coexist ────────────────────────────

TEST_CASE("SessionRegistry: multiple independent sessions", "[session_registry]") {
    SessionRegistry registry;
    std::vector<Session*> sessions;
    for (int i = 0; i < 3; ++i) {
        auto* s = registry.attach("multi_" + std::to_string(i), BS_CMD("cmd.exe /c timeout /t 10", "sleep 10"), 80, 24, "xterm-256color");
        REQUIRE(s != nullptr);
        sessions.push_back(s);
    }
    REQUIRE(registry.count() == 3);

    auto list = registry.list();
    REQUIRE(list.size() == 3);

    // Detach some
    registry.detach("multi_0");
    REQUIRE(registry.get("multi_0")->state == SessionState::Detached);
    REQUIRE(registry.get("multi_1")->state == SessionState::Attached);

    for (auto* s : sessions) {
        registry.kill(s->name);
    }
    REQUIRE(registry.count() == 0);
}

// ── Test 8: prune_idle removes idle detached sessions ────────────

TEST_CASE("SessionRegistry: prune_idle removes idle detached", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_idle", BS_CMD("cmd.exe /c timeout /t 10", "sleep 10"), 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);

    registry.detach("test_idle");
    REQUIRE(registry.count() == 1);

    // Fake last_output_at to be 10 days ago
    s->last_output_at = std::chrono::steady_clock::now() - std::chrono::hours(10 * 24);

    // Prune with 1-second timeout
    registry.prune_idle(std::chrono::seconds(1));
    REQUIRE(registry.count() == 0);
}

// ── Test 9: Persist sessions and resurrect ──────────────────────

TEST_CASE("SessionRegistry: persist and resurrect sessions", "[session_registry]") {
    auto path = temp_persistence_path();
    cleanup_temp_file(path);

    // Create registry and add a session
    {
        SessionRegistry registry;
        registry.set_persistence_path(path);
        auto* s = registry.attach("persist_test", BS_CMD("cmd.exe /c echo persisted", "echo persisted"), 80, 24, "xterm-256color");
        REQUIRE(s != nullptr);
        REQUIRE(registry.count() == 1);

        bool saved = registry.save_persisted_sessions();
        REQUIRE(saved);
        REQUIRE(std::filesystem::exists(path));

        registry.kill("persist_test");
    }

    // New registry loads persisted sessions
    {
        SessionRegistry registry2;
        registry2.set_persistence_path(path);
        registry2.load_persisted_sessions();
        REQUIRE(registry2.count() == 1);

        auto* s = registry2.get("persist_test");
        REQUIRE(s != nullptr);
        REQUIRE(s->name == "persist_test");
        REQUIRE(s->state == SessionState::Recoverable);

        // Resurrect
        auto* r = registry2.resurrect("persist_test", 80, 24, "xterm-256color");
        REQUIRE(r != nullptr);
        REQUIRE(r == s);
        REQUIRE(r->state == SessionState::Attached);
        REQUIRE(registry2.count() == 1);

        registry2.kill("persist_test");
    }

    cleanup_temp_file(path);
}

// ── Test 10: reap_dead detects exited child ─────────────────────

TEST_CASE("SessionRegistry: reap_dead detects exited child", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_reap", BS_CMD("cmd.exe /c exit /b 42", "exit 42"), 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Attached);

    // Wait for process to exit
    std::this_thread::sleep_for(500ms);

    registry.reap_dead();
    REQUIRE(s->state == SessionState::Died);
    REQUIRE(registry.count() == 1);

    registry.kill("test_reap");
}

// ── Test 11: Auto-restart spawns new child on death ─────────────

TEST_CASE("SessionRegistry: auto_restart respawns died child", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_restart", BS_CMD("cmd.exe /c exit /b 1", "exit 1"),
                              80, 24, "xterm-256color", "peer-a");
    REQUIRE(s != nullptr);
    s->auto_restart = true;
    s->reset_restart_failures();

    auto old_generation = s->generation;

    // Wait for process to exit
    std::this_thread::sleep_for(500ms);

    registry.reap_dead();
    REQUIRE(registry.count() == 1);

    auto* s2 = registry.get("test_restart");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2 == s);
    REQUIRE(s2->auto_restart == true);
    REQUIRE(s2->state == SessionState::Attached);
    REQUIRE(s2->peer_ids == std::vector<std::string>{"peer-a"});
    // Respawn must yield a fresh spawn generation. (child_pid/HANDLE can be
    // recycled by the OS, so it is NOT a reliable respawn signal — generation is.)
    REQUIRE(s2->generation != old_generation);
    REQUIRE(s2->generation > old_generation);

    registry.kill("test_restart");
}

// ── Test 12: Circuit breaker stops after 3+ failures ────────────

TEST_CASE("SessionRegistry: circuit breaker after repeated failures", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_cb", BS_CMD("cmd.exe /c timeout /t 30", "sleep 30"), 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    s->auto_restart = true;
    s->reset_restart_failures();

    // Kill child and reap 4 times — circuit breaker should stop after 3 restarts.
    // IMPORTANT: do NOT close the handle or null child_pid before reap_dead — the
    // reaper detects death via WaitForSingleObject and must see a valid handle.
    for (int i = 0; i < 4; ++i) {
        auto* cur = registry.get("test_cb");
        REQUIRE(cur != nullptr);
#ifdef _WIN32
        if (cur->child_pid) {
            TerminateProcess(cur->child_pid, 1);
        }
#else
        if (cur->child_pid > 0) {
            kill(cur->child_pid, SIGTERM);
        }
#endif
        // Give process termination time to take effect, then let reap_dead detect + restart
        std::this_thread::sleep_for(200ms);
        registry.reap_dead();
    }

    // After 4 forced deaths, circuit breaker should have marked session Exited
    auto* final_s = registry.get("test_cb");
    REQUIRE(final_s != nullptr);
    REQUIRE((final_s->state == SessionState::Exited || final_s->state == SessionState::Detached));
    // Count should be 1 (session still in map)
    REQUIRE(registry.count() == 1);

    registry.kill("test_cb");
}

// ── Test 13: Double attach replaces existing attachment ─────────

TEST_CASE("SessionRegistry: double attach replaces existing", "[session_registry]") {
    SessionRegistry registry;
    auto* s1 = registry.attach("test_double", BS_CMD("cmd.exe /c timeout /t 10", "sleep 10"), 80, 24, "xterm-256color");
    REQUIRE(s1 != nullptr);
    REQUIRE(s1->state == SessionState::Attached);

    // Second attach while still attached
    auto* s2 = registry.attach("test_double", "", 120, 40, "xterm");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2 == s1);
    REQUIRE(s2->state == SessionState::Attached);

    registry.kill("test_double");
}
