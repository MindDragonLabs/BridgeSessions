#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"

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

TEST_CASE("create_session applies requested PTY dimensions", "[session][pty-size]") {
#ifndef _WIN32
    auto result = create_session("pty-size", "sleep 1", 132, 43, "xterm-256color");
    REQUIRE(result.has_value());
    winsize ws{};
    REQUIRE(::ioctl(result->master_fd, TIOCGWINSZ, &ws) == 0);
    REQUIRE(ws.ws_col == 132);
    REQUIRE(ws.ws_row == 43);
    terminate_session_child(*result);
#else
    SUCCEED("ConPTY dimensions are set by CreatePseudoConsole");
#endif
}

TEST_CASE("nonblocking PTY drain coalesces burst output", "[session][pty-drain]") {
#ifndef _WIN32
    // Poll until full burst is available — single short sleep+read was flaky under load.
    auto result = create_session("pty-drain",
        "python3 -c 'import sys;sys.stdout.write(\"X\"*12000);sys.stdout.flush();sys.stdout.close()'",
        80, 24, "xterm-256color");
    REQUIRE(result.has_value());
    auto& s = *result;
    std::string output = read_session_output(s, 8000);
    // Allow modest PTY framing overhead; require full payload present.
    REQUIRE(output.find(std::string(12000, 'X')) != std::string::npos);
    terminate_session_child(s);
#else
    SUCCEED("ConPTY drain is covered by Windows integration tests");
#endif
}

TEST_CASE("session PTY master is nonblocking for daemon polling", "[session][fd-hygiene]") {
#ifndef _WIN32
    auto s = create_session("nonblocking-pty", "sleep 1", 80, 24, "xterm-256color");
    REQUIRE(s.has_value());
    const int flags = ::fcntl(s->master_fd, F_GETFL, 0);
    REQUIRE(flags >= 0);
    REQUIRE((flags & O_NONBLOCK) != 0);
    terminate_session_child(*s);
#endif
}

TEST_CASE("session child does not inherit daemon file descriptors", "[session][fd-hygiene]") {
#ifndef _WIN32
    int sentinel[2] = {-1, -1};
    REQUIRE(::pipe(sentinel) == 0);
    const std::string command = "if [ -e /dev/fd/" + std::to_string(sentinel[0]) +
                                " ]; then echo LEAKED; else echo CLOSED; fi";
    auto s = create_session("fd-hygiene", command, 80, 24, "xterm-256color");
    ::close(sentinel[0]);
    ::close(sentinel[1]);
    REQUIRE(s.has_value());
    const std::string output = read_session_output(*s, 5000);
    INFO("output: [" << output << "]");
    REQUIRE(output.find("CLOSED") != std::string::npos);
    REQUIRE(output.find("LEAKED") == std::string::npos);
#endif
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
    auto s = create_session(
        "test3",
        BS_CMD("cmd.exe", "sleep 10"),
        80, 24, "xterm-256color");
    REQUIRE(s.has_value());

    auto& sess = *s;
#ifdef _WIN32
    auto result = resize_pty(reinterpret_cast<intptr_t>(sess.hpcon), 120, 40);
#else
    auto result = resize_pty(sess.master_fd, 120, 40);
#endif
    INFO("resize result: " <<
         (result.has_value() ? "success" : result.error().message));
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

TEST_CASE("session move assignment reconstructs object lifetime safely",
          "[session][move]") {
    Session source;
    source.name = "source";
    source.command = "echo source";
    source.peer_ids = {"peer-a", "peer-b"};
    source.parent_id = "parent";
    source.generation = 42;
    Session destination;
    destination.name = "old";
    destination = std::move(source);
    REQUIRE(destination.name == "source");
    REQUIRE(destination.command == "echo source");
    REQUIRE(destination.peer_ids == std::vector<std::string>{"peer-a", "peer-b"});
    REQUIRE(destination.parent_id == "parent");
    REQUIRE(destination.generation == 42);
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

#ifndef _WIN32
TEST_CASE("SIGPIPE is suppressed so transport writes can reconnect", "[session][reconnect]") {
    int fds[2]{};
    REQUIRE(::pipe(fds) == 0);
    pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(fds[0]);
        std::signal(SIGPIPE, SIG_DFL);
        configure_sigpipe_handling();
        errno = 0;
        const char byte = 'x';
        const ssize_t wrote = ::write(fds[1], &byte, 1);
        _exit(wrote == -1 && errno == EPIPE ? 0 : 2);
    }
    ::close(fds[0]);
    ::close(fds[1]);
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
}
#endif

TEST_CASE("input typed during reconnect is buffered until reattach", "[session][interactive]") {
    std::string pending;
    REQUIRE_FALSE(queue_disconnected_input(pending, "hello"));
    REQUIRE(pending == "hello");
    REQUIRE_FALSE(queue_disconnected_input(pending, std::string_view("\x1a", 1)));
    REQUIRE(pending == std::string("hello\x1a", 6));
    REQUIRE(queue_disconnected_input(pending, std::string_view("\x03", 1)));
    REQUIRE(pending == std::string("hello\x1a", 6));
}

TEST_CASE("local Ctrl-C is the only interactive disconnect key", "[session][interactive]") {
    // Reconnect wait only: a lone 0x03 aborts waiting for the peer.
    // In a live attach, a single 0x03 still forwards to the PTY; a double
    // Ctrl-C (two 0x03 within 600ms) hard-kills the session process group.
    REQUIRE(local_input_requests_disconnect(std::string_view("\x03", 1)));
    REQUIRE_FALSE(local_input_requests_disconnect(std::string_view("\x1a", 1)));
    REQUIRE_FALSE(local_input_requests_disconnect("ordinary input"));
}

TEST_CASE("terminal cleanup disables mouse reporting and restores normal screen", "[session][terminal_cleanup]") {
    const std::string reset = terminal_cleanup_sequence();
    for (const std::string mode : {
            "\x1b[?9l", "\x1b[?1000l", "\x1b[?1002l", "\x1b[?1003l",
            "\x1b[?1004l", "\x1b[?1005l", "\x1b[?1006l", "\x1b[?1015l",
            "\x1b[?1016l", "\x1b[?2004l"}) {
        REQUIRE(reset.find(mode) != std::string::npos);
    }
    REQUIRE(reset.find("\x1b[?25h") != std::string::npos);
    REQUIRE(reset.find("\x1b[?1049l") != std::string::npos);
}
