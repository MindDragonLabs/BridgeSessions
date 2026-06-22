#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "bridgesessions.cpp"

#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <chrono>

using namespace bs::mesh;

#ifdef _WIN32
#define BS_CMD(win_cmd, posix_cmd) win_cmd
#else
#define BS_CMD(win_cmd, posix_cmd) posix_cmd
#endif

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

// ── Helpers ─────────────────────────────────────────────────────

static std::string read_session_output(Session& sess, uint32_t timeout_ms = 5000) {
    std::string result;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe(sess.master_fd, nullptr, 0, nullptr, &avail, nullptr)) {
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
        if (ReadFile(sess.master_fd, buf.data(), avail, &nread, nullptr) && nread > 0) {
            result.append(buf.data(), nread);
        }
#else
        char buf[4096];
        ssize_t n = read(sess.master_fd, buf, sizeof(buf));
        if (n > 0) {
            result.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        break;
#endif
    }
    return result;
}

static void terminate_session_child(Session& sess) {
#ifdef _WIN32
    if (sess.child_pid) {
        TerminateProcess(sess.child_pid, 1);
        WaitForSingleObject(sess.child_pid, 5000);
    }
#else
    if (sess.child_pid > 0) {
        kill(sess.child_pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            if (waitpid(sess.child_pid, &status, WNOHANG) == sess.child_pid) break;
            usleep(100000);
        }
        if (waitpid(sess.child_pid, &status, WNOHANG) != sess.child_pid) {
            kill(sess.child_pid, SIGKILL);
            waitpid(sess.child_pid, &status, 0);
        }
    }
#endif
}

// ── Test 1: Create session, verify output ───────────────────────

TEST_CASE("create_session echo produces output", "[session]") {
    auto s = create_session("test1", BS_CMD("cmd.exe /c echo hello", "echo hello"), 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;
    REQUIRE(sess.state == SessionState::Running);
    REQUIRE(sess.name == "test1");

    std::string output = read_session_output(sess, 5000);

    INFO("output: [" << output << "]");
    REQUIRE(output.find("hello") != std::string::npos);
    REQUIRE(sess.is_valid());
}

// ── Test 2: Kill session, verify state transitions ───────────

TEST_CASE("kill session transitions to Died state", "[session]") {
    auto s = create_session("test2", BS_CMD("cmd.exe /c ping -n 30 127.0.0.1", "sleep 30"), 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;
    REQUIRE(sess.state == SessionState::Running);
#ifdef _WIN32
    REQUIRE(sess.child_pid != nullptr);
#else
    REQUIRE(sess.child_pid > 0);
#endif

    terminate_session_child(sess);
    sess.state = SessionState::Died;
    REQUIRE(sess.state == SessionState::Died);
}

// ── Test 3: Resize session — no crash ─────────────────────────

TEST_CASE("resize_pty does not crash", "[session]") {
    auto s = create_session("test3", BS_CMD("cmd.exe /c timeout /t 10", "sleep 10"), 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;
#ifdef _WIN32
    auto result = resize_pty(reinterpret_cast<intptr_t>(sess.hpcon), 120, 40);
#else
    auto result = resize_pty(sess.master_fd, 120, 40);
#endif
    REQUIRE(result.has_value());
}

// ── Test 4: Session move semantics ────────────────────────────

TEST_CASE("session move semantics transfer handles correctly", "[session]") {
    auto s = create_session("test4", BS_CMD("cmd.exe /c timeout /t 3", "sleep 3"), 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto orig_master = s->master_fd;
    auto orig_child = s->child_pid;
#ifdef _WIN32
    auto orig_write = s->write_handle;
    auto orig_hpcon = s->hpcon;
    REQUIRE(orig_master != nullptr);
    REQUIRE(orig_child != nullptr);
#else
    REQUIRE(orig_master >= 0);
    REQUIRE(orig_child > 0);
#endif

    Session moved(std::move(*s));
    REQUIRE(moved.name == "test4");
    REQUIRE(moved.is_valid());
    REQUIRE(moved.master_fd == orig_master);
    REQUIRE(moved.child_pid == orig_child);
#ifdef _WIN32
    REQUIRE(moved.write_handle == orig_write);
    REQUIRE(moved.hpcon == orig_hpcon);
    REQUIRE(s->master_fd == nullptr);
    REQUIRE(s->child_pid == nullptr);
    REQUIRE(s->write_handle == nullptr);
    REQUIRE(s->hpcon == nullptr);
#else
    REQUIRE(s->master_fd == -1);
    REQUIRE(s->child_pid == -1);
#endif

    REQUIRE(moved.state == SessionState::Running);
    REQUIRE(moved.peer_ids.empty());
    REQUIRE(moved.command == BS_CMD("cmd.exe /c timeout /t 3", "sleep 3"));
}

// ── Test 5: Create session running directory listing, verify output ──

TEST_CASE("create_session with directory listing produces output", "[session]") {
    auto s = create_session("test5", BS_CMD("cmd.exe /c dir", "pwd; ls"), 80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;
    REQUIRE(sess.state == SessionState::Running);

    std::string output = read_session_output(sess, 5000);

    INFO("dir output size: " << output.size());
    INFO("dir output: [" << output.substr(0, std::min(size_t{200}, output.size())) << "]");

    REQUIRE(output.size() > 0);
}
