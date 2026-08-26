// bs-session-worker.h — Per-session worker process for BridgeSessions
//
// Implements the session-worker architecture: each session's PTY and child
// process run in a dedicated worker process that survives controller restarts.
//
// Controller ↔ Worker communicate over a Unix domain socket (POSIX) or
// named pipe/TCP localhost (Windows) using a simple length-prefixed protocol.
//
// See docs/SESSION-WORKER-SPLIT.md for architecture overview.

#pragma once

#include "bs-session.h"

// ────────────────────────────────────────────────────────────────────
// 0. INCLUDES for socket I/O
// ────────────────────────────────────────────────────────────────────

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <namedpipeapi.h>
#include <process.h>
#pragma comment(lib, "Ws2_32.lib")
#ifndef _PID_T_DEFINED
typedef int pid_t;
#define _PID_T_DEFINED
#endif
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>
#endif

#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <filesystem>

namespace worker {

// ────────────────────────────────────────────────────────────────────
// 1. Worker IPC Protocol
// ────────────────────────────────────────────────────────────────────

// Message types for controller ↔ worker communication.
// Wire format: [1 byte type][4 bytes big-endian length][payload]
// This is simpler than the mesh TLS frame protocol — no compression,
// no stream multiplexing. Just a raw byte pipe for PTY I/O.

enum WorkerMsgType : uint8_t {
    // Controller → Worker
    WMSG_INPUT      = 0x01,  // raw bytes → write to PTY master
    WMSG_RESIZE     = 0x02,  // uint16 cols, uint16 rows
    WMSG_DETACH     = 0x03,  // detach one client (uint32 attach_id)
    WMSG_PING       = 0x04,  // liveness check
    WMSG_SHUTDOWN   = 0x05,  // graceful worker shutdown

    // Worker → Controller
    WMSG_OUTPUT     = 0x81,  // raw bytes from PTY output
    WMSG_SCROLLBACK = 0x82,  // scrollback snapshot on new attach
    WMSG_DIED       = 0x83,  // child exited: int32 exit_code, int32 signal
    WMSG_READY      = 0x84,  // worker started ok: string name, int32 pid
    WMSG_PONG       = 0x85,  // liveness reply
    WMSG_ERROR      = 0x86,  // error: string message
};

// ────────────────────────────────────────────────────────────────────
// 2. Wire encoding / decoding helpers
// ────────────────────────────────────────────────────────────────────

inline void write_u32be(uint8_t* p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

inline uint32_t read_u32be(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline void write_u16be(uint8_t* p, uint16_t v) {
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

inline uint16_t read_u16be(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

// Send a worker message on a socket (blocking write).
// Returns true on success, false on failure.
inline bool worker_send(int fd, WorkerMsgType type, const void* data, size_t len) {
    if (len > 16 * 1024 * 1024) return false;  // 16 MB sanity cap
    uint8_t header[5];
    header[0] = static_cast<uint8_t>(type);
    write_u32be(header + 1, static_cast<uint32_t>(len));

    // Write header
    size_t total = 0;
    while (total < 5) {
#ifdef _WIN32
        int n = ::send(fd, reinterpret_cast<const char*>(header + total),
                       static_cast<int>(5 - total), 0);
        if (n <= 0) {
            if (WSAGetLastError() == WSAEINTR) continue;
            return false;
        }
#else
        ssize_t n = ::write(fd, header + total, 5 - total);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
#endif
        total += static_cast<size_t>(n);
    }

    // Write payload (if any)
    if (len > 0 && data) {
        total = 0;
        while (total < len) {
#ifdef _WIN32
            int n = ::send(fd, reinterpret_cast<const char*>(static_cast<const uint8_t*>(data) + total),
                           static_cast<int>(std::min(len - total, static_cast<size_t>(65536))), 0);
            if (n <= 0) {
                if (WSAGetLastError() == WSAEINTR) continue;
                return false;
            }
#else
            ssize_t n = ::write(fd, static_cast<const char*>(data) + total,
                                std::min(len - total, static_cast<size_t>(65536)));
            if (n < 0) { if (errno == EINTR) continue; return false; }
            if (n == 0) return false;
#endif
            total += static_cast<size_t>(n);
        }
    }
    return true;
}

// Convenience overloads
inline bool worker_send(int fd, WorkerMsgType type, const std::string& data) {
    return worker_send(fd, type, data.data(), data.size());
}

inline bool worker_send(int fd, WorkerMsgType type) {
    return worker_send(fd, type, nullptr, 0);
}

// Received message (type + payload bytes)
struct WorkerMessage {
    WorkerMsgType type;
    std::vector<uint8_t> data;
};

// Read one worker message from a socket (blocking, with timeout via select).
// timeout_ms = -1 for blocking, >=0 for timed wait.
// Returns true on success, false on EOF/error/timeout.
inline bool worker_recv(int fd, WorkerMessage& msg, int timeout_ms = -1) {
    // Wait for readability if timeout specified
    if (timeout_ms >= 0) {
#ifndef _WIN32
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) return false;
#else
        // Windows: use select on the socket
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int ready = ::select(0, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) return false;
#endif
    }

    // Read 5-byte header: [type:1][length:4]
    uint8_t header[5];
    size_t total = 0;
    while (total < 5) {
#ifdef _WIN32
        int n = ::recv(fd, reinterpret_cast<char*>(header + total),
                       static_cast<int>(5 - total), 0);
        if (n <= 0) {
            if (WSAGetLastError() == WSAEINTR) continue;
            return false;
        }
#else
        ssize_t n = ::read(fd, header + total, 5 - total);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;  // EOF
#endif
        total += static_cast<size_t>(n);
    }

    msg.type = static_cast<WorkerMsgType>(header[0]);
    uint32_t len = read_u32be(header + 1);
    if (len > 16 * 1024 * 1024) return false;

    // Read payload
    msg.data.resize(len);
    if (len > 0) {
        total = 0;
        while (total < len) {
#ifdef _WIN32
            int n = ::recv(fd, reinterpret_cast<char*>(msg.data.data() + total),
                           static_cast<int>(std::min(len - total, static_cast<size_t>(65536))), 0);
            if (n <= 0) {
                if (WSAGetLastError() == WSAEINTR) continue;
                return false;
            }
#else
            ssize_t n = ::read(fd, msg.data.data() + total,
                               std::min(len - total, static_cast<size_t>(65536)));
            if (n < 0) { if (errno == EINTR) continue; return false; }
            if (n == 0) return false;
#endif
            total += static_cast<size_t>(n);
        }
    }
    return true;
}

// ────────────────────────────────────────────────────────────────────
// 3. Socket path management
// ────────────────────────────────────────────────────────────────────

// Directory for worker Unix domain sockets
inline std::string worker_socket_dir(const std::string& app_home) {
    namespace fs = std::filesystem;
    std::string dir = app_home + "/run/bs-sessions";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // Ensure permissions are restrictive (0700)
#ifndef _WIN32
    ::chmod(dir.c_str(), 0700);
#endif
    return dir;
}

// Full socket path for a named session
inline std::string worker_socket_path(const std::string& app_home, const std::string& session_name) {
    // Sanitize session name for filesystem use
    std::string safe;
    safe.reserve(session_name.size());
    for (char c : session_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
            safe += c;
        } else {
            safe += '_';
        }
    }
    return worker_socket_dir(app_home) + "/" + safe + ".sock";
}

// ────────────────────────────────────────────────────────────────────
// 4. Session Worker Process (the standalone worker)
// ────────────────────────────────────────────────────────────────────

// The worker: spawns a PTY child, listens on a Unix socket, and bridges
// PTY I/O to connected clients. Runs as a standalone process via
// `bridgesessions session-worker`.

struct WorkerConfig {
    std::string socket_path;
    std::string session_name;
    std::string command;     // shell command (empty = default shell)
    uint16_t cols = 80;
    uint16_t rows = 24;
    std::string term = "xterm-256color";
    std::string app_home;    // for scrollback persistence
};

// Result of running a worker
struct WorkerResult {
    int exit_code = 0;
    int signal_num = 0;
};

#ifdef _WIN32

// Windows worker stub — ConPTY implementation deferred (platform parity later).
// The controller falls back to inline forkpty-equivalent on Windows.
inline WorkerResult run_session_worker_win(const WorkerConfig& cfg) {
    // TODO: Windows ConPTY worker
    WorkerResult r;
    r.exit_code = -1;
    return r;
}

#else  // POSIX

inline WorkerResult run_session_worker_posix(const WorkerConfig& cfg) {
    namespace fs = std::filesystem;

    // Close ALL inherited fds except stdin/stdout/stderr.
    // The daemon spawned us (possibly via systemd-run) and we may have
    // inherited its listening ports and peer connection sockets. We must
    // not hold them open — the daemon must be free to restart.
#if defined(__linux__) && defined(SYS_close_range)
    ::syscall(SYS_close_range, 3u, ~0u, 0u);
#endif
    {
        long max_fd = ::sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 1024;
        for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
    }

    // Clean up any stale socket
    ::unlink(cfg.socket_path.c_str());

    // 1. Create the listening Unix domain socket
    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return {-1, 0};
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;

    // Check path length (sun_path is typically 108 bytes)
    if (cfg.socket_path.size() >= sizeof(addr.sun_path)) {
        ::close(listen_fd);
        return {-1, 0};
    }
    std::strncpy(addr.sun_path, cfg.socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd);
        return {-1, 0};
    }

    ::chmod(cfg.socket_path.c_str(), 0600);

    if (::listen(listen_fd, 4) < 0) {
        ::close(listen_fd);
        ::unlink(cfg.socket_path.c_str());
        return {-1, 0};
    }

    // Set listen socket non-blocking for select()
    {
        int fl = ::fcntl(listen_fd, F_GETFL, 0);
        if (fl >= 0) ::fcntl(listen_fd, F_SETFL, fl | O_NONBLOCK);
    }

    // 2. Spawn the PTY child using the existing create_session helper
    auto session_result = create_session(
        cfg.session_name, cfg.command, cfg.cols, cfg.rows, cfg.term);
    if (!session_result) {
        ::close(listen_fd);
        ::unlink(cfg.socket_path.c_str());
        return {-1, 0};
    }

    auto& session = *session_result;
    int master_fd = session.master_fd;
    pid_t child_pid = session.child_pid;

    // The worker event loop must never block on a child that stops reading.
    const int master_flags = ::fcntl(master_fd, F_GETFL, 0);
    if (master_flags < 0 ||
        ::fcntl(master_fd, F_SETFL, master_flags | O_NONBLOCK) < 0) {
        ::close(listen_fd);
        ::unlink(cfg.socket_path.c_str());
        return {-1, 0};
    }

    constexpr size_t kPendingPtyInputCap = 1024u * 1024u;
    std::vector<uint8_t> pending_pty_input;
    size_t pending_pty_offset = 0;
    auto drain_pending_pty_input = [&]() {
        while (pending_pty_offset < pending_pty_input.size()) {
            const ssize_t n = ::write(
                master_fd, pending_pty_input.data() + pending_pty_offset,
                pending_pty_input.size() - pending_pty_offset);
            if (n > 0) {
                pending_pty_offset += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            pending_pty_input.clear();
            pending_pty_offset = 0;
            break;
        }
        if (pending_pty_offset == pending_pty_input.size()) {
            pending_pty_input.clear();
            pending_pty_offset = 0;
        } else if (pending_pty_offset >= 64u * 1024u) {
            pending_pty_input.erase(
                pending_pty_input.begin(),
                pending_pty_input.begin() + static_cast<std::ptrdiff_t>(pending_pty_offset));
            pending_pty_offset = 0;
        }
    };

    // Ignore SIGPIPE — write to dead socket returns error, not signal death
    ::signal(SIGPIPE, SIG_IGN);

    // SIGTERM/SIGINT → graceful shutdown (kill the shell group, then exit).
    // Without this, default SIGTERM would orphan the shell the worker hosts.
    static volatile sig_atomic_t stop_requested = 0;
    stop_requested = 0;
    struct sigaction stop_sa{};
    stop_sa.sa_handler = [](int) { stop_requested = 1; };
    sigemptyset(&stop_sa.sa_mask);
    ::sigaction(SIGTERM, &stop_sa, nullptr);
    ::sigaction(SIGINT, &stop_sa, nullptr);

    // 3. Write PID file for controller discovery
    std::string pid_file = cfg.socket_path + ".pid";
    {
        FILE* f = std::fopen(pid_file.c_str(), "w");
        if (f) {
            std::fprintf(f, "%d\n", static_cast<int>(::getpid()));
            std::fclose(f);
        }
    }

    // Scrollback buffer (same ring buffer as main daemon)
    RingBuffer<1048576> scrollback;

    // Connected clients (controller connections). Non-blocking with bounded
    // per-client queues: a daemon dying mid-frame can never wedge the worker.
    struct WorkerClient {
        int fd = -1;
        std::string rx;        // partial-frame reassembly buffer
        std::string tx;        // framed output queue
        size_t tx_off = 0;
    };
    std::vector<WorkerClient> clients;

    constexpr size_t kClientRxCap = 16u * 1024u * 1024u + 5;  // max frame + header
    constexpr size_t kClientTxCap = 4u * 1024u * 1024u;

    // Queue one frame to a client. Returns false when the client's backlog is
    // saturated — the caller must DROP that client (silently discarding frames
    // would create undetectable terminal-output gaps and could eat PONG/DIED).
    auto queue_to_client = [&](WorkerClient& c, WorkerMsgType t,
                               const void* data, size_t len) {
        if (c.fd < 0) return false;
        if (c.tx.size() - c.tx_off + 5 + len > kClientTxCap) return false;
        uint8_t hdr[5];
        hdr[0] = static_cast<uint8_t>(t);
        write_u32be(hdr + 1, static_cast<uint32_t>(len));
        c.tx.append(reinterpret_cast<const char*>(hdr), 5);
        if (len > 0 && data)
            c.tx.append(reinterpret_cast<const char*>(data), len);
        return true;
    };

    // Best-effort non-blocking flush of one client's tx queue. Compacts the
    // consumed prefix periodically so sustained partial progress cannot grow
    // the buffer without bound. Returns false only on hard error (drop client).
    auto flush_client = [&](WorkerClient& c) {
        while (c.tx_off < c.tx.size()) {
            const ssize_t n = ::write(c.fd, c.tx.data() + c.tx_off,
                                      c.tx.size() - c.tx_off);
            if (n > 0) { c.tx_off += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            return false;
        }
        if (c.tx_off == c.tx.size()) {
            c.tx.clear();
            c.tx_off = 0;
        } else if (c.tx_off >= 64 * 1024) {
            c.tx.erase(0, c.tx_off);
            c.tx_off = 0;
        }
        return true;
    };

    WorkerResult result{0, 0};
    bool child_died = false;

    // 4. Event loop
    while (!child_died) {
        if (stop_requested) goto worker_shutdown;
        fd_set read_fds;
        fd_set write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(listen_fd, &read_fds);
        FD_SET(master_fd, &read_fds);
        if (pending_pty_offset < pending_pty_input.size())
            FD_SET(master_fd, &write_fds);
        int max_fd = std::max(listen_fd, master_fd);

        for (const auto& c : clients) {
            if (c.fd >= 0) {
                FD_SET(c.fd, &read_fds);
                if (c.tx_off < c.tx.size()) FD_SET(c.fd, &write_fds);
                if (c.fd > max_fd) max_fd = c.fd;
            }
        }

        timeval tv{0, 100000};  // 100 ms child-death/input-latency bound
        int ready = ::select(max_fd + 1, &read_fds, &write_fds, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(master_fd, &write_fds)) {
            drain_pending_pty_input();
        }

        // Accept new controller connections
        if (FD_ISSET(listen_fd, &read_fds)) {
            int cfd = ::accept(listen_fd, nullptr, nullptr);
            if (cfd >= 0) {
                const int cfl = ::fcntl(cfd, F_GETFL, 0);
                if (cfl < 0 || ::fcntl(cfd, F_SETFL, cfl | O_NONBLOCK) < 0) {
                    ::close(cfd);
                } else {
                    // READY payload = UTF-8 session name followed by child pid
                    // (u32be), then a scrollback snapshot. Both queue through
                    // the per-client tx buffer — never block the loop.
                    WorkerClient wc;
                    wc.fd = cfd;
                    std::string ready_payload = cfg.session_name;
                    uint8_t pid_bytes[4];
                    write_u32be(pid_bytes, static_cast<uint32_t>(child_pid));
                    ready_payload.append(reinterpret_cast<const char*>(pid_bytes),
                                         sizeof(pid_bytes));
                    bool queued = queue_to_client(wc, WMSG_READY,
                                    ready_payload.data(), ready_payload.size());
                    auto sb = scrollback.read_last_lines(8192);
                    if (queued && !sb.empty())
                        queued = queue_to_client(wc, WMSG_SCROLLBACK, sb.data(), sb.size());
                    if (queued && flush_client(wc)) clients.push_back(std::move(wc));
                    else ::close(cfd);
                }
            }
        }

        // Read PTY output → queue to all clients (flush happens below). A
        // client whose backlog exceeds the cap is dropped, never silently
        // under-fed.
        if (master_fd >= 0 && FD_ISSET(master_fd, &read_fds)) {
            char buf[65536];
            ssize_t n = ::read(master_fd, buf, sizeof(buf));
            if (n > 0) {
                // Store in scrollback
                scrollback.write(std::string_view(buf, static_cast<size_t>(n)));
                for (auto it = clients.begin(); it != clients.end(); ) {
                    if (!queue_to_client(*it, WMSG_OUTPUT, buf, static_cast<size_t>(n))) {
                        ::close(it->fd);
                        it = clients.erase(it);
                        continue;
                    }
                    ++it;
                }
            }
            // n == 0 would mean EOF on PTY master, handled by child death check below
        }

        // Read from controller clients → write to PTY (incremental, non-blocking)
        for (auto it = clients.begin(); it != clients.end(); ) {
            WorkerClient& c = *it;
            if (c.fd < 0) { ++it; continue; }
            bool drop = false;

            if (FD_ISSET(c.fd, &write_fds)) drop = !flush_client(c);

            if (!drop && FD_ISSET(c.fd, &read_fds)) {
                // Drain whatever is available; never block on a partial frame.
                for (;;) {
                    char rbuf[65536];
                    const ssize_t n = ::read(c.fd, rbuf, sizeof(rbuf));
                    if (n > 0) { c.rx.append(rbuf, static_cast<size_t>(n)); continue; }
                    if (n < 0 && errno == EINTR) continue;
                    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                    drop = true;   // EOF (daemon died) or hard error
                    break;
                }
                if (c.rx.size() > kClientRxCap) drop = true;

                // Process complete frames even when the peer already closed:
                // a daemon that sends SHUTDOWN and closes immediately has
                // already delivered the bytes — dropping them would orphan
                // the worker (and its shell) forever.
                size_t off = 0;
                while (c.rx.size() - off >= 5) {
                    const auto* p = reinterpret_cast<const uint8_t*>(c.rx.data() + off);
                    const uint8_t type = p[0];
                    const uint32_t len = read_u32be(p + 1);
                    if (len > 16u * 1024u * 1024u) { drop = true; break; }  // desync
                    if (c.rx.size() - off < 5u + len) break;                // partial
                    const uint8_t* payload = p + 5;
                    switch (static_cast<WorkerMsgType>(type)) {
                        case WMSG_INPUT: {
                            if (len > 0) {
                                const size_t pending_bytes =
                                    pending_pty_input.size() - pending_pty_offset;
                                if (len > kPendingPtyInputCap -
                                              std::min(pending_bytes,
                                                       kPendingPtyInputCap)) {
                                    // Backpressure contract: disconnect a client
                                    // that exceeds the bounded input queue.
                                    drop = true;
                                    break;
                                }
                                if (pending_pty_offset > 0) {
                                    pending_pty_input.erase(
                                        pending_pty_input.begin(),
                                        pending_pty_input.begin() +
                                            static_cast<std::ptrdiff_t>(pending_pty_offset));
                                    pending_pty_offset = 0;
                                }
                                pending_pty_input.insert(pending_pty_input.end(),
                                                         payload, payload + len);
                                drain_pending_pty_input();
                            }
                            break;
                        }
                        case WMSG_RESIZE: {
                            if (len >= 4) {
                                uint16_t cols = read_u16be(payload);
                                uint16_t rows = read_u16be(payload + 2);
                                struct winsize ws{};
                                ws.ws_col = cols;
                                ws.ws_row = rows;
                                (void)::ioctl(master_fd, TIOCSWINSZ, &ws);
                            }
                            break;
                        }
                        case WMSG_DETACH: {
                            drop = true;   // close just this client
                            break;
                        }
                        case WMSG_PING: {
                            if (!queue_to_client(c, WMSG_PONG, nullptr, 0)) {
                                drop = true;
                                break;
                            }
                            (void)flush_client(c);
                            break;
                        }
                        case WMSG_SHUTDOWN: {
                            goto worker_shutdown;
                        }
                        default:
                            break;
                    }
                    off += 5u + len;
                }
                if (off > 0) c.rx.erase(0, off);
            }
            if (drop) {
                ::close(c.fd);
                it = clients.erase(it);
                continue;
            }
            ++it;
        }

        // Check child exit (non-blocking waitpid)
        if (child_pid > 0) {
            int status = 0;
            pid_t wres = ::waitpid(child_pid, &status, WNOHANG);
            if (wres == child_pid) {
                child_died = true;
                if (WIFEXITED(status)) {
                    result.exit_code = WEXITSTATUS(status);
                    result.signal_num = 0;
                } else if (WIFSIGNALED(status)) {
                    result.exit_code = 128 + WTERMSIG(status);
                    result.signal_num = WTERMSIG(status);
                }

                // Drain any final PTY output
                char buf[4096];
                while (true) {
                    ssize_t n = ::read(master_fd, buf, sizeof(buf));
                    if (n <= 0) break;
                    scrollback.write(std::string_view(buf, static_cast<size_t>(n)));
                    for (auto& c : clients)
                        queue_to_client(c, WMSG_OUTPUT, buf, static_cast<size_t>(n));
                }

                // Send DIED to all clients
                uint8_t died_payload[8];
                write_u32be(died_payload, static_cast<uint32_t>(result.exit_code));
                write_u32be(died_payload + 4, static_cast<uint32_t>(result.signal_num));
                for (auto& c : clients)
                    queue_to_client(c, WMSG_DIED, died_payload, 8);
                // Bounded best-effort flush so a live daemon sees the death
                // notice before we close; EOF on the socket is the fallback.
                for (int pass = 0; pass < 20; ++pass) {
                    bool all_empty = true;
                    for (auto& c : clients) {
                        if (c.fd < 0) continue;
                        if (!flush_client(c)) { ::close(c.fd); c.fd = -1; continue; }
                        if (c.tx_off < c.tx.size()) all_empty = false;
                    }
                    if (all_empty) break;
                    ::usleep(10000);
                }
            }
        }
    }

worker_shutdown:
    // Cleanup. Transfer ownership away from Session before local RAII runs so
    // no descriptor or child is closed twice.
    if (master_fd >= 0) ::close(master_fd);
    session.master_fd = -1;
    if (child_pid > 0) {
        // forkpty makes the shell a process-group leader. Kill the whole group
        // so background jobs cannot outlive the worker.
        ::kill(-child_pid, SIGTERM);
        int status = 0;
        bool reaped = false;
        for (int i = 0; i < 10; ++i) {
            const pid_t waited = ::waitpid(child_pid, &status, WNOHANG);
            if (waited == child_pid || (waited < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            ::usleep(50000);
        }
        if (!reaped) {
            ::kill(-child_pid, SIGKILL);
            while (::waitpid(child_pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    session.child_pid = -1;

    for (auto& c : clients) {
        if (c.fd >= 0) ::close(c.fd);
    }
    ::close(listen_fd);
    ::unlink(cfg.socket_path.c_str());
    ::unlink(pid_file.c_str());

    return result;
}

#endif  // POSIX

// ────────────────────────────────────────────────────────────────────
// 5. Controller-side: Worker management
// ────────────────────────────────────────────────────────────────────

// On the controller side, each managed worker session has:
// - A Unix domain socket connection to the worker process
// - The worker PID (for process management)
// - A scrollback buffer (populated from WMSG_OUTPUT)

struct ManagedWorker {
    std::string session_name;
    std::string socket_path;
    int fd = -1;           // connection to worker
    pid_t worker_pid = -1; // worker process PID
    pid_t child_pid = -1;  // shell PID inside worker
    bool alive = true;
    RingBuffer<1048576> scrollback;
    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_output_at;
    bool died = false;
    int32_t exit_code = 0;
    int32_t signal_num = 0;
};

// One-time probe: does `systemd-run --user --scope` work in this process
// environment? Cached for the process lifetime. A daemon started via cron,
// ssh non-login, or without a user bus gets a definitive no without paying a
// per-spawn timeout.
inline bool systemd_run_available() {
    static std::atomic<int> cached{-1};
    if (cached.load() >= 0) return cached.load() == 1;

    std::string systemd_run = ::access("/usr/bin/systemd-run", X_OK) == 0
        ? "/usr/bin/systemd-run"
        : (::access("/bin/systemd-run", X_OK) == 0 ? "/bin/systemd-run" : "");
    if (systemd_run.empty()) {
        cached.store(0);
        return false;
    }
    std::string unit = "bs-probe-" + std::to_string(::getpid());
    pid_t pid = ::fork();
    if (pid == 0) {
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
        {
            long max_fd = ::sysconf(_SC_OPEN_MAX);
            if (max_fd < 0) max_fd = 1024;
            for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
        }
        int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        std::vector<std::string> pargs = {systemd_run, "--user", "--scope",
                                          "--quiet", "--unit=" + unit, "/bin/true"};
        std::vector<char*> argv_arr;
        for (auto& a : pargs) argv_arr.push_back(const_cast<char*>(a.c_str()));
        argv_arr.push_back(nullptr);
        ::execv(systemd_run.c_str(), argv_arr.data());
        ::_exit(127);
    }
    if (pid < 0) { cached.store(0); return false; }
    int status = 0;
    bool ok = false;
    for (int i = 0; i < 300; ++i) {   // up to ~3s; normally <50ms
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) { ok = WIFEXITED(status) && WEXITSTATUS(status) == 0; break; }
        if (r < 0 && errno != EINTR) break;
        ::usleep(10000);
    }
    if (!ok) { ::kill(pid, SIGKILL); ::waitpid(pid, &status, 0); }
    cached.store(ok ? 1 : 0);
    return ok;
}

// Spawn a worker process for a new session.
// Returns the direct-fork worker PID (>0), 0 when spawned via systemd-run
// (real worker pid is in cfg.socket_path + ".pid"), or -1 on failure.
//
// Strategy for cgroup escape (so workers survive daemon service stop):
// 1. `systemd-run --user --scope` via a DOUBLE-fork — systemd-run stays alive
//    as the scope supervisor for the worker's whole lifetime, so it must not
//    be our direct child (it would zombie on exit). The grandchild is
//    reparented to PID 1 and reaped there.
// 2. Fall back to plain fork+exec+setsid (may die with daemon on systemd stop)
inline pid_t spawn_session_worker(const WorkerConfig& cfg, const std::string& exe_path) {
#ifndef _WIN32
    // Build the worker command line
    std::vector<std::string> args = {
        exe_path, "session-worker",
        "--socket", cfg.socket_path,
        "--name", cfg.session_name,
        "--command", cfg.command,
        "--cols", std::to_string(cfg.cols),
        "--rows", std::to_string(cfg.rows),
        "--term", cfg.term,
        "--app-home", cfg.app_home
    };

    if (systemd_run_available()) {
        std::string systemd_run = ::access("/usr/bin/systemd-run", X_OK) == 0
            ? "/usr/bin/systemd-run" : "/bin/systemd-run";

        static std::atomic<uint64_t> scope_seq{0};
        std::string unit = "bs-worker-" + cfg.session_name + "-" +
                           std::to_string(::getpid()) + "-" +
                           std::to_string(scope_seq.fetch_add(1));

        std::vector<std::string> full_args;
        full_args.push_back(systemd_run);
        full_args.push_back("--user");
        full_args.push_back("--scope");
        full_args.push_back("--quiet");
        full_args.push_back("--unit=" + unit);
        for (const auto& a : args) full_args.push_back(a);

        const pid_t pid = ::fork();
        if (pid == 0) {
            // Child: fork the systemd-run grandchild and exit immediately so
            // the grandchild (and its scope) is owned by PID 1, never by us.
            const pid_t g = ::fork();
            if (g != 0) ::_exit(g < 0 ? 1 : 0);
#if defined(__linux__) && defined(SYS_close_range)
            if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
            {
                long max_fd = ::sysconf(_SC_OPEN_MAX);
                if (max_fd < 0) max_fd = 1024;
                for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
            }
            int devnull = ::open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                ::dup2(devnull, STDIN_FILENO);
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                ::close(devnull);
            }
            ::signal(SIGHUP, SIG_IGN);
            ::setsid();

            std::vector<char*> argv_arr;
            for (auto& a : full_args) argv_arr.push_back(const_cast<char*>(a.c_str()));
            argv_arr.push_back(nullptr);

            ::execv(systemd_run.c_str(), argv_arr.data());
            ::_exit(127);
        }
        if (pid > 0) {
            // Reap the short-lived middle child (it exits as soon as the
            // grandchild is forked).
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            return 0;   // scope worker's real pid lands in the pid file
        }
        // fork failed → plain fork fallback
    }

    // Strategy 2: Plain fork+exec+setsid (fallback)
    pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid > 0) {
        // Parent — return child PID
        return pid;
    }

    // Child — exec the worker
    // Close ALL inherited fds from the daemon using close_range (Linux 5.9+)
    // or fallback loop. This prevents the worker from holding the daemon's
    // listening ports open.
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
    {
        long max_fd = ::sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 1024;
        for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
    }

    // Redirect stdin/stdout/stderr to /dev/null
    int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::dup2(devnull, STDOUT_FILENO);
        ::dup2(devnull, STDERR_FILENO);
        ::close(devnull);
    }

    // Become session leader so the worker survives parent death
    ::setsid();

    // Ignore SIGHUP (parent death signal)
    ::signal(SIGHUP, SIG_IGN);

    // Try to escape cgroup (best-effort)
    {
        FILE* f = std::fopen("/sys/fs/cgroup/cgroup.procs", "w");
        if (f) {
            std::fprintf(f, "%d\n", static_cast<int>(::getpid()));
            std::fclose(f);
        }
    }

    // Build argv and exec
    std::vector<char*> argv_arr;
    for (auto& a : args) argv_arr.push_back(const_cast<char*>(a.c_str()));
    argv_arr.push_back(nullptr);
    ::execv(exe_path.c_str(), argv_arr.data());

    // If exec failed
    ::_exit(127);
#else
    // Windows: CreateProcess for session-worker mode
    // TODO: full Windows worker implementation
    (void)cfg;
    (void)exe_path;
    return -1;
#endif
}

// Connect to an existing worker socket.
// Returns fd (>0) on success, -1 on failure.
inline int connect_to_worker(const std::string& socket_path, int timeout_ms = 3000) {
#ifndef _WIN32
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    // Set socket non-blocking for connect timeout
    int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    int rc = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }

    if (rc != 0) {
        // Wait for connect with timeout
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        int ready = ::select(fd + 1, nullptr, &wfds, nullptr, &tv);
        if (ready <= 0) {
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t errlen = sizeof(err);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
        if (err != 0) {
            ::close(fd);
            return -1;
        }
    }

    // Restore blocking mode
    ::fcntl(fd, F_SETFL, fl);
    return fd;
#else
    // Windows: connect to named pipe
    // TODO: full Windows worker implementation
    (void)socket_path;
    (void)timeout_ms;
    return -1;
#endif
}

// Ping a worker socket to check if it's alive.
// Returns true if alive, false otherwise.
inline bool ping_worker(const std::string& socket_path) {
    int fd = connect_to_worker(socket_path, 1000);
    if (fd < 0) return false;
    bool ok = worker_send(fd, WMSG_PING);
    if (!ok) {
        ::close(fd);
        return false;
    }
    // The worker announces READY (+ optional SCROLLBACK) on connect before it
    // answers anything — skip those frames and wait for the PONG. Reading only
    // one frame would misread READY as a dead worker, and the caller would
    // unlink a LIVE worker's socket.
    for (int i = 0; i < 32; ++i) {
        WorkerMessage msg;
        if (!worker_recv(fd, msg, 2000)) break;
        if (msg.type == WMSG_PONG) { ::close(fd); return true; }
        if (msg.type == WMSG_READY || msg.type == WMSG_SCROLLBACK ||
            msg.type == WMSG_OUTPUT) continue;
        break;
    }
    ::close(fd);
    return false;
}

// Discover existing worker sockets on controller startup.
// Returns list of {session_name, socket_path} for live workers.
struct DiscoveredWorker {
    std::string session_name;
    std::string socket_path;
    int fd;          // connected fd, or -1 if connection failed
    pid_t child_pid; // from WMSG_READY
};

inline std::vector<DiscoveredWorker> discover_workers(const std::string& app_home) {
    std::vector<DiscoveredWorker> result;
    std::string dir = worker_socket_dir(app_home);
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(dir, ec)) return result;

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::string name = entry.path().filename().string();
        // Look for *.sock files
        if (name.size() < 6 || name.substr(name.size() - 5) != ".sock") continue;

        std::string full_path = entry.path().string();
        std::string session_name = name.substr(0, name.size() - 5);

        int fd = connect_to_worker(full_path, 1000);
        if (fd < 0) {
            // Stale socket — clean up
            std::error_code rm_ec;
            fs::remove(entry.path(), rm_ec);
            fs::remove(entry.path().string() + ".pid", rm_ec);
            continue;
        }

        // Wait for READY message. The final four payload bytes are child pid
        // (u32be); older workers omit them and remain compatible.
        WorkerMessage msg;
        pid_t child_pid = -1;
        if (worker_recv(fd, msg, 2000) && msg.type == WMSG_READY) {
            if (msg.data.size() == session_name.size() + 4 &&
                std::equal(session_name.begin(), session_name.end(),
                           msg.data.begin())) {
                child_pid = static_cast<pid_t>(
                    read_u32be(msg.data.data() + msg.data.size() - 4));
            }
            DiscoveredWorker dw;
            dw.session_name = session_name;
            dw.socket_path = full_path;
            dw.fd = fd;
            dw.child_pid = child_pid;
            result.push_back(std::move(dw));
        } else {
            ::close(fd);
            std::error_code rm_ec;
            fs::remove(entry.path(), rm_ec);
        }
    }

    return result;
}

}  // namespace worker
