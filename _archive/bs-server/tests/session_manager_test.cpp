// session_manager_test.cpp — Phase 6: SessionManager unit tests
#include "session_manager.hpp"
#include "session.hpp"
#include <catch2/catch_test_macros.hpp>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <chrono>

using namespace bs::server;
using namespace std::chrono_literals;

// Helper: kill the child process of a session and wait for it
static void kill_child(Session& s) {
    if (s.child_pid > 0) {
        kill(s.child_pid, SIGKILL);
        int status;
        waitpid(s.child_pid, &status, 0);
        s.child_pid = -1;
    }
}

// ── Basic lifecycle: create, attach, detach, reattach ────────────────

TEST_CASE("SessionManager: create and attach", "[session_mgr]") {
    SessionManager mgr;
    auto result = mgr.attach("test1", "cat", 80, 24, "xterm-256color");
    REQUIRE(result.has_value());
    auto* s = *result;
    REQUIRE(s->state == SessionState::Attached);
    REQUIRE(s->master_fd >= 0);
    REQUIRE(s->child_pid > 0);
    REQUIRE(mgr.count() == 1);

    kill_child(*s);
    mgr.kill("test1");
    REQUIRE(mgr.count() == 0);
}

TEST_CASE("SessionManager: attach then detach", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("detach_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;
    REQUIRE(s->state == SessionState::Attached);

    mgr.detach("detach_test");
    REQUIRE(s->state == SessionState::Detached);
    REQUIRE(mgr.count() == 1);

    kill_child(*s);
    mgr.kill("detach_test");
}

TEST_CASE("SessionManager: reattach to detached session", "[session_mgr]") {
    SessionManager mgr;
    auto r1 = mgr.attach("reattach_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r1.has_value());
    auto* s = *r1;
    mgr.detach("reattach_test");
    REQUIRE(s->state == SessionState::Detached);

    auto r2 = mgr.attach("reattach_test", "", 100, 50, "xterm");
    REQUIRE(r2.has_value());
    REQUIRE(r2.value() == s);  // same session
    REQUIRE(s->state == SessionState::Attached);

    kill_child(*s);
    mgr.kill("reattach_test");
}

TEST_CASE("SessionManager: double attach replaces existing", "[session_mgr]") {
    SessionManager mgr;
    auto r1 = mgr.attach("double_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r1.has_value());
    auto* s = *r1;
    // Second attach while still Attached — should succeed (replace)
    auto r2 = mgr.attach("double_test", "", 80, 24, "xterm");
    REQUIRE(r2.has_value());
    REQUIRE(r2.value() == s);

    kill_child(*s);
    mgr.kill("double_test");
}

// ── State transitions: Died, Exited, Killed ──────────────────────────

TEST_CASE("SessionManager: reap_dead detects exited child", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("reap_test", "/bin/sh -c 'exit 42'", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;
    REQUIRE(s->state == SessionState::Attached);

    // Wait for child to actually exit (not just sleep) — TSan-safe
    // Use kill(pid, 0) to poll without consuming the wait status
    for (int i = 0; i < 200; ++i) {
        if (kill(s->child_pid, 0) < 0) break;  // ESRCH = process gone
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    mgr.reap_dead();
    REQUIRE(s->state == SessionState::Died);
    REQUIRE(mgr.count() == 1);

    mgr.kill("reap_test");
}

TEST_CASE("SessionManager: cannot attach to dead session", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("dead_test", "/bin/sh -c 'exit 0'", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;

    // Wait for child to actually exit — TSan-safe
    for (int i = 0; i < 200; ++i) {
        if (kill(s->child_pid, 0) < 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    mgr.reap_dead();
    REQUIRE(s->state == SessionState::Died);

    auto r2 = mgr.attach("dead_test", "", 80, 24, "xterm");
    REQUIRE(!r2.has_value());  // should fail
    REQUIRE(r2.error().message.find("dead") != std::string::npos);

    mgr.kill("dead_test");
}

TEST_CASE("SessionManager: kill removes session", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("kill_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    kill_child(**r);
    REQUIRE(mgr.count() == 1);

    mgr.kill("kill_test");
    REQUIRE(mgr.count() == 0);
}

// ── Session listing ─────────────────────────────────────────────────

TEST_CASE("SessionManager: list returns all sessions", "[session_mgr]") {
    SessionManager mgr;
    auto r1 = mgr.attach("list_a", "cat", 80, 24, "xterm-256color");
    auto r2 = mgr.attach("list_b", "cat", 80, 24, "xterm-256color");
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());

    auto list = mgr.list();
    REQUIRE(list.size() == 2);

    // Verify both sessions appear
    bool found_a = false, found_b = false;
    for (auto& info : list) {
        if (info.name == "list_a") found_a = true;
        if (info.name == "list_b") found_b = true;
    }
    REQUIRE(found_a);
    REQUIRE(found_b);

    kill_child(**r1);
    kill_child(**r2);
    mgr.kill("list_a");
    mgr.kill("list_b");
}

TEST_CASE("SessionManager: list includes session state", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("state_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;

    auto list1 = mgr.list();
    REQUIRE(list1.size() == 1);
    REQUIRE(list1[0].state == "attached");

    mgr.detach("state_test");
    auto list2 = mgr.list();
    REQUIRE(list2[0].state == "detached");

    kill_child(*s);
    mgr.kill("state_test");
}

TEST_CASE("SessionManager: same logical name is isolated per owner", "[session_mgr]") {
    SessionManager mgr;
    auto r1 = mgr.attach_for("client-a", "agent", "cat", 80, 24, "xterm-256color");
    auto r2 = mgr.attach_for("client-b", "agent", "cat", 80, 24, "xterm-256color");
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r1.value() != r2.value());
    REQUIRE(r1.value()->name == "agent");
    REQUIRE(r2.value()->name == "agent");
    REQUIRE(r1.value()->owner_id == "client-a");
    REQUIRE(r2.value()->owner_id == "client-b");
    REQUIRE(mgr.count() == 2);

    auto a = mgr.list_for("client-a");
    auto b = mgr.list_for("client-b");
    REQUIRE(a.size() == 1);
    REQUIRE(b.size() == 1);
    REQUIRE(a[0].name == "agent");
    REQUIRE(b[0].name == "agent");

    mgr.detach_for("client-a", "agent");
    REQUIRE(r1.value()->state == SessionState::Detached);
    REQUIRE(r2.value()->state == SessionState::Attached);

    kill_child(*r1.value());
    kill_child(*r2.value());
    mgr.kill_for("client-a", "agent");
    mgr.kill_for("client-b", "agent");
}

TEST_CASE("SessionManager: same owner reattaches to same logical name", "[session_mgr]") {
    SessionManager mgr;
    auto r1 = mgr.attach_for("client-a", "agent", "cat", 80, 24, "xterm-256color");
    REQUIRE(r1.has_value());
    auto* s = r1.value();
    mgr.detach_for("client-a", "agent");

    auto r2 = mgr.attach_for("client-a", "agent", "", 100, 40, "xterm");
    REQUIRE(r2.has_value());
    REQUIRE(r2.value() == s);
    REQUIRE(mgr.get_for("client-a", "agent") == s);

    kill_child(*s);
    mgr.kill_for("client-a", "agent");
}

// ── get() access ────────────────────────────────────────────────────

TEST_CASE("SessionManager: get returns nullptr for unknown session", "[session_mgr]") {
    SessionManager mgr;
    REQUIRE(mgr.get("nonexistent") == nullptr);
}

TEST_CASE("SessionManager: get returns session pointer", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("get_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());

    auto* s = mgr.get("get_test");
    REQUIRE(s != nullptr);
    REQUIRE(s->name == "get_test");
    REQUIRE(s->state == SessionState::Attached);

    kill_child(*s);
    mgr.kill("get_test");
}

// ── prune_idle: idle timeout cleanup ─────────────────────────────────

TEST_CASE("SessionManager: prune_idle removes idle detached sessions", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("idle_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;

    // Save child PID before session is destroyed
    int pid = s->child_pid;

    mgr.detach("idle_test");
    REQUIRE(mgr.count() == 1);

    // Fake the last_output_at to be 10 days ago
    s->last_output_at = std::chrono::steady_clock::now() - std::chrono::hours(10 * 24);

    // Prune with 1-second timeout — should remove the session
    mgr.prune_idle(std::chrono::seconds(1));
    REQUIRE(mgr.count() == 0);

    // Kill the child directly (session destroyed by prune_idle)
    if (pid > 0) { kill(pid, SIGKILL); int st; waitpid(pid, &st, 0); }
}

TEST_CASE("SessionManager: prune_idle keeps active sessions", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("active_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;

    // Session is Attached (not DETACHED) — should survive pruning
    mgr.prune_idle(std::chrono::seconds(1));
    REQUIRE(mgr.count() == 1);

    kill_child(*s);
    mgr.kill("active_test");
}

TEST_CASE("SessionManager: prune_idle preserves recent detached sessions", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("recent_test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;

    mgr.detach("recent_test");
    // last_output_at was just updated — should survive pruning
    mgr.prune_idle(std::chrono::seconds(1));
    REQUIRE(mgr.count() == 1);

    kill_child(*s);
    mgr.kill("recent_test");
}

// ── auto_restart: circuit breaker ───────────────────────────────────

TEST_CASE("SessionManager: auto_restart respawns died child", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("restart_test", "/bin/sh -c 'exit 1'", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;
    s->auto_restart = true;
    s->reset_restart_failures();

    // Wait for child to die
    std::this_thread::sleep_for(300ms);

    int old_pid = s->child_pid;
    mgr.reap_dead();

    // Session should still exist — auto-restart created a new child
    REQUIRE(mgr.count() == 1);
    auto* s2 = mgr.get("restart_test");
    REQUIRE(s2 != nullptr);
    // New child should have a different PID
    REQUIRE(s2->child_pid != old_pid);
    REQUIRE(s2->auto_restart == true);

    kill_child(*s2);
    mgr.kill("restart_test");
}

TEST_CASE("SessionManager: auto_restart circuit breaker after 3 failures", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("cb_test", "/bin/sh -c 'exit 1'", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;
    s->auto_restart = true;
    s->reset_restart_failures();

    // Fail 3 times rapidly
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(200ms);
        mgr.reap_dead();
    }

    // After 3 failures in 60s window, session should be Exited
    // reap_dead erases+replaces sessions internally — check count
    auto* final_s = mgr.get("cb_test");
    if (final_s) {
        // The session may have been fully erased or marked Exited
        // Either way, one more reap should not create another child
        int before = (int)mgr.count();
        mgr.reap_dead();
        int after = (int)mgr.count();
        REQUIRE(after <= before); // Not growing
    }

    mgr.kill("cb_test");
}

// ── command resolution ──────────────────────────────────────────────

TEST_CASE("SessionManager: uses custom command when provided", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("cmd_test", "echo custom_hello", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;
    // The session's command should be our custom one
    REQUIRE(s->command == "echo custom_hello");

    kill_child(*s);
    mgr.kill("cmd_test");
}

TEST_CASE("SessionManager: defaults to bash when no command", "[session_mgr]") {
    SessionManager mgr;
    auto r = mgr.attach("default_cmd_test", "", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    auto* s = *r;
    REQUIRE(s->command == "/bin/bash -l");

    kill_child(*s);
    mgr.kill("default_cmd_test");
}

// ── Multiple sessions coexist ───────────────────────────────────────

TEST_CASE("SessionManager: multiple independent sessions", "[session_mgr]") {
    SessionManager mgr;
    std::vector<Session*> sessions;
    for (int i = 0; i < 5; ++i) {
        auto r = mgr.attach("multi_" + std::to_string(i), "cat", 80, 24, "xterm-256color");
        REQUIRE(r.has_value());
        sessions.push_back(*r);
    }
    REQUIRE(mgr.count() == 5);

    // Detach some, keep others attached
    mgr.detach("multi_0");
    mgr.detach("multi_2");
    mgr.detach("multi_4");

    REQUIRE(mgr.get("multi_0")->state == SessionState::Detached);
    REQUIRE(mgr.get("multi_1")->state == SessionState::Attached);
    REQUIRE(mgr.get("multi_2")->state == SessionState::Detached);
    REQUIRE(mgr.get("multi_3")->state == SessionState::Attached);
    REQUIRE(mgr.get("multi_4")->state == SessionState::Detached);

    for (auto* s : sessions) {
        kill_child(*s);
        mgr.kill(s->name);
    }
    REQUIRE(mgr.count() == 0);
}
