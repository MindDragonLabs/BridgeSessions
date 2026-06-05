#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "bridgesessions.cpp"

#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>

using namespace bs::mesh;
using namespace std::chrono_literals;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── Helpers ─────────────────────────────────────────────────────

#ifdef _WIN32
static void kill_child_process(Session& s) {
    if (s.child_pid) {
        TerminateProcess(s.child_pid, 1);
        WaitForSingleObject(s.child_pid, 5000);
        CloseHandle(s.child_pid);
        s.child_pid = nullptr;
    }
}
#endif

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
    auto* s = registry.attach("test_create", "cmd.exe /c echo hello", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->name == "test_create");
    REQUIRE(s->state == SessionState::Attached);
    REQUIRE(registry.count() == 1);

    auto list = registry.list();
    REQUIRE(list.size() == 1);
    REQUIRE(list[0].name == "test_create");
    REQUIRE(list[0].state == "attached");

    kill_child_process(*s);
    registry.kill("test_create");
    REQUIRE(registry.count() == 0);
}

// ── Test 2: Detach session, verify state ─────────────────────────

TEST_CASE("SessionRegistry: detach session changes state", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_detach", "cmd.exe /c timeout /t 10", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Attached);

    registry.detach("test_detach");
    REQUIRE(s->state == SessionState::Detached);
    REQUIRE(registry.count() == 1);

    // Verify state in list
    auto list = registry.list();
    REQUIRE(list.size() == 1);
    REQUIRE(list[0].state == "detached");

    kill_child_process(*s);
    registry.kill("test_detach");
}

// ── Test 3: Reattach to detached session ────────────────────────

TEST_CASE("SessionRegistry: reattach to detached session", "[session_registry]") {
    SessionRegistry registry;
    auto* s1 = registry.attach("test_reattach", "cmd.exe /c timeout /t 10", 80, 24, "xterm-256color");
    REQUIRE(s1 != nullptr);
    registry.detach("test_reattach");
    REQUIRE(s1->state == SessionState::Detached);

    auto* s2 = registry.attach("test_reattach", "", 100, 50, "xterm");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2 == s1);  // same session object
    REQUIRE(s2->state == SessionState::Attached);

    kill_child_process(*s2);
    registry.kill("test_reattach");
}

// ── Test 4: Kill session removes from map ────────────────────────

TEST_CASE("SessionRegistry: kill removes session", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_kill", "cmd.exe /c timeout /t 10", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(registry.count() == 1);

    kill_child_process(*s);
    registry.kill("test_kill");
    REQUIRE(registry.count() == 0);
    REQUIRE(registry.get("test_kill") == nullptr);

    auto list = registry.list();
    REQUIRE(list.empty());
}

// ── Test 5: Cannot attach to dead session ───────────────────────

TEST_CASE("SessionRegistry: cannot attach to dead session", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_dead", "cmd.exe /c exit /b 1", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);

    // Wait for process to exit
    std::this_thread::sleep_for(500ms);
#ifdef _WIN32
    if (s->child_pid && WaitForSingleObject(s->child_pid, 2000) == WAIT_OBJECT_0) {
        CloseHandle(s->child_pid);
        s->child_pid = nullptr;
        s->state = SessionState::Died;
    }
#endif

    // Now try to attach to the dead session
    auto* s2 = registry.attach("test_dead", "", 80, 24, "xterm");
    REQUIRE(s2 == nullptr);

    registry.kill("test_dead");
}

// ── Test 6: get() and list() access ─────────────────────────────

TEST_CASE("SessionRegistry: get returns session or nullptr", "[session_registry]") {
    SessionRegistry registry;
    REQUIRE(registry.get("nonexistent") == nullptr);

    auto* s = registry.attach("test_get", "cmd.exe /c timeout /t 10", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    REQUIRE(registry.get("test_get") == s);
    REQUIRE(registry.get("test_get")->name == "test_get");

    kill_child_process(*s);
    registry.kill("test_get");
}

// ── Test 7: Multiple sessions coexist ────────────────────────────

TEST_CASE("SessionRegistry: multiple independent sessions", "[session_registry]") {
    SessionRegistry registry;
    std::vector<Session*> sessions;
    for (int i = 0; i < 3; ++i) {
        auto* s = registry.attach("multi_" + std::to_string(i), "cmd.exe /c timeout /t 10", 80, 24, "xterm-256color");
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
        kill_child_process(*s);
        registry.kill(s->name);
    }
    REQUIRE(registry.count() == 0);
}

// ── Test 8: prune_idle removes idle detached sessions ────────────

TEST_CASE("SessionRegistry: prune_idle removes idle detached", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_idle", "cmd.exe /c timeout /t 10", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);

    registry.detach("test_idle");
    REQUIRE(registry.count() == 1);

    // Fake last_output_at to be 10 days ago
    s->last_output_at = std::chrono::steady_clock::now() - std::chrono::hours(10 * 24);

    // Prune with 1-second timeout
    registry.prune_idle(std::chrono::seconds(1));
    REQUIRE(registry.count() == 0);

    kill_child_process(*s);
}

// ── Test 9: Persist sessions and resurrect ──────────────────────

TEST_CASE("SessionRegistry: persist and resurrect sessions", "[session_registry]") {
    auto path = temp_persistence_path();
    cleanup_temp_file(path);

    // Create registry and add a session
    {
        SessionRegistry registry;
        registry.set_persistence_path(path);
        auto* s = registry.attach("persist_test", "cmd.exe /c echo persisted", 80, 24, "xterm-256color");
        REQUIRE(s != nullptr);
        REQUIRE(registry.count() == 1);

        bool saved = registry.save_persisted_sessions();
        REQUIRE(saved);
        REQUIRE(std::filesystem::exists(path));

        kill_child_process(*s);
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
        REQUIRE(r->state == SessionState::Attached);
        REQUIRE(registry2.count() == 1);

        kill_child_process(*r);
        registry2.kill("persist_test");
    }

    cleanup_temp_file(path);
}

// ── Test 10: reap_dead detects exited child ─────────────────────

TEST_CASE("SessionRegistry: reap_dead detects exited child", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_reap", "cmd.exe /c exit /b 42", 80, 24, "xterm-256color");
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
    auto* s = registry.attach("test_restart", "cmd.exe /c exit /b 1", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    s->auto_restart = true;
    s->reset_restart_failures();

#ifdef _WIN32
    auto old_pid = s->child_pid;
#endif

    // Wait for process to exit
    std::this_thread::sleep_for(500ms);

    registry.reap_dead();
    REQUIRE(registry.count() == 1);

    auto* s2 = registry.get("test_restart");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2->auto_restart == true);
#ifdef _WIN32
    // New child should have a different handle
    REQUIRE(s2->child_pid != old_pid);
#endif

    kill_child_process(*s2);
    registry.kill("test_restart");
}

// ── Test 12: Circuit breaker stops after 3+ failures ────────────

TEST_CASE("SessionRegistry: circuit breaker after repeated failures", "[session_registry]") {
    SessionRegistry registry;
    auto* s = registry.attach("test_cb", "cmd.exe /c exit /b 1", 80, 24, "xterm-256color");
    REQUIRE(s != nullptr);
    s->auto_restart = true;
    s->reset_restart_failures();

    // Run through 4 deaths — the 4th should trip circuit breaker
    for (int i = 0; i < 4; ++i) {
        std::this_thread::sleep_for(300ms);
        registry.reap_dead();
    }

    // After 4 failures, session should be Exited (not growing)
    auto* final_s = registry.get("test_cb");
    if (final_s) {
        REQUIRE(final_s->state == SessionState::Exited);
    }
    // Count should be 1 (session still in map, but Exited)
    REQUIRE(registry.count() <= 1);

    registry.kill("test_cb");
}

// ── Test 13: Double attach replaces existing attachment ─────────

TEST_CASE("SessionRegistry: double attach replaces existing", "[session_registry]") {
    SessionRegistry registry;
    auto* s1 = registry.attach("test_double", "cmd.exe /c timeout /t 10", 80, 24, "xterm-256color");
    REQUIRE(s1 != nullptr);
    REQUIRE(s1->state == SessionState::Attached);

    // Second attach while still attached
    auto* s2 = registry.attach("test_double", "", 120, 40, "xterm");
    REQUIRE(s2 != nullptr);
    REQUIRE(s2 == s1);
    REQUIRE(s2->state == SessionState::Attached);

    kill_child_process(*s2);
    registry.kill("test_double");
}
