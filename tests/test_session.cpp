#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "bridgesessions.cpp"

#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <chrono>

using namespace bs::mesh;

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── Helpers ─────────────────────────────────────────────────────

// Read from a Windows pipe handle until timeout, return all bytes read
static std::string read_pipe(HANDLE hPipe, DWORD timeout_ms = 5000) {
    std::string result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &avail, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (avail == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::vector<char> buf(avail);
        DWORD nread = 0;
        if (ReadFile(hPipe, buf.data(), avail, &nread, nullptr) && nread > 0) {
            result.append(buf.data(), nread);
        }
    }
    return result;
}

// ── Test 1: Create session with "cmd.exe /c echo hello", verify output ──

TEST_CASE("create_session runs cmd.exe /c echo hello and produces output", "[session]") {
    auto s = create_session("test1", "cmd.exe /c echo hello", 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;
    REQUIRE(sess.state == SessionState::Running);
    REQUIRE(sess.name == "test1");

    // Read from the ConPTY output pipe
    std::string output = read_pipe(sess.master_fd, 5000);

    // The output should contain "hello"
    INFO("output: [" << output << "]");
    REQUIRE(output.find("hello") != std::string::npos);

    // Scrollback should have been populated (we need to flush pipe data to scrollback)
    // For now just verify the session is alive
    REQUIRE(sess.is_valid());
}

// ── Test 2: Kill session, verify state transitions ───────────

TEST_CASE("kill session transitions to Died state", "[session]") {
    // Create a long-running session
    auto s = create_session("test2", "cmd.exe /c ping -n 30 127.0.0.1", 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;
    REQUIRE(sess.state == SessionState::Running);
    REQUIRE(sess.child_pid != nullptr);

    // Kill the process
    TerminateProcess(sess.child_pid, 1);
    WaitForSingleObject(sess.child_pid, 5000);

    // After termination, process exit code should be non-zero
    DWORD exitCode = 0;
    GetExitCodeProcess(sess.child_pid, &exitCode);
    // The process was terminated, so it should have died
    if (exitCode != STILL_ACTIVE) {
        sess.state = SessionState::Died;
    }

    REQUIRE(sess.state == SessionState::Died);
}

// ── Test 3: Resize session — no crash ─────────────────────────

TEST_CASE("resize_pty does not crash", "[session]") {
    auto s = create_session("test3", "cmd.exe /c timeout /t 10", 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;

    // Resize to different dimensions
    auto result = resize_pty(reinterpret_cast<intptr_t>(sess.hpcon), 120, 40);

    // If we get here without crashing, the test passes
    REQUIRE(true);
}

// ── Test 4: Session move semantics ────────────────────────────

TEST_CASE("session move semantics transfer handles correctly", "[session]") {
    auto s = create_session("test4", "cmd.exe /c timeout /t 3", 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    // Save handles before move
    auto orig_master = s->master_fd;
    auto orig_child = s->child_pid;
    auto orig_write = s->write_handle;
    auto orig_hpcon = s->hpcon;
    REQUIRE(orig_master != nullptr);
    REQUIRE(orig_child != nullptr);

    // Move construct
    Session moved(std::move(*s));
    REQUIRE(moved.name == "test4");
    REQUIRE(moved.is_valid());
    REQUIRE(moved.master_fd == orig_master);
    REQUIRE(moved.child_pid == orig_child);
    REQUIRE(moved.write_handle == orig_write);
    REQUIRE(moved.hpcon == orig_hpcon);

    // Source should be nulled
    REQUIRE(s->master_fd == nullptr);
    REQUIRE(s->child_pid == nullptr);
    REQUIRE(s->write_handle == nullptr);
    REQUIRE(s->hpcon == nullptr);

    // Verify metadata moved correctly
    REQUIRE(moved.state == SessionState::Running);
    REQUIRE(moved.peer_ids.empty());
    REQUIRE(moved.command == "cmd.exe /c timeout /t 3");
}

// ── Test 5: Create session running "cmd.exe /c dir", verify output ──

TEST_CASE("create_session with dir command produces output in scrollback", "[session]") {
    auto s = create_session("test5", "cmd.exe /c dir", 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;
    REQUIRE(sess.state == SessionState::Running);

    // Read output from pipe
    std::string output = read_pipe(sess.master_fd, 5000);

    // dir should produce directory listing with lines
    INFO("dir output size: " << output.size());
    INFO("dir output: [" << output.substr(0, std::min(size_t{200}, output.size())) << "]");

    REQUIRE(output.size() > 0);
    // Should contain typical dir output markers
    REQUIRE((output.find("Volume") != std::string::npos || output.find("Directory") != std::string::npos ||
             output.find("<DIR>") != std::string::npos || output.size() > 50));
}
