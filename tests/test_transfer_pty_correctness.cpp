// test_transfer_pty_correctness.cpp — v2.0.6 transfer/PTY correctness
//
// Covers:
//   - async FILE_RECV destination is per Conn/request, not global receive_dir_
//   - POSIX nonblocking PTY input queues remainder on EAGAIN/EINTR and drains
//     from the event loop with bounded high/low-water backpressure

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "../bridgesessions.cpp"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#endif

#include <filesystem>
#include <fstream>
#include <string>

using namespace bs::mesh;
namespace fs = std::filesystem;

// ── Helpers ─────────────────────────────────────────────────────────

static MeshConfig test_cfg(const std::string& name) {
    MeshConfig c;
    c.node_name = name;
    c.listen_port = 19955;
    c.gossip_interval_secs = 300;
    c.ping_interval_secs = 300;
    c.scrollback_lines = 100;
    return c;
}

static MeshController::Conn make_test_conn(const std::string& name,
                                           const std::string& pubkey) {
    MeshController::Conn c;
    c.peer_name = name;
    c.peer_pubkey = pubkey;
    c.peer_addr = "127.0.0.1:9001";
    c.ssl.reset();
    c.sock_fd = INVALID_SOCKET;
    c.last_pong = std::chrono::steady_clock::now();
    c.attached_session = nullptr;
    return c;
}

static std::string temp_suffix() {
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// ── Async FILE_RECV destination per Conn ────────────────────────────

TEST_CASE("async file recv uses per-connection destination directory", "[transfer][async_recv]") {
    auto tmp = fs::temp_directory_path() / ("bs-recv-default-" + temp_suffix());
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto guard = [&] { fs::remove_all(tmp); };

    auto cfg = test_cfg("recv-conn");
    MeshController mc(cfg, tmp.string());

    auto c = make_test_conn("peer-a", std::string(64, 'a'));
    mc.close_conn_for_test(c);  // exercise close clears pending_recv_dir
    REQUIRE(mc.pending_recv_dir_for_test(c).empty());

    // Default destination: no pending_recv_dir set -> falls back to receive_dir_
    FileMetaMsg meta;
    meta.filename = "hello.txt";
    meta.filesize = 5;
    meta.total_chunks = 1;
    meta.checksum = sha256_hex("hello");

    mc.inject_file_meta_for_test(c, meta);
    REQUIRE(mc.file_receive_for_test(c).active);
    REQUIRE(mc.pending_recv_dir_for_test(c).empty());
    REQUIRE(mc.file_receive_for_test(c).path == (tmp / "received" / "hello.txt").string());

    // Custom destination: pending_recv_dir consumed and used
    auto custom = tmp / "custom";
    fs::create_directories(custom);
    c.pending_recv_dir = custom.string();
    mc.inject_file_meta_for_test(c, meta);
    REQUIRE(mc.pending_recv_dir_for_test(c).empty());
    REQUIRE(mc.file_receive_for_test(c).path == (custom / "hello.txt").string());

    mc.close_conn_for_test(c);
    reset_logger_for_test();
    guard();
}

TEST_CASE("async file recv destination resets on meta rejection", "[transfer][async_recv]") {
    auto tmp = fs::temp_directory_path() / ("bs-recv-reject-" + temp_suffix());
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto guard = [&] { fs::remove_all(tmp); };

    auto cfg = test_cfg("recv-reject");
    MeshController mc(cfg, tmp.string());

    auto c = make_test_conn("peer-b", std::string(64, 'b'));
    auto custom = tmp / "custom";
    c.pending_recv_dir = custom.string();

    FileMetaMsg meta;
    meta.filename = ".";  // unsafe -> rejected
    meta.filesize = 5;
    meta.total_chunks = 1;
    meta.checksum = sha256_hex("hello");

    mc.inject_file_meta_for_test(c, meta);
    REQUIRE_FALSE(mc.file_receive_for_test(c).active);
    REQUIRE(mc.pending_recv_dir_for_test(c).empty());  // consumed/reset on failure

    reset_logger_for_test();
    guard();
}

TEST_CASE("async file recv destination resets on connection close", "[transfer][async_recv]") {
    auto tmp = fs::temp_directory_path() / ("bs-recv-close-" + temp_suffix());
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    auto guard = [&] { fs::remove_all(tmp); };

    auto cfg = test_cfg("recv-close");
    MeshController mc(cfg, tmp.string());

    auto c = make_test_conn("peer-c", std::string(64, 'c'));
    c.pending_recv_dir = (tmp / "pending").string();
    mc.close_conn_for_test(c);
    REQUIRE(mc.pending_recv_dir_for_test(c).empty());

    reset_logger_for_test();
    guard();
}

TEST_CASE("async file recv allows only one outstanding request per connection",
          "[transfer][async_recv]") {
    MeshController mc(test_cfg("recv-one-at-a-time"));
    auto c = make_test_conn("peer-d", std::string(64, 'd'));
    REQUIRE(mc.begin_async_receive_for_test(c, "/tmp/first"));
    REQUIRE_FALSE(mc.begin_async_receive_for_test(c, "/tmp/second"));
    REQUIRE(mc.pending_recv_dir_for_test(c) == "/tmp/first");
}

// ── POSIX nonblocking PTY input queue/drain/backpressure ────────────

#ifndef _WIN32

static Session make_test_session_with_pipe(int& read_end /* out */) {
    int fds[2] = {-1, -1};
    REQUIRE(::pipe(fds) == 0);
    for (int fd : fds) {
        int flags = ::fcntl(fd, F_GETFL, 0);
        REQUIRE(flags >= 0);
        REQUIRE(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    }

    Session s;
    s.name = "test";
    s.master_fd = fds[1];  // write end: PTY master input
    s.child_pid = -1;      // no real child
    read_end = fds[0];
    return s;
}

TEST_CASE("PTY input writes immediately when buffer has space", "[pty][input]") {
    int read_end = -1;
    auto s = make_test_session_with_pipe(read_end);
    auto guard = [&] { ::close(read_end); };

    MeshController mc(test_cfg("pty-write"));
    const std::string data = "hello pty";
    REQUIRE(mc.write_pty_input_for_test(s, data.data(), data.size()));
    REQUIRE(mc.pending_input_for_test(s).empty());

    char buf[64] = {};
    ssize_t n = ::read(read_end, buf, sizeof(buf));
    REQUIRE(n == static_cast<ssize_t>(data.size()));
    REQUIRE(std::string_view(buf, static_cast<size_t>(n)) == data);

    guard();
}

TEST_CASE("PTY input queues remainder when pipe is full", "[pty][input]") {
    int read_end = -1;
    auto s = make_test_session_with_pipe(read_end);
    auto guard = [&] { ::close(read_end); };

    MeshController mc(test_cfg("pty-queue"));

    // Keep writing through the handler until the pipe backs up and data is
    // queued in pending_input. The handler must not lose bytes.
    const std::string chunk(16 * 1024, 'F');
    size_t iterations = 0;
    while (mc.pending_input_for_test(s).empty() && iterations < 500) {
        REQUIRE(mc.write_pty_input_for_test(s, chunk.data(), chunk.size()));
        ++iterations;
    }
    REQUIRE_FALSE(mc.pending_input_for_test(s).empty());

    // Now write a marker; it must also be queued, not lost.
    const std::string extra = "overflow-bytes";
    REQUIRE(mc.write_pty_input_for_test(s, extra.data(), extra.size()));
    REQUIRE(mc.pending_input_for_test(s).find("overflow-bytes") != std::string::npos);

    // Drain: read everything currently in the pipe, then repeatedly drain the
    // queue and read what was written until the queue is empty.
    char drain[32 * 1024];
    while (::read(read_end, drain, sizeof(drain)) > 0) {}

    int safety = 0;
    while (!mc.pending_input_for_test(s).empty() && safety < 1000) {
        mc.drain_pending_pty_input_for_test(s);
        while (::read(read_end, drain, sizeof(drain)) > 0) {}
        ++safety;
    }
    REQUIRE(mc.pending_input_for_test(s).empty());

    guard();
}

TEST_CASE("PTY input applies high-water backpressure", "[pty][input][backpressure]") {
    int read_end = -1;
    auto s = make_test_session_with_pipe(read_end);
    auto guard = [&] { ::close(read_end); };

    MeshController mc(test_cfg("pty-backpressure"));

    // Artificially set pending input above the high-water mark.
    s.pending_input = std::string(Session::kPtyInputHighWater + 1, 'X');

    const std::string more = "additional";
    REQUIRE(mc.write_pty_input_for_test(s, more.data(), more.size()));
    // No write() attempt should have been made; all new data queued.
    REQUIRE(mc.pending_input_for_test(s).size() >= Session::kPtyInputHighWater + 1 + more.size());

    guard();
}

TEST_CASE("PTY input refuses to overflow bounded queue", "[pty][input][overflow]") {
    int read_end = -1;
    auto s = make_test_session_with_pipe(read_end);
    auto guard = [&] { ::close(read_end); };

    MeshController mc(test_cfg("pty-overflow"));

    s.pending_input = std::string(Session::kPtyInputMax - 10, 'X');
    std::string too_much(100, 'Y');
    REQUIRE_FALSE(mc.write_pty_input_for_test(s, too_much.data(), too_much.size()));

    guard();
}

TEST_CASE("drain_pending_pty_input returns below-low-water status", "[pty][input]") {
    int read_end = -1;
    auto s = make_test_session_with_pipe(read_end);
    auto guard = [&] { ::close(read_end); };

    MeshController mc(test_cfg("pty-drain"));

    s.pending_input = "queued data";
    REQUIRE(mc.drain_pending_pty_input_for_test(s));
    REQUIRE(mc.pending_input_for_test(s).empty());

    // Verify it was actually written.
    char buf[64] = {};
    REQUIRE(::read(read_end, buf, sizeof(buf)) == 11);
    REQUIRE(std::string_view(buf, 11) == "queued data");

    guard();
}

TEST_CASE("PTY input preserves queued-byte ordering and releases hysteresis below low water",
          "[pty][input][backpressure]") {
    int read_end = -1;
    auto s = make_test_session_with_pipe(read_end);
    auto guard = [&] { ::close(read_end); };
    MeshController mc(test_cfg("pty-order"));

    std::string fill(4096, 'F');
    while (::write(s.master_fd, fill.data(), fill.size()) > 0) {}
    s.pending_input = "old";
    REQUIRE(mc.write_pty_input_for_test(s, "new", 3));
    REQUIRE(mc.pending_input_for_test(s) == "oldnew");

    char drain[8192];
    while (::read(read_end, drain, sizeof(drain)) > 0) {}
    s.input_backpressured = true;
    REQUIRE(mc.drain_pending_pty_input_for_test(s));
    REQUIRE_FALSE(s.input_backpressured);
    REQUIRE(mc.pending_input_for_test(s).empty());
    guard();
}

#endif // !_WIN32

#ifdef _WIN32
TEST_CASE("Windows ConPTY input is delivered by the bounded writer thread",
          "[pty][input][windows]") {
    Session session;
    session.name = "windows-pty";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
    REQUIRE(CreatePipe(&session.master_fd, &session.write_handle, &sa, 0));

    MeshController mc(test_cfg("windows-pty-writer"));
    const std::string payload = "queued conpty input";
    REQUIRE(mc.write_pty_input_for_test(session, payload.data(), payload.size()));

    DWORD available = 0;
    for (int i = 0; i < 200 && available == 0; ++i) {
        REQUIRE(PeekNamedPipe(session.master_fd, nullptr, 0, nullptr,
                              &available, nullptr));
        if (available == 0) Sleep(5);
    }
    REQUIRE(available == payload.size());
    std::string received(payload.size(), '\0');
    DWORD read = 0;
    REQUIRE(ReadFile(session.master_fd, received.data(),
                     static_cast<DWORD>(received.size()), &read, nullptr));
    REQUIRE(read == payload.size());
    REQUIRE(received == payload);
}
#endif

// ── Main ────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
