// persistence_test.cpp — Phase 10: persistence unit tests
#include "persistence.hpp"
#include "session_manager.hpp"
#include "session.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <fstream>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace bs::server;

// Helper: create temp file path
static std::string temp_path() {
    return "/tmp/bs-persist-test-" + std::to_string(getpid()) + ".json";
}

static void cleanup(const std::string& path) {
    ::unlink(path.c_str());
    ::unlink((path + ".tmp").c_str());
}

TEST_CASE("Persistence: save and load roundtrip", "[persistence]") {
    auto path = temp_path();
    cleanup(path);

    std::vector<SessionMeta> input;
    input.push_back({"session1", "", "/bin/bash -l", "detached", "1234567890"});
    input.push_back({"session2", "owner-2", "echo hello", "running", "1234567900"});

    REQUIRE(save_sessions(path, input));

    auto output = load_sessions(path);
    REQUIRE(output.size() == 2);
    REQUIRE(output[0].name == "session1");
    REQUIRE(output[0].command == "/bin/bash -l");
    REQUIRE(output[0].state == "detached");
    REQUIRE(output[1].name == "session2");
    REQUIRE(output[1].owner_id == "owner-2");
    REQUIRE(output[1].command == "echo hello");
    REQUIRE(output[1].state == "running");

    cleanup(path);
}

TEST_CASE("Persistence: load missing file returns empty", "[persistence]") {
    auto sessions = load_sessions("/tmp/bs-persist-nonexistent-foobar.json");
    REQUIRE(sessions.empty());
}

TEST_CASE("Persistence: load corrupt JSON returns empty", "[persistence]") {
    auto path = temp_path();
    { std::ofstream f(path); f << "not valid json {{{"; }
    auto sessions = load_sessions(path);
    REQUIRE(sessions.empty());
    cleanup(path);
}

TEST_CASE("Persistence: atomic save — temp file removed", "[persistence]") {
    auto path = temp_path();
    cleanup(path);

    std::vector<SessionMeta> input;
    input.push_back({"atomic-test", "", "/bin/sh", "detached", "0"});
    REQUIRE(save_sessions(path, input));

    // Temp file should not exist (renamed to target)
    std::ifstream tmp(path + ".tmp");
    REQUIRE(!tmp.good());

    // Target file should exist
    std::ifstream target(path);
    REQUIRE(target.good());

    cleanup(path);
}

TEST_CASE("Persistence: empty sessions list saves empty array", "[persistence]") {
    auto path = temp_path();
    cleanup(path);

    std::vector<SessionMeta> empty;
    REQUIRE(save_sessions(path, empty));

    auto loaded = load_sessions(path);
    REQUIRE(loaded.empty());

    cleanup(path);
}

TEST_CASE("SessionManager: load_persisted_sessions sets RECOVERABLE state", "[persistence]") {
    auto path = temp_path();
    cleanup(path);

    std::vector<SessionMeta> input;
    input.push_back({"recover-me", "", "cat", "detached", "0"});
    save_sessions(path, input);

    SessionManager mgr;
    mgr.set_persistence_path(path);
    mgr.load_persisted_sessions();

    auto* s = mgr.get("recover-me");
    REQUIRE(s != nullptr);
    REQUIRE(s->state == SessionState::Recoverable);
    REQUIRE(s->name == "recover-me");
    REQUIRE(s->command == "cat");

    // RECOVERABLE sessions should be skippable by attach (won't reattach)
    cleanup(path);
}

TEST_CASE("SessionManager: resurrect spawns PTY for RECOVERABLE session", "[persistence]") {
    auto path = temp_path();
    cleanup(path);

    std::vector<SessionMeta> input;
    input.push_back({"rez-test", "", "cat", "detached", "0"});
    save_sessions(path, input);

    SessionManager mgr;
    mgr.set_persistence_path(path);
    mgr.load_persisted_sessions();

    auto result = mgr.resurrect("rez-test", 80, 24, "xterm-256color");
    REQUIRE(result.has_value());
    auto* s = *result;
    REQUIRE(s->state == SessionState::Attached);
    REQUIRE(s->master_fd >= 0);
    REQUIRE(s->child_pid > 0);
    REQUIRE(s->name == "rez-test");
    REQUIRE(s->command == "cat");

    // Cleanup
    if (s->child_pid > 0) { kill(s->child_pid, SIGKILL); int st; waitpid(s->child_pid, &st, 0); }
    mgr.kill("rez-test");
    cleanup(path);
}

TEST_CASE("SessionManager: resurrect nonexistent returns error", "[persistence]") {
    SessionManager mgr;
    auto result = mgr.resurrect("nope", 80, 24, "xterm-256color");
    REQUIRE(!result.has_value());
    REQUIRE(result.error().message.find("not found") != std::string::npos);
}

TEST_CASE("SessionManager: resurrect non-RECOVERABLE session fails", "[persistence]") {
    SessionManager mgr;
    auto r = mgr.attach("running-sesh", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());

    auto res = mgr.resurrect("running-sesh", 80, 24, "xterm-256color");
    REQUIRE(!res.has_value());
    REQUIRE(res.error().message.find("not recoverable") != std::string::npos);

    auto* s = *r;
    if (s->child_pid > 0) { kill(s->child_pid, SIGKILL); int st; waitpid(s->child_pid, &st, 0); }
    mgr.kill("running-sesh");
}

TEST_CASE("SessionManager: save_persisted_sessions preserves RECOVERABLE", "[persistence]") {
    auto path = temp_path();
    cleanup(path);

    std::vector<SessionMeta> preload;
    preload.push_back({"keep-me", "", "cat", "detached", "0"});
    save_sessions(path, preload);

    SessionManager mgr;
    mgr.set_persistence_path(path);
    mgr.load_persisted_sessions();  // loads as RECOVERABLE

    REQUIRE(mgr.save_persisted_sessions());  // should keep RECOVERABLE across restart cycles

    auto loaded = load_sessions(path);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].name == "keep-me");
    REQUIRE(loaded[0].command == "cat");
    REQUIRE(loaded[0].state == "recoverable");

    cleanup(path);
}

TEST_CASE("SessionManager: save_persisted_sessions saves ATTACHED/RUNNING/DETACHED", "[persistence]") {
    auto path = temp_path();
    cleanup(path);

    SessionManager mgr;
    mgr.set_persistence_path(path);

    auto r = mgr.attach("save-test", "cat", 80, 24, "xterm-256color");
    REQUIRE(r.has_value());
    mgr.detach("save-test");

    REQUIRE(mgr.save_persisted_sessions());

    auto loaded = load_sessions(path);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].name == "save-test");
    REQUIRE(loaded[0].command == "cat");
    REQUIRE(loaded[0].state == "detached");

    auto* s = *r;
    if (s->child_pid > 0) { kill(s->child_pid, SIGKILL); int st; waitpid(s->child_pid, &st, 0); }
    mgr.kill("save-test");
    cleanup(path);
}
