// bs-client: bridgesessions terminal relay agent
// Phase 12 — CLI11 argparse

#include <bstransport/tls.hpp>
#include <bstransport/frame_io.hpp>
#include <bsprotocol/codec.hpp>

#include "terminal_raw.hpp"
#include "host_config.hpp"
#include "image_render.hpp"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#ifndef _WIN32
#include <unistd.h>   // getpid() for banner
#endif
#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <afunix.h>
#include <io.h>
#pragma comment(lib, "ws2_32.lib")
using ssize_t = long long;
#undef min
#undef max
#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#else
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

using namespace bs::protocol;
using namespace bs::transport;
using namespace bs::client;
using namespace std::chrono_literals;

// Phase 9: keygen subcommand (defined in keygen.cpp)
extern int cmd_keygen();

// Clipboard bridge interface (platform-specific implementation in src/clipboard_bridge.mm or src/clipboard_linux.cpp)
#include "clipboard_bridge.hpp"

namespace {

#ifdef _WIN32
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_winch{false};

BOOL WINAPI ctrl_handler(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_LOGOFF_EVENT ||
        ctrl_type == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}
#define SOCKET_CLOSE closesocket
#define SOCKET_ERR SOCKET_ERROR
#else
volatile sig_atomic_t g_running = 1;
volatile sig_atomic_t g_winch = 0;

void sig_handler(int sig) {
    if (sig == SIGWINCH) g_winch = 1;
    else g_running = 0;
}
#define SOCKET_CLOSE close
#define SOCKET_ERR -1
#endif

constexpr const char* kClientVersion = "26.05.31";
constexpr const char* kClientBanner = "BridgeSpace Sessions Client v26.05.31";

// ── Exponential backoff with jitter ──────────────────────────
struct Backoff {
    std::chrono::milliseconds current = 100ms;
    std::chrono::milliseconds max = 5000ms;
    std::minstd_rand rng{std::random_device{}()};

    void wait() {
        if (!g_running) return;
        auto jitter = std::uniform_int_distribution<int>(
            -(int)(current.count() / 4), (int)(current.count() / 4))(rng);
        auto delay = std::chrono::milliseconds(current.count() + jitter);
        if (delay < 50ms) delay = 50ms;
        std::cerr << "reconnecting in " << (delay.count() / 1000.0) << "s..." << std::endl;
        std::this_thread::sleep_for(delay);
        current = std::min(current * 2, max);
    }

    void reset() { current = 100ms; }
};

// ── Signal character detection ────────────────────────────────
bool is_signal_char(const char* data, ssize_t n, SignalMsg& out) {
    if (n != 1) return false;
    switch (data[0]) {
        case 0x03: out.signal = SignalMsg::SignalType::CtrlC; return true;
        case 0x1A: out.signal = SignalMsg::SignalType::CtrlZ; return true;
        case 0x1C: out.signal = SignalMsg::SignalType::CtrlBackslash; return true;
    }
    return false;
}

[[nodiscard]] bool write_all(int fd, const char* data, size_t size) {
    const char* p = data;
    while (size > 0) {
        ssize_t n = ::write(fd, p, size);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

std::string default_known_servers_file() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOMEDRIVE") ? (std::string(std::getenv("HOMEDRIVE")) + std::getenv("HOMEPATH")).c_str() : nullptr;
#else
    const char* home = std::getenv("HOME");
#endif
    if (home && *home) return std::string(home) + "/.bridgesessions/known_servers";
    return "/tmp/bs-known-servers";
}

std::string default_stats_dir() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOMEDRIVE") ? (std::string(std::getenv("HOMEDRIVE")) + std::getenv("HOMEPATH")).c_str() : nullptr;
#else
    const char* home = std::getenv("HOME");
#endif
    if (home && *home) return std::string(home) + "/.bridgesessions/clients";
    return "/tmp/bs-clients";
}

std::string default_stats_file(const std::string& stats_dir) {
    return stats_dir + "/bs-client-" + std::to_string(::getpid()) + ".json";
}

std::string now_iso8601() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 3) { value /= 1024.0; ++unit; }
    std::ostringstream os;
    if (unit == 0) os << bytes << units[unit];
    else os << std::fixed << std::setprecision(1) << value << units[unit];
    return os.str();
}

std::string human_duration(uint64_t seconds) {
    std::ostringstream os;
    uint64_t hours = seconds / 3600;
    uint64_t minutes = (seconds % 3600) / 60;
    uint64_t secs = seconds % 60;
    if (hours) os << hours << "h" << minutes << "m";
    else if (minutes) os << minutes << "m" << secs << "s";
    else os << secs << "s";
    return os.str();
}

bool pid_running(int pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h) { DWORD code; GetExitCodeProcess(h, &code); CloseHandle(h); return code == STILL_ACTIVE; }
    return false;
#else
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
#endif
}

struct ClientStats {
    int pid = static_cast<int>(::getpid());
    std::string stats_file;
    std::string mode;
    std::string server;
    uint16_t port = 0;
    std::string session;
    std::string attach_name;
    std::string term;
    uint16_t cols = 0;
    uint16_t rows = 0;
    std::string started_at = now_iso8601();
    std::string last_event = "starting";
    bool connected = false;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last_stats_write = std::chrono::steady_clock::time_point::min();

    uint64_t connect_attempts = 0;
    uint64_t reconnects = 0;
    uint64_t bytes_local_to_server = 0;
    uint64_t bytes_server_to_local = 0;
    uint64_t frames_local_to_server = 0;
    uint64_t frames_server_to_local = 0;
    uint64_t signal_frames = 0;
    uint64_t resize_frames = 0;
    uint64_t clipboard_put = 0;
    uint64_t clipboard_get = 0;
    uint64_t clipboard_echo = 0;
    uint64_t ping_rx = 0;
    uint64_t pong_tx = 0;
    uint64_t scrollback_frames = 0;
    uint64_t output_frames = 0;
    uint64_t local_reads = 0;
    uint64_t key_control_bytes = 0;
    uint64_t key_escape_bytes = 0;
    uint64_t key_meta_sequences = 0;
    uint64_t key_high_bytes = 0;
    uint64_t key_printable_bytes = 0;
};

uint64_t uptime_seconds(const ClientStats& stats) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - stats.started).count());
}

void write_stats(ClientStats& stats, bool force = false) {
    if (stats.stats_file.empty()) return;
    auto now = std::chrono::steady_clock::now();
    if (!force && now - stats.last_stats_write < 1s) return;
    stats.last_stats_write = now;

    nlohmann::json j;
    j["pid"] = stats.pid;
    j["version"] = kClientVersion;
    j["banner"] = kClientBanner;
    j["mode"] = stats.mode;
    j["server"] = stats.server;
    j["port"] = stats.port;
    j["session"] = stats.session;
    j["attach_name"] = stats.attach_name;
    j["term"] = stats.term;
    j["cols"] = stats.cols;
    j["rows"] = stats.rows;
    j["started_at"] = stats.started_at;
    j["updated_at"] = now_iso8601();
    j["uptime_seconds"] = uptime_seconds(stats);
    j["connected"] = stats.connected;
    j["last_event"] = stats.last_event;
    j["connect_attempts"] = stats.connect_attempts;
    j["reconnects"] = stats.reconnects;
    j["bytes_local_to_server"] = stats.bytes_local_to_server;
    j["bytes_server_to_local"] = stats.bytes_server_to_local;
    j["frames_local_to_server"] = stats.frames_local_to_server;
    j["frames_server_to_local"] = stats.frames_server_to_local;
    j["signal_frames"] = stats.signal_frames;
    j["resize_frames"] = stats.resize_frames;
    j["clipboard_put"] = stats.clipboard_put;
    j["clipboard_get"] = stats.clipboard_get;
    j["clipboard_echo"] = stats.clipboard_echo;
    j["ping_rx"] = stats.ping_rx;
    j["pong_tx"] = stats.pong_tx;
    j["scrollback_frames"] = stats.scrollback_frames;
    j["output_frames"] = stats.output_frames;
    j["local_reads"] = stats.local_reads;
    j["key_control_bytes"] = stats.key_control_bytes;
    j["key_escape_bytes"] = stats.key_escape_bytes;
    j["key_meta_sequences"] = stats.key_meta_sequences;
    j["key_high_bytes"] = stats.key_high_bytes;
    j["key_printable_bytes"] = stats.key_printable_bytes;

    std::error_code ec;
    auto parent = std::filesystem::path(stats.stats_file).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::string tmp = stats.stats_file + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return;
        out << j.dump(2) << '\n';
        out.flush();
        if (!out) { ::unlink(tmp.c_str()); return; }
    }
    if (::rename(tmp.c_str(), stats.stats_file.c_str()) != 0) {
        ::unlink(tmp.c_str());
    }
}

void record_key_bytes(ClientStats* stats, const char* data, size_t size) {
    if (!stats || size == 0) return;
    stats->bytes_local_to_server += size;
    stats->frames_local_to_server++;
    stats->local_reads++;
    bool saw_escape = false;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == 0x1B) { stats->key_escape_bytes++; saw_escape = true; }
        else if (c < 0x20 || c == 0x7F) stats->key_control_bytes++;
        else if (c >= 0x80) stats->key_high_bytes++;
        else stats->key_printable_bytes++;
    }
    if (saw_escape && size > 1) stats->key_meta_sequences++;
}

std::string bytes_hex(std::string_view bytes) {
    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) os << ' ';
        os << std::setw(2) << static_cast<int>(static_cast<unsigned char>(bytes[i]));
    }
    return os.str();
}

std::string key_class(std::string_view bytes) {
    if (bytes.empty()) return "empty";
    if (bytes.size() == 1) {
        unsigned char c = static_cast<unsigned char>(bytes[0]);
        if (c == 0x03) return "ctrl-c";
        if (c == 0x04) return "ctrl-d";
        if (c == 0x1A) return "ctrl-z";
        if (c == 0x1C) return "ctrl-backslash";
        if (c == 0x1B) return "escape";
        if (c < 0x20 || c == 0x7F) return "control";
        if (c >= 0x80) return "high-bit/meta";
        return "printable";
    }
    if (static_cast<unsigned char>(bytes[0]) == 0x1B) return "escape/meta-sequence";
    return "byte-sequence";
}

void log_key_event(const std::string& key_log_file, const char* data, size_t size) {
    if (key_log_file.empty()) return;
    std::error_code ec;
    auto parent = std::filesystem::path(key_log_file).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    std::ofstream out(key_log_file, std::ios::app);
    if (!out) return;
    std::string_view bytes(data, size);
    out << now_iso8601() << " bytes=" << size
        << " class=" << key_class(bytes)
        << " hex=" << bytes_hex(bytes) << '\n';
}

void emit_start_screen(int output_fd, bool clean_start, bool show_banner) {
    std::string out;
    if (clean_start) {
        // Reset attributes, show cursor, clear screen, clear scrollback, home cursor.
        out += "\x1b[0m\x1b[?25h\x1b[H\x1b[2J\x1b[3J";
    }
    if (show_banner) {
        out += kClientBanner;
        out += "\r\n";
    }
    if (!out.empty()) (void)write_all(output_fd, out.data(), out.size());
}

int cmd_image_preview(const std::string& path, bool quiet) {
    try {
        auto msg = make_image_data_message(std::filesystem::path(path));
        if (!render_image_message(msg, STDOUT_FILENO, quiet)) return 1;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "image: " << e.what() << std::endl;
        return 1;
    }
}

int cmd_anim_preview(const std::string& path, bool quiet) {
    try {
        auto msg = make_image_frame_message(std::filesystem::path(path));
        if (!render_image_message(msg, STDOUT_FILENO, quiet)) return 1;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "anim: " << e.what() << std::endl;
        return 1;
    }
}

int cmd_stats(const std::string& stats_dir, bool json_output) {
    nlohmann::json rows = nlohmann::json::array();
    std::error_code ec;
    if (std::filesystem::exists(stats_dir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(stats_dir, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") continue;
            try {
                std::ifstream in(entry.path());
                auto j = nlohmann::json::parse(in);
                int pid = j.value("pid", 0);
                j["running"] = pid_running(pid);
                rows.push_back(std::move(j));
            } catch (...) {
                // Ignore partially-written or stale garbage files.
            }
        }
    }

    if (json_output) {
        std::cout << rows.dump(2) << '\n';
        return 0;
    }

    if (rows.empty()) {
        std::cout << "no bs-client stats found in " << stats_dir << '\n';
        return 0;
    }

    std::cout << std::left
              << std::setw(8) << "PID"
              << std::setw(5) << "RUN"
              << std::setw(10) << "MODE"
              << std::setw(18) << "SESSION"
              << std::setw(24) << "SERVER"
              << std::setw(9) << "UPTIME"
              << std::setw(10) << "IN"
              << std::setw(10) << "OUT"
              << std::setw(8) << "KEYS"
              << std::setw(8) << "META"
              << "LAST" << '\n';
    for (const auto& j : rows) {
        std::string server = j.value("server", "") + ":" + std::to_string(j.value("port", 0));
        uint64_t keys = j.value("frames_local_to_server", 0ULL);
        uint64_t meta = j.value("key_meta_sequences", 0ULL);
        std::cout << std::left
                  << std::setw(8) << j.value("pid", 0)
                  << std::setw(5) << (j.value("running", false) ? "yes" : "no")
                  << std::setw(10) << j.value("mode", "")
                  << std::setw(18) << j.value("session", "")
                  << std::setw(24) << server
                  << std::setw(9) << human_duration(j.value("uptime_seconds", 0ULL))
                  << std::setw(10) << human_bytes(j.value("bytes_local_to_server", 0ULL))
                  << std::setw(10) << human_bytes(j.value("bytes_server_to_local", 0ULL))
                  << std::setw(8) << keys
                  << std::setw(8) << meta
                  << j.value("last_event", "") << '\n';
    }
    return 0;
}

TofuCallback make_tofu_callback(std::string server_key, std::string path, bool quiet) {
    return [server_key = std::move(server_key), path = std::move(path), quiet](const std::string& fp) {
        // Canonicalize hex fingerprints to lowercase for comparison — the
        // runtime callback emits lowercase ("%02x") but external tools
        // (openssl x509 -fingerprint) write uppercase. Don't reject legitimate
        // matches just because of case.
        auto lower = [](std::string s) {
            for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        const std::string fp_lc = lower(fp);

        std::ifstream in(path);
        std::string host, cached;
        while (in >> host >> cached) {
            if (host == server_key) {
                if (lower(cached) == fp_lc) return true;
                std::cerr << "TOFU mismatch for " << server_key << "\n"
                          << "  expected: " << cached << "\n"
                          << "  received: " << fp << std::endl;
                return false;
            }
        }

        std::error_code ec;
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        std::ofstream out(path, std::ios::app);
        if (!out) {
            std::cerr << "TOFU: cannot write known server cache " << path << std::endl;
            return false;
        }
        out << server_key << ' ' << fp_lc << '\n';
        if (!out) return false;
        if (!quiet) std::cerr << "TOFU: trusted " << server_key << " " << fp << std::endl;
        return true;
    };
}

// ── TLS connect helper ────────────────────────────────────────
struct TlsConnection {
    SslCtxPtr ctx;
    SslPtr ssl;
#ifdef _WIN32
    SOCKET sock_fd = INVALID_SOCKET;
#else
    int sock_fd = -1;
#endif
    bool ok = false;
};

TlsConnection tls_connect(const std::string& server_addr, uint16_t server_port,
                          const std::string& cert_file, const std::string& key_file,
                          bool quiet)
{
    TlsConnection c;
    std::string port_str = std::to_string(server_port);
    struct addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* ai = nullptr;
    int gai_err = getaddrinfo(server_addr.c_str(), port_str.c_str(), &hints, &ai);
    if (gai_err != 0 || !ai) {
        if (!quiet) std::cerr << "resolve " << server_addr << ": " << gai_strerror(gai_err) << std::endl;
        return c;
    }
    auto sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock == SOCKET_ERR) { freeaddrinfo(ai); return c; }
    c.sock_fd = sock;
    if (::connect(c.sock_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
#ifdef _WIN32
        int sock_err = WSAGetLastError();
        if (!quiet) std::cerr << "connect: winsock error " << sock_err << std::endl;
#else
        if (!quiet) std::cerr << "connect: " << strerror(errno) << std::endl;
#endif
        SOCKET_CLOSE(c.sock_fd); c.sock_fd = SOCKET_ERR; freeaddrinfo(ai); return c;
    }
    freeaddrinfo(ai);

    ClientConfig cfg; cfg.cert_file = cert_file.c_str(); cfg.key_file = key_file.c_str();
    cfg.known_servers_file = default_known_servers_file();
    std::string server_key = server_addr + ":" + std::to_string(server_port);
    try {
        c.ctx = create_client_context(cfg, make_tofu_callback(server_key, cfg.known_servers_file, quiet));
        c.ssl = SslPtr(SSL_new(c.ctx.get()));
        SSL_set_fd(c.ssl.get(), static_cast<int>(c.sock_fd));
        if (SSL_connect(c.ssl.get()) <= 0) {
            if (!quiet) std::cerr << "SSL_connect failed" << std::endl;
            SOCKET_CLOSE(c.sock_fd); c.sock_fd = SOCKET_ERR; return c;
        }
    } catch (...) { SOCKET_CLOSE(c.sock_fd); c.sock_fd = SOCKET_ERR; return c; }
    c.ok = true;
    return c;
}

// ── Core relay loop (shared by stdin/stdout and bridge modes) ──
// Terminal mode uses separate fds (stdin -> server, server -> stdout).
// Bridge mode passes the same bidirectional Unix socket for both.
#ifdef _WIN32
// Windows: two-thread relay — stdin (console) is not a socket, can't poll() with WSAPoll.
// Thread 1: network → stdout (SSL_read → WriteFile)
// Thread 2: stdin → network (_read → SSL_write with mutex)
bool run_relay(TlsConnection& conn, int input_fd, int output_fd,
               const std::string& attach_name, uint16_t cols, uint16_t rows,
               const std::string& term_env, bool quiet, bool passthrough_input,
               ClientStats* stats, bool clean_start, bool show_banner,
               const std::string& key_log_file)
{
    if (stats) {
        stats->connected = true;
        stats->last_event = "attaching";
        write_stats(*stats, true);
    }
    emit_start_screen(output_fd, clean_start, show_banner);

    // Attach
    AttachMsg at;
    at.session_name = attach_name; at.cols = cols; at.rows = rows; at.term = term_env;
    write_frame(conn.ssl.get(), at);
    if (!quiet) std::cerr << "attached to '" << attach_name << "'" << std::endl;
    if (stats) { stats->last_event = "attached"; write_stats(*stats, true); }

    // Clipboard bridge
    std::string last_clipboard_hash;
    ClipboardBridge clipboard([&](std::string text, std::string hash) {
        if (passthrough_input || hash == last_clipboard_hash) return;
        ClipboardMsg c; c.text = std::move(text); c.hash = hash;
        last_clipboard_hash = hash;
        if (stats) { stats->clipboard_put++; stats->last_event = "clipboard put"; }
        try { write_frame(conn.ssl.get(), c); } catch(...) {}
    });

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    std::mutex ssl_write_mutex;
    std::atomic<bool> network_done{false};
    std::atomic<bool> input_done{false};

    // Thread 1: Network → stdout (poll socket before blocking read)
    std::jthread network_thread([&] {
        try {
            while (g_running) {
                WSAPOLLFD pfd;
                pfd.fd = conn.sock_fd;
                pfd.events = POLLIN;
                pfd.revents = 0;
                int ret = WSAPoll(&pfd, 1, 100);
                if (ret < 0) break;
                if (ret == 0) continue;
                if (!(pfd.revents & POLLIN)) continue;

                auto msg = read_frame(conn.ssl.get());
                if (auto* out = std::get_if<OutputMsg>(&msg)) {
                    if (stats) {
                        stats->frames_server_to_local++;
                        stats->output_frames++;
                        stats->bytes_server_to_local += out->data.size();
                        stats->last_event = "output";
                    }
                    if (!write_all(output_fd, out->data.data(), out->data.size())) break;
                    if (stats) write_stats(*stats);
                } else if (std::get_if<ExitCodeMsg>(&msg)) {
                    if (stats) { stats->last_event = "remote exited"; write_stats(*stats, true); }
                    break;
                } else if (std::get_if<SessionDiedMsg>(&msg)) {
                    if (stats) { stats->last_event = "session died"; write_stats(*stats, true); }
                    break;
                } else if (std::get_if<PingMsg>(&msg)) {
                    if (stats) { stats->ping_rx++; stats->pong_tx++; stats->last_event = "ping/pong"; }
                    { std::lock_guard lk(ssl_write_mutex);
                      try { write_frame(conn.ssl.get(), PongMsg{}); } catch(...) { break; } }
                    if (stats) write_stats(*stats);
                } else if (auto* sb = std::get_if<ScrollbackMsg>(&msg)) {
                    if (stats) {
                        stats->frames_server_to_local++;
                        stats->scrollback_frames++;
                        stats->bytes_server_to_local += sb->data.size();
                        stats->last_event = "scrollback";
                    }
                    if (!write_all(output_fd, sb->data.data(), sb->data.size())) break;
                    { std::lock_guard lk(ssl_write_mutex);
                      write_frame(conn.ssl.get(), ScrollbackAckMsg{}); }
                    if (stats) write_stats(*stats);
                } else if (auto* img = std::get_if<ImageDataMsg>(&msg)) {
                    if (stats) {
                        stats->frames_server_to_local++;
                        stats->bytes_server_to_local += img->data.size();
                        stats->last_event = "image";
                    }
                    if (!render_image_message(*img, output_fd, quiet)) {
                        std::string note = "[image rendering unavailable: " + img->name + "]\r\n";
                        (void)write_all(output_fd, note.data(), note.size());
                    }
                    { std::lock_guard lk(ssl_write_mutex);
                      try { write_frame(conn.ssl.get(), ImageAckMsg{}); } catch(...) { break; } }
                    if (stats) write_stats(*stats);
                } else if (auto* anim = std::get_if<ImageFrameMsg>(&msg)) {
                    if (stats) {
                        stats->frames_server_to_local++;
                        stats->bytes_server_to_local += anim->data.size();
                        stats->last_event = "animation";
                    }
                    if (!render_image_message(*anim, output_fd, quiet)) {
                        std::string note = "[animation rendering unavailable]\r\n";
                        (void)write_all(output_fd, note.data(), note.size());
                    }
                    { std::lock_guard lk(ssl_write_mutex);
                      try { write_frame(conn.ssl.get(), ImageAckMsg{}); } catch(...) { break; } }
                    if (stats) write_stats(*stats);
                } else if (std::get_if<ImageAckMsg>(&msg)) {
                    if (stats) { stats->last_event = "image ack"; write_stats(*stats); }
                } else if (auto* cg = std::get_if<ClipboardMsg>(&msg)) {
                    if (!passthrough_input) {
                        clipboard.write_to_clipboard(cg->text);
                        last_clipboard_hash = cg->hash;
                        if (stats) { stats->clipboard_get++; stats->last_event = "clipboard get"; write_stats(*stats); }
                    }
                } else if (auto* ce = std::get_if<ClipboardEchoMsg>(&msg)) {
                    if (!passthrough_input) {
                        last_clipboard_hash = ce->hash;
                        if (stats) { stats->clipboard_echo++; stats->last_event = "clipboard echo"; write_stats(*stats); }
                    }
                }
            }
        } catch (const std::exception&) {
            if (!quiet) std::cerr << "server disconnected" << std::endl;
            if (stats) { stats->last_event = "server disconnected"; write_stats(*stats, true); }
        }
        network_done = true;
        g_running = false;
    });

    // Thread 2: stdin → network
    std::jthread input_thread([&] {
        char buf[65536];
        HANDLE hIn = (input_fd == STDIN_FILENO) ? GetStdHandle(STD_INPUT_HANDLE)
                     : reinterpret_cast<HANDLE>(_get_osfhandle(input_fd));
        while (g_running && !input_done) {
            DWORD n = 0;
            if (!ReadFile(hIn, buf, sizeof(buf), &n, nullptr) || n == 0) {
                input_done = true;  // EOF — signal but don't kill relay yet
                break;
            }
            record_key_bytes(stats, buf, static_cast<size_t>(n));
            log_key_event(key_log_file, buf, static_cast<size_t>(n));

            if (!passthrough_input && n == 1 && buf[0] == 0x04) {
                { std::lock_guard lk(ssl_write_mutex);
                  try { write_frame(conn.ssl.get(), DetachMsg{}); } catch(...) {} }
                if (stats) { stats->last_event = "detach"; write_stats(*stats, true); }
                break;
            }
            if (!passthrough_input) {
                SignalMsg sig;
                if (is_signal_char(buf, static_cast<ssize_t>(n), sig)) {
                    if (stats) { stats->signal_frames++; stats->last_event = key_class(std::string_view(buf, n)); }
                    { std::lock_guard lk(ssl_write_mutex);
                      try { write_frame(conn.ssl.get(), sig); } catch(...) { break; } }
                    if (stats) write_stats(*stats);
                    continue;
                }
            }
            KeystrokeMsg ks; ks.data.assign(buf, n);
            if (stats) stats->last_event = passthrough_input ? "pipe key bytes" : "key bytes";
            { std::lock_guard lk(ssl_write_mutex);
              try { write_frame(conn.ssl.get(), ks); } catch(...) { break; } }
            if (stats) write_stats(*stats);
        }
        input_done = true;
    });

    // Main thread: clipboard poll + keepalive. Wait for network to finish or timeout.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (g_running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!passthrough_input) clipboard.poll();
        // If input closed and network hasn't received anything in 2s, give up
        if (input_done && !network_done) {
            auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!network_done && std::chrono::steady_clock::now() < wait_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            break;
        }
    }

    g_running = false;
    network_thread.join();
    input_thread.join();

    SSL_shutdown(conn.ssl.get());
    if (stats) { stats->connected = false; stats->last_event = "relay ended"; write_stats(*stats, true); }
    return true;
}
#else
bool run_relay(TlsConnection& conn, int input_fd, int output_fd,
               const std::string& attach_name, uint16_t cols, uint16_t rows,
               const std::string& term_env, bool quiet, bool passthrough_input,
               ClientStats* stats, bool clean_start, bool show_banner,
               const std::string& key_log_file)
{
    if (stats) {
        stats->connected = true;
        stats->last_event = "attaching";
        write_stats(*stats, true);
    }
    emit_start_screen(output_fd, clean_start, show_banner);

    // Attach
    AttachMsg at;
    at.session_name = attach_name; at.cols = cols; at.rows = rows; at.term = term_env;
    write_frame(conn.ssl.get(), at);
    if (!quiet) std::cerr << "attached to '" << attach_name << "'" << std::endl;
    if (stats) { stats->last_event = "attached"; write_stats(*stats, true); }

    // Clipboard bridge (skip in bridge/pipe mode — BridgeSpace or the pipe owner handles clipboard)
    std::string last_clipboard_hash;
    ClipboardBridge clipboard([&](std::string text, std::string hash) {
        if (passthrough_input || hash == last_clipboard_hash) return;
        ClipboardMsg c; c.text = std::move(text); c.hash = hash;
        last_clipboard_hash = hash;
        if (stats) { stats->clipboard_put++; stats->last_event = "clipboard put"; }
        try { write_frame(conn.ssl.get(), c); } catch(...) {}
    });

    signal(SIGWINCH, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    struct pollfd fds[2];
    fds[0].fd = input_fd; fds[0].events = POLLIN;
    fds[1].fd = conn.sock_fd; fds[1].events = POLLIN;
    char buf[65536];

    while (g_running) {
        if (g_winch && !passthrough_input) {
            g_winch = 0;
            auto ws = get_winsize();
            if (ws && ws->first > 0 && ws->second > 0) {
                ResizeMsg rs; rs.cols = ws->first; rs.rows = ws->second;
                if (stats) { stats->resize_frames++; stats->last_event = "resize"; }
                try { write_frame(conn.ssl.get(), rs); } catch(...) { break; }
                if (stats) write_stats(*stats);
            }
        }

        int ret = ::poll(fds, 2, 100);
        if (ret < 0) { if (errno == EINTR) continue; break; }

        if (!passthrough_input) clipboard.poll();

        // Local I/O (stdin or bridge socket)
        if (fds[0].revents & POLLIN) {
            ssize_t n = ::read(input_fd, buf, sizeof(buf));
            if (n <= 0) break;
            record_key_bytes(stats, buf, static_cast<size_t>(n));
            log_key_event(key_log_file, buf, static_cast<size_t>(n));
            if (!passthrough_input && n == 1 && buf[0] == 0x04) {
                try { write_frame(conn.ssl.get(), DetachMsg{}); } catch(...) {}
                if (stats) { stats->last_event = "detach"; write_stats(*stats, true); }
                return false;
            }
            // In bridge/pipe mode, pass control chars as keystrokes; the embedding terminal owns shortcuts.
            if (!passthrough_input) {
                SignalMsg sig;
                if (is_signal_char(buf, n, sig)) {
                    if (stats) { stats->signal_frames++; stats->last_event = key_class(std::string_view(buf, static_cast<size_t>(n))); }
                    try { write_frame(conn.ssl.get(), sig); } catch(...) { break; }
                    if (stats) write_stats(*stats);
                    continue;
                }
            }
            KeystrokeMsg ks; ks.data.assign(buf, n);
            if (stats) stats->last_event = passthrough_input ? "pipe key bytes" : "key bytes";
            try { write_frame(conn.ssl.get(), ks); } catch(...) { break; }
            if (stats) write_stats(*stats);
        }

        // Server → local
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            try {
                auto msg = read_frame(conn.ssl.get());
                if (auto* out = std::get_if<OutputMsg>(&msg)) {
                    if (stats) {
                        stats->frames_server_to_local++;
                        stats->output_frames++;
                        stats->bytes_server_to_local += out->data.size();
                        stats->last_event = "output";
                    }
                    if (!write_all(output_fd, out->data.data(), out->data.size())) break;
                    if (stats) write_stats(*stats);
                } else if (std::get_if<ExitCodeMsg>(&msg)) {
                    if (stats) { stats->last_event = "remote exited"; write_stats(*stats, true); }
                    return false;
                } else if (std::get_if<SessionDiedMsg>(&msg)) {
                    if (stats) { stats->last_event = "session died"; write_stats(*stats, true); }
                    return false;
                } else if (std::get_if<PingMsg>(&msg)) {
                    if (stats) { stats->ping_rx++; stats->pong_tx++; stats->last_event = "ping/pong"; }
                    try { write_frame(conn.ssl.get(), PongMsg{}); } catch(...) { break; }
                    if (stats) write_stats(*stats);
                } else if (auto* sb = std::get_if<ScrollbackMsg>(&msg)) {
                    if (stats) {
                        stats->frames_server_to_local++;
                        stats->scrollback_frames++;
                        stats->bytes_server_to_local += sb->data.size();
                        stats->last_event = "scrollback";
                    }
                    if (!write_all(output_fd, sb->data.data(), sb->data.size())) break;
                    write_frame(conn.ssl.get(), ScrollbackAckMsg{});
                    if (stats) write_stats(*stats);
                } else if (auto* img = std::get_if<ImageDataMsg>(&msg)) {
                    if (stats) {
                        stats->frames_server_to_local++;
                        stats->bytes_server_to_local += img->data.size();
                        stats->last_event = "image";
                    }
                    if (!render_image_message(*img, output_fd, quiet)) {
                        std::string note = "[image rendering unavailable: " + img->name + "]\r\n";
                        (void)write_all(output_fd, note.data(), note.size());
                    }
                    try { write_frame(conn.ssl.get(), ImageAckMsg{}); } catch(...) { break; }
                    if (stats) write_stats(*stats);
                } else if (auto* anim = std::get_if<ImageFrameMsg>(&msg)) {
                    if (stats) {
                        stats->frames_server_to_local++;
                        stats->bytes_server_to_local += anim->data.size();
                        stats->last_event = "animation";
                    }
                    if (!render_image_message(*anim, output_fd, quiet)) {
                        std::string note = "[animation rendering unavailable]\r\n";
                        (void)write_all(output_fd, note.data(), note.size());
                    }
                    try { write_frame(conn.ssl.get(), ImageAckMsg{}); } catch(...) { break; }
                    if (stats) write_stats(*stats);
                } else if (std::get_if<ImageAckMsg>(&msg)) {
                    if (stats) { stats->last_event = "image ack"; write_stats(*stats); }
                } else if (auto* cg = std::get_if<ClipboardMsg>(&msg)) {
                    if (!passthrough_input) {
                        clipboard.write_to_clipboard(cg->text);
                        last_clipboard_hash = cg->hash;
                        if (stats) { stats->clipboard_get++; stats->last_event = "clipboard get"; write_stats(*stats); }
                    }
                } else if (auto* ce = std::get_if<ClipboardEchoMsg>(&msg)) {
                    if (!passthrough_input) {
                        last_clipboard_hash = ce->hash;
                        if (stats) { stats->clipboard_echo++; stats->last_event = "clipboard echo"; write_stats(*stats); }
                    }
                }
            } catch (...) {
                if (!quiet) std::cerr << "server disconnected" << std::endl;
                if (stats) { stats->last_event = "server disconnected"; write_stats(*stats, true); }
                break;
            }
        }
    }

    SSL_shutdown(conn.ssl.get());
    SOCKET_CLOSE(conn.sock_fd);
    if (stats) { stats->connected = false; stats->last_event = "relay ended"; write_stats(*stats, true); }
    return true;
}
#endif // _WIN32

// ── connect_and_relay ─────────────────────────────────────────
bool connect_and_relay(const std::string& server_addr, uint16_t server_port,
                       const std::string& session_name, const std::string& command_override,
                       const std::string& cert_file, const std::string& key_file,
                       uint16_t cols, uint16_t rows, const std::string& term_env,
                       bool quiet, bool passthrough_input, ClientStats* stats,
                       bool clean_start, bool show_banner, const std::string& key_log_file)
{
    std::string attach_name = session_name;
    if (!command_override.empty()) attach_name += ":" + command_override;
    if (stats) {
        stats->connect_attempts++;
        stats->connected = false;
        stats->last_event = "connecting";
        write_stats(*stats, true);
    }
    auto conn = tls_connect(server_addr, server_port, cert_file, key_file, quiet);
    if (!conn.ok) {
        if (stats) { stats->last_event = "connect failed"; write_stats(*stats, true); }
        return true;
    }
    if (!quiet) std::cerr << "connected to " << server_addr << ":" << server_port
                          << " (TLS handshake complete)" << std::endl;
    if (stats) { stats->last_event = "connected"; write_stats(*stats, true); }
    bool reconnect = run_relay(conn, STDIN_FILENO, STDOUT_FILENO,
                               attach_name, cols, rows, term_env, quiet, passthrough_input,
                               stats, clean_start, show_banner, key_log_file);
    return command_override.empty() ? reconnect : false;
}

// ── bridge_relay — Unix socket mode (Phase 11) ────────────────
bool bridge_relay(const std::string& bridge_path,
                  const std::string& server_addr, uint16_t server_port,
                  const std::string& session_name, const std::string& command_override,
                  const std::string& cert_file, const std::string& key_file,
                  uint16_t cols, uint16_t rows, const std::string& term_env,
                  bool quiet, ClientStats* stats, bool clean_start, bool show_banner,
                  const std::string& key_log_file)
{
    std::string attach_name = session_name;
    if (!command_override.empty()) attach_name += ":" + command_override;

    // Connect to BridgeSpace via Unix socket
    int bridge_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (bridge_fd < 0) {
        if (!quiet) std::cerr << "bridge socket: " << strerror(errno) << std::endl;
        return false;
    }
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, bridge_path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(bridge_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (!quiet) std::cerr << "bridge connect " << bridge_path << ": " << strerror(errno) << std::endl;
        SOCKET_CLOSE(bridge_fd);
        return false;
    }

    if (stats) {
        stats->connect_attempts++;
        stats->last_event = "connecting bridge";
        write_stats(*stats, true);
    }
    auto conn = tls_connect(server_addr, server_port, cert_file, key_file, quiet);
    if (!conn.ok) {
        if (stats) { stats->last_event = "connect failed"; write_stats(*stats, true); }
        SOCKET_CLOSE(bridge_fd);
        return true;
    }
    if (!quiet) std::cerr << "bridged to " << server_addr << ":" << server_port
                          << " via " << bridge_path << std::endl;
    if (stats) { stats->last_event = "connected"; write_stats(*stats, true); }

    bool result = run_relay(conn, bridge_fd, bridge_fd, attach_name, cols, rows, term_env, quiet, true,
                            stats, clean_start, show_banner, key_log_file);
    SOCKET_CLOSE(bridge_fd);
    return result;
}

} // anonymous namespace

// ── main ─────────────────────────────────────────────────────
int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return 1;
    }
#endif
    CLI::App app{"bs-client — bridgesessions terminal relay agent"};
#ifndef __clang_analyzer__
    app.set_version_flag("--version,-V", kClientVersion);
#endif

    std::string server_addr = "127.0.0.1:9948";
    uint16_t server_port = 9948;
    std::string host_target;
    std::string hosts_file = default_hosts_file();
    std::string session_name = "default";
    std::string command_override;
    std::string cert_file, key_file;
    std::string term_env;
    std::string bridge_path;  // Phase 11: Unix socket path for BridgeSpace
    std::string stats_dir = default_stats_dir();
    std::string stats_file;
    std::string key_log_file;
    bool quiet = false;
    bool force_pipe = false;
    bool no_clean_start = false;
    bool no_banner = false;
    bool stats_json = false;

    app.add_option("target", host_target, "Host alias or server[:port] (ssh-style: bs-client test-pc1)")->expected(0, 1);
    app.add_option("-s,--server", server_addr, "Server host:port")->default_val("127.0.0.1:9948");
    app.add_option("--hosts", hosts_file, "Host alias file")->default_val(default_hosts_file());
    app.add_option("-n,--name", session_name, "Session name")->default_val("default");
    app.add_option("-x,--cmd", command_override, "Command override (name:cmd shorthand still works)");
    app.add_option("-c,--cert", cert_file, "TLS certificate file (PEM)");
    app.add_option("-k,--key", key_file, "TLS private key file (PEM)");
    app.add_option("-t,--term", term_env, "TERM environment override");
    app.add_flag("-q,--quiet", quiet, "Suppress stderr status messages");
    app.add_option("-b,--bridge", bridge_path, "BridgeSpace Unix socket path (enables bridge mode)");
    app.add_flag("--pipe", force_pipe, "Force stdio pipe mode: no raw TTY, pass control/meta bytes verbatim");
    app.add_flag("--no-clean-start", no_clean_start, "Do not clear/reset the terminal at attach start");
    app.add_flag("--no-banner", no_banner, "Do not emit the BridgeSpace Sessions Client startup banner");
    app.add_option("--stats-dir", stats_dir, "Directory for live client stats JSON files")->default_val(default_stats_dir());
    app.add_option("--stats-file", stats_file, "Exact live stats JSON path for this client");
    app.add_option("--key-log", key_log_file, "Diagnostics: append received key bytes as hex/class labels");

    // Helpers: each needs server:port parsed. Run this after parse().
    auto parse_server_port = [&]() {
        auto colon = server_addr.find(':');
        if (colon != std::string::npos) {
            server_port = (uint16_t)std::stoul(server_addr.substr(colon+1));
            server_addr = server_addr.substr(0, colon);
        }
    };

    // Subcommand: keygen
    int keygen_result = 0;
    auto* kg_cmd = app.add_subcommand("keygen", "Generate ed25519 keypair");
    kg_cmd->callback([&]() { keygen_result = cmd_keygen(); });

    // Subcommand: health — TLS Ping/Pong health check
    bool health_ok = false;
    auto* hc_cmd = app.add_subcommand("health", "Health check (TLS connect, Ping/Pong, exit 0)");

    // Subcommand: image — render a static PNG/JPEG file through chafa
    std::string image_path;
    auto* image_cmd = app.add_subcommand("image", "Preview a static image in terminal");
    image_cmd->add_option("path", image_path, "PNG/JPEG file path")->required();

    // Subcommand: anim — render an animated GIF through chafa
    std::string anim_path;
    auto* anim_cmd = app.add_subcommand("anim", "Preview an animated GIF in terminal");
    anim_cmd->add_option("path", anim_path, "GIF file path")->required();

    // Subcommand: stats — show live client-side stats JSON written by running clients
    auto* stats_cmd = app.add_subcommand("stats", "Show live bs-client stats from ~/.bridgesessions/clients");
    stats_cmd->add_flag("--json", stats_json, "Emit raw JSON array");

    // Subcommand: host — ssh-like named server aliases
    int host_result = 0;
    auto* host_cmd = app.add_subcommand("host", "Manage named hosts (~/.bridgesessions/hosts)");
    host_cmd->require_subcommand(1);

    auto* host_list_cmd = host_cmd->add_subcommand("list", "List configured hosts");
    host_list_cmd->callback([&]() {
        auto hosts = load_hosts_file(hosts_file);
        for (const auto& h : hosts) {
            std::cout << h.name << '\t' << h.server;
            if (!h.key_file.empty()) std::cout << "\tkey=" << h.key_file;
            if (!h.cert_file.empty()) std::cout << "\tcert=" << h.cert_file;
            if (!h.session_name.empty()) std::cout << "\tsession=" << h.session_name;
            std::cout << '\n';
        }
    });

    std::string add_host_name, add_host_server, add_host_key, add_host_cert, add_host_session;
    auto* host_add_cmd = host_cmd->add_subcommand("add", "Add or update a host alias");
    host_add_cmd->add_option("name", add_host_name, "Alias name")->required();
    host_add_cmd->add_option("server", add_host_server, "Server host[:port]")->required();
    host_add_cmd->add_option("--key", add_host_key, "Default client private key for this host");
    host_add_cmd->add_option("--cert", add_host_cert, "Default client cert for this host");
    host_add_cmd->add_option("--session", add_host_session, "Default session name for this host");
    host_add_cmd->callback([&]() {
        HostEntry h{.name=add_host_name, .server=add_host_server, .key_file=add_host_key,
                    .cert_file=add_host_cert, .session_name=add_host_session};
        if (!upsert_host(hosts_file, h)) {
            std::cerr << "host add: failed to write " << hosts_file << std::endl;
            host_result = 1;
            return;
        }
        std::cout << "host " << add_host_name << " -> " << add_host_server << std::endl;
    });

    std::string remove_host_name;
    auto* host_remove_cmd = host_cmd->add_subcommand("remove", "Remove a host alias");
    host_remove_cmd->add_option("name", remove_host_name, "Alias name")->required();
    host_remove_cmd->callback([&]() {
        if (!remove_host(hosts_file, remove_host_name)) {
            std::cerr << "host remove: failed to write " << hosts_file << std::endl;
            host_result = 1;
            return;
        }
        std::cout << "removed host " << remove_host_name << std::endl;
    });

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }

    parse_server_port();  // split host:port from --server/alias/target — must run before any banner/output

    if (app.got_subcommand(kg_cmd)) return keygen_result;
    if (app.got_subcommand(image_cmd)) return cmd_image_preview(image_path, quiet);
    if (app.got_subcommand(anim_cmd)) return cmd_anim_preview(anim_path, quiet);
    if (app.got_subcommand(host_cmd)) return host_result;
    if (app.got_subcommand(stats_cmd)) return cmd_stats(stats_dir, stats_json);

    if (!quiet) {
        std::cerr
            << kClientBanner << "\n"
            << "  server    : " << server_addr << ":" << server_port << "\n"
            << "  cert      : " << cert_file << "\n"
            << "  key       : " << key_file << "\n"
            << "  session   : " << session_name
            << (command_override.empty() ? "" : "  (cmd: " + command_override + ")")
            << "\n"
            << "  pid       : "
#ifdef _WIN32
            << ::GetCurrentProcessId()
#else
            << ::getpid()
#endif
            << "\n"
            << std::flush;
    }

    {
        ConnectionOptions opts;
        opts.server = server_addr;
        opts.key_file = key_file;
        opts.cert_file = cert_file;
        opts.session_name = session_name;
        auto hosts = load_hosts_file(hosts_file);
        if (auto err = apply_target_and_hosts(host_target, hosts, opts)) {
            std::cerr << *err << std::endl;
            return 2;
        }
        server_addr = opts.server;
        key_file = opts.key_file;
        cert_file = opts.cert_file;
        session_name = opts.session_name;
    }

    parse_server_port();  // split host:port from --server/alias/target  (re-call is harmless)

    if (app.got_subcommand(hc_cmd)) {
        auto conn = tls_connect(server_addr, server_port, cert_file, key_file, true);
        if (!conn.ok) { std::cerr << "health: connect failed" << std::endl; return 1; }
        try {
            write_frame(conn.ssl.get(), PingMsg{});
            for (;;) {
                auto reply = read_frame(conn.ssl.get());
                if (std::get_if<PongMsg>(&reply)) { health_ok = true; break; }
                if (std::get_if<PingMsg>(&reply)) {
                    write_frame(conn.ssl.get(), PongMsg{});
                    continue;
                }
                std::cerr << "health: expected Pong" << std::endl;
                break;
            }
        } catch (const std::exception& e) {
            std::cerr << "health: " << e.what() << std::endl;
        }
        SSL_shutdown(conn.ssl.get());
        SOCKET_CLOSE(conn.sock_fd);
        return health_ok ? 0 : 1;
    }

    bool pipe_mode = false;
    if (bridge_path.empty()) {
        pipe_mode = force_pipe || !::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO);
    }
    bool terminal_mode = bridge_path.empty() && !pipe_mode;

    // Auto-generate cert if not provided
    if (cert_file.empty() || key_file.empty()) {
        auto [c,k] = generate_cert_key_pair("bs-client");
        const char* home = std::getenv("HOME");
        std::string dir = home ? std::string(home) + "/.bridgesessions" : "/tmp";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        auto write_pem = [&](const std::string& name, const std::string& content) {
            std::string path = dir + "/" + name;
            std::ofstream out(path, std::ios::trunc);
            if (!out) throw std::runtime_error("cannot write " + path);
            out << content; out.close();
            std::filesystem::permissions(path,
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace, ec);
            return path;
        };
        cert_file = write_pem("_bs_autocert.pem", c);
        key_file  = write_pem("_bs_autokey.pem", k);
    }

    // Terminal raw mode (skip in bridge/pipe mode)
#ifdef _WIN32
    SavedConsole saved_term{};
#else
    struct termios saved_term{};
#endif
    bool raw_enabled = false;
    if (terminal_mode) {
        auto saved = enable_raw_mode();
        if (!saved) { std::cerr << "raw mode: " << saved.error().message << std::endl; return 1; }
        saved_term = *saved;
        raw_enabled = true;
    }

    uint16_t cols = 80, rows = 24;
    if (!bridge_path.empty()) {
        cols = 120; rows = 40;  // Bridge mode: fixed default
    } else if (pipe_mode) {
        cols = 120; rows = 40;  // Pipe mode: no TTY ioctl available in common BridgeSpace pipe setups
    } else {
        auto ws = get_winsize();
        if (ws && ws->first > 0 && ws->second > 0) { cols = ws->first; rows = ws->second; }
    }
    const char* term = term_env.empty() ? getenv("TERM") : term_env.c_str();
    std::string term_str = (term && *term) ? term : "xterm-256color";

#ifdef _WIN32
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
#else
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif

    ClientStats stats;
    stats.stats_file = stats_file.empty() ? default_stats_file(stats_dir) : stats_file;
    stats.mode = !bridge_path.empty() ? "bridge" : (pipe_mode ? "pipe" : "terminal");
    stats.server = server_addr;
    stats.port = server_port;
    stats.session = session_name;
    stats.attach_name = command_override.empty() ? session_name : session_name + ":" + command_override;
    stats.term = term_str;
    stats.cols = cols;
    stats.rows = rows;
    stats.last_event = "starting";
    write_stats(stats, true);

    bool clean_start = !no_clean_start;
    bool show_banner = !no_banner;

    // Phase 11: Bridge mode — single-shot, no backoff
    if (!bridge_path.empty()) {
        bridge_relay(bridge_path, server_addr, server_port, session_name,
                     command_override, cert_file, key_file, cols, rows, term_str, quiet,
                     &stats, clean_start, show_banner, key_log_file);
        return 0;
    }

    Backoff backoff;
    auto deadline = std::chrono::steady_clock::now() + 30s;

    while (g_running && std::chrono::steady_clock::now() < deadline) {
        bool reconnect = connect_and_relay(
            server_addr, server_port, session_name, command_override,
            cert_file, key_file, cols, rows, term_str, quiet,
            pipe_mode, &stats, clean_start, show_banner, key_log_file);
        if (!reconnect || !g_running) break;
        stats.reconnects++;
        stats.last_event = "reconnect backoff";
        write_stats(stats, true);
        backoff.wait();
    }

    if (raw_enabled) restore_terminal(saved_term);
    if (g_running && !quiet) {
        std::cerr << "session '" << session_name
                  << "' survived — reattach with: bs-client --server="
                  << server_addr << ":" << server_port
                  << " --name=" << session_name << std::endl;
    }
    stats.connected = false;
    stats.last_event = g_running ? "exited" : "interrupted";
    write_stats(stats, true);
    return 0;
}
