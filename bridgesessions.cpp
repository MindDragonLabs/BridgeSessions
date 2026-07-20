// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bridgesessions.cpp — Mesh peer-to-peer terminal sharing
// Single-file architecture: all protocol, TLS, session, and mesh logic in one file.
// Namespace: bs::mesh

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <csignal>
#include <sys/wait.h>
#include <termios.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#endif
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <cstring>
#include <algorithm>
#include <span>
#include <optional>
#include <expected>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iostream>
#include <array>
#include <utility>
#include <stdexcept>
#include <functional>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <process.h>
#endif
#include <zstd.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <atomic>
#include <memory>
#include <new>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <unordered_map>
#include <queue>
#include <filesystem>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

// ── D15: WebRTC (libdatachannel, Windows-only for now) ─────────────
#ifdef _WIN32
#ifndef BS_NO_WEBRTC
#include <rtc/rtc.hpp>
#endif
#endif

// ── D17: NAT traversal (miniupnpc) ─────────────────────────────────
#ifndef BS_NO_NAT
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#endif

// ────────────────────────────────────────────────────────────────────
// 1. MESSAGE TYPES (ported from bs-protocol, namespace bs::mesh)
// ────────────────────────────────────────────────────────────────────

namespace bs::mesh {

#ifndef BS_VERSION
#define BS_VERSION "0.0.0-dev"
#endif
inline constexpr std::string_view kBridgeSessionsVersion = BS_VERSION;

// ────────────────────────────────────────────────────────────────────
// 0. RING BUFFER (thread-safe circular buffer for PTY scrollback)
// ────────────────────────────────────────────────────────────────────

template <size_t Capacity>
class RingBuffer {
    // Power-of-2 capacity enables mask-based indexing
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be power of 2");

    static constexpr size_t kMask = Capacity - 1;

    std::unique_ptr<std::array<char, Capacity>> buf_{
        std::make_unique<std::array<char, Capacity>>()};
    std::atomic<size_t> write_pos_{0};
    mutable std::shared_mutex mutex_;

public:
    RingBuffer() = default;

    // Move-only (atomics are non-copyable)
    RingBuffer(RingBuffer&& other) noexcept
        : write_pos_(other.write_pos_.load(std::memory_order_relaxed))
    {
        std::unique_lock lock(other.mutex_);
        std::memcpy(buf_->data(), other.buf_->data(), Capacity);
    }

    RingBuffer& operator=(RingBuffer&& other) noexcept {
        if (this != &other) {
            write_pos_.store(other.write_pos_.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
            std::scoped_lock lock(mutex_, other.mutex_);
            std::memcpy(buf_->data(), other.buf_->data(), Capacity);
        }
        return *this;
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // ── Write ──────────────────────────────────────────────────
    // Append data to the buffer. Thread-safe: exclusive lock.
    void write(std::span<const char> data) {
        if (data.empty()) return;
        std::unique_lock lock(mutex_);

        size_t pos = write_pos_.load(std::memory_order_relaxed);
        size_t len = data.size();

        if (len >= Capacity) {
            // Data larger than buffer — keep only the tail
            auto tail = data.subspan(data.size() - Capacity);
            std::memcpy(buf_->data(), tail.data(), Capacity);
            write_pos_.store(Capacity, std::memory_order_release);
            return;
        }

        size_t idx = pos & kMask;
        size_t space_to_end = Capacity - idx;

        if (len <= space_to_end) {
            std::memcpy(buf_->data() + idx, data.data(), len);
        } else {
            std::memcpy(buf_->data() + idx, data.data(), space_to_end);
            std::memcpy(buf_->data(), data.data() + space_to_end, len - space_to_end);
        }

        write_pos_.store(pos + len, std::memory_order_release);
    }

    void write(std::string_view data) {
        write(std::span<const char>(data.data(), data.size()));
    }

    // ── Read helpers ───────────────────────────────────────────

    // Total bytes written since creation (for uptime/statistics)
    size_t total_written() const noexcept {
        return write_pos_.load(std::memory_order_acquire);
    }

    // Current content size (up to Capacity)
    size_t size() const noexcept {
        size_t wp = write_pos_.load(std::memory_order_acquire);
        return std::min(wp, Capacity);
    }

    // ── Snapshot read — full buffer copy (shared lock) ─────────
    std::vector<char> snapshot() const {
        std::shared_lock lock(mutex_);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t n = std::min(wp, Capacity);

        std::vector<char> result(n);
        if (n == 0) return result;

        if (wp <= Capacity) {
            // Buffer hasn't wrapped yet
            std::memcpy(result.data(), buf_->data(), n);
        } else {
            // Wrapped — read in order from oldest to newest
            size_t idx = wp & kMask;
            size_t tail = Capacity - idx;
            std::memcpy(result.data(), buf_->data() + idx, tail);
            std::memcpy(result.data() + tail, buf_->data(), idx);
        }
        return result;
    }

    // ── read_last_lines — backward scan for N most recent lines ─
    std::string read_last_lines(size_t num_lines) const {
        if (num_lines == 0) return {};

        std::shared_lock lock(mutex_);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t n = std::min(wp, Capacity);
        if (n == 0) return {};

        // Build linear view of buffer
        std::vector<char> linear(n);
        if (wp <= Capacity) {
            std::memcpy(linear.data(), buf_->data(), n);
        } else {
            size_t idx = wp & kMask;
            size_t tail = Capacity - idx;
            std::memcpy(linear.data(), buf_->data() + idx, tail);
            std::memcpy(linear.data() + tail, buf_->data(), idx);
        }

        // Scan backward for newlines
        // If the last char is '\n', we need num_lines+1 transitions to skip the empty trailing line.
        // Otherwise we need exactly num_lines transitions.
        size_t target_newlines = (linear.back() == '\n') ? num_lines + 1 : num_lines;
        size_t lines_found = 0;
        ptrdiff_t cut = static_cast<ptrdiff_t>(n);
        for (ptrdiff_t i = static_cast<ptrdiff_t>(n) - 1; i >= 0; --i) {
            if (linear[i] == '\n') {
                ++lines_found;
                if (lines_found >= target_newlines) {
                    cut = i + 1;
                    break;
                }
            }
        }
        if (lines_found < target_newlines) cut = 0;

        return std::string(linear.data() + cut, n - static_cast<size_t>(cut));
    }

    // ── read_range — offset + length for chunked replay ─────────
    std::string read_range(size_t offset, size_t length) const {
        std::shared_lock lock(mutex_);
        size_t wp = write_pos_.load(std::memory_order_acquire);
        size_t n = std::min(wp, Capacity);
        if (offset >= n) return {};

        size_t actual_len = std::min(length, n - offset);

        std::vector<char> linear(n);
        if (wp <= Capacity) {
            std::memcpy(linear.data(), buf_->data(), n);
        } else {
            size_t idx = wp & kMask;
            size_t tail = Capacity - idx;
            std::memcpy(linear.data(), buf_->data() + idx, tail);
            std::memcpy(linear.data() + tail, buf_->data(), idx);
        }

        return std::string(linear.data() + offset, actual_len);
    }

    // ── Clear ─────────────────────────────────────────────────
    void clear() {
        std::unique_lock lock(mutex_);
        write_pos_.store(0, std::memory_order_release);
    }

    // ── Capacity (compile-time constant, but accessible) ─────
    static constexpr size_t capacity() noexcept { return Capacity; }
};

// ── Message Type Enum ─────────────────────────────────────────────
// 20 original types + 2 new mesh types = 22 total

enum class MessageType : uint8_t {
    Keystroke      = 0x01,  // client → server: raw key bytes
    Output         = 0x02,  // server → client: PTY stdout
    Resize         = 0x03,  // client → server: {cols, rows}
    ClipboardGet   = 0x04,  // server → client: OSC 52 captured
    ClipboardPut   = 0x05,  // client → server: user pasted
    Attach         = 0x06,  // client → server: {session_name, cols, rows, term}
    Detach         = 0x07,  // client → server: preserve session
    SessionList    = 0x08,  // server → client: [{name, state, uptime}]
    ServerInfo     = 0x09,  // server → client: {hostname, version, load}
    Ping           = 0x0A,  // bidirectional: keepalive
    Pong           = 0x0B,  // bidirectional: keepalive response
    Scrollback     = 0x0C,  // server → client: replay chunk
    Signal         = 0x0D,  // client → server: ^C ^Z ^Backslash
    ProcExited     = 0x0E,  // server -> client: foreground process exited
    ScrollbackAck  = 0x0F,  // client → server: ready for next chunk
    SessionDied    = 0x10,  // server → client: PTY crashed
    ClipboardEcho  = 0x11,  // server → client: confirms hash receipt
    ImageData      = 0x12,  // bidirectional: static image payload (PNG/JPEG)
    ImageFrame     = 0x13,  // bidirectional: animated image payload (GIF frame/blob)
    ImageAck       = 0x14,  // bidirectional: image/frame consumed acknowledgement
    Hello          = 0x15,  // bidirectional: mesh node introduction
    Gossip         = 0x16,  // bidirectional: mesh peer list exchange
    SessionSearch  = 0x17,  // bidirectional: search for a session across the mesh
    SdpOffer       = 0x18,  // bidirectional: WebRTC SDP offer (over TCP gossip)
    SdpAnswer      = 0x19,  // bidirectional: WebRTC SDP answer (over TCP gossip)
    DhtFindNode    = 0x1A,  // bidirectional: Kademlia find-node query
    DhtFindValue   = 0x1B,  // bidirectional: Kademlia find-value query
    FileMeta       = 0x1C,  // bidirectional: file metadata (name, size, checksum, total_chunks)
    FileChunk      = 0x1D,  // bidirectional: file data chunk (chunk_index, data)
    FileAck        = 0x1E,  // bidirectional: file chunk acknowledgement / next chunk request
    FileRequest    = 0x1F,  // client → server: request file transfer (path)
};

// ── Empty Message Structs (must be declared before variant) ──────

struct DetachMsg { bool operator==(const DetachMsg&) const = default; };
struct PingMsg   { bool operator==(const PingMsg&)   const = default; };
struct PongMsg   { bool operator==(const PongMsg&)   const = default; };
struct ScrollbackAckMsg { bool operator==(const ScrollbackAckMsg&) const = default; };
struct ImageAckMsg { bool operator==(const ImageAckMsg&) const = default; };

// ── Original Payload Message Structs ──────────────────────────────

struct KeystrokeMsg {
    std::string data;  // raw key bytes or bracketed-paste
};

struct OutputMsg {
    std::string data;  // PTY stdout (already rendered)
    bool render_markdown = false;  // hint: GUI should render as markdown HTML
};

// Heuristic: does text look like markdown?
inline bool looks_like_markdown(const std::string& text, size_t max_check = 200) {
    size_t end = (std::min)(text.size(), max_check);
    if (end == 0) return false;
    std::string_view sv(text.data(), end);
    int md_lines = 0;
    size_t line_count = 0;
    size_t pos = 0;
    while (pos < sv.size() && line_count < 20) {
        size_t nl = sv.find('\n', pos);
        std::string_view line = sv.substr(pos, nl - pos);
        while (!line.empty() && (line[0] == ' ' || line[0] == '	')) line.remove_prefix(1);
        if (line.empty()) { pos = (nl == sv.npos) ? sv.size() : nl + 1; ++line_count; continue; }
        bool is_md = false;
        if ((line.size() >= 2 && line[0] == '#' && line[1] == ' ') ||
            (line.size() >= 3 && line[0] == '#' && line[1] == '#' && line[2] == ' ') ||
            (line.size() >= 2 && line[0] == '-' && line[1] == ' ') ||
            (line.size() >= 3 && line[0] == '`' && line[1] == '`' && line[2] == '`') ||
            (line.size() >= 2 && line[0] == '|' && line.find('|', 1) != sv.npos))
            ++md_lines;
        pos = (nl == sv.npos) ? sv.size() : nl + 1;
        ++line_count;
    }
    return md_lines >= 2;
}

struct ResizeMsg {
    uint16_t cols = 0;
    uint16_t rows = 0;
};

struct ClipboardMsg {  // reused for ClipboardGet + ClipboardPut
    std::string text;  // clipboard payload
    std::string hash;  // SHA-256 hex
};

struct ClipboardEchoMsg {
    std::string hash;  // echo'd hash
};

struct ImageDataMsg {
    uint8_t format = 0;  // 0=PNG, 1=JPEG
    std::string name;    // filename hint
    std::vector<uint8_t> data;  // raw encoded image bytes

    bool operator==(const ImageDataMsg&) const = default;
};

struct ImageFrameMsg {
    uint8_t format = 2;  // GIF for now
    uint32_t delay_ms = 0;
    uint32_t loop_count = 0;  // 0 = infinite
    std::vector<uint8_t> data;  // raw encoded animation/frame bytes

    bool operator==(const ImageFrameMsg&) const = default;
};

struct AttachMsg {
    std::string session_name;
    std::string routing;    // target node name for multi-hop (empty = handle locally)
    uint16_t cols = 80;
    uint16_t rows = 24;
    std::string term = "xterm-256color";
    std::string command;    // optional command to run in the attached session (exec-based attach)
    std::string signal_on_detach; // optional: HUP/TERM/INT/KILL sent to child on last-peer detach
};

struct SessionInfo {
    std::string name;
    std::string state;  // "attached" | "detached" | "died"
    uint64_t uptime_seconds = 0;
};

struct SessionListMsg {
    std::vector<SessionInfo> sessions;
};

struct ServerInfoMsg {
    std::string hostname;
    std::string version;
    double load = 0.0;
};

struct ScrollbackMsg {
    std::string data;      // replay chunk
    uint32_t total_lines = 0;
    uint32_t chunk_index = 0;
};

struct SignalMsg {
    enum class SignalType : uint8_t { CtrlC = 0, CtrlZ = 1, CtrlBackslash = 2, Restart = 3 };
    SignalType signal = SignalType::CtrlC;
    std::string process;  // process name to restart (only for Restart signal)
};

struct ExitCodeMsg {
    int32_t code = 0;
};

struct SessionDiedMsg {
    int32_t exit_code = 0;
    int32_t signal_num = 0;
};

// ── NEW Mesh Message Structs ────────────────────────────────────

struct PeerInfo {
    std::string name;        // "windows-peer", "linux-peer", etc.
    std::string addr;        // "host:port"
    std::string pubkey_hex;  // ed25519 public key
    uint64_t last_seen = 0;  // unix timestamp

    bool operator==(const PeerInfo&) const = default;
};

struct HelloMsg {
    std::string node_name;
    std::string version;
    std::string pubkey_hex;
    std::vector<PeerInfo> known_peers;

    bool operator==(const HelloMsg&) const = default;
};

struct GossipMsg {
    std::vector<PeerInfo> peers;

    bool operator==(const GossipMsg&) const = default;
};

struct SessionSearchMsg {
    std::string session_name;
    std::string routing;    // target node name
    uint16_t cols = 80;
    uint16_t rows = 24;
    std::string term = "xterm-256color";

    bool operator==(const SessionSearchMsg&) const = default;
};

// ── File Transfer Message Structs (v1.5, P1) ────────────────────

struct FileMetaMsg {
    std::string filename;       // basename only
    uint64_t filesize = 0;      // total file size in bytes
    std::string checksum;       // SHA-256 hex of entire file
    uint32_t total_chunks = 0;  // total number of chunks
    bool operator==(const FileMetaMsg&) const = default;
};

struct FileChunkMsg {
    uint32_t chunk_index = 0;   // 0-based chunk index
    uint32_t total_chunks = 0;  // total number of chunks (repeated for convenience)
    std::vector<uint8_t> data;  // zstd-compressed chunk payload
    bool operator==(const FileChunkMsg&) const = default;
};

struct FileAckMsg {
    uint32_t chunk_index = 0;   // acknowledged chunk index
    uint32_t next_requested = 0; // next chunk we want (for resume: > chunk_index+1)
    bool error = false;          // true = transfer aborted (e.g. disk full)
    std::string error_msg;       // human-readable error detail
    bool operator==(const FileAckMsg&) const = default;
};

struct FileRequestMsg {
    std::string path;            // file path on the remote peer
    bool operator==(const FileRequestMsg&) const = default;
};

// ── WebRTC SDP Exchange Message Structs (D15) ───────────────────

struct SdpOfferMsg {
    std::string sdp;        // SDP offer string
    std::string peer_name;  // offering peer name

    bool operator==(const SdpOfferMsg&) const = default;
};

struct SdpAnswerMsg {
    std::string sdp;        // SDP answer string
    std::string peer_name;  // answering peer name

    bool operator==(const SdpAnswerMsg&) const = default;
};

// ── DHT Message Structs (D16) ───────────────────────────────────

struct DhtFindNodeMsg {
    std::array<uint8_t, 32> target_id{};  // SHA-256 of target pubkey
    std::string sender_name;              // querying node name

    bool operator==(const DhtFindNodeMsg&) const = default;
};

struct DhtFindValueMsg {
    std::array<uint8_t, 32> key{};  // SHA-256 key being sought
    std::string sender_name;        // querying node name

    bool operator==(const DhtFindValueMsg&) const = default;
};

// ── Message Variant ──────────────────────────────────────────────

using Message = std::variant<
    KeystrokeMsg,       // 0
    OutputMsg,          // 1
    ResizeMsg,          // 2
    ClipboardMsg,       // 3
    ClipboardEchoMsg,   // 4
    AttachMsg,          // 5
    DetachMsg,          // 6
    SessionListMsg,     // 7
    ServerInfoMsg,      // 8
    PingMsg,            // 9
    PongMsg,            // 10
    ScrollbackMsg,      // 11
    SignalMsg,          // 12
    ExitCodeMsg,        // 13
    ScrollbackAckMsg,   // 14
    SessionDiedMsg,     // 15
    ImageDataMsg,       // 16
    ImageFrameMsg,      // 17
    ImageAckMsg,        // 18
    HelloMsg,           // 19 — NEW
    GossipMsg,          // 20 — NEW
    SessionSearchMsg,   // 21 — NEW
    SdpOfferMsg,        // 22 — D15
    SdpAnswerMsg,       // 23 — D15
    DhtFindNodeMsg,     // 24 — D16
    DhtFindValueMsg,    // 25 — D16
    FileMetaMsg,        // 26 — P1 file transfer
    FileChunkMsg,       // 27 — P1 file transfer
    FileAckMsg,         // 28 — P1 file transfer
    FileRequestMsg      // 29 — P1 file transfer
>;

// ── Frame ──────────────────────────────────────────────────────────
// Wire format: [stream_id: u16][type: u8][flags: u8][length: u16][data]
// flags bit 0 = compressed (zstd), bit 1 = control frame

constexpr uint16_t CONTROL_STREAM_ID = 0;
constexpr size_t   FRAME_HEADER_SIZE  = 6;
constexpr uint16_t MAX_FRAME_SIZE     = 65535;
constexpr uint16_t COMPRESSION_THRESHOLD = 256;
constexpr size_t    MAX_IMAGE_BYTES    = 50ull * 1024ull * 1024ull;

enum FrameFlags : uint8_t {
    FLAG_COMPRESSED      = 0x01,
    FLAG_CONTROL         = 0x02,
    FLAG_RENDER_MARKDOWN = 0x04,
};

struct Frame {
    uint16_t stream_id = 0;
    MessageType type = MessageType::Ping;
    uint8_t flags = 0;
    std::vector<uint8_t> data;
};

// ── Type mapping (variant index → MessageType byte) ──────────
// Must match the variant ordering exactly. 21 alternatives = 21 entries.

namespace {
constexpr MessageType index_to_type[] = {
    MessageType::Keystroke,     // 0
    MessageType::Output,        // 1
    MessageType::Resize,        // 2
    MessageType::ClipboardGet,  // 3  (ClipboardMsg used for both Get and Put)
    MessageType::ClipboardEcho, // 4
    MessageType::Attach,        // 5
    MessageType::Detach,        // 6
    MessageType::SessionList,   // 7
    MessageType::ServerInfo,    // 8
    MessageType::Ping,          // 9
    MessageType::Pong,          // 10
    MessageType::Scrollback,    // 11
    MessageType::Signal,        // 12
    MessageType::ProcExited,    // 13  (0x0E wire, maps to ExitCodeMsg variant slot)
    MessageType::ScrollbackAck, // 14
    MessageType::SessionDied,   // 15
    MessageType::ImageData,     // 16
    MessageType::ImageFrame,    // 17
    MessageType::ImageAck,      // 18
    MessageType::Hello,         // 19 — NEW
    MessageType::Gossip,        // 20 — NEW
    MessageType::SessionSearch, // 21 — NEW
    MessageType::SdpOffer,      // 22 — D15
    MessageType::SdpAnswer,     // 23 — D15
    MessageType::DhtFindNode,   // 24 — D16
    MessageType::DhtFindValue,  // 25 — D16
    MessageType::FileMeta,      // 26 — P1
    MessageType::FileChunk,     // 27 — P1
    MessageType::FileAck,       // 28 — P1
    MessageType::FileRequest,   // 29 — P1
};

static_assert(std::size(index_to_type) == std::variant_size_v<Message>,
              "index_to_type must have one entry per variant alternative");
} // anonymous namespace

[[nodiscard]] MessageType message_type(const Message& msg) {
    return index_to_type[msg.index()];
}

// ── Serializer ────────────────────────────────────────────────────

namespace {

uint16_t read_u16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

void write_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v & 0xFF);
}

struct Serializer {
    std::vector<uint8_t>& out;
    void bytes(const uint8_t* p, size_t n) {
        if (n == 0) return;
        out.insert(out.end(), p, p + n);
    }
    void bytes(std::span<const uint8_t> b) { bytes(b.data(), b.size()); }
    void u16(uint16_t v) { uint8_t b[2]; write_u16(b, v); bytes(b, 2); }
    void u32be(uint32_t v) { for (int i=3; i>=0; --i) out.push_back(v>>(i*8)); }
    void u8(uint8_t v) { out.push_back(v); }
    void str(const std::string& s) { bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
    void str_prefixed(const std::string& s) {
        if (s.size() > UINT8_MAX) throw std::runtime_error("prefixed string exceeds 255 bytes");
        u8(static_cast<uint8_t>(s.size()));
        str(s);
    }
    void str_prefixed_u16(const std::string& s) {
        if (s.size() > UINT16_MAX) throw std::runtime_error("prefixed string exceeds 65535 bytes");
        u16(static_cast<uint16_t>(s.size()));
        str(s);
    }
};

void serialize_msg(Serializer& s, const KeystrokeMsg&    m) { s.str(m.data); }
void serialize_msg(Serializer& s, const OutputMsg&       m) { s.u8(m.render_markdown ? 1 : 0); s.str(m.data); }
void serialize_msg(Serializer& s, const ResizeMsg&       m) { s.u16(m.cols); s.u16(m.rows); }
void serialize_msg(Serializer& s, const ClipboardMsg&    m) { s.str(m.hash); s.u8('\n'); s.str(m.text); }
void serialize_msg(Serializer& s, const ClipboardEchoMsg& m) { s.str(m.hash); }
void serialize_msg(Serializer& s, const ImageDataMsg&    m) {
    if (m.data.size() > MAX_IMAGE_BYTES) throw std::runtime_error("image payload exceeds 50MB cap");
    s.u8(m.format);
    s.str_prefixed_u16(m.name);
    s.u32be(static_cast<uint32_t>(m.data.size()));
    if (!m.data.empty()) s.bytes(std::span<const uint8_t>(m.data.data(), m.data.size()));
}
void serialize_msg(Serializer& s, const ImageFrameMsg&   m) {
    if (m.data.size() > MAX_IMAGE_BYTES) throw std::runtime_error("image frame payload exceeds 50MB cap");
    s.u8(m.format);
    s.u32be(m.delay_ms);
    s.u32be(m.loop_count);
    s.u32be(static_cast<uint32_t>(m.data.size()));
    if (!m.data.empty()) s.bytes(std::span<const uint8_t>(m.data.data(), m.data.size()));
}
void serialize_msg(Serializer& s, const ImageAckMsg&)     {}
void serialize_msg(Serializer& s, const AttachMsg&       m) { s.u16(m.cols); s.u16(m.rows); s.str_prefixed(m.term); s.str_prefixed(m.session_name); s.str_prefixed(m.routing); s.str_prefixed_u16(m.command); s.str_prefixed_u16(m.signal_on_detach); }
void serialize_msg(Serializer& s, const DetachMsg&)        {}
void serialize_msg(Serializer& s, const PingMsg&)          {}
void serialize_msg(Serializer& s, const PongMsg&)          {}
void serialize_msg(Serializer& s, const ScrollbackAckMsg&) {}
void serialize_msg(Serializer& s, const SessionListMsg&  m) { for (auto& si : m.sessions) { s.str_prefixed(si.name); s.str_prefixed(si.state); s.u32be(si.uptime_seconds); } }
void serialize_msg(Serializer& s, const ServerInfoMsg&   m) { s.str_prefixed_u16(m.hostname); s.str_prefixed_u16(m.version); s.bytes(reinterpret_cast<const uint8_t*>(&m.load), 8); }
void serialize_msg(Serializer& s, const ScrollbackMsg&   m) { s.u32be(m.total_lines); s.u32be(m.chunk_index); s.str(m.data); }
void serialize_msg(Serializer& s, const SignalMsg&       m) { s.u8(static_cast<uint8_t>(m.signal)); s.str_prefixed_u16(m.process); }
void serialize_msg(Serializer& s, const ExitCodeMsg&     m) { s.u32be(static_cast<uint32_t>(m.code)); }
void serialize_msg(Serializer& s, const SessionDiedMsg&  m) { s.u32be(static_cast<uint32_t>(m.exit_code)); s.u32be(static_cast<uint32_t>(m.signal_num)); }
void serialize_msg(Serializer& s, const HelloMsg&        m) {
    s.str_prefixed(m.node_name);
    s.str_prefixed(m.version);
    s.str_prefixed(m.pubkey_hex);
    for (auto& p : m.known_peers) {
        s.str_prefixed(p.name);
        s.str_prefixed(p.addr);
        s.str_prefixed(p.pubkey_hex);
        s.u32be(static_cast<uint32_t>(p.last_seen));
    }
}
void serialize_msg(Serializer& s, const GossipMsg&       m) {
    for (auto& p : m.peers) {
        s.str_prefixed(p.name);
        s.str_prefixed(p.addr);
        s.str_prefixed(p.pubkey_hex);
        s.u32be(static_cast<uint32_t>(p.last_seen));
    }
}
void serialize_msg(Serializer& s, const SessionSearchMsg& m) {
    s.str_prefixed(m.session_name); s.str_prefixed(m.routing);
    s.u16(m.cols); s.u16(m.rows); s.str_prefixed(m.term);
}
void serialize_msg(Serializer& s, const SdpOfferMsg& m) {
    s.str_prefixed(m.peer_name);
    s.str_prefixed_u16(m.sdp);
}
void serialize_msg(Serializer& s, const SdpAnswerMsg& m) {
    s.str_prefixed(m.peer_name);
    s.str_prefixed_u16(m.sdp);
}
void serialize_msg(Serializer& s, const DhtFindNodeMsg& m) {
    s.bytes(m.target_id.data(), 32);
    s.str_prefixed(m.sender_name);
}
void serialize_msg(Serializer& s, const DhtFindValueMsg& m) {
    s.bytes(m.key.data(), 32);
    s.str_prefixed(m.sender_name);
}
void serialize_msg(Serializer& s, const FileMetaMsg& m) {
    s.str_prefixed(m.filename);
    s.u32be(static_cast<uint32_t>(m.filesize >> 32));
    s.u32be(static_cast<uint32_t>(m.filesize & 0xFFFFFFFF));
    s.str_prefixed(m.checksum);
    s.u32be(m.total_chunks);
}
void serialize_msg(Serializer& s, const FileChunkMsg& m) {
    s.u32be(m.chunk_index);
    s.u32be(m.total_chunks);
    if (m.data.size() > MAX_FRAME_SIZE)
        throw std::runtime_error("file chunk payload exceeds MAX_FRAME_SIZE");
    s.u32be(static_cast<uint32_t>(m.data.size()));
    if (!m.data.empty()) s.bytes(std::span<const uint8_t>(m.data.data(), m.data.size()));
}
void serialize_msg(Serializer& s, const FileRequestMsg& m) {
    s.str_prefixed_u16(m.path);
}
void serialize_msg(Serializer& s, const FileAckMsg& m) {
    s.u32be(m.chunk_index);
    s.u32be(m.next_requested);
    s.u8(m.error ? 1 : 0);
    s.str_prefixed_u16(m.error_msg);
}

// ── Zstd ──────────────────────────────────────────────────────

std::vector<uint8_t> zstd_compress(std::span<const uint8_t> data) {
    std::vector<uint8_t> out(ZSTD_compressBound(data.size()));
    size_t sz = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 3);
    if (ZSTD_isError(sz)) throw std::runtime_error(std::string("zstd compress: ") + ZSTD_getErrorName(sz));
    out.resize(sz);
    return out;
}

std::vector<uint8_t> zstd_decompress(std::span<const uint8_t> data) {
    uint64_t bound = ZSTD_getFrameContentSize(data.data(), data.size());
    if (bound == ZSTD_CONTENTSIZE_ERROR) throw std::runtime_error("zstd: invalid frame");
    if (bound == ZSTD_CONTENTSIZE_UNKNOWN) throw std::runtime_error("zstd: unknown decompressed size");
    if (bound > MAX_FRAME_SIZE) throw std::runtime_error("zstd: decompressed frame exceeds MAX_FRAME_SIZE");
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    size_t sz = ZSTD_decompress(out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(sz)) throw std::runtime_error(std::string("zstd decompress: ") + ZSTD_getErrorName(sz));
    if (sz > MAX_FRAME_SIZE) throw std::runtime_error("zstd: decoded frame exceeds MAX_FRAME_SIZE");
    out.resize(sz);
    return out;
}

// ── Decode helpers ─────────────────────────────────────────────

struct Decoder {
    const uint8_t* p = nullptr;
    const uint8_t* end = nullptr;

    bool ok(size_t need = 0) const {
        return p <= end && need <= static_cast<size_t>(end - p);
    }

    void ensure(size_t need) {
        if (!ok(need)) throw std::runtime_error("frame truncated");
    }

    uint16_t u16() { ensure(2); uint16_t v = read_u16(p); p += 2; return v; }
    uint8_t  u8()  { ensure(1); return *p++; }

    uint32_t u32be() {
        ensure(4);
        uint32_t v = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16)
                   | (static_cast<uint32_t>(p[2]) << 8)  | p[3];
        p += 4;
        return v;
    }

    std::string str_size(size_t n) {
        if (n == 0) return {};
        ensure(n);
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
    std::string str_prefixed() { return str_size(u8()); }
    std::string str_prefixed_u16() { return str_size(u16()); }
    std::vector<uint8_t> bytes_size(size_t n) {
        if (n == 0) return {};
        ensure(n);
        std::vector<uint8_t> b(p, p + n);
        p += n;
        return b;
    }
};

// ── Type mapping (variant index → MessageType byte) is already above ──

} // anonymous namespace

// ── Public API ─────────────────────────────────────────────────

std::string sha256_hex(std::string_view data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx ||
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, md, &len) != 1) {
        EVP_MD_CTX_free(ctx);
        return {};
    }
    EVP_MD_CTX_free(ctx);
    std::string hex;
    for (unsigned int i = 0; i < len; ++i) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", md[i]);
        hex += buf;
    }
    return hex;
}

// Incremental SHA-256 for large file transfers (no full-file RAM).
class Sha256Stream {
public:
    Sha256Stream() {
        ctx_ = EVP_MD_CTX_new();
        if (!ctx_ || EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) != 1) {
            if (ctx_) { EVP_MD_CTX_free(ctx_); ctx_ = nullptr; }
        }
    }
    ~Sha256Stream() {
        if (ctx_) EVP_MD_CTX_free(ctx_);
    }
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;
    bool ok() const { return ctx_ != nullptr; }
    bool update(const void* data, size_t n) {
        if (!ctx_ || !data || n == 0) return ctx_ != nullptr;
        return EVP_DigestUpdate(ctx_, data, n) == 1;
    }
    bool update(std::string_view sv) { return update(sv.data(), sv.size()); }
    bool update(const std::vector<uint8_t>& v) {
        return update(v.data(), v.size());
    }
    std::string final_hex() {
        if (!ctx_) return {};
        unsigned char md[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        if (EVP_DigestFinal_ex(ctx_, md, &len) != 1) return {};
        EVP_MD_CTX_free(ctx_);
        ctx_ = nullptr;
        std::string hex;
        for (unsigned int i = 0; i < len; ++i) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", md[i]);
            hex += buf;
        }
        return hex;
    }
private:
    EVP_MD_CTX* ctx_ = nullptr;
};

// Transfer timeouts: idle stall + size-aware overall (v2.0.5).
// min ~0.25 MiB/s for ETA budget; floor 5 min; ceiling 2 h.
[[nodiscard]] inline std::chrono::seconds transfer_overall_timeout(uint64_t bytes) {
    constexpr uint64_t kMinBps = 256ull * 1024ull; // 0.25 MiB/s
    uint64_t secs = bytes / kMinBps;
    if (secs < 300) secs = 300;
    if (secs > 7200) secs = 7200;
    return std::chrono::seconds(static_cast<int64_t>(secs));
}
constexpr int kTransferIdleTimeoutSec = 120;
constexpr int kTransferProgressIntervalSec = 10;
constexpr size_t kTransferChunkRawSize = 48 * 1024;

struct TransferMetadataValidation {
    bool ok = false;
    uint32_t expected_chunks = 0;
    std::string reason;
};

[[nodiscard]] inline TransferMetadataValidation calculate_transfer_metadata(
    uint64_t filesize, uint64_t max_bytes) {
    if (max_bytes > 0 && filesize > max_bytes) {
        return {false, 0, "file exceeds transfer.max_bytes"};
    }
    uint64_t expected = filesize / kTransferChunkRawSize;
    if (filesize % kTransferChunkRawSize != 0) ++expected;
    if (expected == 0) expected = 1;  // zero-byte files still carry one empty chunk
    if (expected > std::numeric_limits<uint32_t>::max()) {
        return {false, 0, "file requires too many chunks"};
    }
    const auto expected_u32 = static_cast<uint32_t>(expected);
    return {true, expected_u32, {}};
}

[[nodiscard]] inline TransferMetadataValidation validate_transfer_metadata(
    uint64_t filesize, uint32_t total_chunks, uint64_t max_bytes) {
    auto result = calculate_transfer_metadata(filesize, max_bytes);
    if (!result.ok) return result;
    const auto expected_u32 = result.expected_chunks;
    if (total_chunks != expected_u32) {
        return {false, expected_u32, "declared chunk count does not match file size"};
    }
    return result;
}

struct TransferChunkValidation {
    bool ok = false;
    std::string reason;
};

[[nodiscard]] inline TransferChunkValidation validate_transfer_chunk(
    uint64_t expected_size,
    uint64_t received_bytes,
    uint32_t expected_index,
    uint32_t expected_total_chunks,
    uint32_t chunk_index,
    uint32_t chunk_total_chunks,
    size_t decompressed_size) {
    if (chunk_total_chunks != expected_total_chunks) {
        return {false, "chunk total does not match transfer metadata"};
    }
    if (chunk_index != expected_index || chunk_index >= expected_total_chunks) {
        return {false, "unexpected chunk index"};
    }
    if (received_bytes > expected_size) {
        return {false, "received byte count already exceeds declared size"};
    }
    const uint64_t remaining = expected_size - received_bytes;
    const uint64_t expected_chunk_size =
        std::min<uint64_t>(remaining, kTransferChunkRawSize);
    if (decompressed_size != expected_chunk_size) {
        return {false, "chunk bytes do not match declared file size"};
    }
    return {true, {}};
}

[[nodiscard]] inline std::string format_transfer_progress(
    const char* phase,
    const std::string& file,
    uint32_t chunks_done,
    uint32_t chunks_total,
    uint64_t bytes_done,
    uint64_t bytes_total,
    double rate_mibs,
    int eta_sec) {
    double pct = 0.0;
    if (bytes_total > 0)
        pct = 100.0 * static_cast<double>(bytes_done) / static_cast<double>(bytes_total);
    else if (chunks_total > 0)
        pct = 100.0 * static_cast<double>(chunks_done) / static_cast<double>(chunks_total);
    char buf[512];
    snprintf(buf, sizeof(buf),
             "PROGRESS phase=%s file=%s chunks=%u/%u bytes=%llu/%llu pct=%.1f rate_mibs=%.2f eta_sec=%d",
             phase, file.c_str(), chunks_done, chunks_total,
             static_cast<unsigned long long>(bytes_done),
             static_cast<unsigned long long>(bytes_total),
             pct, rate_mibs, eta_sec);
    return std::string(buf);
}

[[nodiscard]] inline std::optional<std::string> consume_transfer_ipc_chunk(
    std::string& pending,
    std::string_view chunk,
    const std::function<void(const std::string&)>& emit_progress) {
    pending.append(chunk);
    size_t consumed = 0;
    while (true) {
        const size_t newline = pending.find('\n', consumed);
        if (newline == std::string::npos) break;
        std::string line = pending.substr(consumed, newline - consumed);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        consumed = newline + 1;

        if (line.rfind("PROGRESS ", 0) == 0) {
            emit_progress(line);
            continue;
        }
        if (line.rfind("OK ", 0) == 0 || line.rfind("ERROR", 0) == 0) {
            pending.erase(0, consumed);
            return line;
        }
    }
    if (consumed > 0) pending.erase(0, consumed);
    return std::nullopt;
}

[[nodiscard]] inline std::string sha256_file_stream(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha256Stream h;
    if (!h.ok()) return {};
    std::array<char, 64 * 1024> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        auto n = in.gcount();
        if (n > 0 && !h.update(buf.data(), static_cast<size_t>(n))) return {};
    }
    if (in.bad() || !in.eof()) return {};
    return h.final_hex();
}

std::vector<uint8_t> encode(const Message& msg, uint16_t stream_id) {
    std::vector<uint8_t> payload;
    Serializer s{payload};
    std::visit([&](const auto& m) { serialize_msg(s, m); }, msg);

    if (payload.size() > MAX_FRAME_SIZE)
        throw std::runtime_error("logical payload exceeds MAX_FRAME_SIZE");

    bool compress = payload.size() > COMPRESSION_THRESHOLD;
    if (compress) payload = zstd_compress(payload);

    uint8_t flags = 0;
    if (compress) flags |= FLAG_COMPRESSED;
    if (stream_id == CONTROL_STREAM_ID) flags |= FLAG_CONTROL;

    if (payload.size() > MAX_FRAME_SIZE)
        throw std::runtime_error("encoded payload exceeds MAX_FRAME_SIZE");

    std::vector<uint8_t> frame(FRAME_HEADER_SIZE + payload.size());
    write_u16(frame.data(), stream_id);
    frame[2] = static_cast<uint8_t>(message_type(msg));
    frame[3] = flags;
    write_u16(frame.data() + 4, static_cast<uint16_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), frame.begin() + FRAME_HEADER_SIZE);

    return frame;
}

Message decode(std::span<const uint8_t> raw) {
    if (raw.size() < FRAME_HEADER_SIZE) throw std::runtime_error("frame too short");

    uint8_t  type_byte = raw[2];
    uint8_t  flags     = raw[3];
    uint16_t length    = read_u16(raw.data() + 4);

    if (raw.size() < FRAME_HEADER_SIZE + length) throw std::runtime_error("frame truncated");

    auto payload = raw.subspan(FRAME_HEADER_SIZE, length);

    // Decompress if needed
    std::vector<uint8_t> decompressed;
    if ((flags & FLAG_COMPRESSED) && length > 0) {
        decompressed = zstd_decompress(payload);
        payload = decompressed;
    }
    if (payload.size() > MAX_FRAME_SIZE) throw std::runtime_error("decoded payload exceeds MAX_FRAME_SIZE");

    Decoder d{payload.data(), payload.data() + payload.size()};

    switch (type_byte) {
    case 0x01: { KeystrokeMsg m; m.data = d.str_size(payload.size()); return m; }
    case 0x02: { OutputMsg m; m.render_markdown = d.u8() != 0; m.data = d.str_size(static_cast<size_t>(d.end - d.p)); return m; }
    case 0x03: { ResizeMsg    m; m.cols = d.u16(); m.rows = d.u16(); return m; }
    case 0x04: // ClipboardGet — fall through
    case 0x05: { // ClipboardPut
        ClipboardMsg m;
        auto nl = std::find(d.p, d.end, static_cast<uint8_t>('\n'));
        if (nl == d.end) throw std::runtime_error("Clipboard: missing hash separator");
        m.hash = d.str_size(static_cast<size_t>(nl - d.p));
        d.u8(); // skip newline
        m.text = d.str_size(static_cast<size_t>(d.end - d.p));
        return m;
    }
    case 0x11: { ClipboardEchoMsg m; m.hash = d.str_size(payload.size()); return m; }
    case 0x06: { AttachMsg m; m.cols = d.u16(); m.rows = d.u16(); m.term = d.str_prefixed(); m.session_name = d.str_prefixed(); m.routing = d.str_prefixed(); /* command is optional (v1.7+, str_prefixed_u16). v1.6 clients don't send it. */ m.command = d.ok(2) ? d.str_prefixed_u16() : std::string{}; /* signal_on_detach is optional (v2.0.7+, str_prefixed_u16). */ m.signal_on_detach = d.ok(2) ? d.str_prefixed_u16() : std::string{}; return m; }
    case 0x07: return DetachMsg{};
    case 0x08: {
        SessionListMsg m;
        while (d.ok(1)) {
            SessionInfo si;
            si.name  = d.str_prefixed();
            si.state = d.str_prefixed();
            si.uptime_seconds = d.u32be();
            m.sessions.push_back(std::move(si));
        }
        return m;
    }
    case 0x09: {
        ServerInfoMsg m;
        // Try old newline-delimited format first (backward compat).
        // If the hostname or version contains a literal newline, the old
        // parse will fail and we fall through to the length-prefixed path.
        const auto* saved = d.p;
        auto nl1 = std::find(d.p, d.end, static_cast<uint8_t>('\n'));
        if (nl1 != d.end) {
            m.hostname = d.str_size(static_cast<size_t>(nl1 - d.p));
            d.u8();
            auto nl2 = std::find(d.p, d.end, static_cast<uint8_t>('\n'));
            if (nl2 != d.end) {
                m.version = d.str_size(static_cast<size_t>(nl2 - d.p));
                d.u8();
                if (d.ok(8)) std::memcpy(&m.load, d.p, 8);
                return m;
            }
        }
        // Prefixed-u16 format (v2.0.7+).
        d.p = saved;
        m.hostname = d.str_prefixed_u16();
        m.version = d.str_prefixed_u16();
        if (d.ok(8)) std::memcpy(&m.load, d.p, 8);
        return m;
    }
    case 0x0A: return PingMsg{};
    case 0x0B: return PongMsg{};
    case 0x0C: {
        ScrollbackMsg m;
        m.total_lines = d.u32be();
        m.chunk_index = d.u32be();
        m.data = d.str_size(static_cast<size_t>(d.end - d.p));
        return m;
    }
    case 0x0D: { SignalMsg m; m.signal = static_cast<SignalMsg::SignalType>(d.u8()); m.process = d.str_prefixed_u16(); return m; }
    case 0x0E: { ExitCodeMsg m; m.code = static_cast<int32_t>(d.u32be()); return m; }
    case 0x0F: return ScrollbackAckMsg{};
    case 0x10: { SessionDiedMsg m; m.exit_code = static_cast<int32_t>(d.u32be()); m.signal_num = static_cast<int32_t>(d.u32be()); return m; }
    case 0x12: {
        ImageDataMsg m;
        m.format = d.u8();
        m.name = d.str_prefixed_u16();
        auto data_len = static_cast<size_t>(d.u32be());
        if (data_len > MAX_IMAGE_BYTES) throw std::runtime_error("image payload exceeds 50MB cap");
        m.data = d.bytes_size(data_len);
        return m;
    }
    case 0x13: {
        ImageFrameMsg m;
        m.format = d.u8();
        m.delay_ms = d.u32be();
        m.loop_count = d.u32be();
        auto data_len = static_cast<size_t>(d.u32be());
        if (data_len > MAX_IMAGE_BYTES) throw std::runtime_error("image frame payload exceeds 50MB cap");
        m.data = d.bytes_size(data_len);
        return m;
    }
    case 0x14: return ImageAckMsg{};
    case 0x15: {
        HelloMsg m;
        m.node_name = d.str_prefixed();
        m.version   = d.str_prefixed();
        m.pubkey_hex = d.str_prefixed();
        while (d.ok(1)) {
            PeerInfo pi;
            pi.name      = d.str_prefixed();
            pi.addr      = d.str_prefixed();
            pi.pubkey_hex = d.str_prefixed();
            pi.last_seen = d.u32be();
            m.known_peers.push_back(std::move(pi));
        }
        return m;
    }
    case 0x16: {
        GossipMsg m;
        while (d.ok(1)) {
            PeerInfo pi;
            pi.name      = d.str_prefixed();
            pi.addr      = d.str_prefixed();
            pi.pubkey_hex = d.str_prefixed();
            pi.last_seen = d.u32be();
            m.peers.push_back(std::move(pi));
        }
        return m;
    }
    case 0x17: {
        SessionSearchMsg m;
        m.session_name = d.str_prefixed();
        m.routing = d.str_prefixed();
        m.cols = d.u16();
        m.rows = d.u16();
        m.term = d.str_prefixed();
        return m;
    }
    case 0x18: {
        SdpOfferMsg m;
        m.peer_name = d.str_prefixed();
        m.sdp = d.str_prefixed_u16();
        return m;
    }
    case 0x19: {
        SdpAnswerMsg m;
        m.peer_name = d.str_prefixed();
        m.sdp = d.str_prefixed_u16();
        return m;
    }
    case 0x1A: {
        DhtFindNodeMsg m;
        auto target_bytes = d.bytes_size(32);
        std::copy(target_bytes.begin(), target_bytes.end(), m.target_id.begin());
        m.sender_name = d.str_prefixed();
        return m;
    }
    case 0x1B: {
        DhtFindValueMsg m;
        auto key_bytes = d.bytes_size(32);
        std::copy(key_bytes.begin(), key_bytes.end(), m.key.begin());
        m.sender_name = d.str_prefixed();
        return m;
    }
    case 0x1C: {
        FileMetaMsg m;
        m.filename = d.str_prefixed();
        uint64_t hi = d.u32be();
        uint64_t lo = d.u32be();
        m.filesize = (hi << 32) | lo;
        m.checksum = d.str_prefixed();
        m.total_chunks = d.u32be();
        return m;
    }
    case 0x1D: {
        FileChunkMsg m;
        m.chunk_index = d.u32be();
        m.total_chunks = d.u32be();
        uint32_t sz = d.u32be();
        if (sz > 0) m.data = d.bytes_size(sz);
        return m;
    }
    case 0x1E: {
        FileAckMsg m;
        m.chunk_index = d.u32be();
        m.next_requested = d.u32be();
        m.error = d.u8() != 0;
        m.error_msg = d.str_prefixed_u16();
        return m;
    }
    case 0x1F: {
        FileRequestMsg m;
        m.path = d.str_prefixed_u16();
        return m;
    }
    }

    throw std::runtime_error("unknown message type: " + std::to_string(type_byte));
}

size_t max_encoded_size(const Message&) {
    return MAX_FRAME_SIZE;
}

// ────────────────────────────────────────────────────────────────────
// 3. TLS TRANSPORT (ed25519 mTLS, unified Listen/Connect)
// ────────────────────────────────────────────────────────────────────

// ── RAII deleters ───────────────────────────────────────────────

struct SslCtxDeleter { void operator()(SSL_CTX* ctx) noexcept { SSL_CTX_free(ctx); } };
struct SslDeleter    { void operator()(SSL* ssl) noexcept    { SSL_free(ssl);     } };
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
using SslPtr    = std::unique_ptr<SSL, SslDeleter>;

// ── enum for TLS mode ───────────────────────────────────────────

enum class TlsMode { Listen, Connect };

// ── NodeTlsConfig ───────────────────────────────────────────────

struct NodeTlsConfig {
    std::string cert_file;
    std::string key_file;
    std::string authorized_keys_file;           // for Listen mode
    std::function<bool(const std::string&)> tofu_cb;  // for Connect mode
};

// ── Internal helpers ────────────────────────────────────────────

// Forward decl at bs::mesh scope so the TLS verify callbacks (in the anonymous
// namespace below) can emit R1.1/R1.2 accept/reject logs. Defined ~line 2350.
inline void log_event(const std::string& event, const std::string& detail);

namespace {

// ── BIO helpers ──────────────────────────────────────────────

std::string bio_to_string(BIO* bio) {
    char* data = nullptr;
    long len = BIO_get_mem_data(bio, &data);
    return std::string(data, len);
}

// ── PEM I/O ──────────────────────────────────────────────────

std::string cert_to_pem(X509* cert) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) throw std::runtime_error("BIO_new failed");
    PEM_write_bio_X509(bio, cert);
    auto s = bio_to_string(bio);
    BIO_free(bio);
    return s;
}

std::string key_to_pem(EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (!bio) throw std::runtime_error("BIO_new failed");
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    auto s = bio_to_string(bio);
    BIO_free(bio);
    return s;
}

EVP_PKEY* key_from_pem(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (!bio) return nullptr;
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

// ── Key generation ────────────────────────────────────────────

std::pair<EVP_PKEY*, X509*> generate_ed25519_cert(const char* cn) {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!pctx) throw std::runtime_error("EVP_PKEY_CTX_new_id(ED25519) failed");

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        throw std::runtime_error("ed25519 keygen failed");
    }
    EVP_PKEY_CTX_free(pctx);

    X509* cert = X509_new();
    if (!cert) { EVP_PKEY_free(pkey); throw std::runtime_error("X509_new failed"); }
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 365LL * 24LL * 3600LL * 10LL);
    X509_set_pubkey(cert, pkey);

    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(cn), -1, -1, 0);
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, pkey, nullptr) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        throw std::runtime_error("X509_sign failed");
    }
    return {pkey, cert};
}

// ── Public key helpers ────────────────────────────────────────

std::vector<uint8_t> extract_raw_pubkey(EVP_PKEY* key) {
    std::vector<uint8_t> raw(32);
    size_t len = 32;
    if (EVP_PKEY_get_raw_public_key(key, raw.data(), &len) <= 0)
        return {};
    raw.resize(len);
    return raw;
}

std::string pubkey_hex(EVP_PKEY* key) {
    auto raw = extract_raw_pubkey(key);
    if (raw.empty()) return "";
    std::string hex;
    for (auto b : raw) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", b);
        hex += buf;
    }
    return hex;
}

std::vector<uint8_t> hex_decode(const std::string& hex) {
    if ((hex.size() % 2) != 0) return {};
    std::vector<uint8_t> raw;
    raw.reserve(hex.size() / 2);
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        raw.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return raw;
}

namespace {

bool write_private_text_file_impl(const std::string& path,
                                  std::string_view content,
                                  bool append) {
#ifdef _WIN32
    const int flags = _O_WRONLY | _O_CREAT | _O_BINARY |
                      (append ? _O_APPEND : _O_TRUNC);
    const int fd = _open(path.c_str(), flags, _S_IREAD | _S_IWRITE);
    if (fd < 0) return false;
    bool ok = _chmod(path.c_str(), _S_IREAD | _S_IWRITE) == 0;
    size_t offset = 0;
    while (ok && offset < content.size()) {
        const size_t remaining = content.size() - offset;
        const unsigned int chunk = static_cast<unsigned int>(
            std::min<size_t>(remaining, 1u << 30));
        const int written = _write(fd, content.data() + offset, chunk);
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && _commit(fd) != 0) ok = false;
    if (_close(fd) != 0) ok = false;
    return ok;
#else
    const int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC)
#ifdef O_CLOEXEC
                      | O_CLOEXEC
#endif
                      ;
    const int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    bool ok = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0;
    size_t offset = 0;
    while (ok && offset < content.size()) {
        const ssize_t written = ::write(fd, content.data() + offset,
                                        content.size() - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            ok = false;
            break;
        }
        offset += static_cast<size_t>(written);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    return ok;
#endif
}

} // anonymous namespace

[[nodiscard]] bool write_private_text_file(const std::string& path,
                                           std::string_view content) {
    return write_private_text_file_impl(path, content, false);
}

[[nodiscard]] bool append_private_text_file(const std::string& path,
                                            std::string_view content) {
    return write_private_text_file_impl(path, content, true);
}

[[nodiscard]] bool ensure_private_directory(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) return false;
#ifndef _WIN32
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace, ec);
    if (ec) return false;
#endif
    return true;
}

[[nodiscard]] bool restrict_private_file_permissions(const std::string& path) {
#ifdef _WIN32
    return ::_chmod(path.c_str(), _S_IREAD | _S_IWRITE) == 0;
#else
    return ::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0;
#endif
}

[[nodiscard]] int run_editor_process(const std::string& editor,
                                     const std::string& local_path) {
    if (editor.empty() || local_path.empty()) return -1;
#ifdef _WIN32
    const intptr_t result = _spawnlp(_P_WAIT, editor.c_str(), editor.c_str(),
                                     local_path.c_str(), nullptr);
    return result < 0 ? -1 : static_cast<int>(result);
#else
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        ::execlp(editor.c_str(), editor.c_str(), local_path.c_str(),
                 static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
#endif
}

} // anonymous namespace

// ── AuthorizedKeys ────────────────────────────────────────────

struct AuthorizedKeys {
    std::vector<std::vector<uint8_t>> keys;
    std::string file_path;  // stored for R4.1 hot-reload

    void load_from_file(const std::string& path) {
        file_path = path;
        keys.clear();
        if (path.empty()) return;
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            auto hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                   line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (!line.empty()) {
                auto raw = hex_decode(line);
                if (raw.size() == 32) keys.push_back(std::move(raw));
            }
        }
    }

    // R4.1: reload from disk — called per-accept so revocations take effect immediately
    void reload() { if (!file_path.empty()) load_from_file(file_path); }

    bool contains(const std::vector<uint8_t>& key) const {
        for (auto& k : keys) if (k == key) return true;
        return false;
    }

#ifdef BS_TESTING
    // Convenience for tests: accept a lowercase hex pubkey string
    bool is_authorized(const std::string& hex) const {
        if (hex.size() % 2 != 0) return false;
        std::vector<uint8_t> raw;
        raw.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            unsigned int b = 0;
            if (std::sscanf(hex.c_str() + i, "%2x", &b) != 1) return false;
            raw.push_back(static_cast<uint8_t>(b));
        }
        return contains(raw);
    }
#endif
};

// ── Custom cert verify callbacks ──────────────────────────────

namespace {

// Local bytes->hex for verify-callback logging (avoids depending on later helpers).
inline std::string verify_bytes_hex(const std::vector<uint8_t>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (uint8_t c : b) { s.push_back(d[c >> 4]); s.push_back(d[c & 0xF]); }
    return s;
}

// Server: verifies client's ed25519 raw public key against authorized_keys (R4.1: reloads per-accept)
int server_cert_verify_cb(X509_STORE_CTX* ctx, void* arg) {
    auto* auth = static_cast<AuthorizedKeys*>(arg);
    auth->reload();  // R4.1: pick up key additions/revocations without restart
    X509* cert = X509_STORE_CTX_get0_cert(ctx);
    if (!cert) return 0;
    EVP_PKEY* pk = X509_get0_pubkey(cert);
    if (!pk) return 0;
    auto raw = extract_raw_pubkey(pk);
    if (raw.empty()) return 0;
    std::string pk_hex = verify_bytes_hex(raw);
    if (auth->contains(raw)) {
        log_event("tls_verify_server", pk_hex + " result=accept");  // R1.1
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    log_event("tls_verify_server", pk_hex + " result=reject");  // R1.1
    return 0;
}

// Client: TOFU via SHA-256 fingerprint callback
int client_cert_verify_cb(X509_STORE_CTX* ctx, void* arg) {
    auto* cb = static_cast<std::function<bool(const std::string&)>*>(arg);
    X509* cert = X509_STORE_CTX_get0_cert(ctx);
    if (!cert) return 0;
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (!X509_digest(cert, EVP_sha256(), md, &len)) return 0;
    std::string fp;
    for (unsigned int i = 0; i < len; ++i) {
        char h[3];
        snprintf(h, sizeof(h), "%02x", md[i]);
        fp += h;
    }
    if ((*cb)(fp)) {
        log_event("tls_verify_client", fp + " result=accept");  // R1.2
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    log_event("tls_verify_client", fp + " result=reject");  // R1.2
    return 0;
}

void free_owned_authorized_keys(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
    delete static_cast<AuthorizedKeys*>(ptr);
}

void free_owned_tofu_callback(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*) {
    delete static_cast<std::function<bool(const std::string&)>*>(ptr);
}

int owned_authorized_keys_index() {
    static const int index = SSL_CTX_get_ex_new_index(
        0, nullptr, nullptr, nullptr, free_owned_authorized_keys);
    return index;
}

int owned_tofu_callback_index() {
    static const int index = SSL_CTX_get_ex_new_index(
        0, nullptr, nullptr, nullptr, free_owned_tofu_callback);
    return index;
}

} // anonymous namespace

// ── Public: generate_cert_key_pair ────────────────────────────────

std::pair<std::string, std::string> generate_cert_key_pair(const char* common_name) {
    auto [pkey, cert] = generate_ed25519_cert(common_name);
    auto c = cert_to_pem(cert);
    auto k = key_to_pem(pkey);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    return {c, k};
}

// ── Public: pubkey_hex_from_pem ──────────────────────────────────

std::string pubkey_hex_from_pem(const std::string& key_pem) {
    EVP_PKEY* pkey = key_from_pem(key_pem);
    if (!pkey) return "";
    auto hex = pubkey_hex(pkey);
    EVP_PKEY_free(pkey);
    return hex;
}

// ── Public: peer_public_key_hex ──────────────────────────────────

std::string peer_public_key_hex(SSL* ssl) {
    if (!ssl) return "";
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return "";
    EVP_PKEY* pkey = X509_get0_pubkey(cert);
    std::string hex = pkey ? pubkey_hex(pkey) : "";
    X509_free(cert);
    return hex;
}

// R1.4: one-line cert subject for handshake observability logs
std::string peer_cert_subject_oneline(SSL* ssl) {
    if (!ssl) return "";
    X509* cert = SSL_get1_peer_certificate(ssl);
    if (!cert) return "";
    char* subj = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
    std::string s = subj ? subj : "";
    OPENSSL_free(subj);
    X509_free(cert);
    return s;
}

// ── Public: bootstrap_identity ───────────────────────────────────
// Auto-generate ed25519 keypair on first run into ~/.bridgesessions/

void bootstrap_identity(const std::string& home_dir) {
    namespace fs = std::filesystem;
    fs::path dir(home_dir);
    if (!ensure_private_directory(dir.string()))
        throw std::runtime_error("cannot create private identity directory " + dir.string());

    // If the standard identity already exists, nothing to do
    fs::path id_key   = dir / "id_ed25519.pem";
    fs::path id_cert  = dir / "id_ed25519-cert.pem";
    fs::path id_pub   = dir / "id_ed25519.pub";

    if (fs::exists(id_key)) {
        for (const auto& path : {id_key, id_cert, id_pub}) {
            if (fs::exists(path) && !restrict_private_file_permissions(path.string()))
                throw std::runtime_error("cannot restrict permissions on " + path.string());
        }
        return;
    }

    // Migration: if legacy _bs_autocert.pem + _bs_autokey.pem exist, copy them
    fs::path legacy_cert = dir / "_bs_autocert.pem";
    fs::path legacy_key  = dir / "_bs_autokey.pem";

    if (fs::exists(legacy_cert) && fs::exists(legacy_key)) {
        const auto read_text = [](const fs::path& path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) throw std::runtime_error("cannot read " + path.string());
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        const std::string cert_pem = read_text(legacy_cert);
        const std::string key_pem = read_text(legacy_key);
        if (!write_private_text_file(id_cert.string(), cert_pem) ||
            !write_private_text_file(id_key.string(), key_pem))
            throw std::runtime_error("cannot securely migrate legacy identity");

        // Also generate the .pub file from the migrated key
        std::string hex = pubkey_hex_from_pem(key_pem);
        if (hex.empty() || !write_private_text_file(id_pub.string(), hex + "\n"))
            throw std::runtime_error("cannot securely write migrated public key");
        return;
    }

    // Fresh bootstrap: generate keypair
    auto [cert_pem, key_pem] = generate_cert_key_pair("bridgesessions");
    std::string pubkey_hex = pubkey_hex_from_pem(key_pem);

    if (!write_private_text_file(id_key.string(), key_pem) ||
        !write_private_text_file(id_cert.string(), cert_pem) ||
        !write_private_text_file(id_pub.string(), pubkey_hex + "\n"))
        throw std::runtime_error("cannot securely write generated identity");
}

// ── Public: unified create_node_tls ──────────────────────────────
//
// auth_storage / tofu_storage: optional caller-owned storage for the
// cert-verify callback context. When supplied, the context is written there
// and NOT heap-allocated, so the caller controls its lifetime (must outlive the
// returned SSL_CTX). When null, fallback storage is attached to SSL_CTX ex-data
// and destroyed automatically with the context.

SslCtxPtr create_node_tls(const NodeTlsConfig& cfg, TlsMode mode,
                          AuthorizedKeys* auth_storage = nullptr,
                          std::function<bool(const std::string&)>* tofu_storage = nullptr) {
    SslCtxPtr ctx;

    if (mode == TlsMode::Listen) {
        ctx = SslCtxPtr(SSL_CTX_new(TLS_server_method()));
        if (!ctx) throw std::runtime_error("TLS_server_method failed");
    } else {
        ctx = SslCtxPtr(SSL_CTX_new(TLS_client_method()));
        if (!ctx) throw std::runtime_error("TLS_client_method failed");
    }

    // TLS 1.2+ (prefer 1.3). TLS 1.3-only handshakes stalled as SSL_ERROR_WANT_READ
    // across macOS/Linux/Windows Tailscale paths with self-signed Ed25519 certs
    // (fleet RCA). Product docs: TLS 1.2 minimum, TLS 1.3 preferred — not 1.3-only.
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
#if defined(TLS1_3_VERSION)
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION);
#endif

    // Load own certificate + key
    if (!cfg.cert_file.empty()) {
        if (SSL_CTX_use_certificate_file(ctx.get(), cfg.cert_file.c_str(),
                                          SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("load cert: " + cfg.cert_file);
    }
    if (!cfg.key_file.empty()) {
        if (SSL_CTX_use_PrivateKey_file(ctx.get(), cfg.key_file.c_str(),
                                         SSL_FILETYPE_PEM) <= 0)
            throw std::runtime_error("load key: " + cfg.key_file);
    }

    if (mode == TlsMode::Listen) {
        // Server: verify client cert + fail if no cert presented
        SSL_CTX_set_verify(ctx.get(),
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           nullptr);

        AuthorizedKeys* auth = auth_storage;
        std::unique_ptr<AuthorizedKeys> owned_auth;
        if (!auth) {
            owned_auth = std::make_unique<AuthorizedKeys>();
            auth = owned_auth.get();
            const int index = owned_authorized_keys_index();
            if (index < 0 || SSL_CTX_set_ex_data(ctx.get(), index, auth) != 1) {
                throw std::runtime_error("attach authorized_keys callback storage failed");
            }
            (void)owned_auth.release();
        }
        if (!cfg.authorized_keys_file.empty())
            auth->load_from_file(cfg.authorized_keys_file);
        SSL_CTX_set_cert_verify_callback(ctx.get(), server_cert_verify_cb, auth);

        // TLS session cache — reuse sessions across reconnects
        SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ctx.get(), 256);
    } else {
        // Client: verify server cert via TOFU
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

        std::function<bool(const std::string&)>* cb = tofu_storage;
        if (cb) *cb = cfg.tofu_cb;
        else {
            auto owned_cb = std::make_unique<std::function<bool(const std::string&)>>(cfg.tofu_cb);
            cb = owned_cb.get();
            const int index = owned_tofu_callback_index();
            if (index < 0 || SSL_CTX_set_ex_data(ctx.get(), index, cb) != 1) {
                throw std::runtime_error("attach TOFU callback storage failed");
            }
            (void)owned_cb.release();
        }
        SSL_CTX_set_cert_verify_callback(ctx.get(), client_cert_verify_cb, cb);
    }

    return ctx;
}

// ────────────────────────────────────────────────────────────────────
// 4. FRAME I/O (ssl_check, read_frame, write_frame)
// ────────────────────────────────────────────────────────────────────

// SSL_get_error() is only reliable when the calling thread's OpenSSL error
// queue was empty before the I/O operation. The mesh event loop also performs
// outbound handshakes on this thread; a failed dial can otherwise poison the
// next healthy connection's SSL_read_ex() classification and turn WANT_READ
// into a fatal SSL_ERROR_SSL.
inline void clear_stale_ssl_errors_before_io() {
    ERR_clear_error();
}

namespace {

void ssl_check(int ret, SSL* ssl, const char* op) {
    if (ret <= 0) {
        int err = SSL_get_error(ssl, ret);
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        throw std::runtime_error(std::string(op) + " failed: SSL error " + std::to_string(err) + " " + buf);
    }
}

} // anonymous namespace

Message read_frame(SSL* ssl) {
    // Read header
    uint8_t header[FRAME_HEADER_SIZE];
    size_t total = 0;
    while (total < FRAME_HEADER_SIZE) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_read_ex(ssl, header + total, FRAME_HEADER_SIZE - total, &n);
        ssl_check(ret, ssl, "SSL_read header");
        total += n;
    }

    uint16_t length = read_u16(header + 4);

    if (length > MAX_FRAME_SIZE)
        throw std::runtime_error("frame payload exceeds MAX_FRAME_SIZE");

    // Read payload
    std::vector<uint8_t> raw(FRAME_HEADER_SIZE + length);
    std::memcpy(raw.data(), header, FRAME_HEADER_SIZE);

    if (length > 0) {
        total = 0;
        while (total < length) {
            size_t n = 0;
            clear_stale_ssl_errors_before_io();
            int ret = SSL_read_ex(ssl, raw.data() + FRAME_HEADER_SIZE + total, length - total, &n);
            ssl_check(ret, ssl, "SSL_read payload");
            total += n;
        }
    }

    return decode(raw);
}

[[nodiscard]] inline bool buffered_bytes_hold_complete_frame(
    std::span<const uint8_t> bytes) {
    if (bytes.size() < FRAME_HEADER_SIZE) return false;
    const uint16_t length = read_u16(bytes.data() + 4);
    if (length > MAX_FRAME_SIZE) return true;  // let decode surface the protocol error
    return bytes.size() >= FRAME_HEADER_SIZE + length;
}

[[nodiscard]] inline std::vector<Message> drain_complete_frames(
    std::vector<uint8_t>& buffered) {
    std::vector<Message> messages;
    size_t consumed = 0;
    while (buffered.size() - consumed >= FRAME_HEADER_SIZE) {
        const uint8_t* start = buffered.data() + consumed;
        const uint16_t length = read_u16(start + 4);
        if (length > MAX_FRAME_SIZE)
            throw std::runtime_error("frame payload exceeds MAX_FRAME_SIZE");
        const size_t frame_size = FRAME_HEADER_SIZE + length;
        if (buffered.size() - consumed < frame_size) break;
        messages.push_back(decode(std::span<const uint8_t>(start, frame_size)));
        consumed += frame_size;
    }
    if (consumed > 0)
        buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(consumed));
    return messages;
}

[[nodiscard]] inline bool ssl_has_complete_buffered_frame(SSL* ssl) {
    const int pending = SSL_pending(ssl);
    if (pending < static_cast<int>(FRAME_HEADER_SIZE)) return false;
    std::array<uint8_t, FRAME_HEADER_SIZE> header{};
    size_t peeked = 0;
    if (SSL_peek_ex(ssl, header.data(), header.size(), &peeked) <= 0 ||
        peeked < header.size()) return false;
    return buffered_bytes_hold_complete_frame(
        std::span<const uint8_t>(header.data(), header.size())) ||
        pending >= static_cast<int>(FRAME_HEADER_SIZE + read_u16(header.data() + 4));
}

void write_frame(SSL* ssl, const Message& msg, uint16_t stream_id) {
    auto frame = encode(msg, stream_id);

    size_t total = 0;
    while (total < frame.size()) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_write_ex(ssl, frame.data() + total, frame.size() - total, &n);
        ssl_check(ret, ssl, "SSL_write");
        total += n;
    }
}

// ── Non-blocking frame I/O helpers (for handshake state machine) ────
// These variants never block; they drain or emit what is immediately
// available and buffer the rest. They are used only during the initial
// TLS + Hello handshake so the event loop stays responsive.

[[nodiscard]] inline std::optional<Message> read_frame_nonblocking(
    SSL* ssl, std::vector<uint8_t>& rx_buffer, int* want_error = nullptr) {
    if (want_error) *want_error = SSL_ERROR_WANT_READ;
    for (;;) {
        std::array<uint8_t, 4096> chunk{};
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_read_ex(ssl, chunk.data(), chunk.size(), &n);
        if (ret > 0 && n > 0) {
            rx_buffer.insert(rx_buffer.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        int err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (want_error) *want_error = err;
            break;
        }
        if (err == SSL_ERROR_ZERO_RETURN) break;
        throw std::runtime_error("SSL_read failed during handshake");
    }
    if (buffered_bytes_hold_complete_frame(rx_buffer)) {
        auto messages = drain_complete_frames(rx_buffer);
        if (!messages.empty()) return std::move(messages.front());
    }
    return std::nullopt;
}

// Returns true when the encoded frame has been fully written.
// On WANT_READ/WANT_WRITE returns false and leaves unwritten bytes in tx_buffer.
[[nodiscard]] inline bool write_frame_nonblocking(
    SSL* ssl, const Message& msg, uint16_t stream_id, std::vector<uint8_t>& tx_buffer,
    int* want_error = nullptr) {
    if (want_error) *want_error = SSL_ERROR_WANT_WRITE;
    if (tx_buffer.empty()) tx_buffer = encode(msg, stream_id);
    while (!tx_buffer.empty()) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_write_ex(ssl, tx_buffer.data(), tx_buffer.size(), &n);
        if (ret > 0 && n > 0) {
            tx_buffer.erase(tx_buffer.begin(), tx_buffer.begin() + static_cast<std::ptrdiff_t>(n));
            continue;
        }
        int err = SSL_get_error(ssl, ret);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            if (want_error) *want_error = err;
            return false;
        }
        throw std::runtime_error("SSL_write failed during handshake");
    }
    return true;
}

// ────────────────────────────────────────────────────────────────────
// 5. OSC 52 CLIPBOARD SCANNER
// ────────────────────────────────────────────────────────────────────
// Scans PTY output for OSC 52 sequences, extracts base64 content,
// strips them from the terminal stream so they're never rendered.
//
// OSC 52 format:  ESC ] 52 ; Pc ; <base64> ST
//  - ESC = \x1b, ST = \x1b\\ or \x07 (BEL)
//  - Pc = clipboard target (c, p, q; usually 'c' for system clipboard)

#ifdef _WIN32
using ssize_t = long long;
#endif

struct Osc52Result {
    std::string cleaned_text;              // text with OSC 52 sequences stripped
    std::optional<std::string> clipboard_text;  // decoded clipboard content, if any
};

// Scan a buffer for OSC 52 sequences. Returns cleaned text + optional clipboard.
// Thread-safe: pure function, no shared state.
[[nodiscard]] Osc52Result scan_osc52(std::string_view input);

// v1.7.1: strip ANSI/VT escape sequences from PTY output for non-interactive
// one-shot exec capture (`shell <peer> --cmd ...`). ConPTY (and POSIX PTYs)
// emit cursor/title/screen-clear control sequences as a normal side effect
// of hosting even a single non-interactive command — a one-shot caller only
// wants the plain text. Handles CSI (ESC [ ... final-byte), OSC (ESC ]
// ... BEL or ESC \), and bare two-byte ESC sequences. Thread-safe: pure
// function, no shared state (called from the background exec thread).
[[nodiscard]] inline std::string strip_ansi_escapes(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == 0x1B && i + 1 < input.size()) { // ESC
            char next = input[i + 1];
            if (next == '[') { // CSI: ESC [ params... final-byte(@-~)
                size_t j = i + 2;
                while (j < input.size() &&
                       !(input[j] >= 0x40 && input[j] <= 0x7E)) ++j;
                i = (j < input.size()) ? j : input.size() - 1;
                continue;
            } else if (next == ']') { // OSC: ESC ] ... BEL or ST (ESC backslash)
                size_t j2 = i + 2;
                while (j2 < input.size() && input[j2] != '\a' &&
                       !(input[j2] == 0x1B && j2 + 1 < input.size() && input[j2 + 1] == '\\')) ++j2;
                if (j2 < input.size() && input[j2] == '\a') { i = j2; continue; }
                if (j2 + 1 < input.size()) { i = j2 + 1; continue; }
                i = input.size() - 1;
                continue;
            } else {
                // Bare 2-byte escape (e.g. ESC = , ESC > )
                i += 1;
                continue;
            }
        }
        // Drop other C0 control chars that don't carry text meaning, but
        // keep \n, \r, \t.
        if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') continue;
        out.push_back(input[i]);
    }
    return out;
}

// ── Internal helpers ────────────────────────────────────────────

namespace detail {

// Find the end of an OSC sequence starting at pos (after ESC ] 52 ;)
inline std::ptrdiff_t find_osc_end(std::string_view s, size_t start) {
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] == '\x07') return (std::ptrdiff_t)i;           // BEL terminator
        if (s[i] == '\x1b' && i+1 < s.size() && s[i+1] == '\\')
            return (std::ptrdiff_t)i;                            // ST terminator (ESC \)
    }
    return -1; // incomplete sequence — leave in stream
}

// Extract and decode base64 content from OSC 52 body.
// Body is "Pc;<base64>" — the "52;" prefix is already stripped.
inline std::optional<std::string> decode_osc52_body(std::string_view body) {
    auto semicolon = body.find(';');
    if (semicolon == std::string_view::npos) return std::nullopt;

    std::string_view b64 = body.substr(semicolon + 1);
    if (b64.empty()) return std::nullopt;  // empty clipboard = clear

    // Base64 decode (hand-rolled to avoid OpenSSL dependency for this)
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve(b64.size() * 3 / 4);

    int acc = 0, bits = 0;
    for (char c : b64) {
        if (c == '=') break;
        const char* p = std::strchr(kTable, c);
        if (!p) continue;  // skip whitespace / invalid chars
        acc = (acc << 6) | (int)(p - kTable);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back((char)(acc >> bits));
            acc &= (1 << bits) - 1;
        }
    }
    return result;
}

} // namespace detail

inline Osc52Result scan_osc52(std::string_view input) {
    Osc52Result result;
    result.cleaned_text.reserve(input.size());

    size_t pos = 0;
    while (pos < input.size()) {
        // Look for ESC ] (OSC introducer)
        if (input[pos] == '\x1b' && pos + 1 < input.size() && input[pos + 1] == ']') {
            // Check for "52;" after ESC ]
            size_t after_esc = pos + 2;
            if (after_esc + 2 < input.size() &&
                input[after_esc] == '5' && input[after_esc+1] == '2' && input[after_esc+2] == ';')
            {
                // OSC 52 sequence found — find the end
                auto end = detail::find_osc_end(input, after_esc + 3);
                if (end >= 0) {
                    // Extract the body (between "52;" and ST/BEL)
                    std::string_view body = input.substr(
                        after_esc + 3, (size_t)end - (after_esc + 3));
                    auto decoded = detail::decode_osc52_body(body);
                    if (decoded && !decoded->empty()) {
                        result.clipboard_text = std::move(*decoded);
                    }
                    // Skip past the terminator
                    pos = (size_t)end;
                    if (input[pos] == '\x1b' && pos + 1 < input.size() && input[pos+1] == '\\')
                        pos += 2;
                    else if (input[pos] == '\x07')
                        pos += 1;
                    continue;
                }
                // Incomplete — leave in stream
            }
        }
        // Regular character — pass through
        result.cleaned_text.push_back(input[pos]);
        ++pos;
    }

    return result;
}

// ────────────────────────────────────────────────────────────────────
// 6. SESSION & PTY (ported from bs-server, namespace bs::mesh)
// ────────────────────────────────────────────────────────────────────

// ── PtyError ──────────────────────────────────────────────────────

struct PtyError {
    std::string message;
};

// ── SessionState ──────────────────────────────────────────────────

enum class SessionState : uint8_t {
    Created,
    Running,
    Detached,
    Attached,
    Died,
    Exited,
    Killed,
    Recoverable,
};

inline const char* session_state_str(SessionState s) {
    switch (s) {
        case SessionState::Created:     return "created";
        case SessionState::Running:     return "running";
        case SessionState::Detached:    return "detached";
        case SessionState::Attached:    return "attached";
        case SessionState::Died:        return "died";
        case SessionState::Exited:      return "exited";
        case SessionState::Killed:      return "killed";
        case SessionState::Recoverable: return "recoverable";
    }
    return "unknown";
}

// ── Session struct ────────────────────────────────────────────────

// Default ring buffer: 1 MB (2^20)
constexpr size_t kDefaultRingBufferSize = 1'048'576;

struct Session {
    std::string name;
    std::vector<std::string> peer_ids; // pubkey hex of all peers currently attached (empty if detached)
    std::string command;
    std::string detach_signal; // optional HUP/TERM/INT/KILL requested by the attaching peer
#ifdef _WIN32
    HANDLE master_fd = nullptr;     // ConPTY output read handle (child stdout -> server)
    HANDLE child_pid = nullptr;     // process handle
    HANDLE write_handle = nullptr;  // ConPTY input write handle (server -> child stdin)
    HPCON hpcon = nullptr;          // for ResizePseudoConsole
#else
    int master_fd = -1;
    int child_pid = -1;
    // v2.0.6: bounded pending input queue for nonblocking PTY master writes.
    // Keystrokes/clipboard pasted faster than the child consumes are queued
    // here and drained by the event loop. High/low water marks apply
    // backpressure by temporarily skipping reads from the attached peer.
    std::string pending_input;
    bool input_backpressured = false;
    static constexpr size_t kPtyInputHighWater = 64 * 1024;
    static constexpr size_t kPtyInputLowWater  = 16 * 1024;
    static constexpr size_t kPtyInputMax       = 256 * 1024;
#endif
    SessionState state = SessionState::Created;

    RingBuffer<kDefaultRingBufferSize> scrollback;

    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_output_at;
    std::chrono::steady_clock::time_point last_attach_at;

    bool auto_restart = false;
    int restart_failures = 0;
    std::chrono::steady_clock::time_point restart_window_start;
    bool history_recorded = false;

    // Monotonic spawn generation. Bumped every time a child process is spawned
    // (initial create + each auto-restart/resurrect). Unlike child_pid/HANDLE,
    // this never repeats, so callers/tests can reliably detect a respawn even
    // when the OS recycles the freed PID or HANDLE-table slot.
    uint64_t generation = 0;

    Session();
    ~Session();

    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void touch_output();
    void reset_restart_failures();
#ifdef _WIN32
    bool is_valid() const { return master_fd != nullptr; }
    bool is_pollable() const { return master_fd != nullptr && child_pid != nullptr; }
#else
    bool is_valid() const { return master_fd >= 0; }
    bool is_pollable() const { return master_fd >= 0 && child_pid > 0; }
#endif
};

// ── Session implementation ────────────────────────────────────────

// Process-wide monotonic spawn counter. Every successful child spawn gets a
// unique value, so a respawn is detectable even when the OS recycles the freed
// PID/HANDLE. Starts at 1 so 0 reliably means "never spawned".
static std::atomic<uint64_t> g_session_generation{0};

Session::Session()
    : created_at(std::chrono::steady_clock::now())
    , last_output_at(created_at)
    , last_attach_at(created_at)
{}

Session::~Session() {
#ifdef _WIN32
    if (master_fd) {
        CloseHandle(master_fd);
        master_fd = nullptr;
    }
    if (write_handle) {
        CloseHandle(write_handle);
        write_handle = nullptr;
    }
    if (child_pid) {
        TerminateProcess(child_pid, 1);
        WaitForSingleObject(child_pid, 5000);
        CloseHandle(child_pid);
        child_pid = nullptr;
    }
    if (hpcon) {
        ClosePseudoConsole(hpcon);
        hpcon = nullptr;
    }
#else
    if (master_fd >= 0) {
        close(master_fd);
        master_fd = -1;
    }
    if (child_pid > 0) {
        kill(child_pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 50; ++i) {
            if (waitpid(child_pid, &status, WNOHANG) == child_pid) break;
            usleep(100000);
        }
        if (waitpid(child_pid, &status, WNOHANG) != child_pid) {
            kill(child_pid, SIGKILL);
            waitpid(child_pid, &status, 0);
        }
        child_pid = -1;
    }
#endif
}

Session::Session(Session&& other) noexcept
    : name(std::move(other.name))
    , peer_ids(std::move(other.peer_ids))
    , command(std::move(other.command))
    , detach_signal(std::move(other.detach_signal))
    , master_fd(other.master_fd)
    , child_pid(other.child_pid)
#ifndef _WIN32
    , pending_input(std::move(other.pending_input))
    , input_backpressured(other.input_backpressured)
#endif
    , state(other.state)
    , scrollback(std::move(other.scrollback))
    , created_at(other.created_at)
    , last_output_at(other.last_output_at)
    , last_attach_at(other.last_attach_at)
    , auto_restart(other.auto_restart)
    , restart_failures(other.restart_failures)
    , restart_window_start(other.restart_window_start)
    , generation(other.generation)
{
#ifdef _WIN32
    write_handle = other.write_handle;
    hpcon = other.hpcon;
    other.write_handle = nullptr;
    other.hpcon = nullptr;
    other.master_fd = nullptr;
    other.child_pid = nullptr;
#else
    other.master_fd = -1;
    other.child_pid = -1;
#endif
}

Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        this->~Session();
        name = std::move(other.name);
        peer_ids = std::move(other.peer_ids);
        command = std::move(other.command);
        detach_signal = std::move(other.detach_signal);
        master_fd = other.master_fd;
        child_pid = other.child_pid;
#ifndef _WIN32
        pending_input = std::move(other.pending_input);
        input_backpressured = other.input_backpressured;
#endif
        state = other.state;
        scrollback = std::move(other.scrollback);
        created_at = other.created_at;
        last_output_at = other.last_output_at;
        last_attach_at = other.last_attach_at;
        auto_restart = other.auto_restart;
        restart_failures = other.restart_failures;
        restart_window_start = other.restart_window_start;
        generation = other.generation;
#ifdef _WIN32
        write_handle = other.write_handle;
        hpcon = other.hpcon;
        other.write_handle = nullptr;
        other.hpcon = nullptr;
        other.master_fd = nullptr;
        other.child_pid = nullptr;
#else
        other.master_fd = -1;
        other.child_pid = -1;
#endif
    }
    return *this;
}

void Session::touch_output() {
    last_output_at = std::chrono::steady_clock::now();
}

void Session::reset_restart_failures() {
    restart_failures = 0;
    restart_window_start = std::chrono::steady_clock::now();
}

// ── PTY functions ─────────────────────────────────────────────────

#ifdef _WIN32

[[nodiscard]] std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return std::wstring(value.begin(), value.end());
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

[[nodiscard]] std::wstring first_windows_command_token(
    const std::wstring& command_line) {
    size_t begin = command_line.find_first_not_of(L" \t");
    if (begin == std::wstring::npos) return {};
    if (command_line[begin] == L'\"') {
        const size_t end = command_line.find(L'\"', begin + 1);
        if (end == std::wstring::npos) return {};
        return command_line.substr(begin + 1, end - begin - 1);
    }
    const size_t end = command_line.find_first_of(L" \t", begin);
    return command_line.substr(begin, end == std::wstring::npos
                                      ? std::wstring::npos
                                      : end - begin);
}

[[nodiscard]] std::wstring resolve_windows_application(
    const std::wstring& command_line) {
    const std::wstring token = first_windows_command_token(command_line);
    if (token.empty()) return {};
    std::vector<wchar_t> resolved(32768, L'\0');
    const DWORD len = SearchPathW(nullptr, token.c_str(), nullptr,
                                  static_cast<DWORD>(resolved.size()),
                                  resolved.data(), nullptr);
    if (len == 0 || len >= resolved.size()) return {};
    return std::wstring(resolved.data(), len);
}

// Forward decls — full definitions live next to prepare_session_command.
[[nodiscard]] bool is_windows_cli_oneshot_command(const std::string& command);
[[nodiscard]] bool command_has_direct_windows_exe_token(const std::string& command);

[[nodiscard]] std::expected<Session, PtyError> create_session(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    // v2.0.1: one-shot commands (cmd /c …) skip ConPTY. ConPTY/conhost often
    // only delivers mode CSI to the pipe while command text never arrives
    // before SessionDied (fleet RCA). Plain anonymous pipes capture stdout
    // reliably for health/shell --cmd.
    // v2.0.2: also treat direct powershell/pwsh -Command/-File one-shots as
    // pipe captures (they no longer go through cmd /c, so /c detection alone
    // is insufficient).
    const bool oneshot = is_windows_cli_oneshot_command(command);
    if (oneshot) {
        HANDLE out_r = nullptr, out_w = nullptr, in_r = nullptr, in_w = nullptr;
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        if (!CreatePipe(&out_r, &out_w, &sa, 0))
            return std::unexpected(PtyError{"CreatePipe(out) failed"});
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) {
            CloseHandle(out_r); CloseHandle(out_w);
            return std::unexpected(PtyError{"CreatePipe(in) failed"});
        }
        // Parent keeps out_r / in_w non-inheritable; child gets out_w / in_r.
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out_w;
        si.hStdError = out_w;
        si.hStdInput = in_r;

        std::wstring cmdline = utf8_to_wide(command);
        std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
        mutable_cmdline.push_back(L'\0');
        const std::wstring application = resolve_windows_application(cmdline);

        PROCESS_INFORMATION pi{};
        BOOL created = CreateProcessW(
            application.empty() ? nullptr : application.c_str(),
            mutable_cmdline.data(),
            nullptr, nullptr,
            TRUE,  // inherit the pipe ends we left inheritable
            CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr, nullptr,
            &si, &pi);
        const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
        // Parent must close the ends it handed to the child.
        CloseHandle(out_w);
        CloseHandle(in_r);
        if (!created) {
            CloseHandle(out_r);
            CloseHandle(in_w);
            return std::unexpected(PtyError{
                "CreateProcessW(oneshot) failed: " + std::to_string(create_error)});
        }
        CloseHandle(pi.hThread);

        Session s;
        s.name = name;
        s.command = command;
        s.master_fd = out_r;
        s.write_handle = in_w;
        s.child_pid = pi.hProcess;
        s.hpcon = nullptr;
        s.generation = ++g_session_generation;
        s.state = SessionState::Running;
        log_event("session_oneshot_pipes", name);
        (void)cols; (void)rows; (void)term;
        return s;
    }

    HANDLE hPipeInRead = nullptr, hPipeInWrite = nullptr;
    HANDLE hPipeOutRead = nullptr, hPipeOutWrite = nullptr;

    // Create pipes for ConPTY
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE}; // inheritable
    if (!CreatePipe(&hPipeInRead, &hPipeInWrite, &sa, 0))
        return std::unexpected(PtyError{"CreatePipe(in) failed"});
    if (!CreatePipe(&hPipeOutRead, &hPipeOutWrite, &sa, 0)) {
        CloseHandle(hPipeInRead); CloseHandle(hPipeInWrite);
        return std::unexpected(PtyError{"CreatePipe(out) failed"});
    }

    // Create pseudo console
    COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    HPCON hPC = nullptr;
    HRESULT hr = CreatePseudoConsole(size, hPipeInRead, hPipeOutWrite, 0, &hPC);
    if (FAILED(hr)) {
        CloseHandle(hPipeInRead); CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead); CloseHandle(hPipeOutWrite);
        return std::unexpected(PtyError{"CreatePseudoConsole failed: " + std::to_string(hr)});
    }

    // Keep the ends passed to CreatePseudoConsole open until the child has
    // been attached with CreateProcessW. Closing them before attachment can
    // tear down the pseudoconsole and leave an HPCON that rejects resize.

    // The daemon-side pipe ends must never be inherited by the child. ConPTY
    // owns the opposite ends and brokers the child's standard streams itself.
    if (!SetHandleInformation(hPipeInWrite, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(hPipeOutRead, HANDLE_FLAG_INHERIT, 0)) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "SetHandleInformation failed: " + std::to_string(GetLastError())});
    }

    // Set up STARTUPINFOEX for the child process
    STARTUPINFOEXW siEx{};
    siEx.StartupInfo.cb = sizeof(siEx);

    // Add the ConPTY to the process attribute list.
    SIZE_T attrSize = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    if (attrSize == 0) {
        const DWORD error = GetLastError();
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "InitializeProcThreadAttributeList sizing failed: " +
            std::to_string(error)});
    }
    siEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!siEx.lpAttributeList) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{"HeapAlloc for process attributes failed"});
    }
    if (!InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0,
                                           &attrSize)) {
        const DWORD error = GetLastError();
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "InitializeProcThreadAttributeList failed: " +
            std::to_string(error)});
    }
    if (!UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(HPCON),
            nullptr, nullptr)) {
        const DWORD error = GetLastError();
        DeleteProcThreadAttributeList(siEx.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInRead);
        CloseHandle(hPipeOutWrite);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "UpdateProcThreadAttribute failed: " + std::to_string(error)});
    }

    // Named/default shell profiles are complete command lines and become the
    // ConPTY root process directly. Client one-shot commands are wrapped once
    // by prepare_session_command before reaching this function.
    std::wstring cmdline = utf8_to_wide(command);
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');
    const std::wstring application = resolve_windows_application(cmdline);

    // Set TERM environment for terminal-aware children.
    const std::wstring wide_term = utf8_to_wide(term);
    SetEnvironmentVariableW(L"TERM", wide_term.c_str());

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(
        application.empty() ? nullptr : application.c_str(),
        mutable_cmdline.data(),
        nullptr, nullptr,           // process/thread security
        FALSE,                      // ConPTY child inherits no daemon handles
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_PROCESS_GROUP,
        nullptr,                    // environment (use parent's)
        nullptr,                    // current directory
        &siEx.StartupInfo,
        &pi);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();

    // The pseudoconsole duplicated these ends. Release our references only
    // after the child process has been attached.
    CloseHandle(hPipeInRead);
    CloseHandle(hPipeOutWrite);
    hPipeInRead = nullptr;
    hPipeOutWrite = nullptr;

    DeleteProcThreadAttributeList(siEx.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);

    if (!created) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{
            "CreateProcessW failed: " + std::to_string(create_error)});
    }

    CloseHandle(pi.hThread);

    Session s;
    s.name = name;
    s.command = command;
    s.master_fd = hPipeOutRead;     // read: child stdout -> server
    s.write_handle = hPipeInWrite;  // write: server -> child stdin
    s.child_pid = pi.hProcess;      // process handle
    s.hpcon = hPC;                  // for ResizePseudoConsole
    s.generation = ++g_session_generation;
    s.state = SessionState::Running;
    return s;
}

[[nodiscard]] std::expected<void, PtyError> resize_pty(intptr_t handle, uint16_t cols, uint16_t rows) {
    HPCON hPC = reinterpret_cast<HPCON>(handle);
    COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    const HRESULT result = ResizePseudoConsole(hPC, size);
    if (SUCCEEDED(result))
        return {};
    return std::unexpected(PtyError{
        "ResizePseudoConsole failed: HRESULT=" +
        std::to_string(static_cast<unsigned long>(result)) +
        " GetLastError=" + std::to_string(GetLastError())});
}

#else // POSIX create_session via fork+execpty

inline void close_nonstdio_fds_before_exec() {
#if defined(__linux__) && defined(SYS_close_range)
    if (::syscall(SYS_close_range, 3u, ~0u, 0u) == 0) return;
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
    ::closefrom(3);
    return;
#endif
    long max_fd = ::sysconf(_SC_OPEN_MAX);
    if (max_fd < 0) max_fd = 1024;
    for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
}

[[nodiscard]] std::expected<Session, PtyError> create_session(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
    int master_fd = -1;
    struct winsize initial_ws {rows, cols, 0, 0};
    pid_t child = forkpty(&master_fd, nullptr, nullptr, &initial_ws);
    if (child < 0)
        return std::unexpected(PtyError{"forkpty failed"});
    if (child == 0) {
        // The relay ignores SIGPIPE so failed TLS writes become reconnectable
        // errors. Restore the normal disposition for the user's shell/process.
        ::signal(SIGPIPE, SIG_DFL);
        setenv("TERM", term.c_str(), 1);
        close_nonstdio_fds_before_exec();
        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }
    const int master_flags = fcntl(master_fd, F_GETFL, 0);
    if (master_flags < 0 || fcntl(master_fd, F_SETFL, master_flags | O_NONBLOCK) < 0) {
        ::kill(child, SIGTERM);
        ::close(master_fd);
        return std::unexpected(PtyError{"failed to set PTY master nonblocking"});
    }

    Session s;
    s.name = name; s.command = command;
    s.master_fd = master_fd; s.child_pid = child;
    s.generation = ++g_session_generation;
    s.state = SessionState::Running;
    return s;
}

[[nodiscard]] std::expected<void, PtyError> resize_pty(intptr_t handle, uint16_t cols, uint16_t rows) {
    struct winsize ws = {rows, cols, 0, 0};
    if (ioctl(static_cast<int>(handle), TIOCSWINSZ, &ws) == 0) return {};
    return std::unexpected(PtyError{"TIOCSWINSZ failed"});
}

#endif // _WIN32

[[nodiscard]] std::string read_available_pty_output(Session& session,
                                                     size_t max_bytes = 256 * 1024) {
    std::string output;
    output.reserve((std::min)(max_bytes, size_t{16 * 1024}));
    std::array<char, 16 * 1024> buf{};

    while (output.size() < max_bytes) {
        const size_t wanted = (std::min)(buf.size(), max_bytes - output.size());
#ifdef _WIN32
        DWORD available = 0;
        if (!PeekNamedPipe(session.master_fd, nullptr, 0, nullptr, &available, nullptr) || available == 0)
            break;
        DWORD nread = 0;
        if (!ReadFile(session.master_fd, buf.data(),
                      static_cast<DWORD>((std::min)(wanted, static_cast<size_t>(available))),
                      &nread, nullptr) || nread == 0)
            break;
        output.append(buf.data(), static_cast<size_t>(nread));
#else
        const ssize_t nread = ::read(session.master_fd, buf.data(), wanted);
        if (nread > 0) {
            output.append(buf.data(), static_cast<size_t>(nread));
            continue;
        }
        if (nread < 0 && errno == EINTR) continue;
        if (nread < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        break;
#endif
    }
    return output;
}

// ────────────────────────────────────────────────────────────────────
// CONFIG PARSER — key=value config file parser
// ────────────────────────────────────────────────────────────────────

struct PeerEntry {
    std::string name;
    std::string addr;       // "host:port"
    std::string pubkey_hex; // learned via Hello, empty until then
    uint64_t last_seen = 0;
};

enum class HostPlatform : uint8_t { Windows, MacOS, Posix };

[[nodiscard]] std::string default_shell_for_platform(
    HostPlatform platform, bool pwsh_available) {
    switch (platform) {
        case HostPlatform::Windows:
            return pwsh_available ? "pwsh.exe -NoLogo"
                                  : "powershell.exe -NoLogo";
        case HostPlatform::MacOS:
            return "/bin/zsh -il";
        case HostPlatform::Posix:
            return "/bin/bash -l";
    }
    return "/bin/bash -l";
}

#ifdef _WIN32
[[nodiscard]] bool windows_executable_available(const wchar_t* executable) {
    DWORD needed = SearchPathW(nullptr, executable, nullptr, 0, nullptr, nullptr);
    return needed > 0;
}
#endif

[[nodiscard]] std::string platform_default_shell() {
#ifdef _WIN32
    return default_shell_for_platform(
        HostPlatform::Windows, windows_executable_available(L"pwsh.exe"));
#elif defined(__APPLE__)
    return default_shell_for_platform(HostPlatform::MacOS, false);
#else
    return default_shell_for_platform(HostPlatform::Posix, false);
#endif
}

struct MeshConfig {
    // Runtime provenance only; never serialized. The reload watcher must follow
    // the file actually loaded by --config rather than assuming root/config.
    std::string source_path;
    std::string node_name = "unnamed";
    std::string listen_addr = "0.0.0.0";
    uint16_t listen_port = 19949;
    size_t max_peers = 50;
    int gossip_interval_secs = 30;
    int reconnect_backoff_max_secs = 30;
    int ping_interval_secs = 5;
    int pong_timeout_secs = 30;
    // When true (default), outbound seed/discovered dials require pubkey= pin and
    // post-handshake cert/Hello identity binding. TLS fingerprint TOFU alone is
    // not sufficient for mesh trust (independent review 2026-07-16 P0-1).
    bool require_seed_pins = true;
    // Hard cap on inbound file transfer size (bytes). 0 = unlimited.
    // Default 8 GiB so 500MB+ agent artifacts work; override with transfer.max_bytes.
    uint64_t transfer_max_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    std::vector<PeerEntry> seeds;
    std::vector<PeerEntry> discovered;
    std::string authorized_keys_path = "~/.bridgesessions/authorized_keys";
    std::string persistence_path = "~/.bridgesessions/sessions.json";
    int scrollback_lines = 2000;
    int idle_timeout_hours = 168;
    std::string default_shell;
    std::string terminal = "xterm-256color";
    std::string render_hint = "auto";  // "auto", "markdown", "raw"
    std::unordered_map<std::string, std::string> session_commands;

    // P5: Virtual folder mappings
    struct VFolderEntry {
        std::string name;
        std::string local_path;
        std::string remote_peer;
        std::string remote_path;
        std::string direction = "bidirectional";  // push, pull, bidirectional
        int sync_interval_secs = 30;
    };
    std::vector<VFolderEntry> vfolders;

    // Last sync times for vfolders
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> vfolder_last_sync_;

    // D15: WebRTC transport
    bool webrtc_enabled = false;

    // D16: DHT
    bool dht_enabled = false;

    // D17: NAT traversal via UPnP
    bool upnp_enabled = false;

    // mDNS LAN discovery: disabled by default. Gossip/mDNS announcements are
    // only merged when the announced pubkey is explicitly trusted.
    bool mdns_enabled = false;

    MeshConfig() : default_shell(platform_default_shell()) {}
};

// ── expand_home — resolve ~ to HOME/USERPROFILE ─────────────────────

[[nodiscard]] std::string expand_home(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;

    std::string home;
#ifdef _WIN32
    const char* env = std::getenv("USERPROFILE");
    if (env) home = env;
#else
    const char* env = std::getenv("HOME");
    if (env) home = env;
#endif
    if (home.empty()) return path; // fallback: return as-is

    // strip leading ~  (and optional /)
    std::string rest = path.substr(1);
    // Ensure single separator
    if (!rest.empty() && (rest[0] == '/' || rest[0] == '\\')) {
        // If home ends with separator and rest starts with one, skip the first char of rest
        if (!home.empty() && (home.back() == '/' || home.back() == '\\')) {
            rest = rest.substr(1);
        }
    }
    return home + rest;
}

// ── Helpers ─────────────────────────────────────────────────────────

namespace {

// Trim leading whitespace
std::string_view ltrim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    return s;
}

// Trim trailing whitespace
std::string_view rtrim(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

// Trim both sides
std::string_view trim(std::string_view s) {
    return ltrim(rtrim(s));
}

// Parse an integer from a string_view
std::optional<int> parse_int(std::string_view s) {
    s = trim(s);
    if (s.empty()) return std::nullopt;
    try {
        int sign = 1;
        size_t pos = 0;
        if (s[pos] == '-') { sign = -1; ++pos; }
        if (pos >= s.size()) return std::nullopt;
        int val = 0;
        for (; pos < s.size(); ++pos) {
            if (s[pos] < '0' || s[pos] > '9') return std::nullopt;
            val = val * 10 + (s[pos] - '0');
        }
        return val * sign;
    } catch (...) {
        return std::nullopt;
    }
}

// Parse host:port string → fills addr and port
void parse_listen_addr(const std::string& raw, std::string& out_addr, uint16_t& out_port) {
    auto colon = raw.rfind(':');
    if (colon == std::string::npos) {
        // No colon — treat as address only, keep default port
        out_addr = raw;
        return;
    }
    std::string addr_part = raw.substr(0, colon);
    std::string port_part = raw.substr(colon + 1);

    if (addr_part.empty()) {
        out_addr = "0.0.0.0";
    } else {
        out_addr = addr_part;
    }

    auto port_opt = parse_int(port_part);
    if (port_opt.has_value() && *port_opt > 0 && *port_opt <= 65535) {
        out_port = static_cast<uint16_t>(*port_opt);
    }
}

// Write a seed/discovered line to output
void write_peer_line(std::ostream& os, const std::string& prefix, const PeerEntry& p) {
    os << prefix << " " << p.name << " " << p.addr;
    if (!p.pubkey_hex.empty()) {
        os << " pubkey=" << p.pubkey_hex;
    }
    if (p.last_seen > 0) {
        os << " last_seen=" << p.last_seen;
    }
    os << "\n";
}

} // anonymous namespace

// ── load_config — parse key=value config file ────────────────────────

[[nodiscard]] MeshConfig load_config(const std::string& path) {
    MeshConfig cfg;
    std::string resolved = expand_home(path);
    cfg.source_path = resolved;

    std::ifstream f(resolved);
    if (!f.is_open()) return cfg; // missing file = all defaults

    std::string line;
    while (std::getline(f, line)) {
        // Strip a trailing CR so CRLF (Windows-authored) configs parse on POSIX.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Trim the line
        std::string_view sv(line);
        sv = trim(sv);

        // Skip blank lines and comments
        if (sv.empty() || sv[0] == '#') continue;

        // Find the first space or tab delimiter
        size_t space_pos = std::string::npos;
        for (size_t i = 0; i < sv.size(); ++i) {
            if (sv[i] == ' ' || sv[i] == '\t') {
                space_pos = i;
                break;
            }
        }
        if (space_pos == std::string::npos) continue; // no delimiter, malformed

        std::string_view key = trim(sv.substr(0, space_pos));
        std::string_view val = trim(sv.substr(space_pos + 1));
        if (key.empty()) continue;

        std::string key_str(key);

        // ── node.<key> ───────────────────────────────────────
        if (key_str == "node.name") {
            cfg.node_name = std::string(val);
        } else if (key_str == "node.listen") {
            parse_listen_addr(std::string(val), cfg.listen_addr, cfg.listen_port);
        }
        // ── mesh.<key> ───────────────────────────────────────
        else if (key_str == "mesh.max_peers") {
            auto v = parse_int(val);
            if (v.has_value() && *v >= 0) cfg.max_peers = static_cast<size_t>(*v);
        } else if (key_str == "mesh.gossip_interval_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.gossip_interval_secs = *v;
        } else if (key_str == "mesh.reconnect_backoff_max_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.reconnect_backoff_max_secs = *v;
        } else if (key_str == "mesh.ping_interval_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.ping_interval_secs = *v;
        } else if (key_str == "mesh.pong_timeout_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.pong_timeout_secs = *v;
        } else if (key_str == "mesh.require_seed_pins") {
            std::string s(val);
            for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            cfg.require_seed_pins = !(s == "false" || s == "0" || s == "no" || s == "off");
        } else if (key_str == "mesh.mdns_enabled") {
            std::string_view t = trim(val);
            cfg.mdns_enabled = (t == "true" || t == "1" || t == "yes");
        } else if (key_str == "transfer.max_bytes") {
            try {
                cfg.transfer_max_bytes = static_cast<uint64_t>(std::stoull(std::string(val)));
            } catch (...) {}
        }
        // ── transport.<key> (D15 WebRTC) ─────────────────────
        else if (key_str == "transport.webrtc_enabled") {
            std::string_view t = trim(val);
            cfg.webrtc_enabled = (t == "true" || t == "1" || t == "yes");
        }
        // ── dht.<key> (D16) ───────────────────────────────────
        else if (key_str == "dht.enabled") {
            std::string_view t = trim(val);
            cfg.dht_enabled = (t == "true" || t == "1" || t == "yes");
        }
        // ── vfolder.<name>.<key> (P5) ────────────────────────
        else if (key_str.rfind("vfolder.", 0) == 0) {
            auto rest = key_str.substr(8);  // "vfolder." = 8 chars
            auto dot = rest.find('.');
            if (dot != std::string::npos) {
                std::string vname = std::string(rest.substr(0, dot));
                std::string vkey = std::string(rest.substr(dot + 1));
                bool found = false;
                for (auto& v : cfg.vfolders) {
                    if (v.name == vname) { found = true;
                        if (vkey == "local") v.local_path = std::string(trim(val));
                        else if (vkey == "peer") v.remote_peer = std::string(trim(val));
                        else if (vkey == "remote") v.remote_path = std::string(trim(val));
                        else if (vkey == "direction") v.direction = std::string(trim(val));
                        else if (vkey == "interval") { auto iv = parse_int(val); if (iv.has_value()) v.sync_interval_secs = *iv; }
                        break;
                    }
                }
                if (!found) {
                    MeshConfig::VFolderEntry ve;
                    ve.name = vname;
                    if (vkey == "local") ve.local_path = std::string(trim(val));
                    else if (vkey == "peer") ve.remote_peer = std::string(trim(val));
                    else if (vkey == "remote") ve.remote_path = std::string(trim(val));
                    else if (vkey == "direction") ve.direction = std::string(trim(val));
                    else if (vkey == "interval") { auto iv = parse_int(val); if (iv.has_value()) ve.sync_interval_secs = *iv; }
                    cfg.vfolders.push_back(std::move(ve));
                }
            }
        }
        // ── upnp.<key> (D17) ──────────────────────────────────
        else if (key_str == "upnp.enabled") {
            std::string_view t = trim(val);
            cfg.upnp_enabled = (t == "true" || t == "1" || t == "yes");
        }
        // ── sessions.<key> ───────────────────────────────────
        else if (key_str == "sessions.scrollback_lines") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.scrollback_lines = *v;
        } else if (key_str == "sessions.idle_timeout_hours") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.idle_timeout_hours = *v;
        } else if (key_str == "sessions.default_shell") {
            cfg.default_shell = std::string(val);
        } else if (key_str == "sessions.terminal") {
            cfg.terminal = std::string(val);
        } else if (key_str == "sessions.persistence_path") {
            cfg.persistence_path = std::string(val);
        } else if (key_str == "sessions.authorized_keys_path") {
            cfg.authorized_keys_path = std::string(val);
        }
        // ── session.<name>.command ──────────────────────────────
        else if (key_str.rfind("session.", 0) == 0 &&
                 key_str.size() > 16 &&
                 key_str.ends_with(".command")) {
            constexpr size_t prefix_len = 8;  // "session."
            constexpr size_t suffix_len = 8;  // ".command"
            std::string name = key_str.substr(
                prefix_len, key_str.size() - prefix_len - suffix_len);
            if (!name.empty()) cfg.session_commands[std::move(name)] = std::string(val);
        }
        // ── seed <name> <addr> ───────────────────────────────
        else if (key_str == "seed") {
            // Parse: seed <name> <addr> [pubkey=<hex>]
            std::string v2(val);
            std::istringstream iss(v2);
            std::string seed_name, seed_addr;
            if (!(iss >> seed_name >> seed_addr)) continue;
            if (seed_name.empty() || seed_addr.empty()) continue;

            PeerEntry parsed;
            parsed.name = std::move(seed_name);
            parsed.addr = std::move(seed_addr);
            std::string extra;
            while (iss >> extra) {
                if (extra.starts_with("pubkey=")) {
                    parsed.pubkey_hex = extra.substr(7);
                }
            }

            // Deduplicate by name: if a seed with this name already exists, update fields.
            bool found = false;
            for (auto& s : cfg.seeds) {
                if (s.name == parsed.name) {
                    s.addr = parsed.addr;
                    s.pubkey_hex = parsed.pubkey_hex;
                    found = true;
                    break;
                }
            }
            if (!found) cfg.seeds.push_back(std::move(parsed));
        }
        // ── discovered <name> <addr> [pubkey=<hex>] [last_seen=<unix>] ──
        else if (key_str == "discovered") {
            // Parse: discovered <name> <addr> [pubkey=<hex>] [last_seen=<ts>]
            std::string v2(val);
            std::istringstream iss(v2);
            std::string d_name, d_addr;
            if (!(iss >> d_name >> d_addr)) continue; // need at least name and addr

            PeerEntry p;
            p.name = d_name;
            p.addr = d_addr;

            std::string extra;
            while (iss >> extra) {
                if (extra.starts_with("pubkey=")) {
                    p.pubkey_hex = extra.substr(7);
                } else if (extra.starts_with("last_seen=")) {
                    auto ts = parse_int(std::string_view(extra).substr(10));
                    if (ts.has_value()) p.last_seen = static_cast<uint64_t>(*ts);
                }
            }
            cfg.discovered.push_back(std::move(p));
        }
        // ── unknown keys silently ignored ───────────────────
    }

    return cfg;
}

[[nodiscard]] std::string parse_ssh_g_hostname(const std::string& expanded) {
    std::istringstream input(expanded);
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key)) continue;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key != "hostname") continue;
        std::string hostname;
        if (fields >> hostname) return hostname;
    }
    return {};
}

[[nodiscard]] bool config_peer_name_eq(const std::string& a,
                                       const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

[[nodiscard]] bool import_ssh_alias_peer(
    MeshConfig& cfg,
    const std::string& alias,
    const std::string& expanded_ssh_config) {
    std::string hostname = parse_ssh_g_hostname(expanded_ssh_config);
    std::string resolved_addr = hostname.empty() ? std::string{} : hostname + ":19949";

    auto refresh_existing = [&](std::vector<PeerEntry>& peers) {
        for (auto& peer : peers) {
            if (!config_peer_name_eq(peer.name, alias)) continue;
            if (!resolved_addr.empty()) peer.addr = resolved_addr;
            return true;
        }
        return false;
    };
    if (refresh_existing(cfg.seeds) || refresh_existing(cfg.discovered)) return true;
    if (resolved_addr.empty()) return false;

    auto copy_identity_for_addr = [&](const std::vector<PeerEntry>& peers) {
        for (const auto& peer : peers) {
            if (peer.addr == resolved_addr && !peer.pubkey_hex.empty()) {
                return peer.pubkey_hex;
            }
        }
        return std::string{};
    };
    std::string pubkey = copy_identity_for_addr(cfg.seeds);
    if (pubkey.empty()) pubkey = copy_identity_for_addr(cfg.discovered);
    cfg.seeds.push_back(PeerEntry{alias, resolved_addr, std::move(pubkey), 0});
    return true;
}

[[nodiscard]] std::string trusted_peer_pubkey(const MeshConfig& cfg,
                                               const std::string& peer_name) {
    for (const auto& peer : cfg.seeds) {
        if (config_peer_name_eq(peer.name, peer_name)) return peer.pubkey_hex;
    }
    for (const auto& peer : cfg.discovered) {
        if (config_peer_name_eq(peer.name, peer_name)) return peer.pubkey_hex;
    }
    return {};
}

[[nodiscard]] bool peer_identity_matches(const std::string& expected,
                                         const std::string& actual) {
    return !expected.empty() && expected == actual;
}

// ── Outbound peer identity (mesh + direct) ─────────────────────────
// Independent review 2026-07-16 P0-1: mesh connector must not trust TLS alone.
struct OutboundPeerVerifyResult {
    bool ok = false;
    std::string reason;
};

[[nodiscard]] const PeerEntry* find_peer_entry_by_addr(const MeshConfig& cfg,
                                                       const std::string& addr) {
    for (const auto& peer : cfg.seeds) {
        if (peer.addr == addr) return &peer;
    }
    for (const auto& peer : cfg.discovered) {
        if (peer.addr == addr) return &peer;
    }
    return nullptr;
}

// Single verification routine for outbound links: pin ↔ cert ↔ Hello.
// require_pin: when true, empty expected_pubkey is a hard fail.
[[nodiscard]] OutboundPeerVerifyResult verify_outbound_peer_identity(
    const std::string& expected_pubkey,
    const std::string& cert_pubkey,
    const std::string& hello_pubkey,
    const std::string& expected_name,
    const std::string& hello_name,
    bool require_pin) {
    if (cert_pubkey.empty()) {
        return {false, "empty peer certificate public key"};
    }
    if (require_pin && expected_pubkey.empty()) {
        return {false, "no pinned public key (seed/discovered pubkey= required)"};
    }
    if (!expected_pubkey.empty() &&
        !peer_identity_matches(expected_pubkey, cert_pubkey)) {
        return {false, "certificate public key does not match pin"};
    }
    if (hello_pubkey.empty()) {
        return {false, "empty Hello pubkey"};
    }
    if (hello_pubkey != cert_pubkey) {
        return {false, "Hello pubkey does not match TLS certificate key"};
    }
    if (hello_name.empty()) {
        return {false, "empty Hello node name"};
    }
    if (!expected_name.empty() &&
        !config_peer_name_eq(expected_name, hello_name)) {
        return {false, "Hello node name does not match expected peer name"};
    }
    return {true, {}};
}

// Inbound links are already authorized by the certificate callback, but the
// application Hello still has to identify the same key and must not claim a
// configured name belonging to another key. Otherwise an authorized peer can
// impersonate a different peer in name-based command routing.
[[nodiscard]] OutboundPeerVerifyResult verify_inbound_peer_identity(
    const MeshConfig& cfg,
    const std::string& cert_pubkey,
    const std::string& hello_pubkey,
    const std::string& hello_name) {
    if (cert_pubkey.empty()) return {false, "empty peer certificate public key"};
    if (hello_pubkey.empty()) return {false, "empty Hello pubkey"};
    if (hello_pubkey != cert_pubkey) {
        return {false, "Hello pubkey does not match TLS certificate key"};
    }
    if (hello_name.empty()) return {false, "empty Hello node name"};

    auto check_peer = [&](const PeerEntry& peer) -> std::optional<std::string> {
        if (!peer.pubkey_hex.empty() && peer.pubkey_hex == cert_pubkey &&
            !config_peer_name_eq(peer.name, hello_name)) {
            return "certificate key is configured for a different peer name";
        }
        if (!peer.pubkey_hex.empty() &&
            config_peer_name_eq(peer.name, hello_name) &&
            peer.pubkey_hex != cert_pubkey) {
            return "Hello node name is pinned to a different certificate key";
        }
        return std::nullopt;
    };
    for (const auto& peer : cfg.seeds) {
        if (auto reason = check_peer(peer)) return {false, *reason};
    }
    for (const auto& peer : cfg.discovered) {
        if (auto reason = check_peer(peer)) return {false, *reason};
    }
    return {true, {}};
}

// ── File transfer path containment (P0-3) ──────────────────────────
[[nodiscard]] std::optional<std::string> sanitize_transfer_filename(
    std::string_view name) {
    if (name.empty() || name.size() > 255) return std::nullopt;
    // Reject absolute paths / drive letters before basename.
    if (name[0] == '/' || name[0] == '\\') return std::nullopt;
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) &&
        name[1] == ':') {
        return std::nullopt;
    }
    std::string s(name);
    // Basename only (reject if empty after strip).
    const auto slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    if (s.empty() || s == "." || s == "..") return std::nullopt;
    if (s.find('/') != std::string::npos || s.find('\\') != std::string::npos) {
        return std::nullopt;
    }
    for (unsigned char c : s) {
        if (c < 32 || c == 127) return std::nullopt;
    }
    // Windows reserved device names (case-insensitive).
    std::string upper = s;
    for (char& c : upper) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    static constexpr const char* kReserved[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    std::string stem = upper;
    const auto dot = stem.find('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    for (const char* r : kReserved) {
        if (stem == r) return std::nullopt;
    }
    return s;
}

[[nodiscard]] bool path_is_inside_directory(const std::filesystem::path& candidate,
                                            const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path root_abs = fs::weakly_canonical(root, ec);
    if (ec) root_abs = fs::absolute(root, ec);
    if (ec) return false;
    fs::path cand_abs = fs::weakly_canonical(candidate, ec);
    if (ec) cand_abs = fs::absolute(candidate, ec);
    if (ec) return false;
    auto root_s = root_abs.lexically_normal().string();
    auto cand_s = cand_abs.lexically_normal().string();
#ifdef _WIN32
    for (char& c : root_s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : cand_s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    for (char& c : root_s) if (c == '/') c = '\\';
    for (char& c : cand_s) if (c == '/') c = '\\';
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    if (root_s.empty()) return false;
    if (!root_s.empty() && root_s.back() == sep) {
        // ok
    } else {
        root_s.push_back(sep);
    }
    return cand_s == root_s.substr(0, root_s.size() - 1) ||
           cand_s.rfind(root_s, 0) == 0;
}

// ── App home isolation (--config-dir) ─────────────────────────────
// When --config-dir is set, ALL identity/config/receive/state live under that
// directory (not under $HOME/.bridgesessions). Audit residual R1.
struct AppPaths {
    std::string root;
    std::string config;
    std::string received;
    std::string authorized_keys;
    std::string sessions;
    std::string key_pem;
    std::string cert_pem;
    std::string pub;
    std::string logs;
    std::string state;
};

[[nodiscard]] inline AppPaths make_app_paths(std::string root) {
    if (root.empty()) root = expand_home("~/.bridgesessions");
    // Strip trailing slashes
    while (root.size() > 1 && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    const std::filesystem::path root_path(root);
    AppPaths p;
    p.root = root_path.string();
    p.config = (root_path / "config").string();
    p.received = (root_path / "received").string();
    p.authorized_keys = (root_path / "authorized_keys").string();
    p.sessions = (root_path / "sessions.json").string();
    p.key_pem = (root_path / "id_ed25519.pem").string();
    p.cert_pem = (root_path / "id_ed25519-cert.pem").string();
    p.pub = (root_path / "id_ed25519.pub").string();
    p.logs = (root_path / "logs").string();
    p.state = (root_path / "state").string();
    return p;
}

// Rewrite legacy ~/.bridgesessions/... defaults into an isolated app root.
[[nodiscard]] inline std::string resolve_under_app_home(const std::string& path,
                                                        const std::string& app_root) {
    if (path.empty()) return path;
    constexpr std::string_view kLegacy = "~/.bridgesessions";
    if (path == kLegacy || path.starts_with(std::string(kLegacy) + "/") ||
        path.starts_with(std::string(kLegacy) + "\\")) {
        std::string relative = path.substr(kLegacy.size());
        while (!relative.empty() &&
               (relative.front() == '/' || relative.front() == '\\')) {
            relative.erase(relative.begin());
        }
        if (relative.empty()) return std::filesystem::path(app_root).string();
        return (std::filesystem::path(app_root) / relative).string();
    }
    return expand_home(path);
}

inline void apply_app_home_defaults(MeshConfig& cfg, const std::string& app_root) {
    cfg.authorized_keys_path = resolve_under_app_home(cfg.authorized_keys_path, app_root);
    cfg.persistence_path = resolve_under_app_home(cfg.persistence_path, app_root);
}

// ── Per-daemon IPC authentication token ─────────────────────────────
// Each daemon instance generates a fresh CSPRNG token after binding its
// loopback IPC socket. The token is written owner-only under the app home;
// every CLI helper must read it and prepend it to each IPC request.
// There is no unauthenticated fallback.

[[nodiscard]] inline std::string ipc_token_path(const std::string& app_home) {
    return (std::filesystem::path(make_app_paths(app_home).root) /
            "ipc-token").string();
}

[[nodiscard]] inline std::string generate_ipc_token() {
    std::array<uint8_t, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("RAND_bytes failed for IPC token");
    }
    static const char* d = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        token.push_back(d[b >> 4]);
        token.push_back(d[b & 0xF]);
    }
    return token;
}

[[nodiscard]] inline bool write_ipc_token_file(const std::string& app_home,
                                              const std::string& token) {
    return write_private_text_file(ipc_token_path(app_home), token);
}

[[nodiscard]] inline std::string load_ipc_token(const std::string& app_home) {
    std::ifstream f(ipc_token_path(app_home));
    if (!f.is_open()) return {};
    std::string token;
    if (std::getline(f, token)) {
        // Strip trailing CR in case the file was edited on Windows.
        if (!token.empty() && token.back() == '\r') token.pop_back();
    }
    return token;
}

[[nodiscard]] std::string expand_ssh_alias(const std::string& alias) {
    if (alias.empty() || !std::all_of(alias.begin(), alias.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '.' || c == '_' || c == '-';
        })) {
        return {};
    }
#ifdef _WIN32
    std::string command = "ssh -G " + alias + " 2>NUL";
    FILE* pipe = _popen(command.c_str(), "r");
#else
    std::string command = "ssh -G " + alias + " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return {};
    std::string output;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), pipe)) output += buffer;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return output;
}

[[nodiscard]] bool import_ssh_alias_peer(MeshConfig& cfg,
                                         const std::string& alias) {
    return import_ssh_alias_peer(cfg, alias, expand_ssh_alias(alias));
}

enum class SessionCommandSource : uint8_t {
    ClientOverride,
    NamedProfile,
    ConfigDefault,
};

struct ResolvedSessionCommand {
    std::string command;
    SessionCommandSource source = SessionCommandSource::ConfigDefault;
};

// Escape a payload for cmd.exe `/S /C "..."`.
// Nested double-quotes must be doubled (`"` → `""`) or cmd terminates the
// outer `/C` string early. Prefer NOT wrapping PowerShell in cmd at all
// (see build_windows_command_line) — empirical: even with doubled quotes,
// `cmd /S /C "powershell -Command ""...| ForEach-Object { $_ }..."""` still
// breaks pipes so cmd tries to run ForEach-Object as its own command.
// `$` itself is not special to cmd; quote/pipe destruction makes `$_` look
// "mistreated". Callers still must protect `$` from *bash* expansion.
[[nodiscard]] std::string escape_for_cmd_slash_c(const std::string& payload) {
    std::string out;
    out.reserve(payload.size() + 8);
    for (unsigned char ch : payload) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

// First argv token of a Windows command line (quote-aware, best-effort).
[[nodiscard]] std::string first_windows_cli_token(const std::string& command) {
    size_t i = 0;
    while (i < command.size() && (command[i] == ' ' || command[i] == '\t')) ++i;
    if (i >= command.size()) return {};
    if (command[i] == '"') {
        const size_t end = command.find('"', i + 1);
        if (end == std::string::npos) return command.substr(i + 1);
        return command.substr(i + 1, end - (i + 1));
    }
    const size_t end = command.find_first_of(" \t", i);
    return command.substr(i, end == std::string::npos ? std::string::npos : end - i);
}

// Heuristic: command line starts with a real Windows application rather than a
// cmd builtin (`dir`, `echo`, …). Used to skip cmd /c wrapping so PowerShell
// scriptblocks with `$_` and pipes survive CreateProcess.
[[nodiscard]] bool command_has_direct_windows_exe_token(const std::string& command) {
    std::string token = first_windows_cli_token(command);
    if (token.empty()) return false;
    // basename lower
    size_t slash = token.find_last_of("\\/");
    std::string base = slash == std::string::npos ? token : token.substr(slash + 1);
    for (char& c : base) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (base.size() >= 4 && base.compare(base.size() - 4, 4, ".exe") == 0) {
        return true;
    }
    // bare names that SearchPath resolves with PATHEXT
    static constexpr const char* kKnown[] = {
        "powershell", "powershell.exe", "pwsh", "pwsh.exe",
        "cmd", "cmd.exe", "python", "python.exe", "python3", "python3.exe",
        "py", "py.exe",
    };
    for (const char* k : kKnown) {
        if (base == k) return true;
    }
    return false;
}

// True for non-interactive client-override launches that must use anonymous
// pipes (not ConPTY) so --cmd stdout is captured reliably.
[[nodiscard]] bool is_windows_cli_oneshot_command(const std::string& command) {
    if (command.find("/c ") != std::string::npos ||
        command.find("/C ") != std::string::npos ||
        command.find("/c\"") != std::string::npos ||
        command.find("/C\"") != std::string::npos) {
        return true;
    }
    std::string lower = command;
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    const bool is_ps =
        lower.find("powershell") != std::string::npos ||
        lower.find("pwsh") != std::string::npos;
    if (!is_ps) return false;
    return lower.find("-command") != std::string::npos ||
           lower.find("-encodedcommand") != std::string::npos ||
           lower.find("-file") != std::string::npos ||
           lower.find(" -c ") != std::string::npos ||
           lower.find(" -c\"") != std::string::npos;
}

[[nodiscard]] std::string build_windows_command_line(
    const ResolvedSessionCommand& resolved,
    const std::string& comspec,
    bool direct_executable_available = true) {
    // Named/default profile whose first token is a real application: direct.
    if (resolved.source != SessionCommandSource::ClientOverride &&
        direct_executable_available) {
        return resolved.command;
    }
    // ClientOverride that is already an executable cmdline (powershell …):
    // do NOT wrap in cmd /c. Wrapping destroys nested quotes, pipes, and $_.
    // Empirically even quote-doubling under cmd /S /C still breaks PS pipes.
    if (resolved.source == SessionCommandSource::ClientOverride &&
        direct_executable_available &&
        command_has_direct_windows_exe_token(resolved.command)) {
        return resolved.command;
    }
    // Builtins (`dir`, …) and non-resolvable commands: cmd /c + doubled quotes.
    const std::string shell = comspec.empty() ? "cmd.exe" : comspec;
    return "\"" + shell + "\" /d /s /c \"" +
           escape_for_cmd_slash_c(resolved.command) + "\"";
}

[[nodiscard]] std::string prepare_session_command(
    const ResolvedSessionCommand& resolved) {
#ifdef _WIN32
    const char* comspec = std::getenv("ComSpec");
    // Always SearchPath the first token — including ClientOverride — so
    // powershell.exe skips cmd wrapping while `dir` still gets cmd /c.
    const bool direct_executable_available =
        !resolve_windows_application(utf8_to_wide(resolved.command)).empty() ||
        command_has_direct_windows_exe_token(resolved.command);
    return build_windows_command_line(
        resolved, comspec ? comspec : "cmd.exe", direct_executable_available);
#else
    return resolved.command;
#endif
}

[[nodiscard]] ResolvedSessionCommand resolve_session_command(
    const MeshConfig& cfg,
    const std::string& session_name,
    const std::string& client_command) {
    if (!client_command.empty()) {
        return {client_command, SessionCommandSource::ClientOverride};
    }
    auto it = cfg.session_commands.find(session_name);
    if (it != cfg.session_commands.end() && !it->second.empty()) {
        return {it->second, SessionCommandSource::NamedProfile};
    }
    return {cfg.default_shell, SessionCommandSource::ConfigDefault};
}

// ── save_config — write MeshConfig back to file ──────────────────────

[[nodiscard]] bool save_config(const std::string& path, const MeshConfig& cfg) {
    std::string resolved = expand_home(path);

    std::ofstream f(resolved, std::ios::trunc);
    if (!f.is_open()) return false;

    f << "# bridgesessions mesh config\n";
    f << "# Generated — edit with care\n\n";

    // Node section
    f << "# ── Node identity ──────────────────────────────────\n";
    f << "node.name " << cfg.node_name << "\n";
    f << "node.listen " << cfg.listen_addr << ":" << cfg.listen_port << "\n";
    f << "\n";

    // Mesh section
    f << "# ── Mesh settings ──────────────────────────────────\n";
    f << "mesh.max_peers " << cfg.max_peers << "\n";
    f << "mesh.gossip_interval_secs " << cfg.gossip_interval_secs << "\n";
    f << "mesh.reconnect_backoff_max_secs " << cfg.reconnect_backoff_max_secs << "\n";
    f << "mesh.ping_interval_secs " << cfg.ping_interval_secs << "\n";
    f << "mesh.pong_timeout_secs " << cfg.pong_timeout_secs << "\n";
    f << "mesh.require_seed_pins " << (cfg.require_seed_pins ? "true" : "false") << "\n";
    f << "mesh.mdns_enabled " << (cfg.mdns_enabled ? "true" : "false") << "\n";
    f << "transfer.max_bytes " << cfg.transfer_max_bytes << "\n";
    f << "transport.webrtc_enabled " << (cfg.webrtc_enabled ? "true" : "false") << "\n";
    f << "dht.enabled " << (cfg.dht_enabled ? "true" : "false") << "\n";
    f << "upnp.enabled " << (cfg.upnp_enabled ? "true" : "false") << "\n";
    // Virtual folders
    for (auto& v : cfg.vfolders) {
        std::string prefix = "vfolder." + v.name + ".";
        f << prefix << "local " << v.local_path << "\n";
        f << prefix << "peer " << v.remote_peer << "\n";
        f << prefix << "remote " << v.remote_path << "\n";
        f << prefix << "direction " << v.direction << "\n";
        f << prefix << "interval " << v.sync_interval_secs << "\n";
    }
    f << "\n";

    // Seeds
    f << "# ── Bootstrap peers ────────────────────────────────\n";
    for (const auto& s : cfg.seeds) {
        write_peer_line(f, "seed", s);
    }
    f << "\n";

    // Discovered peers are runtime state learned via trusted mDNS/gossip.
    // They are intentionally NOT persisted so untrusted LAN announcements cannot
    // be written back to the operator's config file.
    (void)cfg.discovered;

    // Sessions
    f << "# ── Session defaults ───────────────────────────────\n";
    f << "sessions.scrollback_lines " << cfg.scrollback_lines << "\n";
    f << "sessions.idle_timeout_hours " << cfg.idle_timeout_hours << "\n";
    f << "sessions.default_shell " << cfg.default_shell << "\n";
    f << "sessions.terminal " << cfg.terminal << "\n";
    f << "sessions.persistence_path " << cfg.persistence_path << "\n";
    f << "sessions.authorized_keys_path " << cfg.authorized_keys_path << "\n";
    if (!cfg.session_commands.empty()) {
        f << "\n# ── Named persistent session commands ───────────────────\n";
        std::vector<std::pair<std::string, std::string>> profiles(
            cfg.session_commands.begin(), cfg.session_commands.end());
        std::sort(profiles.begin(), profiles.end());
        for (const auto& [name, command] : profiles) {
            f << "session." << name << ".command " << command << "\n";
        }
    }

    f.close();
    return true;
}

// ────────────────────────────────────────────────────────────────────
// 8. PERSISTENCE — JSON session persistence (nlohmann/json)
// ────────────────────────────────────────────────────────────────────

struct SessionMeta {
    std::string name;
    std::string owner_id;   // unused in mesh (empty), kept for compat
    std::string command;
    std::string state;
    std::string created_at; // ISO 8601
};

// Save session list to JSON file (atomic write: temp + rename)
// v1:plain format with room for future encryption
inline bool save_sessions(const std::string& path,
                          const std::vector<SessionMeta>& sessions) {
    nlohmann::json j = nlohmann::json::array();
    for (auto& s : sessions) {
        nlohmann::json entry;
        entry["name"] = s.name;
        entry["owner_id"] = s.owner_id;
        entry["command"] = s.command;
        entry["state"] = s.state;
        entry["created_at"] = s.created_at;
        j.push_back(entry);
    }
    std::string plain = j.dump(2);
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << "v1:plain\n" << plain << '\n';
        f.flush();
        if (!f) { std::filesystem::remove(tmp); return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(tmp); return false; }
    return true;
}

// Load session list (supports v1:plain and legacy raw JSON)
inline std::vector<SessionMeta> load_sessions(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::string header;
    if (!std::getline(f, header)) return {};
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string plain;
    if (header == "v1:plain") {
        plain = data;
    } else {
        plain = header + "\n" + data;  // legacy raw JSON
    }
    try {
        auto j = nlohmann::json::parse(plain);
        std::vector<SessionMeta> result;
        for (auto& entry : j) {
            SessionMeta m;
            m.name = entry.value("name", "");
            m.owner_id = entry.value("owner_id", "");
            m.command = entry.value("command", "");
            m.state = entry.value("state", "detached");
            m.created_at = entry.value("created_at", "");
            result.push_back(m);
        }
        return result;
    } catch (...) { return {}; }
}

// ────────────────────────────────────────────────────────────────────
// 9. LOGGING — structured JSON logging via spdlog
// ────────────────────────────────────────────────────────────────────

struct StructuredLoggerState {
    std::mutex mutex;
    std::string app_home;
    std::shared_ptr<spdlog::logger> logger;
};

inline StructuredLoggerState& structured_logger_state() {
    static StructuredLoggerState state;
    return state;
}

inline void configure_logger_home(const std::string& app_home) {
    if (app_home.empty()) return;
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.app_home == app_home) return;
    if (state.logger) state.logger->flush();
    spdlog::drop("bs-mesh");
    state.logger.reset();
    state.app_home = app_home;
}

#ifdef BS_TESTING
inline void reset_logger_for_test() {
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.logger) state.logger->flush();
    spdlog::drop("bs-mesh");
    state.logger.reset();
    state.app_home.clear();
}
#endif

// Thread-safe JSON logger
inline std::shared_ptr<spdlog::logger> get_logger() {
    auto& state = structured_logger_state();
    std::lock_guard lock(state.mutex);
    if (state.logger) return state.logger;

    if (state.app_home.empty()) {
        const char* home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        if (!home || !*home)
            throw std::runtime_error("cannot initialize logger: home directory unavailable");
        state.app_home = (std::filesystem::path(home) / ".bridgesessions").string();
    }
    if (!ensure_private_directory(state.app_home))
        throw std::runtime_error("cannot initialize logger directory " + state.app_home);

    const std::string path =
        (std::filesystem::path(state.app_home) / "bs-mesh.log").string();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        path, 1'048'576, 3);  // 1 MB, 3 rotated files
    file_sink->set_pattern("%v");  // raw JSON lines

    state.logger = std::make_shared<spdlog::logger>("bs-mesh", file_sink);
    state.logger->set_level(spdlog::level::info);
    state.logger->flush_on(spdlog::level::info);
    spdlog::register_logger(state.logger);
    return state.logger;
}

// Log a structured event as a single JSON line
inline void log_event(const std::string& event, const std::string& detail = "") {
    auto l = get_logger();
    nlohmann::json j;
    j["ts"] = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    j["event"] = event;
    if (!detail.empty()) j["detail"] = detail;
    l->info(j.dump());
}

// ────────────────────────────────────────────────────────────────────
// 10. SESSION REGISTRY — thread-safe session lifecycle manager
// ────────────────────────────────────────────────────────────────────

class SessionRegistry {
    struct SessionHistoryEntry {
        std::string name;
        std::string peer;
        std::string pid;
        std::string state;
        int32_t exit_code = 0;
        uint64_t runtime_seconds = 0;
        uint64_t bytes = 0;
    };

    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::vector<SessionHistoryEntry> recent_;
    std::string persistence_path_;
    static constexpr size_t kMaxRecentSessions = 50;

    static ResolvedSessionCommand complete_command(
        const std::string& command) {
        if (command.empty()) {
            return {platform_default_shell(), SessionCommandSource::ConfigDefault};
        }
        return {command, SessionCommandSource::NamedProfile};
    }

    // Replace only the spawned runtime while keeping the Session object's
    // address stable for Conn::attached_session pointers. User-visible
    // scrollback, attached peers, and restart policy survive the respawn.
    static void install_spawned_runtime(Session& target, Session&& spawned,
                                        SessionState state) {
        auto peer_ids = std::move(target.peer_ids);
        auto scrollback = std::move(target.scrollback);
#ifndef _WIN32
        auto pending_input = std::move(target.pending_input);
        const bool input_backpressured = target.input_backpressured;
#endif
        const auto last_attach_at = target.last_attach_at;
        const bool auto_restart = target.auto_restart;
        const int restart_failures = target.restart_failures;
        const auto restart_window_start = target.restart_window_start;

        target.~Session();
        new (&target) Session(std::move(spawned));
        target.peer_ids = std::move(peer_ids);
        target.scrollback = std::move(scrollback);
#ifndef _WIN32
        target.pending_input = std::move(pending_input);
        target.input_backpressured = input_backpressured;
#endif
        target.last_attach_at = last_attach_at;
        target.auto_restart = auto_restart;
        target.restart_failures = restart_failures;
        target.restart_window_start = restart_window_start;
        target.history_recorded = false;
        target.state = state;
    }

    static std::string session_pid_string(const Session& s) {
#ifdef _WIN32
        return s.child_pid ? std::to_string(GetProcessId(s.child_pid)) : "-";
#else
        return s.child_pid > 0 ? std::to_string(s.child_pid) : "-";
#endif
    }

    void record_history_locked(Session& s, int32_t exit_code, const std::string& state) {
        if (s.history_recorded) return;
        auto now = std::chrono::steady_clock::now();
        SessionHistoryEntry h;
        h.name = s.name;
        h.peer = s.peer_ids.empty() ? "-" : s.peer_ids.front().substr(0, 16);
        h.pid = session_pid_string(s);
        h.state = state;
        h.exit_code = exit_code;
        h.runtime_seconds = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - s.created_at).count());
        h.bytes = static_cast<uint64_t>(s.scrollback.total_written());
        recent_.push_back(std::move(h));
        if (recent_.size() > kMaxRecentSessions)
            recent_.erase(recent_.begin(), recent_.begin() + static_cast<ptrdiff_t>(recent_.size() - kMaxRecentSessions));
        s.history_recorded = true;
    }

public:
    SessionRegistry() = default;

    void set_persistence_path(const std::string& path) {
        persistence_path_ = path;
    }

    // ── Attach / Create ─────────────────────────────────────────
    // Returns session pointer. Returns nullptr on error.
    Session* attach(const std::string& name,
                    const ResolvedSessionCommand& resolved,
                    uint16_t cols, uint16_t rows, const std::string& term,
                    const std::string& peer_pubkey = "") {
        std::unique_lock lock(mutex_);

        auto it = sessions_.find(name);
        if (it != sessions_.end()) {
            auto* s = it->second.get();

            if (s->state == SessionState::Running || s->state == SessionState::Detached) {
                s->state = SessionState::Attached;
                s->last_attach_at = std::chrono::steady_clock::now();
                if (!peer_pubkey.empty()) {
                    bool already = false;
                    for (auto& pid : s->peer_ids)
                        if (pid == peer_pubkey) { already = true; break; }
                    if (!already) s->peer_ids.push_back(peer_pubkey);
                }
#ifdef _WIN32
                if (s->hpcon)
                    (void)resize_pty(reinterpret_cast<intptr_t>(s->hpcon), cols, rows);
#else
                if (s->master_fd >= 0)
                    (void)resize_pty(static_cast<intptr_t>(s->master_fd), cols, rows);
#endif
                return s;
            }

            if (s->state == SessionState::Attached) {
                s->last_attach_at = std::chrono::steady_clock::now();
                if (!peer_pubkey.empty()) {
                    bool already = false;
                    for (auto& pid : s->peer_ids)
                        if (pid == peer_pubkey) { already = true; break; }
                    if (!already) s->peer_ids.push_back(peer_pubkey);
                }
#ifdef _WIN32
                if (s->hpcon)
                    (void)resize_pty(reinterpret_cast<intptr_t>(s->hpcon), cols, rows);
#else
                if (s->master_fd >= 0)
                    (void)resize_pty(static_cast<intptr_t>(s->master_fd), cols, rows);
#endif
                return s;
            }

            // Session is Died/Exited/Killed/Recoverable. Recreate the PTY in
            // the existing Session storage so every attached transport keeps a
            // valid pointer across resurrection.
            log_event("session_resurrect_replace", name);
            record_history_locked(*s, -1, session_state_str(s->state));
            const std::string spawn_command = prepare_session_command(resolved);
            auto session_result = create_session(name, spawn_command, cols, rows, term);
            if (!session_result) return nullptr;

            install_spawned_runtime(*s, std::move(*session_result),
                                    SessionState::Attached);
            if (!peer_pubkey.empty() &&
                std::find(s->peer_ids.begin(), s->peer_ids.end(), peer_pubkey) == s->peer_ids.end()) {
                s->peer_ids.push_back(peer_pubkey);
            }
            log_event("session_attach", name);
            return s;
        }

        // Create new session
        const std::string spawn_command = prepare_session_command(resolved);
        auto session_result = create_session(name, spawn_command, cols, rows, term);
        if (!session_result) return nullptr;

        auto s = std::make_unique<Session>(std::move(*session_result));
        s->state = SessionState::Attached;
        if (!peer_pubkey.empty()) s->peer_ids.push_back(peer_pubkey);

        auto* ptr = s.get();
        sessions_[name] = std::move(s);
        log_event("session_attach", name);
        return ptr;
    }

    // Programmatic/internal callers pass complete commands. They are never
    // interpreted as remote one-shot overrides, so Windows launches them
    // directly instead of adding a cmd.exe layer.
    Session* attach(const std::string& name, const std::string& command,
                    uint16_t cols, uint16_t rows, const std::string& term,
                    const std::string& peer_pubkey = "") {
        return attach(name, complete_command(command), cols, rows, term,
                      peer_pubkey);
    }

    // ── Detach ──────────────────────────────────────────────────
    // Resolve a textual signal name (HUP/TERM/INT/KILL/QUIT) to a platform
    // signal token. On POSIX this is the signal number; on Windows it is a
    // small code (0 = Ctrl-C/INT-style, 1 = terminate, -1 = unknown) the
    // detach path maps to GenerateConsoleCtrlEvent / TerminateProcess.
    static int resolve_detach_signal(const std::string& name) {
        if (name.empty()) return -1;
#ifdef _WIN32
        if (name == "INT" || name == "HUP" || name == "QUIT")
            return 0;            // console Ctrl-C event
        if (name == "TERM" || name == "KILL") return 1;  // terminate / hard kill
        return -1;
#else
        if (name == "HUP")  return SIGHUP;
        if (name == "TERM") return SIGTERM;
        if (name == "INT")  return SIGINT;
        if (name == "QUIT") return SIGQUIT;
        if (name == "KILL") return SIGKILL;
        return -1;
#endif
    }

    bool detach(const std::string& name, const std::string& peer_pubkey = "") {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        if (peer_pubkey.empty()) {
            s->peer_ids.clear();
        } else {
            s->peer_ids.erase(
                std::remove(s->peer_ids.begin(), s->peer_ids.end(), peer_pubkey),
                s->peer_ids.end());
        }
        if (s->peer_ids.empty() &&
            (s->state == SessionState::Attached || s->state == SessionState::Running)) {
            // v2.1: signal the child on last-peer detach when the attaching
            // peer requested it. The session stays Detached; we deliver the
            // signal to the child so daemons/long jobs learn the terminal left.
            if (!s->detach_signal.empty()) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0 && s->child_pid) {
#ifdef _WIN32
                    if (sig == 0) {
                        GenerateConsoleCtrlEvent(CTRL_C_EVENT,
                                                 GetProcessId(s->child_pid));
                    } else {
                        TerminateProcess(s->child_pid, 1);
                    }
                    log_event("session_detach_signal",
                              name + " -> " + s->detach_signal);
#else
                    ::kill(s->child_pid, sig);
                    log_event("session_detach_signal",
                              name + " -> " + s->detach_signal);
#endif
                } else if (s->child_pid) {
                    log_event("session_detach_signal_unknown",
                              name + " signal=" + s->detach_signal);
                }
            }
            // Transport loss must not overwrite a terminal child state. In
            // particular, pty_output_poller() can mark a fast one-shot command
            // Died before close_conn() detaches its peer; changing Died back to
            // Detached would make the next named attach reuse a dead PTY.
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return !s->peer_ids.empty();
    }

    // ── List ────────────────────────────────────────────────────
    std::vector<SessionInfo> list() const {
        std::shared_lock lock(mutex_);
        std::vector<SessionInfo> result;
        for (auto& [key, s] : sessions_) {
            auto now = std::chrono::steady_clock::now();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                now - s->created_at).count();
            result.push_back({
                s->name,
                session_state_str(s->state),
                static_cast<uint64_t>(uptime)
            });
        }
        return result;
    }

    std::string summary() const {
        std::shared_lock lock(mutex_);
        std::ostringstream out;
        bool wrote = false;
        auto now = std::chrono::steady_clock::now();
        for (auto& [key, s] : sessions_) {
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - s->created_at).count();
            if (wrote) out << " | ";
            out << "live " << s->name
                << " state=" << session_state_str(s->state)
                << " pid=" << session_pid_string(*s)
                << " peer=" << (s->peer_ids.empty() ? "-" : s->peer_ids.front().substr(0, 16))
                << " uptime=" << uptime << "s"
                << " bytes=" << s->scrollback.total_written();
            wrote = true;
        }
        for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
            if (wrote) out << " | ";
            out << "recent " << it->name
                << " state=" << it->state
                << " pid=" << it->pid
                << " peer=" << it->peer
                << " runtime=" << it->runtime_seconds << "s"
                << " exit=" << it->exit_code
                << " bytes=" << it->bytes;
            wrote = true;
        }
        return wrote ? out.str() : "No sessions.";
    }

    void record_finished(Session& s, int32_t exit_code, const std::string& state) {
        std::unique_lock lock(mutex_);
        record_history_locked(s, exit_code, state);
    }

    // ── Get ─────────────────────────────────────────────────────
    Session* get(const std::string& name) {
        std::shared_lock lock(mutex_);
        auto it = sessions_.find(name);
        return (it != sessions_.end()) ? it->second.get() : nullptr;
    }

    const Session* get(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = sessions_.find(name);
        return (it != sessions_.end()) ? it->second.get() : nullptr;
    }

    // ── Kill ────────────────────────────────────────────────────
    void kill(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it != sessions_.end()) {
            std::string killed_name = it->second->name;
            it->second->state = SessionState::Killed;
            record_history_locked(*it->second, -1, "killed");
            sessions_.erase(it);
            log_event("session_kill", killed_name);
        }
    }

    // ── Reap dead children ──────────────────────────────────────
    void reap_dead() {
        std::unique_lock lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            auto* s = it->second.get();
            if (s->state != SessionState::Running && s->state != SessionState::Attached &&
                s->state != SessionState::Detached) { ++it; continue; }
            bool died = false;
#ifdef _WIN32
            int32_t exit_code = 0;
            if (s->child_pid && WaitForSingleObject(s->child_pid, 0) == WAIT_OBJECT_0) {
                died = true;
                s->state = SessionState::Died;
                DWORD code = 0;
                GetExitCodeProcess(s->child_pid, &code);
                exit_code = static_cast<int32_t>(code);
                record_history_locked(*s, exit_code, "died");
                CloseHandle(s->child_pid);
                s->child_pid = nullptr;
            }
#else
            int32_t exit_code = 0;
            if (s->child_pid > 0) {
                int status = 0;
                pid_t result = waitpid(s->child_pid, &status, WNOHANG);
                if (result == s->child_pid) {
                    died = true;
                    s->state = SessionState::Died;
                    if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
                    else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
                    record_history_locked(*s, exit_code, "died");
                    s->child_pid = -1;
                }
            }
#endif
            if (died) {
                // Auto-restart logic
                if (s->auto_restart) {
                    auto now = std::chrono::steady_clock::now();
                    auto window = std::chrono::seconds(60);
                    if (now - s->restart_window_start > window) {
                        s->reset_restart_failures();
                    }
                    if (s->restart_failures < 3) {
                        ++s->restart_failures;
                        std::string restart_name = s->name;
                        int restart_failures = s->restart_failures;
                        auto new_session = create_session(
                            restart_name, s->command, 80, 24, "xterm-256color");
                        if (new_session) {
                            const SessionState resumed_state = s->peer_ids.empty()
                                ? SessionState::Detached
                                : SessionState::Attached;
                            install_spawned_runtime(*s, std::move(*new_session),
                                                    resumed_state);
                            log_event("session_auto_restart", restart_name + " attempt=" + std::to_string(restart_failures));
                            ++it;
                            continue;
                        }
                    }
                    s->state = SessionState::Exited;
                    log_event("session_circuit_breaker", s->name + " failures=" + std::to_string(s->restart_failures));
                }
            }
            ++it;
        }
    }

    // ── Idle timeout cleanup ────────────────────────────────────
    void prune_idle(std::chrono::seconds max_idle) {
        std::unique_lock lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            auto* s = it->second.get();
            if (s->state == SessionState::Detached) {
                auto idle = now - s->last_output_at;
                if (idle > max_idle) {
                    log_event("session_prune_idle", s->name);
                    record_history_locked(*s, -1, "pruned");
                    it = sessions_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // ── Size ────────────────────────────────────────────────────
    size_t count() const {
        std::shared_lock lock(mutex_);
        return sessions_.size();
    }

    // ── Persistence ────────────────────────────────────────────
    void load_persisted_sessions() {
        if (persistence_path_.empty()) return;
        auto metas = load_sessions(persistence_path_);
        if (metas.empty()) return;

        std::unique_lock lock(mutex_);
        for (auto& m : metas) {
            if (sessions_.find(m.name) != sessions_.end()) continue;
            auto s = std::make_unique<Session>();
            s->name = m.name;
            s->command = m.command;
            s->state = SessionState::Recoverable;
            s->created_at = std::chrono::steady_clock::now();
            s->last_output_at = s->created_at;
            s->last_attach_at = s->created_at;
            sessions_[m.name] = std::move(s);
            log_event("session_loaded", m.name);
        }
    }

    bool save_persisted_sessions() const {
        if (persistence_path_.empty()) return true;
        std::vector<SessionMeta> metas;
        {
            std::shared_lock lock(mutex_);
            for (auto& [key, s] : sessions_) {
                SessionMeta m;
                m.name = s->name;
                m.owner_id = "";
                m.command = s->command;
                m.state = session_state_str(s->state);
                m.created_at = std::to_string(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        s->created_at.time_since_epoch()).count());
                metas.push_back(m);
            }
        }
        bool ok = save_sessions(persistence_path_, metas);
        if (ok) log_event("session_persist_saved");
        return ok;
    }

    // ── Resurrect a RECOVERABLE session ────────────────────────
    Session* resurrect(const std::string& name,
                       uint16_t cols, uint16_t rows, const std::string& term) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return nullptr;
        auto* s = it->second.get();
        if (s->state != SessionState::Recoverable) return nullptr;

        const std::string spawn_command = prepare_session_command(
            {s->command, SessionCommandSource::NamedProfile});
        auto session_result = create_session(name, spawn_command, cols, rows, term);
        if (!session_result) return nullptr;

        install_spawned_runtime(*s, std::move(*session_result),
                                SessionState::Attached);
        log_event("session_resurrect", name);
        return s;
    }
};

// ────────────────────────────────────────────────────────────────────
// 11. MESH CONTROLLER — connection manager, event loop, gossip
// ────────────────────────────────────────────────────────────────────

// Platform socket headers
#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <csignal>
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSESOCK close
#else
// windows.h / winsock2.h already included at top
#define CLOSESOCK closesocket
#endif

enum class ConnectFailReason { None, Refused, Timeout, TlsRejected, HelloRejected };

// R2: bound blocking connect/handshake/read time on a socket so a dead or
// silent peer can't hang the daemon or a CLI command indefinitely. Sets both
// SO_RCVTIMEO and SO_SNDTIMEO. Best-effort: failures are ignored (the prior
// behaviour was no timeout at all, so we never make things worse). Defined here,
// after the POSIX SOCKET/timeval definitions above, so it compiles on both
// platforms.
inline void set_socket_timeouts(SOCKET fd, int ms) {
    if (fd == INVALID_SOCKET) return;
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

inline bool socket_selectable(SOCKET fd) {
    if (fd == INVALID_SOCKET) return false;
#ifdef _WIN32
    return true;
#else
    return fd >= 0 && fd < FD_SETSIZE;
#endif
}

struct TimedConnectResult {
    bool connected = false;
    bool timed_out = false;
    int error = 0;
};

[[nodiscard]] inline TimedConnectResult connect_socket_with_timeout(
    SOCKET fd, const sockaddr* address, socklen_t address_len, int timeout_ms) {
    TimedConnectResult result;
    if (!socket_selectable(fd) || !address || timeout_ms < 0) {
        result.error = EINVAL;
        return result;
    }

#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(fd, FIONBIO, &nonblocking) != 0) {
        result.error = WSAGetLastError();
        return result;
    }
    const auto restore_blocking = [&]() {
        u_long blocking = 0;
        ioctlsocket(fd, FIONBIO, &blocking);
    };
#else
    const int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags < 0 || fcntl(fd, F_SETFL, original_flags | O_NONBLOCK) != 0) {
        result.error = errno;
        return result;
    }
    const auto restore_blocking = [&]() {
        fcntl(fd, F_SETFL, original_flags);
    };
#endif

    const int connect_result =
        connect(fd, address, static_cast<int>(address_len));
    if (connect_result == 0) {
        restore_blocking();
        result.connected = true;
        return result;
    }

#ifdef _WIN32
    const int pending_error = WSAGetLastError();
    if (pending_error != WSAEWOULDBLOCK && pending_error != WSAEINPROGRESS) {
        restore_blocking();
        result.error = pending_error;
        return result;
    }
#else
    if (errno != EINPROGRESS) {
        result.error = errno;
        restore_blocking();
        return result;
    }
#endif

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(fd, &write_fds);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int selected =
        select(static_cast<int>(fd) + 1, nullptr, &write_fds, nullptr, &timeout);
    if (selected == 0) {
        result.timed_out = true;
        result.error = ETIMEDOUT;
        restore_blocking();
        return result;
    }
    if (selected < 0) {
#ifdef _WIN32
        result.error = WSAGetLastError();
#else
        result.error = errno;
#endif
        restore_blocking();
        return result;
    }

    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char*>(&socket_error), &error_len) != 0) {
#ifdef _WIN32
        result.error = WSAGetLastError();
#else
        result.error = errno;
#endif
        restore_blocking();
        return result;
    }
    restore_blocking();
    result.error = socket_error;
    result.connected = socket_error == 0;
    return result;
}

[[nodiscard]] inline bool socket_peer_half_closed(SOCKET fd) {
    if (fd == INVALID_SOCKET) return true;
#ifndef _WIN32
#ifdef POLLRDHUP
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLRDHUP;
    if (poll(&pfd, 1, 0) <= 0) return false;
    return (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
#else
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 0) <= 0) return false;
    return (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0;
#endif
#else
    return false;
#endif
}

// Default handshake/connect timeout for outbound mesh + CLI paths (R2).
constexpr int kConnectTimeoutMs = 3000;      // bounded: a dead-addr dial must not starve the event loop
constexpr int kHealthConnectTimeoutMs = 5000;
constexpr int kAcceptHandshakeTimeoutMs = 2000;
// Steady-state recv timeout on established peer sockets. SSL_pending() only
// guarantees >=1 buffered byte, not a whole frame, so a frame split across TLS
// records (only the front half buffered) makes read_frame block in SSL_read_ex
// on the rest. With no timeout that stalls the single-threaded event loop until
// the peer sends more. A bounded timeout degrades that to "drop + reconnect"
// (check_conn_read's catch closes the conn; backoff redials) instead of a freeze.
// This applies only after select() reports frame data; idle healthy links do not
// enter the blocking read, so the bound can stay below the ping cadence.
constexpr int kPeerRecvTimeoutMs = 3000;
constexpr uint16_t kDefaultMeshCliPort = 19980;

[[nodiscard]] inline uint16_t resolve_mesh_cli_port(const char* value) {
    if (!value || !*value) return kDefaultMeshCliPort;
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535)
        return kDefaultMeshCliPort;
    return static_cast<uint16_t>(parsed);
}

inline uint16_t mesh_cli_port() {
    static const uint16_t port =
        resolve_mesh_cli_port(std::getenv("BRIDGESESSIONS_IPC_PORT"));
    return port;
}

// ── Base64 helpers (RFC 4648, no padding) ──────────────────────
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline std::string b64enc(const void* data, size_t len) {
    auto* p = static_cast<const uint8_t*>(data);
    std::string out; out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t(p[i]) << 16) | (i+1<len ? uint32_t(p[i+1]) << 8 : 0) | (i+2<len ? uint32_t(p[i+2]) : 0);
        out.push_back(kB64[(v>>18)&63]); out.push_back(kB64[(v>>12)&63]);
        out.push_back(i+1<len ? kB64[(v>>6)&63] : '='); out.push_back(i+2<len ? kB64[v&63] : '=');
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}
inline std::string b64enc(const std::string& s) { return b64enc(s.data(), s.size()); }
inline std::string b64dec(const std::string& s) {
    std::string out; out.reserve((s.size() * 3) / 4);
    int t[256] = {}; for (int i = 0; i < 64; ++i) t[static_cast<uint8_t>(kB64[i])] = i; t['='] = 0;
    uint32_t acc = 0; int bits = 0;
    for (char c : s) { acc = (acc << 6) | t[static_cast<uint8_t>(c)]; bits += 6; if (bits >= 8) { bits -= 8; out.push_back(static_cast<char>((acc >> bits) & 0xFF)); } }
    return out;
}

inline int tls_last_syscall_errno() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

inline void append_ssl_connect_error_detail(std::string& detail, int ssl_err) {
    if (ssl_err == SSL_ERROR_SYSCALL) {
        int se = tls_last_syscall_errno();
        if (se != 0)
            detail += " syscall_errno=" + std::to_string(se);
    }
}

inline ConnectFailReason classify_ssl_connect_fail(int ssl_err) {
    if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
        return ConnectFailReason::Timeout;
    if (ssl_err == SSL_ERROR_SYSCALL) {
        int se = tls_last_syscall_errno();
#ifdef _WIN32
        if (se == WSAETIMEDOUT || se == WSAECONNRESET || se == WSAECONNABORTED)
            return ConnectFailReason::Timeout;
#else
        if (se == ETIMEDOUT || se == ECONNRESET || se == ECONNABORTED)
            return ConnectFailReason::Timeout;
#endif
        if (se == 0)
            return ConnectFailReason::TlsRejected;  // clean EOF during handshake
    }
    return ConnectFailReason::TlsRejected;
}

inline bool wait_socket_ready(SOCKET fd, bool want_read, int timeout_ms) {
    if (!socket_selectable(fd)) return false;
    fd_set rfds;
    fd_set wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (want_read) FD_SET(fd, &rfds); else FD_SET(fd, &wfds);
    timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int rc = select(static_cast<int>(fd) + 1, want_read ? &rfds : nullptr,
                    want_read ? nullptr : &wfds, nullptr, &tv);
    return rc > 0;
}

// Bounded blocking SSL handshake. Some platforms surface socket timeouts as
// SSL_ERROR_WANT_READ/WRITE even on blocking sockets. Retrying immediately can
// spin/hang. Wait for socket readiness until deadline instead.
inline int ssl_handshake_blocking(SSL* ssl, SOCKET fd, bool server_side, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int last_rc = -1;
    for (;;) {
        last_rc = server_side ? SSL_accept(ssl) : SSL_connect(ssl);
        if (last_rc > 0) return last_rc;
        int err = SSL_get_error(ssl, last_rc);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return last_rc;
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return last_rc;
        int remain = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        if (!wait_socket_ready(fd, err == SSL_ERROR_WANT_READ, std::max(1, remain))) return last_rc;
    }
}

inline int ssl_connect_blocking(SSL* ssl, SOCKET fd, int timeout_ms) {
    return ssl_handshake_blocking(ssl, fd, false, timeout_ms);
}

inline int ssl_accept_blocking(SSL* ssl, SOCKET fd, int timeout_ms) {
    return ssl_handshake_blocking(ssl, fd, true, timeout_ms);
}

#ifndef _WIN32
static std::atomic<bool> g_config_reload_requested{false};
static void sighup_reload_handler(int) { g_config_reload_requested.store(true); }
#endif


// ────────────────────────────────────────────────────────────────────
// 9. TERMINAL RAW MODE (Windows + POSIX) — moved before MeshController for shell_peer
// ────────────────────────────────────────────────────────────────────

struct SavedConsole {
#ifdef _WIN32
    DWORD input_mode = 0;
    DWORD output_mode = 0;
    CONSOLE_SCREEN_BUFFER_INFO buffer_info{};
#else
    struct termios saved_termios {};
#endif
};

struct TermError { std::string msg; };

inline void configure_sigpipe_handling() noexcept {
#ifndef _WIN32
    ::signal(SIGPIPE, SIG_IGN);
#endif
}

inline bool local_input_requests_disconnect(std::string_view input) {
    return input.find('\x03') != std::string_view::npos;
}

inline bool queue_disconnected_input(std::string& pending, std::string_view input) {
    if (local_input_requests_disconnect(input)) return true;
    constexpr size_t kMaxPendingInput = 64 * 1024;
    if (pending.size() < kMaxPendingInput) {
        const size_t room = kMaxPendingInput - pending.size();
        pending.append(input.substr(0, room));
    }
    return false;
}

inline std::string terminal_cleanup_sequence() {
    // A remote TUI can leave these modes enabled when its transport disappears.
    // Reset every common mouse protocol plus focus/bracketed-paste, restore the
    // cursor, and leave the alternate screen only when the local client exits.
    return "\x1b[?9l"
           "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1004l"
           "\x1b[?1005l\x1b[?1006l\x1b[?1015l\x1b[?1016l"
           "\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l";
}

inline void cleanup_terminal_modes() {
    std::cout << terminal_cleanup_sequence() << std::flush;
}

#ifdef _WIN32

inline SavedConsole enable_raw_mode(bool forward_ctrl_c = true) {
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SavedConsole saved{};
    GetConsoleMode(hIn, &saved.input_mode);
    GetConsoleMode(hOut, &saved.output_mode);
    GetConsoleScreenBufferInfo(hOut, &saved.buffer_info);
    // Baseline: strip line-editing/echo, enable VT input passthrough.
    DWORD newIn = saved.input_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)
                | ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!forward_ctrl_c) {
        // When signal forwarding is OFF, keep ENABLE_PROCESSED_INPUT so the
        // local console raises a Ctrl-C control event for the CLI to catch
        // (matching POSIX behavior where ISIG is preserved in raw mode).
        newIn |= ENABLE_PROCESSED_INPUT;
    }
    SetConsoleMode(hIn, newIn);
    DWORD newOut = saved.output_mode
                 | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    SetConsoleMode(hOut, newOut);
    return saved;
}

inline void restore_terminal(const SavedConsole& saved) {
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved.input_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved.output_mode);
}

inline std::pair<uint16_t, uint16_t> get_winsize() {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    return {static_cast<uint16_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1),
            static_cast<uint16_t>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1)};
}

#else

#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

inline SavedConsole enable_raw_mode(bool forward_ctrl_c = true) {
    SavedConsole saved{};
    if (::tcgetattr(STDIN_FILENO, &saved.saved_termios) < 0) {
        throw std::runtime_error("tcgetattr failed: " + std::string(std::strerror(errno)));
    }
    struct termios raw = saved.saved_termios;
    ::cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    // When --signal-forward is off, keep ISIG so the local terminal
    // delivers SIGINT on Ctrl-C instead of sending byte 0x03 to the
    // remote PTY.  The CLI then catches SIGINT and sends SignalMsg.
    if (forward_ctrl_c) {
        raw.c_lflag &= ~(tcflag_t)ISIG;
    }
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        throw std::runtime_error("tcsetattr failed: " + std::string(std::strerror(errno)));
    }
    return saved;
}

inline void restore_terminal(const SavedConsole& saved) {
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved.saved_termios);
}

inline std::pair<uint16_t, uint16_t> get_winsize() {
    struct winsize ws {};
    if (::ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) < 0) return {80, 24};
    return {ws.ws_col, ws.ws_row};
}

#endif

class InteractiveTerminalGuard {
    SavedConsole saved_;
    bool active_ = true;
public:
    InteractiveTerminalGuard(bool forward_ctrl_c = true) : saved_(enable_raw_mode(forward_ctrl_c)) {}
    InteractiveTerminalGuard(const InteractiveTerminalGuard&) = delete;
    InteractiveTerminalGuard& operator=(const InteractiveTerminalGuard&) = delete;
    ~InteractiveTerminalGuard() { restore(); }

    void restore() noexcept {
        if (!active_) return;
        // Cleanup must be interpreted while Windows VT output processing is on.
        try { cleanup_terminal_modes(); } catch (...) {}
        try { restore_terminal(saved_); } catch (...) {}
        active_ = false;
    }
};

inline bool stdin_is_terminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

[[nodiscard]] inline bool shell_command_uses_interactive_mode(
    std::string_view command, bool stdin_tty) {
    return command.empty() || stdin_tty;
}

// ── TLS close_notify helper — clean TLS shutdown before closing socket ──
inline void ssl_close(SSL* ssl, SOCKET sfd) {
    if (ssl && socket_selectable(sfd)) {
        SSL_shutdown(ssl);
        // drain pending data for 1s
        fd_set fds; FD_ZERO(&fds); FD_SET(sfd, &fds);
        timeval tv{1, 0};
#ifdef _WIN32
        select(0, &fds, nullptr, nullptr, &tv);  // Winsock ignores nfds
#else
        select((int)sfd + 1, &fds, nullptr, nullptr, &tv);  // R8.2: POSIX needs maxfd+1
#endif
    }
    if (sfd != INVALID_SOCKET) CLOSESOCK(sfd);
}

// ────────────────────────────────────────────────────────────────────
// D15: WebRTC DataChannel wrapper (behind #ifndef BS_NO_WEBRTC)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_NO_WEBRTC
struct WebRtcChannel {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    bool dc_open = false;
    std::mutex dc_mutex;
    std::vector<uint8_t> recv_buf;

    // Create a PeerConnection offering client
    static std::shared_ptr<rtc::PeerConnection> create_offerer(
        const std::string& sdp, std::string& out_local_sdp)
    {
        rtc::Configuration config;
        auto pc = std::make_shared<rtc::PeerConnection>(config);
        pc->setRemoteDescription(rtc::Description(sdp, "offer"));
        auto desc = pc->createAnswer();
        pc->setLocalDescription(desc.type());
        out_local_sdp = std::string(desc);
        return pc;
    }

    // Create a PeerConnection answering client
    static std::shared_ptr<rtc::PeerConnection> create_answerer(
        const std::string& sdp, std::string& out_local_sdp)
    {
        rtc::Configuration config;
        auto pc = std::make_shared<rtc::PeerConnection>(config);
        pc->setRemoteDescription(rtc::Description(sdp, "answer"));
        out_local_sdp = std::string(*pc->localDescription());
        return pc;
    }
};
#endif // BS_NO_WEBRTC

// ────────────────────────────────────────────────────────────────────
// D16: Kademlia-style DHT (behind #ifndef BS_NO_DHT)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_NO_DHT

using NodeId = std::array<uint8_t, 32>;

inline NodeId pubkey_to_node_id(const std::string& pubkey_hex) {
    std::string sha = bs::mesh::sha256_hex(pubkey_hex);
    NodeId id{};
    for (size_t i = 0; i < 32 && i * 2 + 1 < sha.size(); ++i) {
        char buf[3] = {sha[i*2], sha[i*2+1], 0};
        id[i] = static_cast<uint8_t>(std::strtoul(buf, nullptr, 16));
    }
    return id;
}

inline unsigned xor_leading_zeros(const NodeId& a, const NodeId& b) {
    unsigned leading = 0;
    for (size_t i = 0; i < 32; ++i) {
        uint8_t diff = a[i] ^ b[i];
        if (diff == 0) {
            leading += 8;
            continue;
        }
        // Count leading zeros of this byte
        for (int j = 7; j >= 0; --j) {
            if ((diff >> j) & 1) break;
            ++leading;
        }
        break;
    }
    return leading;
}

struct DhtPeer {
    std::string name;
    std::string addr;
    NodeId node_id{};
    uint64_t last_seen = 0;
};

class DhtNode {
    NodeId our_id_;
    std::string our_name_;
    std::string our_addr_;
    static constexpr size_t kBucketSize = 20;
    static constexpr size_t kNumBuckets = 256;
    std::vector<std::vector<DhtPeer>> buckets_{kNumBuckets};

    mutable std::shared_mutex mutex_;

    int bucket_index(const NodeId& id) const {
        unsigned z = xor_leading_zeros(our_id_, id);
        return std::min<int>(static_cast<int>(z), static_cast<int>(kNumBuckets - 1));
    }

public:
    DhtNode() = default;

    void init(const std::string& our_pubkey, const std::string& our_name, const std::string& our_addr) {
        our_id_ = pubkey_to_node_id(our_pubkey);
        our_name_ = our_name;
        our_addr_ = our_addr;
    }

    void bootstrap(const std::vector<DhtPeer>& seeds) {
        std::unique_lock lock(mutex_);
        for (auto& s : seeds) {
            int idx = bucket_index(s.node_id);
            auto& bucket = buckets_[static_cast<size_t>(idx)];
            bool exists = false;
            for (auto& b : bucket) {
                if (b.node_id == s.node_id) { exists = true; break; }
            }
            if (!exists) {
                if (bucket.size() >= kBucketSize) bucket.erase(bucket.begin());
                bucket.push_back(s);
            }
        }
    }

    std::vector<DhtPeer> find_closest(const NodeId& target, int k = 20) const {
        std::shared_lock lock(mutex_);
        std::vector<DhtPeer> all;
        for (auto& bucket : buckets_) {
            for (auto& p : bucket) {
                all.push_back(p);
            }
        }
        std::sort(all.begin(), all.end(), [&](const DhtPeer& a, const DhtPeer& b) {
            // Sort by XOR distance from target
            for (int i = 31; i >= 0; --i) {
                uint8_t da = a.node_id[i] ^ target[i];
                uint8_t db = b.node_id[i] ^ target[i];
                if (da != db) return da < db;
            }
            return false;
        });
        if (all.size() > static_cast<size_t>(k)) all.resize(static_cast<size_t>(k));
        return all;
    }

    void add_peer(const DhtPeer& peer) {
        std::unique_lock lock(mutex_);
        int idx = bucket_index(peer.node_id);
        auto& bucket = buckets_[static_cast<size_t>(idx)];
        for (auto& existing : bucket) {
            if (existing.node_id == peer.node_id) {
                existing.last_seen = peer.last_seen;
                existing.addr = peer.addr;
                return;
            }
        }
        if (bucket.size() >= kBucketSize) {
            bucket.erase(bucket.begin());
        }
        bucket.push_back(peer);
    }

    const NodeId& our_id() const { return our_id_; }
};

#endif // BS_NO_DHT

// ────────────────────────────────────────────────────────────────────
// D17: NAT traversal via UPnP (behind #ifndef BS_NO_NAT)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_NO_NAT

class UpnpNat {
    bool initialized_ = false;
    std::string external_ip_;
    std::string lan_addr_;
    struct UPNPUrls urls_;
    struct IGDdatas data_;
    std::vector<char> devlist_buf_;

public:
    UpnpNat() {
        std::memset(&urls_, 0, sizeof(urls_));
        std::memset(&data_, 0, sizeof(data_));
    }

    ~UpnpNat() { cleanup(); }

    bool init() {
        char lan_addr[64] = {};
        char wan_addr[64] = {};
        int error = 0;

        // Discover UPnP devices (timeout 2000ms)
        struct UPNPDev* devlist = upnpDiscover(
            2000, nullptr, nullptr, 0, 0, 2, &error);

        if (!devlist) {
            return false;
        }

        // Copy devlist for later cleanup
        struct UPNPDev* cur = devlist;
        while (cur) {
            size_t off = devlist_buf_.size();
            devlist_buf_.resize(off + sizeof(UPNPDev));
            cur = cur->pNext;
        }

        // Get valid IGD
        int ret = UPNP_GetValidIGD(devlist, &urls_, &data_,
                                    lan_addr, sizeof(lan_addr),
                                    wan_addr, sizeof(wan_addr));
        if (ret != 1) {
            freeUPNPDevlist(devlist);
            return false;
        }

        lan_addr_ = lan_addr;
        external_ip_ = wan_addr;
        initialized_ = true;

        freeUPNPDevlist(devlist);
        return true;
    }

    bool setup_port_mapping(uint16_t port) {
        if (!initialized_) return false;

        std::string port_str = std::to_string(port);
        int ret = UPNP_AddPortMapping(
            urls_.controlURL, data_.first.servicetype,
            port_str.c_str(), port_str.c_str(),
            lan_addr_.c_str(),
            "bridgesessions", "TCP", nullptr, "0");

        return ret == UPNPCOMMAND_SUCCESS;
    }

    const std::string& external_ip() const { return external_ip_; }
    bool is_initialized() const { return initialized_; }

    void cleanup() {
        if (urls_.controlURL) {
            // Delete port mapping if we set one up
            // (We don't track the port for cleanup in this simple version)
            FreeUPNPUrls(&urls_);
        }
        initialized_ = false;
    }
};

#endif // BS_NO_NAT

// ── Long-operation worker pool (v2.0.6) ─────────────────────────────
// Moves FILE_SEND/RECV wait, EDIT_DL/UP, VFOLDER_SYNC, and remote FileRequest
// work off the MeshController event loop. Each task runs on a fixed-size pool
// of joinable worker threads. The handler receives the IPC socket for wait-style
// operations so progress/final responses can stream without blocking the daemon.

struct LongOperationTask {
    enum class Type {
        FileSendWait,
        FileRecvWait,
        EditDownload,
        EditUpload,
        VFolderSync,
        RemoteFileRequest,
    };
    Type type;
    std::string peer_name;
    std::string path1;  // local path / remote path / vfolder name
    std::string path2;  // local dest / local path
    SSL* ssl = nullptr;
    SOCKET sock_fd = INVALID_SOCKET;
    std::shared_ptr<std::atomic<bool>> exec_busy;
    std::shared_ptr<std::atomic<bool>> exec_completed;
    SOCKET ipc_fd = INVALID_SOCKET;  // owned by worker for wait-style ops
    std::shared_ptr<std::atomic<bool>> cancelled;
    // Shared "last progress" timestamp. The worker refreshes it on each
    // transfer progress tick; the exec watchdog uses it so a *healthy,
    // progressing* long transfer (file send / vfolder sync) is never killed
    // at the 90s deadline, while a *stalled* transfer (no progress for 90s)
    // still trips it. Set at enqueue time to now.
    std::shared_ptr<std::atomic<std::chrono::steady_clock::time_point::rep>>
        last_progress_at;
};

class LongOperationWorkerPool {
public:
    using Handler = std::function<void(const LongOperationTask&)>;

    explicit LongOperationWorkerPool(size_t thread_count, Handler handler)
        : handler_(std::move(handler)) {
        for (size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~LongOperationWorkerPool() { shutdown(); }

    void enqueue(LongOperationTask task) {
        {
            std::lock_guard lock(mutex_);
            if (shutdown_) return;
            queue_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    size_t pending_count() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

private:
    void worker_loop() {
        while (true) {
            LongOperationTask task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
                if (shutdown_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }
            if (!task.cancelled || !task.cancelled->load()) {
                try {
                    handler_(task);
                } catch (...) {
                    // Worker errors are returned to the IPC client or logged by handler.
                }
            }
            if (task.exec_completed) task.exec_completed->store(true);
            if (task.exec_busy) task.exec_busy->store(false);
            if (task.ipc_fd != INVALID_SOCKET) {
                CLOSESOCK(task.ipc_fd);
            }
        }
    }

    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<LongOperationTask> queue_;
    bool shutdown_ = false;
    Handler handler_;
};

class MeshController {
public:
    enum class ConnectionPurpose : uint8_t {
        Unknown,
        Mesh,
        DirectSession,
    };

    struct FileReceiveState {
        std::string filename;
        std::string path;          // full output path
        std::string checksum;      // expected SHA-256
        uint64_t expected_size = 0;
        uint64_t received_bytes = 0;
        uint32_t total_chunks = 0;
        uint32_t received_chunks = 0;
        std::ofstream file;
        bool active = false;
    };

    struct Conn {
        std::string peer_name;
        std::string peer_pubkey;
        std::string peer_addr;
        SslPtr ssl;
        SOCKET sock_fd = INVALID_SOCKET;
        bool is_outbound = false;
        ConnectionPurpose purpose = ConnectionPurpose::Mesh;
        std::chrono::steady_clock::time_point last_pong;
        std::chrono::steady_clock::time_point connected_at = std::chrono::steady_clock::now();
        uint64_t bytes_in = 0;
        uint64_t bytes_out = 0;
        std::vector<uint8_t> rx_buffer;
        Session* attached_session = nullptr;
        std::string remote_session;
        // v1.7 fix (Known Issue #2): set while a background thread owns this
        // conn's socket/SSL object for a one-shot `daemon_shell_exec` relay.
        // The main event loop must not select()/read/write this fd while
        // busy — the exec thread has exclusive access — otherwise two
        // threads touch the same SSL* concurrently and corrupt the TLS
        // record stream. Also used to skip ping/pong bookkeeping so a
        // long-running exec doesn't get treated as a stalled peer.
        std::shared_ptr<std::atomic<bool>> exec_busy = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> exec_completed = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<std::atomic<bool>> exec_cancelled = std::make_shared<std::atomic<bool>>(false);
        bool heartbeat_suspended_for_busy = false;
        // A detached exec worker may own ssl/sock_fd. Close paths mark this
        // and defer destruction until exec_busy is released.
        // v2.0.1: timestamp when an exec/transfer began,
        // so check_stale_exec() can force-release a stuck exec_busy flag
        // if the CLI timed out and the worker thread outlived its caller.
        std::chrono::steady_clock::time_point exec_started_at = {};
        // Shared "last progress" timestamp used by the exec watchdog. Refreshed
        // on each transfer progress tick so a healthy long transfer is not killed
        // at the 90s deadline; a stalled transfer (no progress for 90s) still trips.
        std::shared_ptr<std::atomic<std::chrono::steady_clock::time_point::rep>>
            exec_last_progress_at =
                std::make_shared<std::atomic<std::chrono::steady_clock::time_point::rep>>(0);
        bool close_requested = false;
        FileReceiveState file_receive;
        std::string pending_recv_dir;
        // Initial handshake Hello. Later Hello frames are ignored only if
        // identical; any mismatch closes the connection.
        std::optional<HelloMsg> initial_hello;
    };

    // Return 0 to drop the older connection (i), 1 to drop the newer one (j).
    // Same-direction reconnects replace stale clients immediately.
    static size_t duplicate_index_to_drop(bool i_matches, bool j_matches) {
        if (i_matches && !j_matches) return 1;
        return 0;
    }

    static bool connections_are_mesh_duplicates(const Conn& a, const Conn& b) {
        return a.purpose == ConnectionPurpose::Mesh &&
               b.purpose == ConnectionPurpose::Mesh &&
               !a.peer_pubkey.empty() &&
               a.peer_pubkey == b.peer_pubkey;
    }

    static bool is_live_mesh_transport_for(const Conn& conn,
                                           const std::string& peer_name,
                                           bool require_idle = true) {
        return conn.purpose == ConnectionPurpose::Mesh &&
               conn.sock_fd != INVALID_SOCKET &&
               (!require_idle || !conn.exec_busy || !conn.exec_busy->load()) &&
               peer_name_eq(conn.peer_name, peer_name);
    }

    static bool refresh_heartbeat_after_busy(
            Conn& conn, std::chrono::steady_clock::time_point now) {
        if (conn.exec_busy && conn.exec_busy->load()) {
            conn.heartbeat_suspended_for_busy = true;
            return true;
        }
        const bool completed = conn.exec_completed && conn.exec_completed->exchange(false);
        if (completed || conn.heartbeat_suspended_for_busy) {
            conn.heartbeat_suspended_for_busy = false;
            conn.last_pong = now;
            return true;
        }
        return false;
    }

    static std::string shell_ipc_relay_policy_response() {
        // One-shot shell commands use their own direct TLS transport. Sharing a
        // mesh Conn with a detached IPC worker lets the event loop and worker
        // race the same SSL object during reconnect/duplicate cleanup.
        return "ERROR direct TLS required\n";
    }

    static bool should_fallback_to_direct_shell(
            int ipc_result, const std::string& output) {
        return ipc_result == -1 &&
               (output.empty() || output == "direct TLS required");
    }

#ifdef BS_TESTING
    void close_conn_for_test(Conn& conn) { (void)close_conn(conn); }
#endif

private:
    MeshConfig config_;
    SessionRegistry sessions_;
    // R8.3: own the TLS cert-verify callback contexts so they are freed with the
    // controller instead of leaking via `new`. MUST be declared BEFORE the
    // SSL_CTX pointers below — members destruct in reverse declaration order, so
    // declaring these first means they are destroyed AFTER tls_listen_/tls_connect_,
    // guaranteeing the SSL_CTX never references freed callback storage.
    AuthorizedKeys authorized_keys_;
    std::function<bool(const std::string&)> tofu_cb_;
    SslCtxPtr tls_listen_;
    SslCtxPtr tls_connect_;
    std::string our_pubkey_;
    std::string home_dir_;
    std::string receive_dir_ = "~/.bridgesessions/received";

    // R8.4: `conns_` is touched only from MeshController::run()'s single-threaded
    // event loop and from CLI methods that run before/after the loop — never
    // concurrently from multiple threads. Long-operation workers (v2.0.6) capture
    // SSL* / SOCKET while exec_busy is set; the event loop skips busy conns.
    std::vector<Conn> conns_;
    static constexpr size_t kMaxConnections = 64;

    // Backoff state per seed
    struct Backoff {
        int delay_ms = 100;
        int max_ms = 30000;
        int attempt = 0;
        std::chrono::steady_clock::time_point next_attempt{};
    };
    std::unordered_map<std::string, Backoff> backoffs_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> accept_only_until_;
    static constexpr int kTieBreakAcceptWindowMs = 12000;
    static constexpr int kForcedReconnectDeadlineMs = 20000;

    // ── Non-blocking TLS + Hello handshake state ────────────────────
    struct PendingHandshake {
        enum class State {
            TcpConnect,
            TlsHandshake,
            ReadHello,
            WriteHello,   // outbound: sent our Hello, waiting for reply
            Done,
            Failed
        };
        SOCKET sock_fd = INVALID_SOCKET;
        SslPtr ssl;
        bool server_side = false;
        State state = State::TlsHandshake;
        std::chrono::steady_clock::time_point deadline;
        std::string expected_addr;        // for outbound: seed addr
        std::string expected_pubkey;      // for outbound: pinned pubkey
        std::string expected_name;        // for outbound: pinned name
        std::vector<uint8_t> rx_buffer;
        std::vector<uint8_t> tx_buffer;   // buffered outbound Hello
        HelloMsg outbound_hello;          // client side: our Hello already built
        HelloMsg peer_hello;              // authenticated peer Hello retained across partial writes
        std::string peer_pk;              // cert pubkey once TLS completes
        bool want_read = true;
        bool want_write = false;
    };
    std::vector<PendingHandshake> pending_handshakes_;
    static constexpr size_t kMaxPendingHandshakes = 16;


    // Shutdown flag for event loop
    std::atomic<bool> running_{false};

    // Last gossip/ping/mdns broadcast times
    std::chrono::steady_clock::time_point last_ping_time_;
    std::chrono::steady_clock::time_point last_gossip_time_;
    std::chrono::steady_clock::time_point last_mdns_time_;
    std::chrono::steady_clock::time_point last_session_prune_time_{};
    std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();
    // mDNS LAN discovery
    SOCKET mdns_fd_ = INVALID_SOCKET;
    static constexpr const char* kMdnsGroup = "224.0.0.252";
    static constexpr uint16_t kMdnsPort = 19949;

    // Listen socket
    SOCKET listen_fd_ = INVALID_SOCKET;
    std::atomic<uint16_t> actual_listen_port_{0};

    std::string config_file_path_;
    std::chrono::steady_clock::time_point last_config_reload_check_{};
    std::chrono::steady_clock::time_point last_authorization_check_{};
    std::filesystem::file_time_type config_mtime_{};
    bool config_mtime_set_ = false;

    int outbound_connect_timeout_ms_ = kConnectTimeoutMs;
    SOCKET cli_listen_fd_ = INVALID_SOCKET;
    std::string ipc_token_;
    std::string ipc_token_path_;

    // D15: WebRTC transport
#ifndef BS_NO_WEBRTC
    std::unordered_map<std::string, WebRtcChannel> webrtc_channels_;
    mutable std::mutex webrtc_mutex_;
#endif

    // D16: DHT node
#ifndef BS_NO_DHT
    DhtNode dht_;
    bool dht_inited_ = false;
#endif

    // D17: NAT traversal
#ifndef BS_NO_NAT
    UpnpNat upnp_;
    std::string external_addr_;
#endif

    // v2.0.6: bounded worker pool for long file/edit/vfolder operations.
    std::optional<LongOperationWorkerPool> worker_pool_;
    static constexpr size_t kLongOperationWorkers = 2;

#ifdef _WIN32
    struct WindowsPtyWriteTask {
        HANDLE handle = nullptr;
        std::string data;
    };
    std::mutex windows_pty_mutex_;
    std::condition_variable windows_pty_cv_;
    std::queue<WindowsPtyWriteTask> windows_pty_queue_;
    std::thread windows_pty_writer_;
    bool windows_pty_stop_ = false;
    std::atomic<size_t> windows_pty_pending_bytes_{0};
    static constexpr size_t kWindowsPtyInputHighWater = 64 * 1024;
    static constexpr size_t kWindowsPtyInputMax = 256 * 1024;

    void windows_pty_writer_loop() {
        for (;;) {
            WindowsPtyWriteTask task;
            {
                std::unique_lock lock(windows_pty_mutex_);
                windows_pty_cv_.wait(lock, [this] {
                    return windows_pty_stop_ || !windows_pty_queue_.empty();
                });
                if (windows_pty_stop_) return;
                task = std::move(windows_pty_queue_.front());
                windows_pty_queue_.pop();
            }
            size_t offset = 0;
            while (offset < task.data.size()) {
                DWORD wrote = 0;
                if (!WriteFile(task.handle, task.data.data() + offset,
                               static_cast<DWORD>(task.data.size() - offset),
                               &wrote, nullptr) || wrote == 0) {
                    break;
                }
                offset += wrote;
            }
            windows_pty_pending_bytes_.fetch_sub(task.data.size());
            CloseHandle(task.handle);
        }
    }

    bool enqueue_windows_pty_input(Session& session, std::string_view data) {
        if (!session.write_handle || data.empty()) return data.empty();
        const size_t pending = windows_pty_pending_bytes_.load();
        if (data.size() > kWindowsPtyInputMax ||
            pending > kWindowsPtyInputMax - data.size()) {
            log_event("pty_input_overflow", session.name);
            return false;
        }
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), session.write_handle,
                             GetCurrentProcess(), &duplicate, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            log_event("pty_input_duplicate_failed", session.name);
            return false;
        }
        {
            std::lock_guard lock(windows_pty_mutex_);
            if (windows_pty_stop_) {
                CloseHandle(duplicate);
                return false;
            }
            windows_pty_pending_bytes_.fetch_add(data.size());
            windows_pty_queue_.push(WindowsPtyWriteTask{
                duplicate, std::string(data)});
            if (!windows_pty_writer_.joinable()) {
                windows_pty_writer_ = std::thread([this] {
                    windows_pty_writer_loop();
                });
            }
        }
        windows_pty_cv_.notify_one();
        return true;
    }

    void shutdown_windows_pty_writer() {
        {
            std::lock_guard lock(windows_pty_mutex_);
            windows_pty_stop_ = true;
        }
        windows_pty_cv_.notify_all();
        if (windows_pty_writer_.joinable()) {
            CancelSynchronousIo(reinterpret_cast<HANDLE>(
                windows_pty_writer_.native_handle()));
            windows_pty_writer_.join();
        }
        while (!windows_pty_queue_.empty()) {
            CloseHandle(windows_pty_queue_.front().handle);
            windows_pty_queue_.pop();
        }
        windows_pty_pending_bytes_.store(0);
    }
#endif

    // ── Internal helpers ───────────────────────────────────────

    // Resolve "host:port" → sockaddr_in
    static sockaddr_in resolve_addr(const std::string& addr) {
        auto colon = addr.rfind(':');
        if (colon == std::string::npos) {
            throw std::runtime_error("invalid addr (no port): " + addr);
        }
        std::string host = addr.substr(0, colon);
        std::string port_str = addr.substr(colon + 1);
        int port = std::stoi(port_str);

        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(static_cast<u_short>(port));

        if (host == "localhost") host = "127.0.0.1";
        if (host.empty()) host = "127.0.0.1";

#ifdef _WIN32
        sa.sin_addr.s_addr = inet_addr(host.c_str());
        if (sa.sin_addr.s_addr == INADDR_NONE) {
            struct hostent* he = gethostbyname(host.c_str());
            if (!he) throw std::runtime_error("gethostbyname failed: " + host);
            sa.sin_addr.s_addr = *reinterpret_cast<unsigned long*>(he->h_addr_list[0]);
        }
#else
        if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) <= 0) {
            struct hostent* he = gethostbyname(host.c_str());
            if (!he) throw std::runtime_error("gethostbyname failed: " + host);
            sa.sin_addr.s_addr = *reinterpret_cast<unsigned long*>(he->h_addr_list[0]);
        }
#endif
        return sa;
    }

    // Check if we already have a conn to a peer (by pubkey)
    bool has_conn_for_pubkey(const std::string& pubkey_hex) const {
        for (auto& c : conns_) {
            if (c.purpose == ConnectionPurpose::Mesh &&
                c.peer_pubkey == pubkey_hex && c.sock_fd != INVALID_SOCKET)
                return true;
        }
        return false;
    }

    // Check if we already have a conn to a peer (by addr)
    bool has_conn_for_addr(const std::string& addr) const {
        for (auto& c : conns_) {
            if (c.peer_addr == addr && c.sock_fd != INVALID_SOCKET)
                return true;
        }
        return false;
    }

    static std::string ascii_lower(std::string s) {
        for (char& ch : s) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + 32);
        }
        return s;
    }

    bool has_conn_for_peer(const std::string& peer_name,
                           const std::string& addr,
                           const std::string& pubkey_hex) const {
        for (const auto& c : conns_) {
            if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
            if (!pubkey_hex.empty() && c.peer_pubkey == pubkey_hex) return true;
            if (!peer_name.empty() && peer_name_eq(c.peer_name, peer_name)) return true;
            if (!addr.empty() && c.peer_addr == addr) return true;
        }
        return false;
    }

    std::string peer_listen_addr_for(const std::string& peer_name,
                                     const std::string& pubkey_hex) const {
        for (const auto& s : config_.seeds) {
            if ((!pubkey_hex.empty() && s.pubkey_hex == pubkey_hex) ||
                (!peer_name.empty() && peer_name_eq(s.name, peer_name))) {
                return s.addr;
            }
        }
        for (const auto& d : config_.discovered) {
            if (!is_trusted_pubkey_cached(d.pubkey_hex)) continue;
            if ((!pubkey_hex.empty() && d.pubkey_hex == pubkey_hex) ||
                (!peer_name.empty() && peer_name_eq(d.name, peer_name))) {
                return d.addr;
            }
        }
        return "";
    }

    void clear_accept_only_for(const std::string& peer_name,
                               const std::string& addr,
                               const std::string& pubkey_hex) {
        if (!addr.empty()) accept_only_until_.erase(addr);
        std::string listen_addr = peer_listen_addr_for(peer_name, pubkey_hex);
        if (!listen_addr.empty()) accept_only_until_.erase(listen_addr);
    }

    bool should_accept_only_for(const PeerEntry& peer) const {
        if (!our_pubkey_.empty() && !peer.pubkey_hex.empty() && our_pubkey_ != peer.pubkey_hex) {
            return our_pubkey_ > peer.pubkey_hex;
        }
        if (!config_.node_name.empty() && !peer.name.empty() &&
            !peer_name_eq(config_.node_name, peer.name)) {
            return ascii_lower(config_.node_name) > ascii_lower(peer.name);
        }
        return false;
    }

    bool should_defer_outbound_for(const PeerEntry& peer,
                                   std::chrono::steady_clock::time_point now) {
        if (peer.addr.empty() || !should_accept_only_for(peer)) return false;
        auto it = accept_only_until_.find(peer.addr);
        if (it == accept_only_until_.end()) {
            accept_only_until_[peer.addr] = now + std::chrono::milliseconds(kTieBreakAcceptWindowMs);
            log_event("peer_dial_deferred",
                      peer.name + " addr=" + peer.addr + " accept_only_ms=" +
                      std::to_string(kTieBreakAcceptWindowMs));
            return true;
        }
        return now < it->second;
    }

    // Find conn index by sock_fd
    int find_conn_index(SOCKET fd) const {
        for (size_t i = 0; i < conns_.size(); ++i) {
            if (conns_[i].sock_fd == fd) return static_cast<int>(i);
        }
        return -1;
    }

    // Remove a connection and clean up. A detached worker can temporarily own
    // the TLS transport; in that case keep the Conn object alive until the
    // worker releases exec_busy.
    bool remove_conn(size_t index) {
        if (index >= conns_.size()) return false;
        auto& c = conns_[index];
        if (!close_conn(c)) return false;
        // Remove backoff for this peer so it can be reconnected
        if (!c.peer_addr.empty()) {
            backoffs_.erase(c.peer_addr);
        }
        conns_.erase(conns_.begin() + static_cast<ptrdiff_t>(index));
        return true;
    }

    bool has_replacement_transport(const Conn& c) const {
        if (!c.attached_session) return false;
        for (const auto& other : conns_) {
            if (&other == &c || other.sock_fd == INVALID_SOCKET) continue;
            if (other.attached_session == c.attached_session &&
                other.peer_pubkey == c.peer_pubkey) {
                return true;
            }
        }
        return false;
    }

    // TLS shutdown + socket close; idempotent (safe if already INVALID_SOCKET).
    // A background exec worker has exclusive SSL ownership while exec_busy is
    // true. Never free or shutdown that transport from the event-loop thread.
    bool close_conn(Conn& c) {
        if (c.exec_busy && c.exec_busy->load()) {
            c.close_requested = true;
            return false;
        }
        c.close_requested = false;
        c.pending_recv_dir.clear();
        if (c.file_receive.active) {
            c.file_receive.file.close();
            std::error_code ec;
            std::filesystem::remove(c.file_receive.path + ".part", ec);
            c.file_receive.active = false;
        }
        if (c.attached_session) {
            detach_connection_session(c, has_replacement_transport(c));
        }
        if (c.sock_fd == INVALID_SOCKET) return true;
        ssl_close(c.ssl.get(), c.sock_fd);
        c.sock_fd = INVALID_SOCKET;
        return true;
    }

    // ── Hello exchange ─────────────────────────────────────────

    // Build a HelloMsg with our info + all known peers
    HelloMsg build_hello() const {
        HelloMsg h;
        h.node_name = config_.node_name;
        h.version = kBridgeSessionsVersion;
        h.pubkey_hex = our_pubkey_;

        // Add seeds as known peers. Only gossip peers with pubkeys; empty pubkey
        // seed entries make Hello frames incompatible with 8-bit field lengths and
        // are not useful for auth/routing.
        for (auto& s : config_.seeds) {
            if (s.pubkey_hex.empty()) continue;
            PeerInfo pi;
            pi.name = s.name;
            pi.addr = s.addr;
            pi.pubkey_hex = s.pubkey_hex;
            pi.last_seen = static_cast<uint64_t>(
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            h.known_peers.push_back(std::move(pi));
        }

        // Add discovered peers only while their key remains explicitly trusted.
        for (auto& d : config_.discovered) {
            if (!is_trusted_pubkey_cached(d.pubkey_hex)) continue;
            PeerInfo pi;
            pi.name = d.name;
            pi.addr = d.addr;
            pi.pubkey_hex = d.pubkey_hex;
            pi.last_seen = d.last_seen;
            h.known_peers.push_back(std::move(pi));
        }

        // Add already connected peers
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            PeerInfo pi;
            pi.name = c.peer_name;
            pi.addr = c.peer_addr;
            pi.pubkey_hex = c.peer_pubkey;
            pi.last_seen = static_cast<uint64_t>(
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            h.known_peers.push_back(std::move(pi));
        }

        return h;
    }

    // Cached trust check for const/read-only paths. Runtime discovery paths call
    // is_trusted_pubkey(), which reloads authorized_keys first so revocations
    // take effect before accepting an address update.
    bool is_trusted_pubkey_cached(const std::string& pubkey_hex) const {
        if (pubkey_hex.empty()) return false;
        for (const auto& s : config_.seeds) {
            if (!s.pubkey_hex.empty() && s.pubkey_hex == pubkey_hex) return true;
        }
        std::vector<uint8_t> raw = hex_decode(pubkey_hex);
        if (raw.size() == 32 && authorized_keys_.contains(raw)) return true;
        return false;
    }

    bool is_trusted_pubkey(const std::string& pubkey_hex) {
        authorized_keys_.reload();
        return is_trusted_pubkey_cached(pubkey_hex);
    }

    void prune_revoked_connections() {
        authorized_keys_.reload();
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET || c.peer_pubkey.empty() ||
                is_trusted_pubkey_cached(c.peer_pubkey)) {
                continue;
            }
            if (c.exec_cancelled) c.exec_cancelled->store(true);
            log_event("mesh_peer_revoked", c.peer_name);
            close_conn(c);
        }
    }

    void maybe_prune_revoked_connections() {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_authorization_check_ < std::chrono::seconds(1)) return;
        last_authorization_check_ = now;
        prune_revoked_connections();
    }

    static uint64_t now_unix_seconds() {
        return static_cast<uint64_t>(
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
    }

    // Merge peers from Hello or Gossip into discovered. New peers are added only
    // when their announced pubkey is explicitly trusted (pinned seed or
    // authorized_keys). Existing discovered entries are updated only while the
    // pubkey remains trusted. Untrusted announcements are dropped and never
    // persisted.
    void merge_peers(const std::vector<PeerInfo>& peers) {
        for (auto& p : peers) {
            if (p.pubkey_hex.empty()) continue;          // require identity
            if (p.pubkey_hex == our_pubkey_) continue;   // skip self

            if (!is_trusted_pubkey(p.pubkey_hex)) continue;

            // A seed is trusted only when its configured pin exactly matches.
            // Never learn a missing seed pin from gossip/Hello.
            bool is_seed = false;
            for (auto& s : config_.seeds) {
                if (!s.pubkey_hex.empty() && s.pubkey_hex == p.pubkey_hex) {
                    is_seed = true;
                    if (!p.addr.empty()) s.addr = p.addr;
                    s.last_seen = now_unix_seconds();
                    break;
                }
                if (peer_name_eq(s.name, p.name) && s.pubkey_hex != p.pubkey_hex)
                    is_seed = true;  // name collision: reject the announcement
            }
            if (is_seed) continue;

            // Existing discovered entry: identity is the key, not the name.
            bool found = false;
            for (auto& d : config_.discovered) {
                if (d.pubkey_hex == p.pubkey_hex) {
                    found = true;
                    if (!p.addr.empty()) d.addr = p.addr;
                    if (!p.name.empty()) d.name = p.name;
                    d.last_seen = now_unix_seconds();
                    break;
                }
                if (peer_name_eq(d.name, p.name) && d.pubkey_hex != p.pubkey_hex)
                    found = true;  // name collision: reject the announcement
            }
            if (found) continue;

            PeerEntry pe;
            pe.name = p.name;
            pe.addr = p.addr;
            pe.pubkey_hex = p.pubkey_hex;
            pe.last_seen = now_unix_seconds();
            config_.discovered.push_back(std::move(pe));
        }
    }

    // R4.2/R4.3: reload SEED list when config file changes on disk.
    // NOTE: discovered peers are runtime state — we intentionally do NOT reload
    // them. Reloading discovered from disk creates a churn loop: reload clobbers
    // them, the next Hello re-merges them, merge_peers saves the config, the mtime
    // change triggers another reload, ad infinitum — starving the event loop.
    void reload_seeds_from_disk() {
        if (config_file_path_.empty()) return;
        MeshConfig fresh = load_config(config_file_path_);
        config_.seeds = std::move(fresh.seeds);
        log_event("config_reload", config_file_path_);
    }

    void maybe_reload_config_seeds() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_config_reload_check_ < std::chrono::seconds(30)) return;
        last_config_reload_check_ = now;
        if (config_file_path_.empty()) return;
        namespace fs = std::filesystem;
        std::error_code ec;
        if (!fs::exists(config_file_path_, ec)) return;
        auto mtime = fs::last_write_time(config_file_path_, ec);
        if (ec) return;
        if (!config_mtime_set_) {
            config_mtime_ = mtime;
            config_mtime_set_ = true;
            return;
        }
        if (mtime != config_mtime_) {
            config_mtime_ = mtime;
            reload_seeds_from_disk();
        }
    }

    // Duplicate resolution: when both A and B dial each other simultaneously,
    // two TCP connections form for the same peer pair. We must converge on
    // exactly ONE surviving connection, and BOTH endpoints must independently
    // agree on which physical connection survives.
    //
    // Deterministic rule: keep the connection INITIATED BY the endpoint with
    // the lexicographically smaller pubkey. Concretely, for a duplicate pair
    // with peer_pubkey P:
    //   - if our_pubkey_ < P  → we are the smaller endpoint → keep OUTBOUND (we initiated it)
    //   - if our_pubkey_ > P  → we are the larger endpoint  → keep INBOUND  (they initiated it)
    // Both sides apply the same rule to the same pair, so both keep the single
    // connection that the smaller-pubkey node opened. No mid-handshake teardown,
    // no split — the loser is closed gracefully after the winner is established.
    void resolve_duplicates() {
        for (size_t i = 0; i < conns_.size(); ++i) {
            if (conns_[i].sock_fd == INVALID_SOCKET) continue;
            for (size_t j = i + 1; j < conns_.size(); ++j) {
                if (conns_[j].sock_fd == INVALID_SOCKET) continue;
                if (connections_are_mesh_duplicates(conns_[i], conns_[j])) {
                    const std::string& pk = conns_[i].peer_pubkey;
                    bool we_are_smaller = our_pubkey_ < pk;
                    // Desired surviving direction on THIS endpoint.
                    bool want_outbound = we_are_smaller;
                    // Pick the candidate whose direction matches the desired one.
                    bool i_matches = (conns_[i].is_outbound == want_outbound);
                    bool j_matches = (conns_[j].is_outbound == want_outbound);
                    const size_t relative_drop = duplicate_index_to_drop(i_matches, j_matches);
                    const size_t drop = relative_drop == 0 ? i : j;
                    if (!remove_conn(drop)) return;
                    resolve_duplicates();
                    return;
                }
            }
        }
    }

    // ── Accept new inbound connection ──────────────────────────
    // v2.0.6: accept is now non-blocking. The TLS handshake and Hello exchange
    // happen incrementally in advance_handshakes() driven by select() readiness.

    void accept_inbound() {
        sockaddr_in peer_addr{};
        socklen_t addr_len = sizeof(peer_addr);
        SOCKET cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer_addr), &addr_len);
        if (cfd == INVALID_SOCKET) return;

        const std::string source_ip = inet_ntoa(peer_addr.sin_addr);
        size_t pending_from_source = 0;
        for (const auto& ph : pending_handshakes_) {
            if (ph.server_side &&
                ph.expected_addr.rfind(source_ip + ":", 0) == 0) {
                ++pending_from_source;
            }
        }
        if (pending_from_source >= 2) {
            log_event("handshake_source_limit", source_ip);
            ssl_close(nullptr, cfd);
            return;
        }

        if (conns_.size() + pending_handshakes_.size() >= kMaxConnections) {
            ssl_close(nullptr, cfd);
            return;
        }
        if (pending_handshakes_.size() >= kMaxPendingHandshakes) {
            log_event("handshake_pending_limit", "dropped inbound, pending=" +
                      std::to_string(pending_handshakes_.size()));
            ssl_close(nullptr, cfd);
            return;
        }

        // Make socket non-blocking so the handshake state machine never blocks.
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(cfd, FIONBIO, &nb);
#else
        int fl = fcntl(cfd, F_GETFL, 0);
        if (fl >= 0) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
#endif

        auto ssl = SslPtr(SSL_new(tls_listen_.get()));
        if (!ssl) { ssl_close(nullptr, cfd); return; }
        SSL_set_fd(ssl.get(), static_cast<int>(cfd));
        SSL_set_accept_state(ssl.get());

        PendingHandshake ph;
        ph.sock_fd = cfd;
        ph.ssl = std::move(ssl);
        ph.server_side = true;
        ph.state = PendingHandshake::State::TlsHandshake;
        ph.expected_addr = std::string(inet_ntoa(peer_addr.sin_addr)) + ":" +
                           std::to_string(ntohs(peer_addr.sin_port));
        ph.want_read = true;
        ph.want_write = false;
        ph.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kAcceptHandshakeTimeoutMs);
        pending_handshakes_.push_back(std::move(ph));
        log_event("inbound_accepted", std::string(inet_ntoa(peer_addr.sin_addr)) + ":" +
                  std::to_string(ntohs(peer_addr.sin_port)));
    }

    // ── Incremental TLS + Hello handshake ─────────────────────

    // Called once when TLS completes to verify the peer certificate pubkey.
    bool handshake_verify_cert_pubkey(PendingHandshake& ph) {
        ph.peer_pk = peer_public_key_hex(ph.ssl.get());
        if (ph.peer_pk.empty()) {
            log_event("handshake_no_cert_pubkey", ph.server_side ? "inbound" : "outbound");
            return false;
        }
        return true;
    }

    // Promote a completed handshake to a live Conn.
    void promote_handshake_to_conn(PendingHandshake& ph, const HelloMsg& hello) {
        Conn c;
        c.peer_name = hello.node_name;
        c.peer_pubkey = ph.peer_pk;
        c.peer_addr = ph.expected_addr;
        c.initial_hello = hello;
        c.ssl = std::move(ph.ssl);
        c.sock_fd = ph.sock_fd;
        c.rx_buffer = std::move(ph.rx_buffer);
        c.is_outbound = !ph.server_side;
        c.purpose = ConnectionPurpose::Unknown;
        c.last_pong = std::chrono::steady_clock::now();
        // Steady-state recv timeout for established links.
        set_socket_timeouts(ph.sock_fd, kPeerRecvTimeoutMs);

        merge_peers(hello.known_peers);
        conns_.push_back(std::move(c));
        resolve_duplicates();
        clear_accept_only_for(hello.node_name, ph.expected_addr, ph.peer_pk);

        log_event(ph.server_side ? "mesh_peer_connected" : "mesh_peer_connected_outbound",
                  hello.node_name + " addr=" + (ph.server_side ? "inbound" : ph.expected_addr) +
                  " pubkey=" + ph.peer_pk.substr(0, 16) + "...");

        // Handshake object will be erased; mark fd moved so ssl_close isn't called twice.
        ph.sock_fd = INVALID_SOCKET;
        ph.state = PendingHandshake::State::Done;
    }

    void advance_handshakes() {
        auto now = std::chrono::steady_clock::now();
        std::vector<size_t> to_erase;

        for (size_t i = 0; i < pending_handshakes_.size(); ++i) {
            auto& ph = pending_handshakes_[i];
            if (now > ph.deadline) {
                log_event("handshake_deadline", ph.server_side ? "inbound" : ph.expected_addr);
                ph.state = PendingHandshake::State::Failed;
                to_erase.push_back(i);
                continue;
            }
            if (!socket_selectable(ph.sock_fd)) {
                log_event("handshake_fd_not_selectable", std::to_string(ph.sock_fd));
                ph.state = PendingHandshake::State::Failed;
                to_erase.push_back(i);
                continue;
            }

            fd_set ready_read, ready_write;
            FD_ZERO(&ready_read);
            FD_ZERO(&ready_write);
            if (ph.want_read) FD_SET(ph.sock_fd, &ready_read);
            if (ph.want_write) FD_SET(ph.sock_fd, &ready_write);
            timeval poll_tv{0, 0};
            const int ready = select(static_cast<int>(ph.sock_fd) + 1,
                                     &ready_read, &ready_write, nullptr, &poll_tv);
            if (ready <= 0 && SSL_pending(ph.ssl.get()) <= 0) continue;

            try {
                switch (ph.state) {
                case PendingHandshake::State::TcpConnect: {
                    int so_error = 0;
                    socklen_t len = sizeof(so_error);
                    if (getsockopt(ph.sock_fd, SOL_SOCKET, SO_ERROR,
                                   reinterpret_cast<char*>(&so_error), &len) != 0 || so_error != 0) {
                        ph.state = PendingHandshake::State::Failed;
                        to_erase.push_back(i);
                        break;
                    }
                    ph.state = PendingHandshake::State::TlsHandshake;
                    ph.want_read = true;
                    ph.want_write = true;
                    break;
                }
                case PendingHandshake::State::TlsHandshake: {
                    int ret = ph.server_side ? SSL_accept(ph.ssl.get()) : SSL_connect(ph.ssl.get());
                    if (ret > 0) {
                        if (!handshake_verify_cert_pubkey(ph)) {
                            ph.state = PendingHandshake::State::Failed;
                            to_erase.push_back(i);
                            break;
                        }
                        if (ph.server_side) {
                            ph.state = PendingHandshake::State::ReadHello;
                            ph.want_read = true;
                            ph.want_write = false;
                        } else {
                            ph.outbound_hello = build_hello();
                            ph.state = PendingHandshake::State::WriteHello;
                            ph.want_read = false;
                            ph.want_write = true;
                        }
                    } else {
                        int err = SSL_get_error(ph.ssl.get(), ret);
                        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                            ph.want_read = err == SSL_ERROR_WANT_READ;
                            ph.want_write = err == SSL_ERROR_WANT_WRITE;
                        } else {
                            log_event("tls_handshake_failed",
                                      (ph.server_side ? "inbound ssl_err=" : "outbound ssl_err=") +
                                      std::to_string(err));
                            ph.state = PendingHandshake::State::Failed;
                            to_erase.push_back(i);
                        }
                    }
                    break;
                }
                case PendingHandshake::State::WriteHello: {
                    int want = SSL_ERROR_WANT_WRITE;
                    if (write_frame_nonblocking(ph.ssl.get(), ph.outbound_hello,
                                                CONTROL_STREAM_ID, ph.tx_buffer, &want)) {
                        ph.tx_buffer.clear();
                        ph.state = ph.server_side
                            ? PendingHandshake::State::Done
                            : PendingHandshake::State::ReadHello;
                        ph.want_read = !ph.server_side;
                        ph.want_write = false;
                        if (ph.state == PendingHandshake::State::Done) {
                            promote_handshake_to_conn(ph, ph.peer_hello);
                            to_erase.push_back(i);
                        }
                    } else {
                        ph.want_read = want == SSL_ERROR_WANT_READ;
                        ph.want_write = want == SSL_ERROR_WANT_WRITE;
                    }
                    break;
                }
                case PendingHandshake::State::ReadHello: {
                    int want = SSL_ERROR_WANT_READ;
                    auto msg_opt = read_frame_nonblocking(ph.ssl.get(), ph.rx_buffer, &want);
                    ph.want_read = want == SSL_ERROR_WANT_READ;
                    ph.want_write = want == SSL_ERROR_WANT_WRITE;
                    if (!msg_opt) break;
                    if (!std::holds_alternative<HelloMsg>(*msg_opt)) {
                        log_event("handshake_expected_hello",
                                  ph.server_side ? "inbound" : ph.expected_addr);
                        ph.state = PendingHandshake::State::Failed;
                        to_erase.push_back(i);
                        break;
                    }
                    auto& hello = std::get<HelloMsg>(*msg_opt);
                    ph.peer_hello = hello;

                    if (ph.server_side) {
                        auto identity = verify_inbound_peer_identity(
                            config_, ph.peer_pk, hello.pubkey_hex, hello.node_name);
                        if (!identity.ok) {
                            log_event("hello_identity_rejected",
                                      hello.node_name + " reason=" + identity.reason);
                            ph.state = PendingHandshake::State::Failed;
                            to_erase.push_back(i);
                            break;
                        }
                        // Send Hello reply (possibly non-blocking).
                        ph.outbound_hello = build_hello();
                        ph.state = PendingHandshake::State::WriteHello;
                        ph.want_read = false;
                        ph.want_write = true;
                        int write_want = SSL_ERROR_WANT_WRITE;
                        if (write_frame_nonblocking(ph.ssl.get(), ph.outbound_hello,
                                                    CONTROL_STREAM_ID, ph.tx_buffer, &write_want)) {
                            ph.tx_buffer.clear();
                            promote_handshake_to_conn(ph, ph.peer_hello);
                            to_erase.push_back(i);
                        } else {
                            ph.want_read = write_want == SSL_ERROR_WANT_READ;
                            ph.want_write = write_want == SSL_ERROR_WANT_WRITE;
                        }
                    } else {
                        auto v = verify_outbound_peer_identity(
                            ph.expected_pubkey, ph.peer_pk, hello.pubkey_hex,
                            ph.expected_name, hello.node_name, config_.require_seed_pins);
                        if (!v.ok) {
                            log_event("mesh_peer_identity_rejected",
                                      ph.expected_addr + " name=" + hello.node_name +
                                      " reason=" + v.reason);
                            ph.state = PendingHandshake::State::Failed;
                            to_erase.push_back(i);
                            break;
                        }
                        promote_handshake_to_conn(ph, hello);
                        to_erase.push_back(i);
                    }
                    break;
                }
                case PendingHandshake::State::Done:
                case PendingHandshake::State::Failed:
                    to_erase.push_back(i);
                    break;
                }
            } catch (const std::exception& e) {
                log_event("handshake_exception",
                          (ph.server_side ? "inbound " : ph.expected_addr + " ") + e.what());
                ph.state = PendingHandshake::State::Failed;
                to_erase.push_back(i);
            } catch (...) {
                log_event("handshake_exception",
                          ph.server_side ? "inbound unknown" : ph.expected_addr + " unknown");
                ph.state = PendingHandshake::State::Failed;
                to_erase.push_back(i);
            }
        }

        // Erase failed/done handshakes from back to front to keep indices stable.
        for (auto it = to_erase.rbegin(); it != to_erase.rend(); ++it) {
            auto& ph = pending_handshakes_[*it];
            if (ph.sock_fd != INVALID_SOCKET) {
                if (ph.ssl) SSL_set_quiet_shutdown(ph.ssl.get(), 1);
                CLOSESOCK(ph.sock_fd);
                ph.sock_fd = INVALID_SOCKET;
                ph.ssl.reset();
            }
            pending_handshakes_.erase(pending_handshakes_.begin() + static_cast<std::ptrdiff_t>(*it));
        }
    }

    // ── Start non-blocking outbound handshake to a seed/discovered peer ────
    // Returns true if a handshake was started, false on immediate failure.
    bool start_outbound_handshake(const PeerEntry& peer) {
        for (const auto& ph : pending_handshakes_) {
            if (!ph.server_side &&
                (ph.expected_addr == peer.addr ||
                 (!peer.pubkey_hex.empty() && ph.expected_pubkey == peer.pubkey_hex))) {
                return false;
            }
        }
        if (pending_handshakes_.size() >= kMaxPendingHandshakes) return false;
        try {
            auto sa = resolve_addr(peer.addr);
            SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sfd == INVALID_SOCKET) return false;
            { int o = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&o, sizeof(o)); }

            // Non-blocking connect.
#ifdef _WIN32
            u_long nb = 1;
            ioctlsocket(sfd, FIONBIO, &nb);
#else
            int fl = fcntl(sfd, F_GETFL, 0);
            if (fl >= 0) fcntl(sfd, F_SETFL, fl | O_NONBLOCK);
#endif
            int rc = connect(sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
            bool connected_immediately = rc == 0;
            if (rc != 0) {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                    CLOSESOCK(sfd); return false;
                }
#else
                if (errno != EINPROGRESS) {
                    CLOSESOCK(sfd); return false;
                }
#endif
            }

            auto ssl = SslPtr(SSL_new(tls_connect_.get()));
            if (!ssl) { CLOSESOCK(sfd); return false; }
            SSL_set_fd(ssl.get(), static_cast<int>(sfd));
            SSL_set_connect_state(ssl.get());

            PendingHandshake ph;
            ph.sock_fd = sfd;
            ph.ssl = std::move(ssl);
            ph.server_side = false;
            ph.state = connected_immediately
                ? PendingHandshake::State::TlsHandshake
                : PendingHandshake::State::TcpConnect;
            ph.want_read = connected_immediately;
            ph.want_write = true;
            ph.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(outbound_connect_timeout_ms_);
            ph.expected_addr = peer.addr;
            ph.expected_pubkey = peer.pubkey_hex;
            ph.expected_name = peer.name;
            pending_handshakes_.push_back(std::move(ph));
            return true;
        } catch (...) {
            return false;
        }
    }

    // Helper used by advance_handshakes to check if a non-blocking connect finished.
    static bool socket_connect_finished(SOCKET fd) {
        if (fd == INVALID_SOCKET) return false;
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len) != 0)
            return false;
        return so_error == 0;
    }

    // ── Public API ──────────────────────────────────────────

    bool connect_to_peer_impl(const std::string& addr) {
        try {
            // Check if already connected to this addr
            if (has_conn_for_addr(addr)) return true;

            // Resolve and connect
            auto sa = resolve_addr(addr);
            SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
            if (sfd == INVALID_SOCKET) return false;
            set_socket_timeouts(sfd, outbound_connect_timeout_ms_);
            { int o = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&o, sizeof(o)); }  // R3.6

            const auto connect_result = connect_socket_with_timeout(
                sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa),
                outbound_connect_timeout_ms_);
            if (!connect_result.connected) {
                ssl_close(nullptr, sfd);
                return false;
            }

            // TLS handshake (client side)
            auto ssl = SslPtr(SSL_new(tls_connect_.get()));
            if (!ssl) { ssl_close(nullptr, sfd); return false; }
            SSL_set_fd(ssl.get(), static_cast<int>(sfd));

            int ret = ssl_connect_blocking(ssl.get(), sfd, outbound_connect_timeout_ms_);
            if (ret <= 0) {
                // R1: capture error before ssl_close drains the queue
                int ssl_err = SSL_get_error(ssl.get(), ret);
                char errbuf[256] = {};
                unsigned long e = ERR_get_error();
                if (e) ERR_error_string_n(e, errbuf, sizeof(errbuf));
                std::string detail = "ssl_err=" + std::to_string(ssl_err) +
                                     (errbuf[0] ? std::string(" ") + errbuf : "");
                append_ssl_connect_error_detail(detail, ssl_err);
                log_event("tls_connect_failed", detail);
                ssl_close(ssl.get(), sfd);
                return false;
            }

            // Get peer's Ed25519 public key from the TLS certificate.
            std::string peer_pk = peer_public_key_hex(ssl.get());
            if (peer_pk.empty()) { ssl_close(ssl.get(), sfd); return false; }

            // Look up configured pin for this dial target (seed/discovered).
            const PeerEntry* pe = find_peer_entry_by_addr(config_, addr);
            const std::string expected_pk = pe ? pe->pubkey_hex : std::string{};
            const std::string expected_name = pe ? pe->name : std::string{};
            // Seeds always require pins when require_seed_pins; discovered same.
            const bool require_pin = config_.require_seed_pins;

            // Send our Hello
            write_frame(ssl.get(), build_hello(), CONTROL_STREAM_ID);

            // Read Hello from peer
            Message msg = read_frame(ssl.get());
            if (!std::holds_alternative<HelloMsg>(msg)) {
                ssl_close(ssl.get(), sfd);
                return false;
            }
            auto& hello = std::get<HelloMsg>(msg);

            // P0-1: pin ↔ cert ↔ Hello before merge_peers or trusting the link.
            auto v = verify_outbound_peer_identity(
                expected_pk, peer_pk, hello.pubkey_hex,
                expected_name, hello.node_name, require_pin);
            if (!v.ok) {
                log_event("mesh_peer_identity_rejected",
                          addr + " name=" + hello.node_name + " reason=" + v.reason);
                ssl_close(ssl.get(), sfd);
                return false;
            }

            Conn c;
            c.peer_name = hello.node_name;
            c.peer_pubkey = peer_pk;
            c.peer_addr = addr;
            c.initial_hello = hello;
            std::string subj_out = peer_cert_subject_oneline(ssl.get());  // R1.4 before move
            c.ssl = std::move(ssl);
            c.sock_fd = sfd;
            c.is_outbound = true;
            c.last_pong = std::chrono::steady_clock::now();
            // Steady-state recv timeout (see kPeerRecvTimeoutMs): bound mid-frame
            // stalls to drop+reconnect instead of a single-threaded loop freeze.
            set_socket_timeouts(sfd, kPeerRecvTimeoutMs);

            // Merge known peers from Hello only after identity is verified.
            merge_peers(hello.known_peers);

            conns_.push_back(std::move(c));
            resolve_duplicates();

            // Reset backoff on success
            backoffs_.erase(addr);
            clear_accept_only_for(hello.node_name, addr, peer_pk);

            log_event("mesh_peer_connected_outbound", hello.node_name + " addr=" + addr
                      + " pubkey=" + peer_pk.substr(0, 16) + "..."
                      + " subject=" + subj_out);  // R1.4

#ifndef BS_NO_WEBRTC
            // D15: After TCP connection, try WebRTC upgrade
            if (config_.webrtc_enabled) {
                try_webrtc_upgrade(c);
            }
#endif

            return true;
        } catch (...) {
            return false;
        }
    }

    // ── D15: WebRTC upgrade attempt ──────────────────────────

#ifndef BS_NO_WEBRTC
    void try_webrtc_upgrade(Conn& c) {
        // Send SDP offer over existing TCP gossip channel
        try {
            // NOTE: In a full implementation, we'd create a real SDP offer here
            // using libdatachannel's PeerConnection API.
            // For now, we send a placeholder to signal WebRTC capability.
            SdpOfferMsg offer;
            offer.peer_name = config_.node_name;
            offer.sdp = "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=bridgesessions\r\nt=0 0\r\n";
            write_frame(c.ssl.get(), offer, CONTROL_STREAM_ID);
            log_event("webrtc_offer_sent", c.peer_name);
        } catch (...) {
            log_event("webrtc_offer_send_failed", c.peer_name);
        }
    }
#endif

    // ── Build Gossip message ───────────────────────────────────

    GossipMsg build_gossip() const {
        GossipMsg g;
        for (auto& s : config_.seeds) {
            if (s.pubkey_hex.empty()) continue;
            PeerInfo pi;
            pi.name = s.name;
            pi.addr = s.addr;
            pi.pubkey_hex = s.pubkey_hex;
            pi.last_seen = s.last_seen;
            g.peers.push_back(std::move(pi));
        }
        for (auto& d : config_.discovered) {
            if (!is_trusted_pubkey_cached(d.pubkey_hex)) continue;
            PeerInfo pi;
            pi.name = d.name;
            pi.addr = d.addr;
            pi.pubkey_hex = d.pubkey_hex;
            pi.last_seen = d.last_seen;
            g.peers.push_back(std::move(pi));
        }
        return g;
    }

    // ── D15: WebRTC SDP handlers ──────────────────────────────

    void handle_sdp_offer(Conn& c, const SdpOfferMsg& offer) {
#ifndef BS_NO_WEBRTC
        if (!config_.webrtc_enabled) return;
        try {
            std::string answer_sdp;
            auto pc = WebRtcChannel::create_offerer(offer.sdp, answer_sdp);

            SdpAnswerMsg answer;
            answer.peer_name = config_.node_name;
            answer.sdp = answer_sdp;
            write_frame(c.ssl.get(), answer, CONTROL_STREAM_ID);

            log_event("webrtc_offer_accepted", "from " + offer.peer_name);
        } catch (...) {
            log_event("webrtc_offer_failed", "from " + offer.peer_name);
        }
#endif
    }

    void handle_sdp_answer(Conn& c, const SdpAnswerMsg& answer) {
#ifndef BS_NO_WEBRTC
        if (!config_.webrtc_enabled) return;
        try {
            // TODO: full WebRTC DataChannel setup would complete here
            log_event("webrtc_answer_received", "from " + answer.peer_name);
        } catch (...) {}
#endif
    }

    // ── D16: DHT message handlers ─────────────────────────────

    void handle_dht_find_node(Conn& c, const DhtFindNodeMsg& query) {
#ifndef BS_NO_DHT
        if (!config_.dht_enabled || !dht_inited_) return;
        // Reply with GossipMsg containing closest peers
        auto closest = dht_.find_closest(query.target_id, 20);
        GossipMsg g;
        for (auto& dp : closest) {
            PeerInfo pi;
            pi.name = dp.name;
            pi.addr = dp.addr;
            pi.pubkey_hex = ""; // DHT peers may not have pubkeys
            pi.last_seen = dp.last_seen;
            g.peers.push_back(std::move(pi));
        }
        if (!g.peers.empty()) {
            try {
                write_frame(c.ssl.get(), g, CONTROL_STREAM_ID);
            } catch (...) {}
        }
#endif
    }

    // ── P1: File transfer handlers ──────────────────────────────

    void handle_file_meta(Conn& c, const FileMetaMsg& m) {
        // Prepare receive path — never trust raw remote filenames (P0-3).
        namespace fs = std::filesystem;

        // v2.0.6: the destination directory was bound to this Conn by the async
        // FILE_RECV request. Consume it now (one outstanding receive per Conn).
        // Fall back to the global default only when no per-request dir is set.
        std::string recv_dir = std::move(c.pending_recv_dir);
        if (recv_dir.empty()) recv_dir = receive_dir_;

        auto safe_name = sanitize_transfer_filename(m.filename);
        if (!safe_name) {
            std::string err = "rejected unsafe filename";
            log_event("file_recv_rejected", m.filename + " reason=unsafe_filename");
            try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        const auto metadata = validate_transfer_metadata(
            m.filesize, m.total_chunks, config_.transfer_max_bytes);
        if (!metadata.ok) {
            const std::string& err = metadata.reason;
            log_event("file_recv_rejected",
                      *safe_name + " size=" + std::to_string(m.filesize) +
                      " chunks=" + std::to_string(m.total_chunks) +
                      " reason=" + err);
            try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        std::error_code ec;
        fs::create_directories(recv_dir, ec);
        if (ec) {
            std::string err = "cannot create receive directory";
            log_event("file_recv_failed", recv_dir + " reason=" + ec.message());
            try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }

        // Starting a new transfer on the same connection aborts its old partial.
        auto& state = c.file_receive;
        if (state.active) {
            state.file.close();
            fs::remove(state.path + ".part", ec);
            state.active = false;
        }

        std::string out_path = (fs::path(recv_dir) / *safe_name).string();
        if (!path_is_inside_directory(out_path, recv_dir)) {
            std::string err = "path escapes receive directory";
            log_event("file_recv_rejected", *safe_name + " reason=path_escape");
            try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        int suffix = 1;
        while (fs::exists(out_path) || fs::exists(out_path + ".part")) {
            std::string alt = (fs::path(recv_dir) /
                               (*safe_name + "." + std::to_string(suffix))).string();
            if (!path_is_inside_directory(alt, recv_dir)) break;
            out_path = alt;
            ++suffix;
        }
        const std::string part_path = out_path + ".part";

        state = FileReceiveState{};
        state.filename = *safe_name;
        state.path = out_path;
        state.checksum = m.checksum;
        state.expected_size = m.filesize;
        state.total_chunks = m.total_chunks;
        state.file.open(part_path, std::ios::binary | std::ios::trunc);
        state.active = state.file.is_open();
        if (!state.active) {
            std::string err = "cannot open " + out_path + ".part";
            log_event("file_recv_failed", err);
            try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        log_event("file_recv_start", *safe_name + " -> " + out_path);
        // Acknowledge chunk index 0 to start streaming
        try { write_frame(c.ssl.get(), FileAckMsg{0, 0, false, ""}, CONTROL_STREAM_ID); } catch (...) {}
    }

    void handle_file_chunk(Conn& c, const FileChunkMsg& m) {
        auto& state = c.file_receive;
        if (!state.active) {
            log_event("file_chunk_orphan", "no active receive for chunk " + std::to_string(m.chunk_index));
            try { write_frame(c.ssl.get(), FileAckMsg{m.chunk_index, 0, true, "no active receive"}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        // Decompress chunk data (zstd)
        std::vector<uint8_t> decompressed;
        if (!m.data.empty()) {
            decompressed = zstd_decompress(std::span<const uint8_t>(m.data.data(), m.data.size()));
        }
        const auto chunk_valid = validate_transfer_chunk(
            state.expected_size, state.received_bytes, state.received_chunks,
            state.total_chunks, m.chunk_index, m.total_chunks, decompressed.size());
        if (!chunk_valid.ok) {
            const std::string err = chunk_valid.reason;
            log_event("file_chunk_rejected", state.filename + " reason=" + err);
            state.file.close();
            std::error_code ec;
            std::filesystem::remove(state.path + ".part", ec);
            state.active = false;
            try { write_frame(c.ssl.get(), FileAckMsg{m.chunk_index, state.received_chunks, true, err}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        if (!decompressed.empty()) {
            state.file.write(reinterpret_cast<const char*>(decompressed.data()),
                             static_cast<std::streamsize>(decompressed.size()));
            if (!state.file) {
                const std::string err = "failed to write receive file";
                log_event("file_recv_failed", state.filename + " reason=write_error");
                state.file.close();
                std::error_code ec;
                std::filesystem::remove(state.path + ".part", ec);
                state.active = false;
                try { write_frame(c.ssl.get(), FileAckMsg{m.chunk_index, state.received_chunks, true, err}, CONTROL_STREAM_ID); } catch (...) {}
                return;
            }
        }
        state.received_chunks = m.chunk_index + 1;
        state.received_bytes += decompressed.size();
        if (state.received_chunks >= state.total_chunks) {
            state.file.close();
            namespace fs = std::filesystem;
            const std::string final_path = state.path;
            const std::string part_path = final_path + ".part";
            const std::string actual = sha256_file_stream(part_path);
            const bool checksum_ok = !actual.empty() && actual == state.checksum;
            std::string final_error;
            if (!checksum_ok) {
                final_error = "checksum mismatch";
                std::error_code ec;
                fs::remove(part_path, ec);
            } else {
                std::error_code ec;
                fs::rename(part_path, final_path, ec);
                if (ec) {
                    final_error = "cannot publish received file";
                    log_event("file_recv_rename_failed",
                              part_path + " -> " + final_path + " reason=" + ec.message());
                    fs::remove(part_path, ec);
                }
            }
            const bool complete_ok = final_error.empty();
            log_event("file_recv_complete", state.filename
                       + " " + std::to_string(state.received_chunks) + " chunks"
                       + (complete_ok ? " checksum_ok" : " ERROR=" + final_error));
            state.active = false;
            // Send final ack
            try { write_frame(c.ssl.get(), FileAckMsg{m.chunk_index, m.total_chunks, !complete_ok, final_error}, CONTROL_STREAM_ID); } catch (...) {}
        } else {
            // Request next chunk
            try { write_frame(c.ssl.get(), FileAckMsg{m.chunk_index, m.chunk_index + 1, false, ""}, CONTROL_STREAM_ID); } catch (...) {}
        }
    }

    // ── Daemon file_send (runs inside event loop, reuses existing mesh conn) ──
    bool daemon_file_send(const std::string& peer_name, const std::string& local_path) {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path) || fs::is_directory(local_path)) {
            log_event("file_send_error", "not found or is dir: " + local_path);
            return false;
        }
        // Find the connection
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, peer_name)) { target = &c; break; }
        }
        if (!target) {
            log_event("file_send_error", "no conn to " + peer_name);
            return false;
        }

        uint64_t filesize = static_cast<uint64_t>(fs::file_size(local_path));
        const auto shape = calculate_transfer_metadata(filesize, config_.transfer_max_bytes);
        if (!shape.ok) {
            log_event("file_send_error", shape.reason + ": " + local_path);
            return false;
        }
        std::string filename = fs::path(local_path).filename().string();
        std::string checksum = sha256_file_stream(local_path);
        if (checksum.empty()) { log_event("file_send_error", "cannot hash " + local_path); return false; }

        const uint32_t total_chunks = shape.expected_chunks;

        FileMetaMsg meta;
        meta.filename = filename; meta.filesize = filesize;
        meta.checksum = checksum; meta.total_chunks = total_chunks;
        write_frame(target->ssl.get(), meta, CONTROL_STREAM_ID);
        log_event("file_send_start", filename + " -> " + peer_name);
        std::cout << "sending " << filename << " (" << filesize << " bytes, "
                  << total_chunks << " chunk(s), sha256:" << checksum.substr(0, 12) << "...)\n";

        // Fire all chunks without per-chunk ACK wait (must not block inside event loop).
        std::ifstream infile(local_path, std::ios::binary);
        if (!infile) { log_event("file_send_error", "cannot open " + local_path); return false; }
        std::vector<char> raw(kTransferChunkRawSize);
        for (uint32_t ci = 0; ci < total_chunks; ++ci) {
            infile.read(raw.data(), static_cast<std::streamsize>(kTransferChunkRawSize));
            size_t chunk_sz = static_cast<size_t>(infile.gcount());
            std::vector<uint8_t> compressed;
            if (chunk_sz > 0) {
                compressed = zstd_compress(
                    std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), chunk_sz));
            }
            FileChunkMsg chunk;
            chunk.chunk_index = ci; chunk.total_chunks = total_chunks;
            chunk.data = std::move(compressed);
            try { write_frame(target->ssl.get(), chunk, CONTROL_STREAM_ID); } catch (...) { return false; }
        }
        log_event("file_send_complete", filename + " " + std::to_string(filesize) + " bytes " + std::to_string(total_chunks) + " chunks");
        std::cout << "sent " << filename << " (" << filesize << " bytes, " << total_chunks << " chunks, sha256:" << checksum.substr(0, 12) << "...)\n";
        return true;
    }

    // v2.0.6: transport-agnostic file send-wait. Runs on the event loop or a
    // worker thread; caller must ensure exclusive SSL transport access.
    std::string file_send_wait_on_transport(
            SSL* ssl, SOCKET sock_fd, const std::string& local_path,
            const std::function<bool()>& is_cancelled = {},
            const std::function<void(const std::string&)>& on_progress = {}) {
        if (!socket_selectable(sock_fd)) return "ERROR socket exceeds select limit";
        namespace fs = std::filesystem;
        auto emit = [&](const std::string& line) {
            if (on_progress) on_progress(line);
            else std::cerr << line << "\n";
        };
        if (!fs::exists(local_path) || fs::is_directory(local_path))
            return "ERROR file not found or is a directory: " + local_path;

        uint64_t filesize = static_cast<uint64_t>(fs::file_size(local_path));
        const auto shape = calculate_transfer_metadata(filesize, config_.transfer_max_bytes);
        if (!shape.ok) return "ERROR " + shape.reason;
        std::string filename = fs::path(local_path).filename().string();
        std::string checksum = sha256_file_stream(local_path);
        if (checksum.empty()) return "ERROR cannot hash " + local_path;
        const uint32_t total_chunks = shape.expected_chunks;

        try {
            FileMetaMsg meta;
            meta.filename = filename; meta.filesize = filesize;
            meta.checksum = checksum; meta.total_chunks = total_chunks;
            write_frame(ssl, meta, CONTROL_STREAM_ID);
        } catch (const std::exception& e) {
            return "ERROR send meta: " + std::string(e.what());
        }

        auto overall_deadline = std::chrono::steady_clock::now() + transfer_overall_timeout(filesize);
        auto idle_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(kTransferIdleTimeoutSec);

        auto wait_ack = [&](uint32_t expected_next) -> std::string {
            while (std::chrono::steady_clock::now() < overall_deadline &&
                   std::chrono::steady_clock::now() < idle_deadline) {
                if (is_cancelled && is_cancelled()) return "ERROR cancelled";
                if (SSL_pending(ssl) <= 0) {
                    fd_set rfds; FD_ZERO(&rfds); FD_SET(sock_fd, &rfds);
                    timeval tv{2, 0};
                    if (select(static_cast<int>(sock_fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0)
                        continue;
                }
                try {
                    Message resp = read_frame(ssl);
                    if (std::holds_alternative<FileAckMsg>(resp)) {
                        auto& ack = std::get<FileAckMsg>(resp);
                        if (ack.error) return "ERROR remote: " + ack.error_msg;
                        if (ack.next_requested >= expected_next) {
                            idle_deadline = std::chrono::steady_clock::now() +
                                            std::chrono::seconds(kTransferIdleTimeoutSec);
                            return "OK";
                        }
                    } else if (std::holds_alternative<PingMsg>(resp)) {
                        write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                    }
                } catch (const std::exception& e) {
                    return "ERROR transfer ack: " + std::string(e.what());
                } catch (...) {
                    return "ERROR transfer ack failed";
                }
            }
            if (std::chrono::steady_clock::now() >= overall_deadline)
                return "ERROR transfer overall timeout";
            return "ERROR transfer idle timeout waiting for ack";
        };

        std::string ack = wait_ack(0);
        if (ack.rfind("ERROR", 0) == 0) return ack;

        std::ifstream infile(local_path, std::ios::binary);
        if (!infile) return "ERROR cannot open " + local_path;
        auto t0 = std::chrono::steady_clock::now();
        auto last_progress = t0;
        uint64_t bytes_sent = 0;
        std::vector<char> raw(kTransferChunkRawSize);

        for (uint32_t ci = 0; ci < total_chunks; ++ci) {
            if (is_cancelled && is_cancelled()) return "ERROR cancelled";
            infile.read(raw.data(), static_cast<std::streamsize>(kTransferChunkRawSize));
            size_t chunk_sz = static_cast<size_t>(infile.gcount());
            std::vector<uint8_t> compressed;
            if (chunk_sz > 0) {
                compressed = zstd_compress(
                    std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), chunk_sz));
            }
            FileChunkMsg chunk;
            chunk.chunk_index = ci;
            chunk.total_chunks = total_chunks;
            chunk.data = std::move(compressed);
            try { write_frame(ssl, chunk, CONTROL_STREAM_ID); }
            catch (const std::exception& e) { return "ERROR send chunk: " + std::string(e.what()); }
            bytes_sent += chunk_sz;
            ack = wait_ack(ci + 1);
            if (ack.rfind("ERROR", 0) == 0) return ack;

            auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= std::chrono::seconds(kTransferProgressIntervalSec) ||
                ci + 1 == total_chunks) {
                last_progress = now;
                double elapsed = std::max(0.001, std::chrono::duration<double>(now - t0).count());
                double rate = (static_cast<double>(bytes_sent) / elapsed) / (1024.0 * 1024.0);
                int eta = 0;
                if (rate > 0.001 && filesize > bytes_sent)
                    eta = static_cast<int>((static_cast<double>(filesize - bytes_sent) /
                                           (rate * 1024.0 * 1024.0)));
                emit(format_transfer_progress("send", filename, ci + 1, total_chunks,
                                              bytes_sent, filesize, rate, eta));
            }
        }

        log_event("file_send_wait_complete", filename + " " + std::to_string(filesize) + " bytes");
        return "OK sent " + filename + " " + std::to_string(filesize) + " bytes sha256:" + checksum;
    }

    // v2.0.6: transport-agnostic remote file request fulfillment. Peer asked us
    // to send <path>; this runs on a worker thread with exclusive SSL access.
    std::string file_request_on_transport(
            SSL* ssl, SOCKET sock_fd, const std::string& path,
            const std::function<bool()>& is_cancelled = {},
            const std::function<void(const std::string&)>& on_progress = {}) {
        if (!socket_selectable(sock_fd)) return "ERROR socket exceeds select limit";
        namespace fs = std::filesystem;
        auto emit = [&](const std::string& line) {
            if (on_progress) on_progress(line);
            else std::cerr << line << "\n";
        };

        if (!fs::exists(path) || fs::is_directory(path)) {
            log_event("file_request_error", "not found: " + path);
            try { write_frame(ssl, FileAckMsg{0, 0, true, "file not found: " + path}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR file not found: " + path;
        }
        uint64_t filesize = static_cast<uint64_t>(fs::file_size(path));
        const auto shape = calculate_transfer_metadata(filesize, config_.transfer_max_bytes);
        if (!shape.ok) {
            try { write_frame(ssl, FileAckMsg{0, 0, true, shape.reason}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR " + shape.reason;
        }
        std::string filename = fs::path(path).filename().string();
        std::string checksum = sha256_file_stream(path);
        if (checksum.empty()) {
            log_event("file_request_error", "cannot hash " + path);
            try { write_frame(ssl, FileAckMsg{0, 0, true, "cannot hash file"}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR cannot hash file";
        }
        const uint32_t total_chunks = shape.expected_chunks;

        try {
            FileMetaMsg meta;
            meta.filename = filename; meta.filesize = filesize;
            meta.checksum = checksum; meta.total_chunks = total_chunks;
            write_frame(ssl, meta, CONTROL_STREAM_ID);
        } catch (const std::exception& e) {
            return "ERROR send meta: " + std::string(e.what());
        }
        log_event("file_request_sending", filename + " " + std::to_string(total_chunks) + " chunks");

        std::ifstream infile(path, std::ios::binary);
        if (!infile) { log_event("file_request_error", "cannot open " + path); return "ERROR cannot open " + path; }
        std::vector<char> raw(kTransferChunkRawSize);

        auto overall_deadline = std::chrono::steady_clock::now() + transfer_overall_timeout(filesize);
        auto idle_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(kTransferIdleTimeoutSec);

        uint64_t bytes_sent = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto last_progress = t0;

        for (uint32_t ci = 0; ci < total_chunks; ++ci) {
            if (is_cancelled && is_cancelled()) {
                try { write_frame(ssl, FileAckMsg{ci, ci, true, "cancelled"}, CONTROL_STREAM_ID); } catch (...) {}
                return "ERROR cancelled";
            }
            if (std::chrono::steady_clock::now() >= overall_deadline)
                return "ERROR transfer overall timeout at chunk " + std::to_string(ci);
            if (std::chrono::steady_clock::now() >= idle_deadline)
                return "ERROR transfer idle timeout at chunk " + std::to_string(ci);

            infile.read(raw.data(), static_cast<std::streamsize>(kTransferChunkRawSize));
            size_t chunk_sz = static_cast<size_t>(infile.gcount());
            std::vector<uint8_t> compressed;
            if (chunk_sz > 0) {
                compressed = zstd_compress(
                    std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), chunk_sz));
            }
            FileChunkMsg chunk;
            chunk.chunk_index = ci; chunk.total_chunks = total_chunks;
            chunk.data = std::move(compressed);

            // Wait for write readiness with bounded deadline so a throttled peer
            // cannot stall the event loop indefinitely.
            fd_set wfds; FD_ZERO(&wfds); FD_SET(sock_fd, &wfds);
            fd_set rfds; FD_ZERO(&rfds); FD_SET(sock_fd, &rfds);
            timeval tv;
            auto remaining = idle_deadline - std::chrono::steady_clock::now();
            if (remaining.count() <= 0) return "ERROR transfer idle timeout at chunk " + std::to_string(ci);
            tv.tv_sec = static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(remaining).count());
            tv.tv_usec = static_cast<long>(std::chrono::duration_cast<std::chrono::microseconds>(remaining).count() % 1000000);
            int sel = select(static_cast<int>(sock_fd) + 1, &rfds, &wfds, nullptr, &tv);
            if (sel < 0) return "ERROR select failed at chunk " + std::to_string(ci);
            if (sel == 0) return "ERROR transfer idle timeout at chunk " + std::to_string(ci);

            // Drain any pings before writing so the peer stays alive.
            if (FD_ISSET(sock_fd, &rfds) || SSL_pending(ssl) > 0) {
                try {
                    Message resp = read_frame(ssl);
                    if (std::holds_alternative<PingMsg>(resp)) {
                        write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                    } else if (std::holds_alternative<FileAckMsg>(resp)) {
                        auto& ack = std::get<FileAckMsg>(resp);
                        if (ack.error) return "ERROR remote: " + ack.error_msg;
                    }
                } catch (...) {}
            }

            try { write_frame(ssl, chunk, CONTROL_STREAM_ID); }
            catch (const std::exception& e) {
                log_event("file_request_error", "send chunk failed " + std::to_string(ci));
                return "ERROR send chunk " + std::to_string(ci) + ": " + e.what();
            }
            bytes_sent += chunk_sz;
            idle_deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(kTransferIdleTimeoutSec);

            auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= std::chrono::seconds(kTransferProgressIntervalSec) ||
                ci + 1 == total_chunks) {
                last_progress = now;
                double elapsed = std::max(0.001, std::chrono::duration<double>(now - t0).count());
                double rate = (static_cast<double>(bytes_sent) / elapsed) / (1024.0 * 1024.0);
                int eta = 0;
                if (rate > 0.001 && filesize > bytes_sent)
                    eta = static_cast<int>((static_cast<double>(filesize - bytes_sent) /
                                           (rate * 1024.0 * 1024.0)));
                emit(format_transfer_progress("send", filename, ci + 1, total_chunks,
                                              bytes_sent, filesize, rate, eta));
            }
        }
        log_event("file_request_complete", filename + " " + std::to_string(filesize) + " bytes");
        return "OK sent " + filename + " " + std::to_string(filesize) + " bytes sha256:" + checksum;
    }

    std::string daemon_file_send_wait(const std::string& peer_name, const std::string& local_path,
                                      const std::function<void(const std::string&)>& on_progress = {}) {
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, peer_name)) {
                target = &c; break;
            }
        }
        if (!target) return "ERROR no conn to " + peer_name;
        if (target->exec_busy->exchange(true)) return "ERROR peer busy with another transfer, retry";
        target->exec_completed->store(false);
        struct BusyGuard {
            std::shared_ptr<std::atomic<bool>> busy;
            std::shared_ptr<std::atomic<bool>> completed;
            ~BusyGuard() {
                if (completed) completed->store(true);
                if (busy) busy->store(false);
            }
        } guard{target->exec_busy, target->exec_completed};
        target->exec_started_at = std::chrono::steady_clock::now();
        target->exec_last_progress_at->store(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return file_send_wait_on_transport(target->ssl.get(), target->sock_fd, local_path, {}, on_progress);
    }

    // v2.0.6: dispatch entry point for long-operation worker pool.
    void execute_long_operation_task(const LongOperationTask& task) {
        auto progress_to_ipc = [&](const std::string& line) {
            // Refresh the shared "last progress" timestamp on every transfer
            // progress tick. The exec watchdog (check_stale_exec) measures the
            // 90s deadline from this, so a healthy, actively-streaming transfer
            // is never killed, while a STALLED transfer (no progress for 90s)
            // still trips it (BUG-1 guarantee).
            if (task.last_progress_at)
                task.last_progress_at->store(
                    std::chrono::steady_clock::now().time_since_epoch().count());
            if (task.ipc_fd != INVALID_SOCKET) {
                std::string msg = line + "\n";
                send(task.ipc_fd, msg.data(), (int)msg.size(), 0);
            }
        };

        switch (task.type) {
        case LongOperationTask::Type::FileSendWait: {
            auto is_cancelled = [&]() { return task.cancelled && task.cancelled->load(); };
            std::string result = file_send_wait_on_transport(
                task.ssl, task.sock_fd, task.path1, is_cancelled, progress_to_ipc);
            if (task.ipc_fd != INVALID_SOCKET) {
                result += "\n";
                send(task.ipc_fd, result.data(), (int)result.size(), 0);
            } else {
                log_event("file_send_worker_complete", task.peer_name + " " + result);
            }
            break;
        }
        case LongOperationTask::Type::FileRecvWait: {
            auto is_cancelled = [&]() { return task.cancelled && task.cancelled->load(); };
            std::string result = file_recv_wait_on_transport(
                task.ssl, task.sock_fd, task.path1, task.path2, receive_dir_, is_cancelled, progress_to_ipc);
            if (task.ipc_fd != INVALID_SOCKET) {
                result += "\n";
                send(task.ipc_fd, result.data(), (int)result.size(), 0);
            }
            break;
        }
        case LongOperationTask::Type::RemoteFileRequest: {
            auto is_cancelled = [&]() { return task.cancelled && task.cancelled->load(); };
            std::string result = file_request_on_transport(
                task.ssl, task.sock_fd, task.path1, is_cancelled, progress_to_ipc);
            if (task.ipc_fd != INVALID_SOCKET) {
                result += "\n";
                send(task.ipc_fd, result.data(), (int)result.size(), 0);
            } else {
                log_event("file_request_worker_complete", task.peer_name + " " + result);
            }
            break;
        }
        case LongOperationTask::Type::EditDownload:
        case LongOperationTask::Type::EditUpload:
        case LongOperationTask::Type::VFolderSync:
            // v2.0.6: these paths remain synchronous on the event loop. They
            // depend on temp-directory creation, directory traversal, and
            // multiple request/response pairs that are not yet refactored into
            // transport-agnostic worker-safe forms. Leave unchanged per the
            // directive to avoid cosmetic workarounds.
            log_event("worker_unimplemented", std::to_string(static_cast<int>(task.type)));
            if (task.ipc_fd != INVALID_SOCKET) {
                std::string err = "ERROR operation not yet offloaded to worker\n";
                send(task.ipc_fd, err.data(), (int)err.size(), 0);
            }
            break;
        }

        // Clear busy/completed flags on the captured shared_ptrs.
        if (task.exec_completed) task.exec_completed->store(true);
        if (task.exec_busy) task.exec_busy->store(false);
    }

    // ── Daemon file request handler: peer asks us to send them a file ──
    // v2.0.6: offload the long send to the worker pool so the event loop stays
    // responsive. The worker exclusively owns this SSL transport while busy.
    void handle_file_request(Conn& c, const FileRequestMsg& m) {
        log_event("file_request_received", m.path + " from " + c.peer_name);
        if (c.exec_busy->exchange(true)) {
            log_event("file_request_busy", m.path + " from " + c.peer_name);
            try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, "peer busy with another transfer"}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        c.exec_completed->store(false);
        c.exec_cancelled = std::make_shared<std::atomic<bool>>(false);
        c.exec_started_at = std::chrono::steady_clock::now();
        // Reset the shared last-progress timestamp to now so a healthy,
        // actively-streaming inbound file pull survives the 90s exec watchdog
        // (BUG-1 guarantee). Without this, check_stale_exec() falls back to
        // exec_started_at and force-cancels any pull that exceeds 90s.
        c.exec_last_progress_at->store(
            std::chrono::steady_clock::now().time_since_epoch().count());

        LongOperationTask task;
        task.type = LongOperationTask::Type::RemoteFileRequest;
        task.peer_name = c.peer_name;
        task.path1 = m.path;
        task.ssl = c.ssl.get();
        task.sock_fd = c.sock_fd;
        task.exec_busy = c.exec_busy;
        task.exec_completed = c.exec_completed;
        task.cancelled = c.exec_cancelled;
        task.last_progress_at = c.exec_last_progress_at;
        task.ipc_fd = INVALID_SOCKET;
        worker_pool_->enqueue(std::move(task));
    }

    bool begin_async_receive(Conn& target, const std::string& dest_dir) {
        if (!target.pending_recv_dir.empty() || target.file_receive.active) return false;
        target.pending_recv_dir = dest_dir;
        return true;
    }

    // ── Daemon file recv: send FileRequest to peer (non-blocking) ──
    std::string daemon_file_recv(const std::string& peer_name, const std::string& remote_path,
                                 const std::string& local_dir = "") {
        log_event("file_recv_request", remote_path + " from " + peer_name);
        std::string dest_dir = local_dir.empty() ? receive_dir_ : local_dir;
        if (!local_dir.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(local_dir, ec);
            if (ec) return "ERROR cannot create receive directory " + local_dir;
        }
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, peer_name)) { target = &c; break; }
        }
        if (!target) return "ERROR no conn to " + peer_name;
        if (!begin_async_receive(*target, dest_dir))
            return "ERROR receive already pending for " + peer_name;

        FileRequestMsg req;
        req.path = remote_path;
        try { write_frame(target->ssl.get(), req, CONTROL_STREAM_ID); }
        catch (const std::exception& e) {
            target->pending_recv_dir.clear();
            return "ERROR send request: " + std::string(e.what());
        }

        log_event("file_recv_request_sent", remote_path + " -> " + peer_name + " (async)");
        return "request sent to " + peer_name + " for " + remote_path + " (arrives async in " + dest_dir + ")";
    }

    // v2.0.6: transport-agnostic file recv-wait. Caller must ensure exclusive SSL transport access.
    std::string file_recv_wait_on_transport(
            SSL* ssl, SOCKET sock_fd, const std::string& remote_path,
            const std::string& local_dest, const std::string& receive_dir,
            const std::function<bool()>& is_cancelled = {},
            const std::function<void(const std::string&)>& on_progress = {}) {
        if (!socket_selectable(sock_fd)) return "ERROR socket exceeds select limit";
        namespace fs = std::filesystem;
        auto emit = [&](const std::string& line) {
            if (on_progress) on_progress(line);
            else std::cerr << line << "\n";
        };

        try {
            FileRequestMsg req;
            req.path = remote_path;
            write_frame(ssl, req, CONTROL_STREAM_ID);
        } catch (const std::exception& e) {
            return "ERROR send request: " + std::string(e.what());
        }

        auto overall_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7200);
        auto idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTransferIdleTimeoutSec);
        std::optional<FileMetaMsg> meta;
        while (!meta && std::chrono::steady_clock::now() < overall_deadline &&
               std::chrono::steady_clock::now() < idle_deadline) {
            if (is_cancelled && is_cancelled()) return "ERROR cancelled";
            if (SSL_pending(ssl) <= 0) {
                fd_set rfds; FD_ZERO(&rfds); FD_SET(sock_fd, &rfds);
                timeval tv{2, 0};
                if (select(static_cast<int>(sock_fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0)
                    continue;
            }
            try {
                Message resp = read_frame(ssl);
                if (std::holds_alternative<FileMetaMsg>(resp)) {
                    meta = std::get<FileMetaMsg>(resp);
                    idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTransferIdleTimeoutSec);
                } else if (std::holds_alternative<FileAckMsg>(resp)) {
                    auto& ack = std::get<FileAckMsg>(resp);
                    if (ack.error) return "ERROR remote: " + ack.error_msg;
                } else if (std::holds_alternative<PingMsg>(resp)) {
                    write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                }
            } catch (const std::exception& e) {
                return "ERROR receive meta: " + std::string(e.what());
            }
        }
        if (!meta) return "ERROR transfer timeout waiting for file metadata";

        const auto metadata = validate_transfer_metadata(
            meta->filesize, meta->total_chunks, config_.transfer_max_bytes);
        if (!metadata.ok) {
            try { write_frame(ssl, FileAckMsg{0, 0, true, metadata.reason}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR " + metadata.reason;
        }
        overall_deadline = std::chrono::steady_clock::now() + transfer_overall_timeout(meta->filesize);

        auto safe_name = sanitize_transfer_filename(meta->filename);
        if (!safe_name) return "ERROR rejected unsafe remote filename";

        fs::path dest = local_dest.empty() ? fs::path(receive_dir) : fs::path(local_dest);
        std::string dest_str = dest.string();
        bool dest_is_dir = dest_str.empty() || dest_str.back() == '/' || dest_str.back() == '\\';
        std::error_code ec;
        if (!dest_is_dir && fs::exists(dest, ec) && fs::is_directory(dest, ec)) dest_is_dir = true;
        if (dest_is_dir) dest /= *safe_name;
        if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
        std::string part_path = dest.string() + ".part";

        std::ofstream out(part_path, std::ios::binary | std::ios::trunc);
        if (!out) return "ERROR cannot open " + part_path;
        struct PartialFileGuard {
            std::string path;
            bool committed = false;
            ~PartialFileGuard() {
                if (!committed) {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                }
            }
        } partial_guard{part_path};

        Sha256Stream hasher;
        if (!hasher.ok()) return "ERROR sha256 init failed";

        uint32_t chunks_recv = 0;
        uint64_t bytes_recv = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto last_progress = t0;
        try { write_frame(ssl, FileAckMsg{0, 0, false, ""}, CONTROL_STREAM_ID); } catch (...) {}
        while (chunks_recv < meta->total_chunks &&
               std::chrono::steady_clock::now() < overall_deadline &&
               std::chrono::steady_clock::now() < idle_deadline) {
            if (is_cancelled && is_cancelled()) return "ERROR cancelled";
            if (SSL_pending(ssl) <= 0) {
                fd_set rfds; FD_ZERO(&rfds); FD_SET(sock_fd, &rfds);
                timeval tv{2, 0};
                if (select(static_cast<int>(sock_fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0)
                    continue;
            }
            try {
                Message resp = read_frame(ssl);
                if (std::holds_alternative<FileChunkMsg>(resp)) {
                    auto& chunk = std::get<FileChunkMsg>(resp);
                    std::vector<uint8_t> data;
                    if (!chunk.data.empty())
                        data = zstd_decompress(std::span<const uint8_t>(chunk.data.data(), chunk.data.size()));
                    const auto chunk_valid = validate_transfer_chunk(
                        meta->filesize, bytes_recv, chunks_recv, meta->total_chunks,
                        chunk.chunk_index, chunk.total_chunks, data.size());
                    if (!chunk_valid.ok) {
                        try { write_frame(ssl, FileAckMsg{
                            chunk.chunk_index, chunks_recv, true, chunk_valid.reason},
                            CONTROL_STREAM_ID); } catch (...) {}
                        return "ERROR " + chunk_valid.reason;
                    }
                    if (!data.empty()) {
                        out.write(reinterpret_cast<const char*>(data.data()),
                                  static_cast<std::streamsize>(data.size()));
                        if (!out) return "ERROR failed to write receive file";
                        hasher.update(data);
                        bytes_recv += data.size();
                    }
                    ++chunks_recv;
                    idle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kTransferIdleTimeoutSec);
                    write_frame(ssl, FileAckMsg{chunk.chunk_index, chunks_recv, false, ""}, CONTROL_STREAM_ID);

                    auto now = std::chrono::steady_clock::now();
                    if (now - last_progress >= std::chrono::seconds(kTransferProgressIntervalSec) ||
                        chunks_recv == meta->total_chunks) {
                        last_progress = now;
                        double elapsed = std::max(0.001, std::chrono::duration<double>(now - t0).count());
                        double rate = (static_cast<double>(bytes_recv) / elapsed) / (1024.0 * 1024.0);
                        int eta = 0;
                        if (rate > 0.001 && meta->filesize > bytes_recv)
                            eta = static_cast<int>((static_cast<double>(meta->filesize - bytes_recv) /
                                                   (rate * 1024.0 * 1024.0)));
                        emit(format_transfer_progress("recv", *safe_name, chunks_recv, meta->total_chunks,
                                                      bytes_recv, meta->filesize, rate, eta));
                    }
                } else if (std::holds_alternative<FileAckMsg>(resp)) {
                    auto& ack = std::get<FileAckMsg>(resp);
                    if (ack.error) return "ERROR remote: " + ack.error_msg;
                } else if (std::holds_alternative<PingMsg>(resp)) {
                    write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                }
            } catch (const std::exception& e) {
                return "ERROR receive chunk: " + std::string(e.what());
            }
        }
        out.close();
        if (chunks_recv < meta->total_chunks) {
            if (std::chrono::steady_clock::now() >= overall_deadline)
                return "ERROR transfer overall timeout after " + std::to_string(chunks_recv) + "/" +
                       std::to_string(meta->total_chunks) + " chunks";
            return "ERROR transfer idle timeout after " + std::to_string(chunks_recv) + "/" +
                   std::to_string(meta->total_chunks) + " chunks";
        }

        std::string actual = hasher.final_hex();
        if (actual != meta->checksum) {
            return "ERROR checksum mismatch expected " + meta->checksum + " got " + actual;
        }
        if (bytes_recv != meta->filesize) {
            return "ERROR received byte count does not match metadata";
        }
        fs::rename(part_path, dest, ec);
        if (ec) return "ERROR rename failed: " + ec.message();
        partial_guard.committed = true;

        log_event("file_recv_wait_complete", meta->filename + " -> " + dest.string());
        return "OK received " + dest.string() + " " + std::to_string(bytes_recv) + " bytes sha256:" + actual;
    }

    std::string daemon_file_recv_wait(const std::string& peer_name, const std::string& remote_path,
                                      const std::string& local_dest,
                                      const std::function<void(const std::string&)>& on_progress = {}) {
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, peer_name)) {
                target = &c; break;
            }
        }
        if (!target) return "ERROR no conn to " + peer_name;
        if (target->exec_busy->exchange(true)) return "ERROR peer busy with another transfer, retry";
        target->exec_completed->store(false);
        struct BusyGuard {
            std::shared_ptr<std::atomic<bool>> busy;
            std::shared_ptr<std::atomic<bool>> completed;
            ~BusyGuard() {
                if (completed) completed->store(true);
                if (busy) busy->store(false);
            }
        } guard{target->exec_busy, target->exec_completed};
        target->exec_started_at = std::chrono::steady_clock::now();
        target->exec_last_progress_at->store(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return file_recv_wait_on_transport(target->ssl.get(), target->sock_fd, remote_path, local_dest, receive_dir_, {}, on_progress);
    }

    std::string daemon_reconnect_peer(const std::string& peer_name) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return "ERROR unknown peer: " + peer_name;

        std::string pubkey;
        for (const auto& s : config_.seeds) {
            if (peer_name_eq(s.name, peer_name)) { pubkey = s.pubkey_hex; break; }
        }
        if (pubkey.empty()) {
            for (const auto& d : config_.discovered) {
                if (peer_name_eq(d.name, peer_name)) { pubkey = d.pubkey_hex; break; }
            }
        }

        auto matches_peer = [&](const Conn& c) {
            return c.purpose == ConnectionPurpose::Mesh &&
                   (peer_name_eq(c.peer_name, peer_name) ||
                    (!addr.empty() && c.peer_addr == addr) ||
                    (!pubkey.empty() && c.peer_pubkey == pubkey));
        };

        // Preflight all matching transports before removing any of them. If a
        // worker owns even one duplicate, reject atomically instead of tearing
        // down idle siblings and then discovering the busy transport.
        for (const auto& c : conns_) {
            if (matches_peer(c) && c.exec_busy && c.exec_busy->load())
                return "ERROR peer busy with an active data operation";
        }

        int removed = 0;
        for (size_t i = 0; i < conns_.size();) {
            auto& c = conns_[i];
            if (!matches_peer(c)) { ++i; continue; }
            if (!remove_conn(i))
                return "ERROR peer busy with an active data operation";
            ++removed;
        }

        backoffs_.erase(addr);
        PeerEntry target_peer{peer_name, addr, pubkey};
        if (should_accept_only_for(target_peer)) {
            auto accept_deadline = std::chrono::steady_clock::now() +
                                   std::chrono::milliseconds(kTieBreakAcceptWindowMs);
            accept_only_until_[addr] = accept_deadline;
            log_event("peer_reconnect_defer_dial",
                      peer_name + " addr=" + addr + " accept_only_ms=" +
                      std::to_string(kTieBreakAcceptWindowMs));
            return "OK reconnect waiting for inbound peer " + peer_name;
        }
        if (!start_outbound_handshake(target_peer))
            return "ERROR reconnect already pending or could not start for " + peer_name;
        log_event("peer_reconnect", peer_name + " removed=" + std::to_string(removed) +
                  " async_started");
        return "OK reconnecting " + peer_name;
    }

    void service_reconnect_wait_once(int timeout_ms) {
        if (listen_fd_ == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return;
        }

        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);
        FD_SET(listen_fd_, &read_fds);
        SOCKET max_fd = listen_fd_;
        for (const auto& c : conns_) {
            if (c.exec_busy && c.exec_busy->load()) continue;
            if (c.sock_fd == INVALID_SOCKET) continue;
            FD_SET(c.sock_fd, &read_fds);
            if (c.sock_fd > max_fd) max_fd = c.sock_fd;
        }
        for (auto& ph : pending_handshakes_) {
            if (ph.sock_fd == INVALID_SOCKET) continue;
            if (ph.want_read) FD_SET(ph.sock_fd, &read_fds);
            if (ph.want_write) FD_SET(ph.sock_fd, &write_fds);
            if (ph.sock_fd > max_fd) max_fd = ph.sock_fd;
        }

        timeval tv{};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int nfds = select(static_cast<int>(max_fd) + 1, &read_fds, &write_fds, nullptr, &tv);
        if (nfds <= 0) return;

        if (FD_ISSET(listen_fd_, &read_fds)) {
            accept_inbound();
            --nfds;
        }
        for (int i = 0; i < static_cast<int>(conns_.size()) && nfds > 0; ++i) {
            if (conns_[static_cast<size_t>(i)].sock_fd != INVALID_SOCKET &&
                FD_ISSET(conns_[static_cast<size_t>(i)].sock_fd, &read_fds)) {
                check_conn_read(i);
                --nfds;
            }
        }

        // v2.0.6: advance outbound handshakes while waiting for reconnect.
        advance_handshakes();

        check_stale_exec();
        check_pong_timeouts();
        clean_dead_conns();
    }

    std::string daemon_stats_summary() const {
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started_at_).count();
        size_t live_conns = 0;
        uint64_t bytes_in = 0, bytes_out = 0;
        for (const auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            ++live_conns;
            bytes_in += c.bytes_in;
            bytes_out += c.bytes_out;
        }
        auto live_sessions = sessions_.list();
        return "node=" + config_.node_name
             + " uptime=" + std::to_string(uptime) + "s"
             + " peers=" + std::to_string(live_conns) + "/" + std::to_string(config_.max_peers)
             + " active_sessions=" + std::to_string(live_sessions.size())
             + " bytes_in=" + std::to_string(bytes_in)
             + " bytes_out=" + std::to_string(bytes_out);
    }

    // ── Daemon edit download: request file, receive to temp, return path+checksum ──
    std::string daemon_edit_dl(const std::string& peer_name, const std::string& remote_path) {
        namespace fs = std::filesystem;
        // Create temp dir
        std::string tmp_dir;
#ifdef _WIN32
        char tp[MAX_PATH+1]={}, tb[MAX_PATH+1]={};
        GetTempPathA(sizeof(tp), tp);
        GetTempFileNameA(tp, "bsed", 0, tb);
        DeleteFileA(tb); CreateDirectoryA(tb, nullptr);
        tmp_dir = tb;
#else
        char tmpl[] = "/tmp/bsedit-XXXXXX";
        char* d = mkdtemp(tmpl);
        tmp_dir = d ? d : "/tmp/bsedit";
#endif
        // Find conn
        Conn* target = nullptr;
        for (auto& c : conns_) { if (is_live_mesh_transport_for(c, peer_name)) { target = &c; break; } }
        if (!target) return "ERROR no conn to " + peer_name;

        FileRequestMsg req; req.path = remote_path;
        try { write_frame(target->ssl.get(), req, CONTROL_STREAM_ID); }
        catch (const std::exception& e) { return "ERROR send request: " + std::string(e.what()); }

        // Wait for FileMeta
        std::string filename, checksum;
        uint32_t total_chunks = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(target->sock_fd, &rfds);
            timeval tv{3, 0};
            if (select(static_cast<int>(target->sock_fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
            try {
                Message resp = read_frame(target->ssl.get());
                if (std::holds_alternative<FileMetaMsg>(resp)) {
                    auto& m = std::get<FileMetaMsg>(resp);
                    filename = m.filename; checksum = m.checksum; total_chunks = m.total_chunks;
                    break;
                }
                if (std::holds_alternative<FileAckMsg>(resp)) {
                    auto& ack = std::get<FileAckMsg>(resp);
                    if (ack.error) return "ERROR remote: " + ack.error_msg;
                }
            } catch (...) {}
        }
        if (filename.empty()) return "ERROR no FileMeta from " + peer_name;

        auto safe = sanitize_transfer_filename(filename);
        if (!safe) return "ERROR rejected unsafe remote filename for edit";
        filename = *safe;

        // filesize may be unknown from incomplete meta parse; use chunk count budget.
        // Prefer size-aware overall timeout when meta carried filesize (re-read if needed).
        std::string local_path = tmp_dir + "/" + filename;
        std::string part_path = local_path + ".part";
        {
            std::ofstream out_file(part_path, std::ios::binary);
            if (!out_file) return "ERROR cannot open " + part_path;
            Sha256Stream hasher;
            uint32_t chunks_recv = 0;
            uint64_t bytes_recv = 0;
            auto overall = std::chrono::steady_clock::now() + transfer_overall_timeout(
                static_cast<uint64_t>(total_chunks) * kTransferChunkRawSize);
            auto idle = std::chrono::steady_clock::now() +
                        std::chrono::seconds(kTransferIdleTimeoutSec);
            while (chunks_recv < total_chunks &&
                   std::chrono::steady_clock::now() < overall &&
                   std::chrono::steady_clock::now() < idle) {
                fd_set rfds; FD_ZERO(&rfds); FD_SET(target->sock_fd, &rfds);
                timeval tv{5, 0};
                if (select(static_cast<int>(target->sock_fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
                try {
                    Message resp = read_frame(target->ssl.get());
                    if (std::holds_alternative<FileChunkMsg>(resp)) {
                        auto& chunk = std::get<FileChunkMsg>(resp);
                        if (chunk.chunk_index == chunks_recv) {
                            std::vector<uint8_t> d;
                            if (!chunk.data.empty()) d = zstd_decompress(std::span<const uint8_t>(chunk.data.data(), chunk.data.size()));
                            if (!d.empty()) {
                                out_file.write(reinterpret_cast<const char*>(d.data()),
                                               static_cast<std::streamsize>(d.size()));
                                hasher.update(d);
                                bytes_recv += d.size();
                            }
                            ++chunks_recv;
                            idle = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(kTransferIdleTimeoutSec);
                            try { write_frame(target->ssl.get(), FileAckMsg{chunk.chunk_index, chunks_recv, false, ""}, CONTROL_STREAM_ID); } catch (...) {}
                        }
                    } else if (std::holds_alternative<PingMsg>(resp)) {
                        write_frame(target->ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                    }
                } catch (...) {}
            }
            out_file.close();
            if (chunks_recv < total_chunks) return "ERROR incomplete " + std::to_string(chunks_recv) + "/" + std::to_string(total_chunks);
            if (!checksum.empty()) {
                std::string actual = hasher.final_hex();
                if (actual != checksum) {
                    std::error_code ec; fs::remove(part_path, ec);
                    return "ERROR checksum mismatch";
                }
            }
        }
        fs::rename(part_path, local_path);
        log_event("edit_dl_complete", filename + " -> " + local_path);
        return "OK " + local_path + " " + checksum;
    }

    // ── Daemon edit upload: read local file, upload via handle_file_meta/handle_file_chunk ──
    std::string daemon_edit_up(const std::string& peer_name, const std::string& remote_path, const std::string& local_path) {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path)) return "ERROR file not found: " + local_path;

        Conn* target = nullptr;
        for (auto& c : conns_) { if (is_live_mesh_transport_for(c, peer_name)) { target = &c; break; } }
        if (!target) return "ERROR no conn to " + peer_name;

        uint64_t filesize = static_cast<uint64_t>(fs::file_size(local_path));
        std::string filename = fs::path(local_path).filename().string();
        std::ifstream infile(local_path, std::ios::binary);
        if (!infile) return "ERROR cannot open " + local_path;
        std::string content((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
        std::string checksum = sha256_hex(content);

        const size_t kChunkRawSize = 48 * 1024;
        size_t total = content.size();
        uint32_t total_chunks = static_cast<uint32_t>((total + kChunkRawSize - 1) / kChunkRawSize);
        if (total_chunks == 0) total_chunks = 1;

        FileMetaMsg meta;
        meta.filename = filename; meta.filesize = filesize; meta.checksum = checksum; meta.total_chunks = total_chunks;
        try { write_frame(target->ssl.get(), meta, CONTROL_STREAM_ID); } catch (...) { return "ERROR upload: send meta failed"; }

        // Wait for ACK
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool got_ack = false;
        while (std::chrono::steady_clock::now() < deadline) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(target->sock_fd, &rfds);
            timeval tv{3, 0};
            if (select(static_cast<int>(target->sock_fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
            try {
                Message resp = read_frame(target->ssl.get());
                if (std::holds_alternative<FileAckMsg>(resp)) {
                    auto& ack = std::get<FileAckMsg>(resp);
                    if (ack.error) return "ERROR remote: " + ack.error_msg;
                    got_ack = true; break;
                }
            } catch (...) {}
        }
        if (!got_ack) return "ERROR upload: no ack";

        for (uint32_t ci = 0; ci < total_chunks; ++ci) {
            size_t off = static_cast<size_t>(ci) * kChunkRawSize;
            size_t sz = (std::min)(kChunkRawSize, total - off);
            std::string raw = content.substr(off, sz);
            std::vector<uint8_t> comp;
            if (!raw.empty()) comp = zstd_compress(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()));
            FileChunkMsg c; c.chunk_index = ci; c.total_chunks = total_chunks; c.data = std::move(comp);
            try { write_frame(target->ssl.get(), c, CONTROL_STREAM_ID); } catch (...) { return "ERROR upload: chunk " + std::to_string(ci); }
        }
        log_event("edit_up_complete", filename + " to " + peer_name);
        return "OK uploaded " + filename + " (" + std::to_string(filesize) + " bytes)";
    }

    // One-shot shell IPC deliberately delegates to the client direct-TLS path;
    // no background worker may borrow a mesh connection's SSL transport.


    std::string daemon_vfolder_sync(const std::string& name) {
        MeshConfig::VFolderEntry* vf = nullptr;
        for (auto& v : config_.vfolders) { if (v.name == name) { vf = &v; break; } }
        if (!vf) return "ERROR no vfolder: " + name;
        namespace fs = std::filesystem;
        if (!fs::exists(vf->local_path)) {
            fs::create_directories(vf->local_path);
        }
        // Scan local files, compute SHA-256 for each, send changed files
        int sent = 0;
        int skipped = 0;
        for (auto& entry : fs::recursive_directory_iterator(vf->local_path, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            auto path = entry.path().string();
            auto rel = path.substr(vf->local_path.size() + 1);
            auto remote_file = vf->remote_path + "/" + rel;
            // Read local, compute sha
            std::ifstream f(path, std::ios::binary);
            if (!f) continue;
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string checksum = sha256_hex(content);
            // Find conn
            Conn* target = nullptr;
            for (auto& c : conns_) { if (is_live_mesh_transport_for(c, vf->remote_peer)) { target = &c; break; } }
            if (!target) return "ERROR no conn to " + vf->remote_peer;
            // Send FileMeta
            const size_t kChunkRawSize = 48 * 1024;
            size_t total = content.size();
            uint32_t total_chunks = static_cast<uint32_t>((total + kChunkRawSize - 1) / kChunkRawSize);
            if (total_chunks == 0) total_chunks = 1;
            FileMetaMsg meta;
            meta.filename = fs::path(remote_file).filename().string();
            meta.filesize = static_cast<uint64_t>(total);
            meta.checksum = checksum;
            meta.total_chunks = total_chunks;
            try { write_frame(target->ssl.get(), meta, CONTROL_STREAM_ID); } catch (...) { return "ERROR send meta for " + rel; }
            // Wait for ack
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            bool acked = false;
            while (std::chrono::steady_clock::now() < deadline) {
                fd_set rfds; FD_ZERO(&rfds); FD_SET(target->sock_fd, &rfds);
                timeval tv{3, 0};
                if (select(static_cast<int>(target->sock_fd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
                try {
                    Message resp = read_frame(target->ssl.get());
                    if (std::holds_alternative<FileAckMsg>(resp)) {
                        auto& ack = std::get<FileAckMsg>(resp);
                        if (ack.error) return "ERROR remote for " + rel + ": " + ack.error_msg;
                        acked = true; break;
                    }
                } catch (...) {}
            }
            if (!acked) return "ERROR no ack for " + rel;
            // Send chunks
            for (uint32_t ci = 0; ci < total_chunks; ++ci) {
                size_t offset = static_cast<size_t>(ci) * kChunkRawSize;
                size_t chunk_sz = (std::min)(kChunkRawSize, total - offset);
                std::string raw = content.substr(offset, chunk_sz);
                std::vector<uint8_t> comp;
                if (!raw.empty()) comp = zstd_compress(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()));
                FileChunkMsg c; c.chunk_index = ci; c.total_chunks = total_chunks; c.data = std::move(comp);
                try { write_frame(target->ssl.get(), c, CONTROL_STREAM_ID); } catch (...) { return "ERROR chunk " + std::to_string(ci); }
            }
            ++sent;
            log_event("vfolder_sync_file", rel + " -> " + vf->remote_peer);
        }
        log_event("vfolder_sync_done", name + " sent=" + std::to_string(sent));
        return "OK synced " + name + " (" + std::to_string(sent) + " files)";
    }

    void handle_file_ack(Conn& c, const FileAckMsg& m) {
        if (m.error) {
            log_event("file_send_failed", m.error_msg);
            return;
        }
        log_event("file_chunk_acked", "chunk " + std::to_string(m.chunk_index)
                   + " next=" + std::to_string(m.next_requested));
    }

    void handle_dht_find_value(Conn& c, const DhtFindValueMsg& query) {
#ifndef BS_NO_DHT
        if (!config_.dht_enabled || !dht_inited_) return;
        // Currently no value storage — reply with closest nodes
        auto closest = dht_.find_closest(query.key, 20);
        GossipMsg g;
        for (auto& dp : closest) {
            PeerInfo pi;
            pi.name = dp.name;
            pi.addr = dp.addr;
            pi.last_seen = dp.last_seen;
            g.peers.push_back(std::move(pi));
        }
        if (!g.peers.empty()) {
            try {
                write_frame(c.ssl.get(), g, CONTROL_STREAM_ID);
            } catch (...) {}
        }
#endif
    }

    // ── Dispatch a received message ────────────────────────────

    void dispatch_message(int conn_idx, Message& msg) {
        auto& c = conns_[static_cast<size_t>(conn_idx)];
        if (c.purpose == ConnectionPurpose::Unknown) {
            c.purpose = std::holds_alternative<AttachMsg>(msg)
                ? ConnectionPurpose::DirectSession
                : ConnectionPurpose::Mesh;
        }

        if (std::holds_alternative<PingMsg>(msg)) {
            try {
                write_frame(c.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
            } catch (...) {}
        }
        else if (std::holds_alternative<PongMsg>(msg)) {
            c.last_pong = std::chrono::steady_clock::now();
        }
        else if (std::holds_alternative<HelloMsg>(msg)) {
            auto& h = std::get<HelloMsg>(msg);
            if (!c.initial_hello.has_value()) {
                // Should normally be set during handshake, but handle defensively.
                c.initial_hello = h;
                c.peer_name = h.node_name;
                merge_peers(h.known_peers);
            } else if (*c.initial_hello == h) {
                // Identical retransmission: ignore silently.
                log_event("hello_duplicate_ignored", c.peer_name);
            } else {
                // Mismatched follow-up Hello: close the connection.
                log_event("hello_mismatch_close", c.peer_name);
                c.close_requested = true;
            }
        }
        else if (std::holds_alternative<GossipMsg>(msg)) {
            auto& g = std::get<GossipMsg>(msg);
            merge_peers(g.peers);
        }
        else if (std::holds_alternative<SdpOfferMsg>(msg)) {
            handle_sdp_offer(c, std::get<SdpOfferMsg>(msg));
        }
        else if (std::holds_alternative<SdpAnswerMsg>(msg)) {
            handle_sdp_answer(c, std::get<SdpAnswerMsg>(msg));
        }
        else if (std::holds_alternative<DhtFindNodeMsg>(msg)) {
            handle_dht_find_node(c, std::get<DhtFindNodeMsg>(msg));
        }
        else if (std::holds_alternative<DhtFindValueMsg>(msg)) {
            handle_dht_find_value(c, std::get<DhtFindValueMsg>(msg));
        }
        else if (std::holds_alternative<FileMetaMsg>(msg)) {
            handle_file_meta(c, std::get<FileMetaMsg>(msg));
        }
        else if (std::holds_alternative<FileChunkMsg>(msg)) {
            handle_file_chunk(c, std::get<FileChunkMsg>(msg));
        }
        else if (std::holds_alternative<FileAckMsg>(msg)) {
            handle_file_ack(c, std::get<FileAckMsg>(msg));
        }
        else if (std::holds_alternative<FileRequestMsg>(msg)) {
            handle_file_request(c, std::get<FileRequestMsg>(msg));
        }
        else {
            // Route everything else through session / common handlers
            handle_inbound_session(c, msg);
            handle_outbound_session(c, msg);
            common_message_handler(c, msg);
        }
    }

    // ── Check for data on a connection ─────────────────────────

public:
    // Remove this transport's attachment while leaving the server-owned PTY alive.
    // A replacement connection with the same identity may already be attached
    // during a reconnect race; in that case keep the shared peer-id reference.
    void detach_connection_session(Conn& conn, bool replacement_attached) {
        auto* session = conn.attached_session;
        conn.attached_session = nullptr;
        if (!session || replacement_attached) return;
        sessions_.detach(session->name, conn.peer_pubkey);
        log_event("session_transport_detached", session->name + " from " + conn.peer_name);
    }

    // ────────────────────────────────────────────────────────────────
    // Phase 6: Session message handlers (public for tests)
    // ────────────────────────────────────────────────────────────────

    // write_pty_input — write terminal input to a session's PTY stdin.
    // Windows: duplicate the ConPTY input handle and enqueue to a bounded
    //          dedicated writer so blocking WriteFile never stalls the loop.
    // POSIX:  PTY master is nonblocking. Write as much as possible immediately,
    //         then queue the remainder in session.pending_input. The event loop
    //         drains the queue when the PTY becomes writable. Returns false only
    //         on a hard write error or if the pending queue would exceed its
    //         bounded maximum (no silent overflow).
    bool write_pty_input(Session& session, const void* data, size_t len) {
        if (!data || len == 0) return true;
        if (!session.is_valid()) return false;
#ifdef _WIN32
        return enqueue_windows_pty_input(
            session, std::string_view(static_cast<const char*>(data), len));
#else
        if (session.master_fd < 0) return false;

        // If the child is already backlogged above the high-water mark, do not
        // accept more input now. The event-loop backpressure path will resume
        // reading from the peer once the queue drains below low water.
        if (session.input_backpressured ||
            session.pending_input.size() >= Session::kPtyInputHighWater) {
            if (session.pending_input.size() + len > Session::kPtyInputMax) {
                log_event("pty_input_overflow", session.name);
                return false;
            }
            session.pending_input.append(static_cast<const char*>(data), len);
            session.input_backpressured = true;
            return true;
        }

        // Try to drain any previously queued bytes first, in order.
        if (!session.pending_input.empty()) {
            const ssize_t n = ::write(session.master_fd,
                                      session.pending_input.data(),
                                      session.pending_input.size());
            if (n > 0) {
                session.pending_input.erase(0, static_cast<size_t>(n));
            } else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                session.state = SessionState::Died;
                return false;
            }
            // Preserve ordering: if old input remains queued, append new input
            // behind it instead of writing the new bytes ahead of the backlog.
            if (!session.pending_input.empty()) {
                if (session.pending_input.size() + len > Session::kPtyInputMax) {
                    log_event("pty_input_overflow", session.name);
                    return false;
                }
                session.pending_input.append(static_cast<const char*>(data), len);
                if (session.pending_input.size() >= Session::kPtyInputHighWater)
                    session.input_backpressured = true;
                return true;
            }
        }

        // Immediate write of the new bytes; queue whatever the kernel refuses.
        const char* p = static_cast<const char*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            const ssize_t n = ::write(session.master_fd, p, remaining);
            if (n > 0) {
                p += n;
                remaining -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (session.pending_input.size() + remaining > Session::kPtyInputMax) {
                    log_event("pty_input_overflow", session.name);
                    return false;
                }
                session.pending_input.append(p, remaining);
                if (session.pending_input.size() >= Session::kPtyInputHighWater)
                    session.input_backpressured = true;
                return true;
            }
            session.state = SessionState::Died;
            return false;
        }
        return true;
#endif
    }

#ifndef _WIN32
    // drain_pending_pty_input — called from the event loop when the PTY master
    // is writable. Writes queued input and returns true if the queue dropped
    // below the low-water mark (peer reads may resume).
    bool drain_pending_pty_input(Session& session) {
        if (session.master_fd < 0 || session.pending_input.empty()) return true;
        while (!session.pending_input.empty()) {
            const ssize_t n = ::write(session.master_fd,
                                      session.pending_input.data(),
                                      session.pending_input.size());
            if (n > 0) {
                session.pending_input.erase(0, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            log_event("pty_input_drain_failed", session.name);
            session.state = SessionState::Died;
            return false;
        }
        if (session.pending_input.size() <= Session::kPtyInputLowWater)
            session.input_backpressured = false;
        return !session.input_backpressured;
    }
#endif

    // 1. handle_inbound_session — messages from a remote peer
    //    operating on OUR local sessions
    void handle_inbound_session(Conn& conn, Message& msg) {
        // AttachMsg — peer wants to attach to one of our sessions
        if (std::holds_alternative<AttachMsg>(msg)) {
            auto& a = std::get<AttachMsg>(msg);

            // Multi-hop routing (v2.1): routing="target:ttl" forwards the
            // attach through the mesh when this node is not the destination.
            if (!a.routing.empty()) {
                auto sep = a.routing.find(':');
                std::string hop_target = (sep != std::string::npos)
                    ? a.routing.substr(0, sep) : a.routing;
                int ttl = (sep != std::string::npos)
                    ? std::atoi(a.routing.substr(sep + 1).c_str()) : 0;

                // Forward if we're not the target and TTL allows one more hop
                if (!peer_name_eq(hop_target, config_.node_name) && ttl > 0) {
                    Conn* mesh = nullptr;
                    for (auto& mc : conns_) {
                        if (is_live_mesh_transport_for(mc, hop_target)) {
                            mesh = &mc; break;
                        }
                    }
                    if (mesh && mesh->ssl) {
                        AttachMsg forward = a;
                        forward.routing = hop_target + ":" + std::to_string(ttl - 1);
                        try {
                            write_frame(mesh->ssl.get(), forward, CONTROL_STREAM_ID);
                            log_event("session_attach_forwarded_to_hop",
                                a.session_name + " -> " + hop_target + " ttl=" + std::to_string(ttl));
                        } catch (...) {
                            log_event("session_attach_hop_forward_failed", hop_target);
                        }
                    } else {
                        log_event("session_attach_hop_unreachable", hop_target);
                    }
                    return;
                }
                // If TTL is 0 or we are the target, fall through to local attach
            }

            log_event("session_attach_request",
                      a.session_name + " from " + conn.peer_name);

            const ResolvedSessionCommand shell_cmd = resolve_session_command(
                config_, a.session_name, a.command);
            auto* s = sessions_.attach(a.session_name,
                                       shell_cmd,
                                       a.cols, a.rows, a.term,
                                       conn.peer_pubkey);
            if (s) {
                // Record the detach-signal request (v2.1) so the server can
                // signal the child when the last peer detaches.
                if (!a.signal_on_detach.empty()) s->detach_signal = a.signal_on_detach;
                conn.attached_session = s;

                // Send scrollback to reattaching peer
                auto lines = s->scrollback.read_last_lines(
                    static_cast<size_t>(config_.scrollback_lines));
                if (!lines.empty()) {
                    ScrollbackMsg sb;
                    sb.data = std::move(lines);
                    sb.total_lines = 0; // best-effort
                    sb.chunk_index = 0;
                    try {
                        write_frame(conn.ssl.get(), sb, 0);
                    } catch (...) {}
                }
                log_event("session_attached",
                          a.session_name + " from " + conn.peer_name);
            } else {
                log_event("session_attach_failed",
                          a.session_name + " from " + conn.peer_name);
            }
            return;
        }

        // KeystrokeMsg — peer typed something; forward to PTY
        if (std::holds_alternative<KeystrokeMsg>(msg)) {
            auto& ks = std::get<KeystrokeMsg>(msg);
            if (conn.attached_session && conn.attached_session->is_valid()) {
                if (!write_pty_input(*conn.attached_session,
                                     ks.data.data(), ks.data.size())) {
                    log_event("pty_input_rejected", conn.attached_session->name);
                    conn.close_requested = true;
                }
            }
            return;
        }

        // ResizeMsg — peer resized their terminal
        if (std::holds_alternative<ResizeMsg>(msg)) {
            auto& r = std::get<ResizeMsg>(msg);
            if (conn.attached_session && conn.attached_session->is_valid()) {
#ifdef _WIN32
                if (conn.attached_session->hpcon) {
                    (void)resize_pty(reinterpret_cast<intptr_t>(conn.attached_session->hpcon),
                               r.cols, r.rows);
                }
#else
                if (conn.attached_session->master_fd >= 0) {
                    (void)resize_pty(static_cast<intptr_t>(conn.attached_session->master_fd),
                                     r.cols, r.rows);
                }
#endif
            }
            return;
        }

        // DetachMsg — peer wants to detach from session
        if (std::holds_alternative<DetachMsg>(msg)) {
            if (conn.attached_session) {
                detach_connection_session(conn, has_replacement_transport(conn));
                log_event("session_detached", "from " + conn.peer_name);
            }
            return;
        }

        // SignalMsg — send signal to child process
        if (std::holds_alternative<SignalMsg>(msg)) {
            auto& sig = std::get<SignalMsg>(msg);
            if (conn.attached_session && conn.attached_session->is_valid()) {
#ifdef _WIN32
                if (conn.attached_session->child_pid) {
                    DWORD ctrl_event = 0;
                    switch (sig.signal) {
                        case SignalMsg::SignalType::CtrlC:
                            ctrl_event = CTRL_C_EVENT; break;
                        case SignalMsg::SignalType::CtrlZ:
                            ctrl_event = CTRL_BREAK_EVENT; break;
                        default: break;
                    }
                    if (ctrl_event)
                        GenerateConsoleCtrlEvent(ctrl_event,
                            GetProcessId(conn.attached_session->child_pid));
                }
#else
                if (conn.attached_session->child_pid > 0) {
                    auto& sm = std::get<SignalMsg>(msg);
                    int s = (sm.signal == SignalMsg::SignalType::CtrlC) ? SIGINT :
                            (sm.signal == SignalMsg::SignalType::CtrlZ) ? SIGTSTP :
                            SIGQUIT;
                    kill(conn.attached_session->child_pid, s);
                }
#endif
            }
            // Restart signal: kill + respawn process in same session
            if (sig.signal == SignalMsg::SignalType::Restart) {
                if (conn.attached_session && conn.attached_session->is_valid()) {
                    auto* sess = conn.attached_session;
                    std::string cmd = sess->command;
                    if (!sig.process.empty()) {
                        cmd = prepare_session_command(
                            {sig.process, SessionCommandSource::ClientOverride});
                    }
                    log_event("session_restart", cmd + " on " + sess->name);
                    // Kill the old child and release every PTY resource before
                    // installing the replacement handles.
#ifdef _WIN32
                    if (sess->child_pid) {
                        TerminateProcess(sess->child_pid, 1);
                        WaitForSingleObject(sess->child_pid, 3000);
                        CloseHandle(sess->child_pid);
                        sess->child_pid = nullptr;
                    }
                    if (sess->master_fd) {
                        CloseHandle(sess->master_fd);
                        sess->master_fd = nullptr;
                    }
                    if (sess->write_handle) {
                        CloseHandle(sess->write_handle);
                        sess->write_handle = nullptr;
                    }
                    if (sess->hpcon) {
                        ClosePseudoConsole(sess->hpcon);
                        sess->hpcon = nullptr;
                    }
#else
                    if (sess->child_pid > 0) {
                        kill(sess->child_pid, SIGTERM);
                        int status = 0;
                        for (int i = 0; i < 30; ++i) {
                            if (waitpid(sess->child_pid, &status, WNOHANG) == sess->child_pid) break;
                            usleep(100000);
                        }
                        if (waitpid(sess->child_pid, &status, WNOHANG) != sess->child_pid) {
                            kill(sess->child_pid, SIGKILL);
                            waitpid(sess->child_pid, &status, 0);
                        }
                        sess->child_pid = -1;
                    }
                    if (sess->master_fd >= 0) {
                        close(sess->master_fd);
                        sess->master_fd = -1;
                    }
                    sess->pending_input.clear();
                    sess->input_backpressured = false;
#endif
                    // Spawn the replacement. Session storage remains in place so
                    // attached transports retain a stable pointer.
                    auto new_sess = create_session(sess->name, cmd, 80, 24,
                                                   "xterm-256color");
                    if (new_sess) {
                        sess->master_fd = new_sess->master_fd;
                        sess->child_pid = new_sess->child_pid;
#ifdef _WIN32
                        sess->write_handle = new_sess->write_handle;
                        sess->hpcon = new_sess->hpcon;
                        new_sess->master_fd = nullptr;  // prevent double-close
                        new_sess->child_pid = nullptr;
                        new_sess->write_handle = nullptr;
                        new_sess->hpcon = nullptr;
#else
                        new_sess->master_fd = -1;
                        new_sess->child_pid = -1;
#endif
                        sess->command = new_sess->command;
                        sess->generation = new_sess->generation;
                        sess->last_output_at = new_sess->last_output_at;
                        sess->state = SessionState::Attached;
                        log_event("session_restart_ok", cmd + " respawned ok");
                    } else {
                        log_event("session_restart_failed", "cannot spawn: " + cmd);
                        sess->state = SessionState::Died;
                    }
                }
            }
            return;
        }

        // ClipboardMsg — write bracketed paste to PTY
        // Also echo hash back to confirm receipt
        if (std::holds_alternative<ClipboardMsg>(msg)) {
            auto& cb = std::get<ClipboardMsg>(msg);
            if (conn.attached_session && conn.attached_session->is_valid()) {
                // Write bracketed paste: ESC[200~ <data> ESC[201~
                std::string paste = "\x1b[200~" + cb.text + "\x1b[201~";
                if (!write_pty_input(*conn.attached_session,
                                     paste.data(), paste.size())) {
                    log_event("pty_input_rejected", conn.attached_session->name);
                    conn.close_requested = true;
                }
                // Echo hash back
                if (!cb.hash.empty()) {
                    ClipboardEchoMsg echo;
                    echo.hash = cb.hash;
                    try {
                        write_frame(conn.ssl.get(), echo, 0);
                    } catch (...) {}
                }
            }
            return;
        }
    }

    // 2. handle_outbound_session — messages from our local sessions
    //    destined for a local client (shell_peer CLI mode)
    void handle_outbound_session(Conn& conn, Message& msg) {
        // OutputMsg — write to local stdout (shell_peer display)
        if (std::holds_alternative<OutputMsg>(msg)) {
            auto& o = std::get<OutputMsg>(msg);
            fwrite(o.data.data(), 1, o.data.size(), stdout);
            fflush(stdout);
            return;
        }

        // ClipboardMsg — write to local clipboard
        if (std::holds_alternative<ClipboardMsg>(msg)) {
            auto& cb = std::get<ClipboardMsg>(msg);
#ifdef _WIN32
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                size_t wsize = MultiByteToWideChar(CP_UTF8, 0,
                    cb.text.c_str(), static_cast<int>(cb.text.size()),
                    nullptr, 0);
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE,
                    (wsize + 1) * sizeof(WCHAR));
                if (hMem) {
                    auto* wstr = static_cast<WCHAR*>(GlobalLock(hMem));
                    if (wstr) {
                        MultiByteToWideChar(CP_UTF8, 0,
                            cb.text.c_str(), static_cast<int>(cb.text.size()),
                            wstr, static_cast<int>(wsize));
                        wstr[wsize] = L'\0';
                        GlobalUnlock(hMem);
                    }
                    SetClipboardData(CF_UNICODETEXT, hMem);
                }
                CloseClipboard();
            }
#endif
            return;
        }

        // ExitCodeMsg / SessionDiedMsg — clear remote session tracking
        if (std::holds_alternative<ExitCodeMsg>(msg) ||
            std::holds_alternative<SessionDiedMsg>(msg)) {
            conn.remote_session.clear();
            return;
        }
    }

    // 3. common_message_handler — protocol-level messages
    void common_message_handler(Conn& conn, Message& msg) {
        // PingMsg → PongMsg (already handled in dispatch, but safe to be here)
        if (std::holds_alternative<PingMsg>(msg)) {
            try {
                write_frame(conn.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
            } catch (...) {}
            return;
        }

        // PongMsg → update last_pong (already handled, but safe)
        if (std::holds_alternative<PongMsg>(msg)) {
            conn.last_pong = std::chrono::steady_clock::now();
            return;
        }

        // ScrollbackMsg — write to local stdout (for shell_peer)
        if (std::holds_alternative<ScrollbackMsg>(msg)) {
            auto& sb = std::get<ScrollbackMsg>(msg);
            fwrite(sb.data.data(), 1, sb.data.size(), stdout);
            fflush(stdout);
            // Send ack
            try {
                write_frame(conn.ssl.get(), ScrollbackAckMsg{}, 0);
            } catch (...) {}
            return;
        }

        // ImageDataMsg / ImageFrameMsg — stub, render later (Phase 8)
        if (std::holds_alternative<ImageDataMsg>(msg) ||
            std::holds_alternative<ImageFrameMsg>(msg)) {
            // Phase 8 will handle rendering
            return;
        }

        // SessionListMsg — format and display
        if (std::holds_alternative<SessionListMsg>(msg)) {
            auto& sl = std::get<SessionListMsg>(msg);
            printf("=== Sessions ===\n");
            for (auto& si : sl.sessions) {
                printf("  %s  [%s]  uptime=%llus\n",
                       si.name.c_str(), si.state.c_str(),
                       (unsigned long long)si.uptime_seconds);
            }
            fflush(stdout);
            return;
        }
    }

    // 4. pty_output_poller — poll PTY output for each attached session
    void pty_output_poller() {
        // Drain every live PTY, including detached sessions, so the child never
        // blocks on a full PTY buffer and select() cannot spin on unread output.
        for (const auto& info : sessions_.list()) {
            auto* s = sessions_.get(info.name);
            if (!s || !s->is_pollable()) continue;

            auto fanout = [&](const auto& message) {
                for (auto& target : conns_) {
                    if (target.attached_session != s || target.sock_fd == INVALID_SOCKET ||
                        !target.ssl) {
                        continue;
                    }
                    if (target.exec_busy && target.exec_busy->load()) continue;
                    try { write_frame(target.ssl.get(), message, 0); } catch (...) {}
                }
            };

            // v1.7.1 fix: previously an early `continue` here (when no bytes
            // were pending) skipped the child-exit check further below in
            // the SAME pass, occasionally letting the final chunk of output
            // and the SessionDiedMsg race across two different daemons/
            // connections for fast one-shot commands (e.g. `hostname`),
            // observed as an empty capture with exit 0. Now we always fall
            // through to the exit check even when there's nothing to read.
            // Drain and coalesce the PTY burst in one pass. Full-screen TUIs
            // emit many small cursor-addressing writes; forwarding only 4 KiB
            // per event-loop tick visibly tears the screen apart.
            std::string buf = read_available_pty_output(*s);

            if (buf.empty()) goto check_child_exit;

            // Write to ring buffer
            s->scrollback.write(std::string_view(buf));
            s->touch_output();

            // OSC 52 scan
            {
            auto osc = scan_osc52(buf);
            if (osc.clipboard_text && !osc.clipboard_text->empty()) {
                ClipboardMsg cb;
                cb.text = *osc.clipboard_text;
                cb.hash = sha256_hex(cb.text);
                fanout(cb);
            }

            // Send OutputMsg with cleaned text
            if (!osc.cleaned_text.empty()) {
                OutputMsg om;
                om.data = std::move(osc.cleaned_text);
                // Set render_markdown flag based on heuristic or config override
                if (config_.render_hint == "markdown") om.render_markdown = true;
                else if (config_.render_hint != "raw")
                    om.render_markdown = looks_like_markdown(om.data);
                fanout(om);
            }
            } // end osc scope

            check_child_exit:
            // Check child exit
#ifdef _WIN32
            if (s->child_pid &&
                WaitForSingleObject(s->child_pid, 0) == WAIT_OBJECT_0) {
                // v2.0.1: ConPTY/conhost flushes command text AFTER process exit.
                // Force-close the pseudoconsole to push remaining bytes into the
                // pipe, then drain until quiet. Log fanout failures (was silent).
                auto fanout_out = [&](std::string data) {
                    if (data.empty()) return;
                    OutputMsg lom;
                    lom.data = std::move(data);
                    if (config_.render_hint == "markdown") lom.render_markdown = true;
                    else if (config_.render_hint != "raw")
                        lom.render_markdown = looks_like_markdown(lom.data);
                    int targets = 0;
                    for (auto& target : conns_) {
                        if (target.sock_fd == INVALID_SOCKET || !target.ssl) continue;
                        if (target.exec_busy && target.exec_busy->load()) continue;
                        // Prefer attached_session match; also any DirectSession
                        // peer (one-shot shell uses a dedicated TLS conn).
                        const bool match =
                            target.attached_session == s ||
                            target.purpose == ConnectionPurpose::DirectSession;
                        if (!match) continue;
                        try {
                            write_frame(target.ssl.get(), lom, CONTROL_STREAM_ID);
                            ++targets;
                        } catch (const std::exception& e) {
                            log_event("fanout_output_failed",
                                      s->name + " " + e.what());
                        } catch (...) {
                            log_event("fanout_output_failed", s->name + " unknown");
                        }
                    }
                    log_event("fanout_output",
                              s->name + " targets=" + std::to_string(targets) +
                                  " bytes=" + std::to_string(lom.data.size()));
                };
                auto drain_pipe = [&](int max_ms) {
                    int empty_streak = 0;
                    const auto start = std::chrono::steady_clock::now();
                    size_t total = 0;
                    std::string acc;
                    while (empty_streak < 12) {
                        const auto elapsed =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
                        if (elapsed >= max_ms) break;
                        DWORD avail = 0;
                        if (!s->master_fd ||
                            !PeekNamedPipe(s->master_fd, nullptr, 0, nullptr, &avail, nullptr) ||
                            avail == 0) {
                            Sleep(25);
                            ++empty_streak;
                            continue;
                        }
                        empty_streak = 0;
                        std::string late;
                        late.resize(avail);
                        DWORD got = 0;
                        if (ReadFile(s->master_fd, late.data(), avail, &got, nullptr) && got > 0) {
                            late.resize(got);
                            total += got;
                            s->scrollback.write(std::string_view(late));
                            acc += late;
                        }
                    }
                    // One coalesced OutputMsg after drain (plain text).
                    if (!acc.empty()) {
                        auto osc = scan_osc52(acc);
                        fanout_out(strip_ansi_escapes(osc.cleaned_text));
                    }
                    return total;
                };

                // First pass: drain whatever is already available.
                size_t drained = drain_pipe(250);
                // Closing the pseudo console forces conhost to flush remaining
                // child output into our pipe (Microsoft ConPTY behavior).
                if (s->hpcon) {
                    ClosePseudoConsole(s->hpcon);
                    s->hpcon = nullptr;
                }
                drained += drain_pipe(500);
                // Note: do NOT re-broadcast full session scrollback here — resurrected
                // Session objects retain prior one-shot text and would pollute capture.
                if (drained > 0) {
                    log_event("pty_death_drain",
                              s->name + " bytes=" + std::to_string(drained));
                }

                DWORD exit_code = 0;
                GetExitCodeProcess(s->child_pid, &exit_code);
                sessions_.record_finished(*s, static_cast<int32_t>(exit_code), "died");
                CloseHandle(s->child_pid);
                s->child_pid = nullptr;
                // master_fd / write_handle closed by Session dtor or reattach path
                s->state = SessionState::Died;

                SessionDiedMsg sdm;
                sdm.exit_code = static_cast<int32_t>(exit_code);
                sdm.signal_num = 0;
                for (auto& target : conns_) {
                    if (target.attached_session != s || target.sock_fd == INVALID_SOCKET ||
                        !target.ssl) continue;
                    if (target.exec_busy && target.exec_busy->load()) continue;
                    try {
                        write_frame(target.ssl.get(), sdm, CONTROL_STREAM_ID);
                    } catch (...) {}
                }
                log_event("session_died", s->name + " exit_code=" + std::to_string(exit_code));
            }
#else
            if (s->child_pid > 0) {
                int status = 0;
                pid_t result = waitpid(s->child_pid, &status, WNOHANG);
                if (result == s->child_pid) {
                    SessionDiedMsg sdm;
                    if (WIFEXITED(status)) {
                        sdm.exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        sdm.signal_num = WTERMSIG(status);
                    }
                    sessions_.record_finished(*s, sdm.exit_code, "died");
                    s->child_pid = -1;
                    s->state = SessionState::Died;
                    fanout(sdm);
                }
            }
#endif
        }
    }

private:

    void check_conn_read(int conn_idx) {
        try {
            if (static_cast<size_t>(conn_idx) >= conns_.size()) return;
            Conn& c = conns_[static_cast<size_t>(conn_idx)];
            if (c.sock_fd == INVALID_SOCKET) return;
            if (socket_peer_half_closed(c.sock_fd)) {
                close_conn(c);
                return;
            }

            // Drain only bytes OpenSSL can provide now. Blocking read_frame() can
            // freeze the single-threaded daemon when a TLS record contains a partial
            // next protocol frame. rx_buffer preserves that partial frame until the
            // next readability event.
#ifdef _WIN32
            u_long nonblocking = 1;
            ioctlsocket(c.sock_fd, FIONBIO, &nonblocking);
#else
            const int original_flags = fcntl(c.sock_fd, F_GETFL, 0);
            if (original_flags >= 0)
                fcntl(c.sock_fd, F_SETFL, original_flags | O_NONBLOCK);
#endif
            bool fatal_read = false;
            int fatal_ssl_error = SSL_ERROR_NONE;
            std::array<uint8_t, 64 * 1024> chunk{};
            for (;;) {
                size_t n = 0;
                clear_stale_ssl_errors_before_io();
                const int ret = SSL_read_ex(c.ssl.get(), chunk.data(), chunk.size(), &n);
                if (ret > 0 && n > 0) {
                    c.rx_buffer.insert(c.rx_buffer.end(), chunk.begin(), chunk.begin() +
                                       static_cast<std::ptrdiff_t>(n));
                    c.bytes_in += n;
                    continue;
                }
                const int ssl_err = SSL_get_error(c.ssl.get(), ret);
                if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE) break;
                fatal_read = true;
                fatal_ssl_error = ssl_err;
                break;
            }
#ifdef _WIN32
            nonblocking = 0;
            ioctlsocket(c.sock_fd, FIONBIO, &nonblocking);
#else
            if (original_flags >= 0)
                fcntl(c.sock_fd, F_SETFL, original_flags);
#endif
            if (fatal_read) {
                // SSL_ERROR_ZERO_RETURN is an orderly peer close (normal for a
                // completed one-shot direct session), not an operational error.
                if (fatal_ssl_error != SSL_ERROR_ZERO_RETURN) {
                    log_event("mesh_conn_close", c.peer_name + " reason=fatal_ssl_read ssl_err=" +
                              std::to_string(fatal_ssl_error) + " purpose=" +
                              std::to_string(static_cast<int>(c.purpose)));
                }
                close_conn(c);
                return;
            }

            auto messages = drain_complete_frames(c.rx_buffer);
            for (auto& msg : messages) {
                if (static_cast<size_t>(conn_idx) >= conns_.size()) return;
                if (conns_[static_cast<size_t>(conn_idx)].sock_fd == INVALID_SOCKET) return;
                dispatch_message(conn_idx, msg);
            }
        } catch (const std::exception& e) {
            if (static_cast<size_t>(conn_idx) < conns_.size()) {
                log_event("mesh_conn_close", conns_[static_cast<size_t>(conn_idx)].peer_name +
                          " reason=read_exception detail=" + e.what());
                close_conn(conns_[static_cast<size_t>(conn_idx)]);
            }
        } catch (...) {
            if (static_cast<size_t>(conn_idx) < conns_.size())
                log_event("mesh_conn_close", conns_[static_cast<size_t>(conn_idx)].peer_name +
                          " reason=unknown_read_exception");
            if (static_cast<size_t>(conn_idx) < conns_.size())
                close_conn(conns_[static_cast<size_t>(conn_idx)]);
        }
    }

    // ── Try to connect to seeds/discovered ─────────────────────

    void try_connect_to_seeds() {
        auto now = std::chrono::steady_clock::now();

        // Try seeds first
        for (auto& s : config_.seeds) {
            if (has_conn_for_addr(s.addr)) continue;
            // Also skip by pubkey: an inbound connection from this peer records the
            // peer's EPHEMERAL source port as peer_addr, which never matches the seed's
            // listen addr. Without this guard we would re-dial a peer we are already
            // connected to every loop, creating a second connection that resolve_duplicates
            // then tears down — churning forever and starving the event loop.
            if (!s.pubkey_hex.empty() && has_conn_for_pubkey(s.pubkey_hex)) continue;
            if (config_.require_seed_pins && s.pubkey_hex.empty()) {
                log_event("mesh_seed_missing_pin",
                          s.name + " " + s.addr + " (add pubkey= to seed line)");
                continue;
            }
            if (conns_.size() >= config_.max_peers) break;
            if (should_defer_outbound_for(s, now)) continue;

            auto& bo = backoffs_[s.addr];
            if (bo.attempt > 0 && now < bo.next_attempt) continue;

            // Attempt connect (non-blocking; handshake completes in event loop).
            bool started = start_outbound_handshake(s);
            if (started) {
                backoffs_.erase(s.addr);
            } else {
                bo.attempt++;
                bo.next_attempt = now + std::chrono::milliseconds(bo.delay_ms);
                bo.delay_ms = std::min(std::max(bo.delay_ms * 2, 1000), bo.max_ms);
                break; // one failed bounded dial per loop; keep accept/read responsive
            }
        }

        // Try discovered peers too (if we have room). Snapshot by value:
        // connect_to_peer_impl → merge_peers can push_back to config_.discovered,
        // which would invalidate the loop iterator.
        if (conns_.size() < config_.max_peers) {
            auto discovered_snap = config_.discovered;
            for (auto& d : discovered_snap) {
                if (d.addr.empty()) continue;
                if (!is_trusted_pubkey(d.pubkey_hex)) continue;
                if (has_conn_for_addr(d.addr)) continue;
                if (!d.pubkey_hex.empty() && has_conn_for_pubkey(d.pubkey_hex)) continue;
                if (config_.require_seed_pins && d.pubkey_hex.empty()) continue;
                if (conns_.size() >= config_.max_peers) break;
                if (should_defer_outbound_for(d, now)) continue;

                auto& bo = backoffs_[d.addr];
                if (bo.attempt > 0 && now < bo.next_attempt) continue;
                bool started = start_outbound_handshake(d);
                if (started) {
                    backoffs_.erase(d.addr);
                } else {
                    bo.attempt++;
                    bo.next_attempt = now + std::chrono::milliseconds(bo.delay_ms);
                    bo.delay_ms = std::min(std::max(bo.delay_ms * 2, 1000), bo.max_ms);
                    break; // one failed bounded dial per loop; keep accept/read responsive
                }
            }
        }
    }

    // ── Send Gossip to all connections ─────────────────────────

    void broadcast_gossip() {
        auto g = build_gossip();
        if (g.peers.empty()) return;

        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            if (c.exec_busy && c.exec_busy->load()) continue;
            try {
                write_frame(c.ssl.get(), g, CONTROL_STREAM_ID);
            } catch (...) {}
        }
    }

    // ── Send Ping to all connections ───────────────────────────

    void broadcast_ping() {
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            // A background daemon_shell_exec thread owns this conn's SSL
            // object right now (v1.7 async exec) — it handles Ping/Pong
            // itself; writing here too would race the thread's writes.
            if (c.exec_busy && c.exec_busy->load()) continue;
            try {
                write_frame(c.ssl.get(), PingMsg{}, CONTROL_STREAM_ID);
            } catch (const std::exception& e) {
                log_event("mesh_conn_close", c.peer_name + " reason=ping_write detail=" + e.what());
                close_conn(c);
            } catch (...) {
                log_event("mesh_conn_close", c.peer_name + " reason=ping_write_unknown");
                close_conn(c);
            }
        }
    }

    // ── Check pong timeout ─────────────────────────────────────

    void check_pong_timeouts() {
        auto now = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(config_.pong_timeout_secs);

        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            // A busy worker owns the SSL stream. When it releases ownership,
            // grant a fresh timeout window because it may have consumed Pong
            // frames outside the event loop.
            if (refresh_heartbeat_after_busy(c, now)) continue;
            if (now - c.last_pong > timeout) {
                log_event("mesh_pong_timeout", c.peer_name + " " + c.peer_addr);
                // Attempt graceful TLS shutdown before hard close (POSIX only).
                // Best-effort: if shutdown fails or platform doesn't support it,
                // close_conn still runs.
#ifndef _WIN32
                if (c.ssl && socket_selectable(c.sock_fd)) {
                    int flags = fcntl(c.sock_fd, F_GETFL, 0);
                    fcntl(c.sock_fd, F_SETFL, flags | O_NONBLOCK);
                    auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                    while (std::chrono::steady_clock::now() < dl) {
                        int ret = SSL_shutdown(c.ssl.get());
                        if (ret == 1) break;
                        if (ret == 0) continue;
                        int err = SSL_get_error(c.ssl.get(), ret);
                        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                            fd_set fds; FD_ZERO(&fds);
                            timeval tv{0, 100'000};
                            if (err == SSL_ERROR_WANT_READ) {
                                FD_SET(c.sock_fd, &fds);
                                select(static_cast<int>(c.sock_fd) + 1, &fds, nullptr, nullptr, &tv);
                            } else {
                                FD_SET(c.sock_fd, &fds);
                                select(static_cast<int>(c.sock_fd) + 1, nullptr, &fds, nullptr, &tv);
                            }
                            continue;
                        }
                        break; // unrecoverable
                    }
                    fcntl(c.sock_fd, F_SETFL, flags); // restore blocking
                }
#endif
                close_conn(c);
            }
        }
    }

    // Cancel a transport worker that has exceeded the longest legitimate
    // transfer deadline. Never clear exec_busy here: the worker owns SSL until
    // it exits and clears that flag itself.
    //
    // The deadline is measured from the LAST PROGRESS tick (exec_last_progress_at),
    // not from exec_started_at. This lets a healthy, actively-streaming transfer
    // (file send / vfolder sync / edit upload) run as long as it needs, while a
    // STALLED transfer — one that has made no progress for kExecWatchdogSecs —
    // is still force-released (the original BUG-1 guarantee: a CLI timeout that
    // outlives its worker must not leave exec_busy stuck forever).
    void check_stale_exec() {
        static constexpr auto kExecWatchdogSecs =
            std::chrono::seconds(90);
        auto now = std::chrono::steady_clock::now();
        for (auto& c : conns_) {
            if (!c.exec_busy || !c.exec_busy->load()) continue;
            // If the worker refreshes last-progress (transfers do), use that;
            // otherwise fall back to exec_started_at (non-streaming execs).
            std::chrono::steady_clock::time_point last_activity{};
            if (c.exec_last_progress_at) {
                auto rep = c.exec_last_progress_at->load();
                if (rep != 0)
                    last_activity =
                        std::chrono::steady_clock::time_point(
                            std::chrono::steady_clock::duration(rep));
            }
            if (last_activity == std::chrono::steady_clock::time_point{})
                last_activity = c.exec_started_at;
            if (last_activity == std::chrono::steady_clock::time_point{}) continue;
            if (now - last_activity > kExecWatchdogSecs) {
                log_event("exec_watchdog_timeout", c.peer_name);
                if (c.exec_cancelled) c.exec_cancelled->store(true);
                // Shut down the socket so the blocking worker thread gets an
                // error on its next read/write and exits, which releases
                // exec_busy. Do NOT force-release exec_busy here — the worker
                // owns SSL and must release it cleanly.
                c.close_requested = true;
#ifdef _WIN32
                if (c.sock_fd != INVALID_SOCKET) ::shutdown(c.sock_fd, SD_BOTH);
#else
                if (c.sock_fd != INVALID_SOCKET) ::shutdown(c.sock_fd, SHUT_RDWR);
#endif
            }
        }
    }

    // ── Clean up dead connections ──────────────────────────────

    void clean_dead_conns() {
        // Finish closes that were deferred while a detached exec worker owned
        // the SSL object. This is the only point where a deferred Conn becomes
        // eligible for erasure.
        for (auto& c : conns_) {
            if (c.close_requested &&
                (!c.exec_busy || !c.exec_busy->load())) {
                (void)close_conn(c);
            }
        }
        // Erase entries already closed via close_conn(); do not touch live sockets.
        conns_.erase(
            std::remove_if(conns_.begin(), conns_.end(),
                [](const Conn& c) { return c.sock_fd == INVALID_SOCKET; }),
            conns_.end());
        // Newly accepted links are classified after their first post-Hello
        // message. Resolve mesh duplicates only after that classification so
        // direct session transports can coexist with the background mesh link.
        resolve_duplicates();
    }

public:
    // ── Constructor ───────────────────────────────────────────

    MeshController(const MeshConfig& cfg, std::string app_home = {},
                   std::string config_path = {})
        : config_(cfg)
    {
        configure_sigpipe_handling();
        // app_home is the BridgeSessions root directory (default ~/.bridgesessions).
        // --config-dir sets this so identity/keys/receive never touch $HOME/.bridgesessions.
        if (app_home.empty()) {
            app_home = expand_home("~/.bridgesessions");
        }
        while (app_home.size() > 1 && (app_home.back() == '/' || app_home.back() == '\\')) {
            app_home.pop_back();
        }
        home_dir_ = app_home;
        ipc_token_path_ = ipc_token_path(app_home);
        configure_logger_home(app_home);
        AppPaths paths = make_app_paths(app_home);
        apply_app_home_defaults(config_, app_home);

        receive_dir_ = paths.received;
        bootstrap_identity(paths.root);

        std::string pub_path = paths.pub;
        std::ifstream pf(pub_path);
        if (pf.is_open()) {
            std::getline(pf, our_pubkey_);
            pf.close();
        }

        std::string key_path = paths.key_pem;
        std::string cert_path = paths.cert_pem;

        NodeTlsConfig listen_cfg;
        listen_cfg.cert_file = cert_path;
        listen_cfg.key_file = key_path;
        listen_cfg.authorized_keys_file = resolve_under_app_home(config_.authorized_keys_path, app_home);
        tls_listen_ = create_node_tls(listen_cfg, TlsMode::Listen, &authorized_keys_);

        NodeTlsConfig connect_cfg;
        connect_cfg.cert_file = cert_path;
        connect_cfg.key_file = key_path;
        connect_cfg.tofu_cb = [](const std::string& /*fingerprint*/) {
            return true;
        };
        tls_connect_ = create_node_tls(connect_cfg, TlsMode::Connect, nullptr, &tofu_cb_);

        sessions_.set_persistence_path(resolve_under_app_home(config_.persistence_path, app_home));
        config_file_path_ = !config_path.empty()
            ? expand_home(config_path)
            : (!config_.source_path.empty() ? config_.source_path : paths.config);

        // D16: Initialize DHT if enabled
#ifndef BS_NO_DHT
        if (config_.dht_enabled) {
            std::string our_addr = config_.listen_addr + ":" + std::to_string(config_.listen_port);
            dht_.init(our_pubkey_, config_.node_name, our_addr);
            dht_inited_ = true;
        }
#endif

        // D17: Initialize UPnP if enabled
#ifndef BS_NO_NAT
        if (config_.upnp_enabled) {
            if (upnp_.init()) {
                upnp_.setup_port_mapping(config_.listen_port);
                external_addr_ = upnp_.external_ip();
                if (!external_addr_.empty()) {
                    log_event("upnp_ready", "external ip: " + external_addr_);
                }
            }
        }
#endif

        // v2.0.6: start long-operation worker pool. Handlers run on worker threads
        // and borrow mesh transports while exec_busy is set.
        worker_pool_.emplace(kLongOperationWorkers,
            [this](const LongOperationTask& task) { execute_long_operation_task(task); });
    }

#ifdef BS_TESTING
    [[nodiscard]] const std::string& config_file_path_for_test() const {
        return config_file_path_;
    }
    [[nodiscard]] std::string hello_version_for_test() const {
        return build_hello().version;
    }
    [[nodiscard]] bool direct_connect_rejects_missing_pin_for_test() {
        auto result = connect_and_hello("127.0.0.1:1", {});
        return result.fail == ConnectFailReason::TlsRejected &&
               result.fail_detail == "peer key not pinned";
    }
    // Actual listen port when config asked for 0 (ephemeral). 0 if not listening.
    [[nodiscard]] uint16_t actual_listen_port_for_test() const {
        return actual_listen_port_.load();
    }
    [[nodiscard]] size_t pending_handshake_count_for_test() const {
        return pending_handshakes_.size();
    }
    [[nodiscard]] size_t worker_queue_depth_for_test() const {
        return worker_pool_ ? worker_pool_->pending_count() : 0;
    }
    size_t add_connection_for_test(Conn&& conn) {
        conns_.push_back(std::move(conn));
        return conns_.size() - 1;
    }
    void prune_revoked_connections_for_test() { prune_revoked_connections(); }
    bool connection_open_for_test(size_t index) const {
        return index < conns_.size() && conns_[index].sock_fd != INVALID_SOCKET;
    }
    bool connection_closed_for_test(size_t index) const {
        return !connection_open_for_test(index);
    }
    void expire_exec_watchdog_for_test(size_t index) {
        auto& c = conns_.at(index);
        c.exec_busy->store(true);
        c.exec_cancelled->store(false);
        c.exec_started_at = std::chrono::steady_clock::now() -
                            std::chrono::seconds(7501);
        check_stale_exec();
    }
    // Run the watchdog against the current exec_started_at (no back-dating),
    // for testing the sub-90s "do not fire" path.
    void check_stale_exec_for_test() { check_stale_exec(); }
    bool exec_busy_for_test(size_t index) const {
        return index < conns_.size() && conns_[index].exec_busy->load();
    }
    bool exec_cancelled_for_test(size_t index) const {
        return index < conns_.size() && conns_[index].exec_cancelled->load();
    }
    [[nodiscard]] bool conn_busy_for_test(const std::string& peer_name) const {
        for (const auto& c : conns_) {
            if (peer_name_eq(c.peer_name, peer_name) && c.exec_busy)
                return c.exec_busy->load();
        }
        return false;
    }
    [[nodiscard]] bool conn_close_requested_for_test(const std::string& peer_name) const {
        for (const auto& c : conns_) {
            if (peer_name_eq(c.peer_name, peer_name) && c.close_requested)
                return c.close_requested;
        }
        return false;
    }
    [[nodiscard]] std::vector<std::string> conn_peer_names_for_test() const {
        std::vector<std::string> names;
        for (const auto& c : conns_) names.push_back(c.peer_name);
        return names;
    }
#endif

    // ── Destructor ────────────────────────────────────────────

    ~MeshController() {
        running_ = false;
#ifdef _WIN32
        shutdown_windows_pty_writer();
#endif
        // v2.0.6: join long-operation workers before tearing down SSL transports
        // so no worker touches a Conn after the destructor begins. Cancel active
        // workers and shut down their sockets so blocked selects/reads return.
        if (worker_pool_) {
            for (auto& c : conns_) {
                if (c.exec_cancelled) c.exec_cancelled->store(true);
                if (c.sock_fd != INVALID_SOCKET) {
#ifdef _WIN32
                    ::shutdown(c.sock_fd, SD_BOTH);
#else
                    ::shutdown(c.sock_fd, SHUT_RDWR);
#endif
                }
            }
            worker_pool_->shutdown();
        }
#ifndef BS_NO_NAT
        if (config_.upnp_enabled) {
            upnp_.cleanup();
        }
#endif
        for (auto& ph : pending_handshakes_) {
            if (ph.ssl) SSL_set_quiet_shutdown(ph.ssl.get(), 1);
            if (ph.sock_fd != INVALID_SOCKET) {
                CLOSESOCK(ph.sock_fd);
                ph.sock_fd = INVALID_SOCKET;
            }
            ph.ssl.reset();
        }
        pending_handshakes_.clear();
        for (auto& c : conns_) {
            (void)close_conn(c);
        }
        conns_.clear();
        if (listen_fd_ != INVALID_SOCKET) {
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
        }
        cli_ipc_shutdown();
    }

    bool cli_ipc_init() {
        cli_listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (cli_listen_fd_ == INVALID_SOCKET) return false;
        int opt = 1;
        setsockopt(cli_listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        if (bind(cli_listen_fd_, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
        if (listen(cli_listen_fd_, 8) == SOCKET_ERROR) {
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
#ifndef _WIN32
        if (cli_listen_fd_ >= FD_SETSIZE) {
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
#endif
        // Generate a fresh CSPRNG IPC token only after successfully binding the
        // socket. Write it owner-only under the app home.
        try {
            ipc_token_ = generate_ipc_token();
        } catch (...) {
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
        if (!write_private_text_file(ipc_token_path_, ipc_token_)) {
            ipc_token_.clear();
            CLOSESOCK(cli_listen_fd_); cli_listen_fd_ = INVALID_SOCKET; return false;
        }
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(cli_listen_fd_, FIONBIO, &nb);
#else
        int fl = fcntl(cli_listen_fd_, F_GETFL, 0);
        fcntl(cli_listen_fd_, F_SETFL, fl | O_NONBLOCK);
#endif
        log_event("mesh_cli_ipc_listen", std::to_string(mesh_cli_port()));
        return true;
    }

    void cli_ipc_shutdown() {
        if (cli_listen_fd_ != INVALID_SOCKET) {
            CLOSESOCK(cli_listen_fd_);
            cli_listen_fd_ = INVALID_SOCKET;
        }
        // Best-effort remove the daemon's IPC token so a stale token cannot be
        // replayed after the daemon exits. A new daemon always generates a fresh
        // token and overwrites any existing file on bind.
        if (!ipc_token_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(ipc_token_path_, ec);
        }
        ipc_token_.clear();
    }

    void cli_ipc_accept_one() {
        if (cli_listen_fd_ == INVALID_SOCKET) return;
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        SOCKET cfd = accept(cli_listen_fd_, (sockaddr*)&peer, &plen);
        if (cfd == INVALID_SOCKET) return;
        // Normalize accepted IPC sockets to blocking mode. macOS can inherit
        // O_NONBLOCK from the listener; without this, a fragmented request may
        // return EAGAIN between the token and command and be parsed as empty.
#ifdef _WIN32
        { u_long blocking = 0; ioctlsocket(cfd, FIONBIO, &blocking); }
#else
        {
            int flags = fcntl(cfd, F_GETFL, 0);
            if (flags >= 0) fcntl(cfd, F_SETFL, flags & ~O_NONBLOCK);
        }
#endif
        // CRITICAL: bound the recv. The accepted socket does not reliably inherit
        // the listen socket's non-blocking flag (esp. on macOS), so a client that
        // connects but sends no data would block recv() here and stall the ENTIRE
        // daemon event loop — no peer reads, missed pongs, false pong_timeouts, and
        // the whole mesh collapses. Use a short per-read timeout plus an
        // absolute two-second framing deadline so slow trickle clients cannot
        // multiply the timeout by the 1023-byte request cap.
        set_socket_timeouts(cfd, 250);
        char buf[1024] = {};
        int n = 0;
        const auto request_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(2);
        while (n < static_cast<int>(sizeof(buf) - 1) &&
               std::chrono::steady_clock::now() < request_deadline) {
            int got = recv(cfd, buf + n,
                           static_cast<int>(sizeof(buf) - 1) - n, 0);
            if (got <= 0) break;
            n += got;
            if (std::memchr(buf, '\n', static_cast<size_t>(n)) != nullptr) break;
        }
        std::string response = "ERROR bad request\n";
        bool response_sent = false;
        if (n > 0) {
            buf[n] = '\0';
            std::string line(buf);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            const bool authorized = !ipc_token_.empty() &&
                line.size() > ipc_token_.size() &&
                line.compare(0, ipc_token_.size(), ipc_token_) == 0 &&
                (line[ipc_token_.size()] == ' ' || line[ipc_token_.size()] == '\t');
            if (!authorized) {
                log_event("ipc_auth_rejected", "unauthorized local IPC request");
                response = "ERROR unauthorized\n";
                send(cfd, response.data(), static_cast<int>(response.size()), 0);
                CLOSESOCK(cfd);
                return;
            }
            line.erase(0, ipc_token_.size());
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
                line.erase(line.begin());
            if (line == "DAEMON_PROBE") {
                response = "OK bridgesessions\n";
            }
            else if (line.rfind("HEALTH ", 0) == 0) {
                std::string peer_name = line.substr(7);
                std::string want_addr = find_peer_addr(peer_name);
                bool found = false, ok = false;
                auto now = std::chrono::steady_clock::now();
                auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                                  ? config_.pong_timeout_secs : 30);
                for (auto& c : conns_) {
                    if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
                    bool name_match = peer_name_eq(c.peer_name, peer_name);
                    bool addr_match = !want_addr.empty() && c.peer_addr == want_addr;
                    if (!name_match && !addr_match) continue;
                    found = true;
                    // The daemon's own event loop pings every ping_interval_secs and
                    // updates last_pong. A live conn whose last_pong is within the
                    // pong-timeout window is healthy. Do NOT issue a synchronous ping
                    // here: the main loop owns reads on this socket and would consume
                    // the pong, producing false "no pong" results.
                    ok = (now - c.last_pong) <= fresh;
                    break;
                }
                response = found ? (peer_name + (ok ? " healthy\n" : " no pong\n"))
                                   : (peer_name + " not connected\n");
            }
            else if (line.rfind("RECONNECT ", 0) == 0) {
                std::string peer_name = line.substr(10);
                response = daemon_reconnect_peer(peer_name) + "\n";
            }
            else if (line == "STATS") {
                response = daemon_stats_summary() + "\n";
            }
            else if (line == "SESSIONS") {
                response = sessions_.summary() + "\n";
            }
            else if (line.rfind("FILE_SEND ", 0) == 0) {
                // FILE_SEND <peer> <local-path>
                auto rest = line.substr(10);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: FILE_SEND <peer> <path>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string path = rest.substr(sp + 1);
                    Conn* target = nullptr;
                    for (auto& c : conns_) {
                        if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                    }
                    if (!target) {
                        response = "ERROR no conn to " + peer_name + "\n";
                    } else if (target->exec_busy->exchange(true)) {
                        response = "ERROR peer busy with another transfer, retry\n";
                    } else {
                        target->exec_completed->store(false);
                        target->exec_cancelled = std::make_shared<std::atomic<bool>>(false);
                        target->exec_started_at = std::chrono::steady_clock::now();
                        target->exec_last_progress_at->store(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                        LongOperationTask task;
                        task.type = LongOperationTask::Type::FileSendWait;
                        task.peer_name = peer_name;
                        task.path1 = path;
                        task.ssl = target->ssl.get();
                        task.sock_fd = target->sock_fd;
                        task.exec_busy = target->exec_busy;
                        task.exec_completed = target->exec_completed;
                        task.cancelled = target->exec_cancelled;
                        task.last_progress_at = target->exec_last_progress_at;
                        worker_pool_->enqueue(std::move(task));
                        response = "OK queued send to " + peer_name + "\n";
                    }
                }
            }
            else if (line.rfind("FILE_SEND_WAIT_B64 ", 0) == 0) {
                auto rest = line.substr(19);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: FILE_SEND_WAIT_B64 <peer> <b64-local-path>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string path = b64dec(rest.substr(sp + 1));
                    // v2.0.6: offload the long transfer to a worker thread. The
                    // worker owns the IPC socket and streams PROGRESS + final response.
                    Conn* target = nullptr;
                    for (auto& c : conns_) {
                        if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                    }
                    if (!target) {
                        response = "ERROR no conn to " + peer_name + "\n";
                    } else if (target->exec_busy->exchange(true)) {
                        response = "ERROR peer busy with another transfer, retry\n";
                    } else {
                        target->exec_completed->store(false);
                        target->exec_cancelled = std::make_shared<std::atomic<bool>>(false);
                        target->exec_started_at = std::chrono::steady_clock::now();
                        target->exec_last_progress_at->store(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                        LongOperationTask task;
                        task.type = LongOperationTask::Type::FileSendWait;
                        task.peer_name = peer_name;
                        task.path1 = path;
                        task.ssl = target->ssl.get();
                        task.sock_fd = target->sock_fd;
                        task.exec_busy = target->exec_busy;
                        task.exec_completed = target->exec_completed;
                        task.cancelled = target->exec_cancelled;
                        task.last_progress_at = target->exec_last_progress_at;
                        task.ipc_fd = cfd;
                        worker_pool_->enqueue(std::move(task));
                        // IPC socket ownership transferred; do not send/close here.
                        response.clear();
                        response_sent = true;
                    }
                }
            }
            else if (line.rfind("FILE_RECV ", 0) == 0) {
                auto rest = line.substr(10);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: FILE_RECV <peer> <remote-path>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string path = rest.substr(sp + 1);
                    std::string result = daemon_file_recv(peer_name, path);
                    response = result + "\n";
                }
            }
            else if (line.rfind("FILE_RECV_B64 ", 0) == 0) {
                auto rest = line.substr(14);
                auto sp1 = rest.find(' ');
                auto sp2 = (sp1 == std::string::npos) ? std::string::npos : rest.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) {
                    response = "ERROR usage: FILE_RECV_B64 <peer> <b64-remote-path> <b64-local-dir>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp1);
                    std::string path = b64dec(rest.substr(sp1 + 1, sp2 - sp1 - 1));
                    std::string local_dir = b64dec(rest.substr(sp2 + 1));
                    response = daemon_file_recv(peer_name, path, local_dir) + "\n";
                }
            }
            else if (line.rfind("FILE_RECV_WAIT_B64 ", 0) == 0) {
                auto rest = line.substr(19);
                auto sp1 = rest.find(' ');
                auto sp2 = (sp1 == std::string::npos) ? std::string::npos : rest.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) {
                    response = "ERROR usage: FILE_RECV_WAIT_B64 <peer> <b64-remote-path> <b64-local-dest>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp1);
                    std::string path = b64dec(rest.substr(sp1 + 1, sp2 - sp1 - 1));
                    std::string local_dest = b64dec(rest.substr(sp2 + 1));
                    // v2.0.6: offload to worker thread; worker owns IPC socket.
                    Conn* target = nullptr;
                    for (auto& c : conns_) {
                        if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                    }
                    if (!target) {
                        response = "ERROR no conn to " + peer_name + "\n";
                    } else if (target->exec_busy->exchange(true)) {
                        response = "ERROR peer busy with another transfer, retry\n";
                    } else {
                        target->exec_completed->store(false);
                        target->exec_cancelled = std::make_shared<std::atomic<bool>>(false);
                        target->exec_started_at = std::chrono::steady_clock::now();
                        target->exec_last_progress_at->store(
                            std::chrono::steady_clock::now().time_since_epoch().count());
                        LongOperationTask task;
                        task.type = LongOperationTask::Type::FileRecvWait;
                        task.peer_name = peer_name;
                        task.path1 = path;
                        task.path2 = local_dest;
                        task.ssl = target->ssl.get();
                        task.sock_fd = target->sock_fd;
                        task.exec_busy = target->exec_busy;
                        task.exec_completed = target->exec_completed;
                        task.cancelled = target->exec_cancelled;
                        task.last_progress_at = target->exec_last_progress_at;
                        task.ipc_fd = cfd;
                        worker_pool_->enqueue(std::move(task));
                        response.clear();
                        response_sent = true;
                    }
                }
            }
            else if (line.rfind("SHELL ", 0) == 0) {
                // Interactive and one-shot shell commands use a dedicated direct
                // TLS connection. A detached worker must never borrow a mesh
                // connection's SSL object from the event loop.
                response = shell_ipc_relay_policy_response();
            }
            else if (line.rfind("CANCEL ", 0) == 0) {
                std::string peer_name = line.substr(7);
                Conn* target = nullptr;
                // Allow cancelling a busy transport; the worker exclusively owns it
                // while exec_busy is set, so require_idle must be false.
                for (auto& c : conns_) {
                    if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                }
                if (!target) {
                    response = "ERROR no conn to " + peer_name + "\n";
                } else if (!target->exec_busy || !target->exec_busy->load()) {
                    response = "OK no active operation on " + peer_name + "\n";
                } else {
                    if (target->exec_cancelled) target->exec_cancelled->store(true);
                    if (target->sock_fd != INVALID_SOCKET) {
#ifdef _WIN32
                        ::shutdown(target->sock_fd, SD_BOTH);
#else
                        ::shutdown(target->sock_fd, SHUT_RDWR);
#endif
                    }
                    response = "OK cancelling operation on " + peer_name + "\n";
                }
            }
            else if (line.rfind("EDIT_DL ", 0) == 0) {
                std::string rest = line.substr(8);
                while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
                auto sp = rest.find(' ');
                if (sp == std::string::npos) { response = "ERROR usage: EDIT_DL <peer> <path>\n"; }
                else {
                    std::string peer = rest.substr(0, sp);
                    std::string path = rest.substr(sp + 1);
                    response = daemon_edit_dl(peer, path) + "\n";
                }
            }
            else if (line.rfind("VFOLDER_SYNC ", 0) == 0) {
                std::string vfolder_name = line.substr(13);
                while (!vfolder_name.empty() && (vfolder_name.back() == '\r' || vfolder_name.back() == '\n'))
                    vfolder_name.pop_back();
                response = daemon_vfolder_sync(vfolder_name) + "\n";
            }
            else if (line.rfind("VFOLDER_LIST", 0) == 0) {
                std::string result = "[";
                for (auto& v : config_.vfolders) {
                    if (result.size() > 1) result += ",";
                    result += "{\"name\":\"" + v.name + "\",\"local\":\"" + v.local_path + "\",\"peer\":\"" + v.remote_peer + "\",\"remote\":\"" + v.remote_path + "\",\"interval\":" + std::to_string(v.sync_interval_secs) + ",\"direction\":\"" + v.direction + "\"}";
                }
                result += "]";
                response = result + "\n";
            }
            else if (line.rfind("EDIT_UP ", 0) == 0) {
                std::string rest = line.substr(8);
                while (!rest.empty() && (rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
                auto sp1 = rest.find(' ');
                if (sp1 == std::string::npos) { response = "ERROR usage: EDIT_UP <peer> <remote_path> <local_path>\n"; }
                else {
                    auto sp2 = rest.find(' ', sp1 + 1);
                    if (sp2 == std::string::npos) { response = "ERROR usage: EDIT_UP <peer> <remote_path> <local_path>\n"; }
                    else {
                        std::string peer = rest.substr(0, sp1);
                        std::string remote = rest.substr(sp1 + 1, sp2 - sp1 - 1);
                        std::string local = rest.substr(sp2 + 1);
                        response = daemon_edit_up(peer, remote, local) + "\n";
                    }
                }
            }
            if (response == "ERROR bad request\n") {
                auto separator = line.find(' ');
                log_event("ipc_bad_request",
                          "verb=" + line.substr(0, separator) +
                          " bytes=" + std::to_string(line.size()));
            }
        }
        if (!response_sent) {
            send(cfd, response.data(), (int)response.size(), 0);
            CLOSESOCK(cfd);
        }
        // else: worker owns cfd and will close it after streaming the response.
    }

    std::string daemon_health_via_ipc(const std::string& peer_name, int wait_ms) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return "";
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return "";
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        // Bound connect + recv so a stalled/dead daemon can never hang the CLI.
        set_socket_timeouts(sfd, wait_ms > 0 ? wait_ms : 8000);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd); return "";
        }
        std::string req = token + " HEALTH " + peer_name + "\n";
        send(sfd, req.data(), (int)req.size(), 0);
        char buf[256] = {};
        int total = 0;
        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        while (std::chrono::steady_clock::now() < dl && total < (int)sizeof(buf) - 1) {
            int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n > 0) {
                total += n; buf[total] = '\0';
                if (strchr(buf, '\n')) break;
            } else if (n == 0) {
                break;  // peer closed
            } else {
                // recv timed out (SO_RCVTIMEO) or transient error; stop, don't spin.
                break;
            }
        }
        CLOSESOCK(sfd);
        if (total <= 0) return "";
        std::string line(buf);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        return line;
    }

    // CLI-side: send a file via daemon IPC (reads local file, sends FILE_SEND command,
    // blocks for response with longer wait_ms to cover transfer time).
    std::string daemon_send_via_ipc(const std::string& peer_name, const std::string& path,
                                    int wait_ms, bool wait_for_completion = false) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return "";
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return "";
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        int to = wait_for_completion ? std::max(wait_ms, 7200000) : (wait_ms > 0 ? wait_ms : 120000);
        set_socket_timeouts(sfd, to);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd); return "";
        }
        std::string cmd = token + " " + (wait_for_completion
            ? ("FILE_SEND_WAIT_B64 " + peer_name + " " + b64enc(path) + "\n")
            : ("FILE_SEND " + peer_name + " " + path + "\n"));
        send(sfd, cmd.data(), (int)cmd.size(), 0);
        std::string pending;
        char buf[8192];
        while (true) {
            int n = recv(sfd, buf, (int)sizeof(buf) - 1, 0);
            if (n <= 0) break;
            auto terminal = consume_transfer_ipc_chunk(
                pending, std::string_view(buf, static_cast<size_t>(n)),
                [](const std::string& line) { std::cerr << line << "\n"; });
            if (terminal) {
                CLOSESOCK(sfd);
                return *terminal;
            }
        }
        CLOSESOCK(sfd);
        while (!pending.empty() && (pending.back() == '\r' || pending.back() == '\n'))
            pending.pop_back();
        return pending;
    }

    // CLI-side: shell command relay through daemon IPC.
    // Returns: exit_code on success (>=0), -1 = daemon-unreachable, -2 = timeout.
    // Writes stdout to 'output' param.
    int daemon_shell_via_ipc(const std::string& peer_name, const std::string& session_name,
                             const std::string& cmd, std::string* output,
                             int wait_ms = 60000) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return -1;
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return -1;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        set_socket_timeouts(sfd, wait_ms > 0 ? wait_ms : 60000);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd); return -1;
        }
        std::string req = token + " SHELL " + peer_name + " "
                        + b64enc(session_name) + " "
                        + b64enc(cmd) + "\n";
        send(sfd, req.data(), (int)req.size(), 0);
        char buf[65536] = {}; int total = 0;
        auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
        while (std::chrono::steady_clock::now() < dl && total < (int)sizeof(buf) - 1) {
            int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n > 0) { total += n; buf[total] = '\0'; if (strchr(buf, '\n')) break; }
            else break;
        }
        CLOSESOCK(sfd);
        if (total <= 0) return (std::chrono::steady_clock::now() > dl) ? -2 : -1;
        std::string line(buf);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        // v1.6 daemons without SHELL handler return "ERROR bad request" (plaintext),
        // not base64. Route through daemon_shell_via_ipc caller's fallback.
        if (line.rfind("ERROR ", 0) == 0) {
            *output = line.substr(6);
            return -1;
        }
        std::string decoded = b64dec(line);
        auto colon = decoded.find(':');
        if (colon == std::string::npos) { *output = decoded; return 0; }
        int exit_code = 0;
        try { exit_code = std::stoi(decoded.substr(0, colon)); }
        catch (...) { /* non-numeric prefix (e.g. C:\... paths) → exit 0 */ }
        *output = decoded.substr(colon + 1);
        return exit_code;
    }

    // CLI-side: request a file from a peer via daemon IPC.
    // CLI-side: edit download via daemon IPC
    std::string daemon_edit_dl_via_ipc(const std::string& peer_name, const std::string& path, int wait_ms) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return "";
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return "";
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        set_socket_timeouts(sfd, wait_ms > 0 ? wait_ms : 120000);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) { CLOSESOCK(sfd); return ""; }
        std::string cmd = token + " EDIT_DL " + peer_name + " " + path + "\n";
        send(sfd, cmd.data(), (int)cmd.size(), 0);
        char buf[4096] = {}; int total = 0;
        while (total < (int)sizeof(buf) - 1) {
            int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n > 0) { total += n; buf[total] = '\0'; if (strchr(buf, '\n')) break; }
            else break;
        }
        CLOSESOCK(sfd);
        std::string line(buf);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        return line;
    }

    // CLI-side: edit upload via daemon IPC
    std::string daemon_edit_up_via_ipc(const std::string& peer_name, const std::string& remote_path, const std::string& local_path, int wait_ms) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return "ERROR no daemon";
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return "ERROR socket";
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        set_socket_timeouts(sfd, wait_ms > 0 ? wait_ms : 120000);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) { CLOSESOCK(sfd); return "ERROR no daemon"; }
        std::string cmd = token + " EDIT_UP " + peer_name + " " + remote_path + " " + local_path + "\n";
        send(sfd, cmd.data(), (int)cmd.size(), 0);
        char buf[4096] = {}; int total = 0;
        while (total < (int)sizeof(buf) - 1) {
            int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n > 0) { total += n; buf[total] = '\0'; if (strchr(buf, '\n')) break; }
            else break;
        }
        CLOSESOCK(sfd);
        std::string line(buf);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        return line;
    }

    std::string daemon_recv_via_ipc(const std::string& peer_name, const std::string& path,
                                    const std::string& local_dest, int wait_ms,
                                    bool wait_for_completion = false) {
        std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return "";
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return "";
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        // Large transfers stream PROGRESS lines then final OK/ERROR; allow hours.
        int to = wait_for_completion ? std::max(wait_ms, 7200000) : (wait_ms > 0 ? wait_ms : 120000);
        set_socket_timeouts(sfd, to);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd); return "";
        }
        std::string cmd = token + " " + (wait_for_completion
            ? ("FILE_RECV_WAIT_B64 " + peer_name + " " + b64enc(path) + " " + b64enc(local_dest) + "\n")
            : ("FILE_RECV_B64 " + peer_name + " " + b64enc(path) + " " + b64enc(local_dest) + "\n"));
        send(sfd, cmd.data(), (int)cmd.size(), 0);
        std::string acc;
        char buf[8192];
        while (true) {
            int n = recv(sfd, buf, (int)sizeof(buf) - 1, 0);
            if (n <= 0) break;
            buf[n] = '\0';
            acc.append(buf, n);
            // Stream PROGRESS lines to stderr for AI/operators as they arrive.
            size_t pos = 0;
            while (true) {
                auto nl = acc.find('\n', pos);
                if (nl == std::string::npos) break;
                std::string line = acc.substr(pos, nl - pos);
                pos = nl + 1;
                if (line.rfind("PROGRESS ", 0) == 0) {
                    std::cerr << line << "\n";
                }
            }
            // Keep only incomplete trailing line in buffer for PROGRESS scan; full acc kept for final.
            // Done when we have a non-PROGRESS final line ending with newline after last PROGRESS.
            // Final response is last complete line that starts with OK or ERROR.
            size_t last_nl = acc.rfind('\n');
            if (last_nl != std::string::npos) {
                // Find last full line
                size_t start = acc.rfind('\n', last_nl > 0 ? last_nl - 1 : 0);
                size_t line_start = (start == std::string::npos) ? 0 : start + 1;
                std::string last = acc.substr(line_start, last_nl - line_start);
                if (last.rfind("OK ", 0) == 0 || last.rfind("ERROR", 0) == 0) {
                    CLOSESOCK(sfd);
                    return last;
                }
            }
        }
        CLOSESOCK(sfd);
        while (!acc.empty() && (acc.back() == '\r' || acc.back() == '\n')) acc.pop_back();
        auto last_nl = acc.rfind('\n');
        if (last_nl != std::string::npos) return acc.substr(last_nl + 1);
        return acc;
    }

    // Returns true if another bridgesessions daemon is already running locally
    // and proves possession of the owner-only IPC token. Used as a single-instance guard so a
    // double-click / second `bsmesh` launch cannot squat ports and split the mesh.
    bool another_daemon_running() {
        const std::string token = load_ipc_token(home_dir_);
        if (token.empty()) return false;
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) return false;
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port = htons(mesh_cli_port());
        set_socket_timeouts(sfd, 1500);
        if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
            CLOSESOCK(sfd);
            return false;
        }
        const std::string probe = token + " DAEMON_PROBE\n";
        if (send(sfd, probe.data(), static_cast<int>(probe.size()), 0) <= 0) {
            CLOSESOCK(sfd);
            return false;
        }
        char reply[64] = {};
        int n = recv(sfd, reply, static_cast<int>(sizeof(reply) - 1), 0);
        CLOSESOCK(sfd);
        return n > 0 && std::string_view(reply, static_cast<size_t>(n)).find(
                            "OK bridgesessions") != std::string_view::npos;
    }

    // ── Main event loop ───────────────────────────────────────

    void run() {
        running_ = true;
        // The event loop also accepts inbound CLI sessions. Keep seed dials short so
        // several offline/discovered peers cannot starve accept() for 3s each.
        outbound_connect_timeout_ms_ = 1000;

        // Single-instance guard: if a daemon already owns the CLI IPC port,
        // refuse to start a second one. SO_REUSEADDR otherwise lets a second
        // process silently co-bind the mesh port and split-brain the mesh.
        if (another_daemon_running()) {
            log_event("mesh_already_running",
                      "another daemon is listening on CLI IPC port "
                      + std::to_string(mesh_cli_port()) + "; refusing to start");
            std::cerr << "bridgesessions: another daemon already running (IPC port "
                      << mesh_cli_port() << "). Refusing to start a second instance.\n";
            running_ = false;
            return;
        }
#ifndef _WIN32
        struct sigaction sa{};
        sa.sa_handler = sighup_reload_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGHUP, &sa, nullptr);  // R4.2
#endif

        // Create listen socket
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == INVALID_SOCKET) {
            log_event("mesh_listen_socket_failed");
            return;
        }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in listen_sa{};
        listen_sa.sin_family = AF_INET;
        listen_sa.sin_addr.s_addr = inet_addr(config_.listen_addr.c_str());
        if (listen_sa.sin_addr.s_addr == INADDR_NONE) {
            listen_sa.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        listen_sa.sin_port = htons(config_.listen_port);

        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&listen_sa), sizeof(listen_sa)) == SOCKET_ERROR) {
            int err =
#ifdef _WIN32
                WSAGetLastError();
#else
                errno;
#endif
            log_event("mesh_listen_bind_failed", "errno=" + std::to_string(err));  // R3.5
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return;
        }

        if (listen(listen_fd_, SOMAXCONN) == SOCKET_ERROR) {
            log_event("mesh_listen_failed");
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return;
        }

        sockaddr_in actual_addr{};
        socklen_t actual_len = sizeof(actual_addr);
        if (getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual_addr), &actual_len) == 0)
            actual_listen_port_.store(ntohs(actual_addr.sin_port));

        // Make listen socket non-blocking so accept() never blocks the loop.
#ifdef _WIN32
        { u_long nb = 1; ioctlsocket(listen_fd_, FIONBIO, &nb); }
#else
        { int fl = fcntl(listen_fd_, F_GETFL, 0); if (fl >= 0) fcntl(listen_fd_, F_SETFL, fl | O_NONBLOCK); }
        if (listen_fd_ >= FD_SETSIZE) {
            log_event("mesh_listen_fd_too_high", std::to_string(listen_fd_));
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return;
        }
#endif

        log_event("mesh_listening", config_.listen_addr + ":" + std::to_string(config_.listen_port));

        if (!cli_ipc_init()) {
            log_event("mesh_cli_ipc_failed", "daemon startup aborted");
            running_ = false;
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
            return;
        }

        if (config_.mdns_enabled) mdns_init();
        last_ping_time_ = std::chrono::steady_clock::now();
        last_gossip_time_ = std::chrono::steady_clock::now();
        last_mdns_time_ = std::chrono::steady_clock::now();

        while (running_) {
            // 1. Build fd_set for select()
            fd_set read_fds, write_fds;
            FD_ZERO(&read_fds);
            FD_ZERO(&write_fds);
            FD_SET(listen_fd_, &read_fds);

            SOCKET max_fd = listen_fd_;
            // Make the CLI IPC port event-driven: include its listen fd in select()
            // so `health` queries are serviced the moment they arrive, instead of
            // once per (possibly slow) loop iteration. Without this, a loop tick
            // spent in a peer handshake/read leaves IPC unaccepted and the CLI times
            // out — observed as intermittent "health timed out" on Windows/macOS.
            if (cli_listen_fd_ != INVALID_SOCKET) {
                FD_SET(cli_listen_fd_, &read_fds);
                if (cli_listen_fd_ > max_fd) max_fd = cli_listen_fd_;
            }
            for (auto& c : conns_) {
                // Skip conns whose socket/SSL is currently owned by a
                // background daemon_shell_exec thread (v1.7 async exec fix,
                // Known Issue #2) — reading here would race the thread.
                if (c.exec_busy && c.exec_busy->load()) continue;
#ifdef _WIN32
                if (c.attached_session &&
                    windows_pty_pending_bytes_.load() >= kWindowsPtyInputHighWater)
                    continue;
#else
                if (c.attached_session && c.attached_session->input_backpressured)
                    continue;
#endif
                if (c.sock_fd != INVALID_SOCKET) {
#ifndef _WIN32
                    if (c.sock_fd >= FD_SETSIZE) {
                        log_event("mesh_peer_fd_too_high", c.peer_name);
                        close_conn(c);
                        continue;
                    }
#endif
                    FD_SET(c.sock_fd, &read_fds);
                    if (c.sock_fd > max_fd) max_fd = c.sock_fd;
                }
            }
#ifndef _WIN32
            // PTY output is part of the event loop. Without these descriptors,
            // select() can sleep for three seconds while a TUI is drawing,
            // then forward only a fragment and visibly tear the screen.
            for (const auto& info : sessions_.list()) {
                Session* session = sessions_.get(info.name);
                if (!session || !session->is_pollable() || session->master_fd < 0 ||
                    session->master_fd >= FD_SETSIZE)
                    continue;
                FD_SET(session->master_fd, &read_fds);
                if (session->master_fd > max_fd) max_fd = session->master_fd;
                // If the child is not consuming input fast enough, watch for
                // writability so we can drain the pending queue.
                if (!session->pending_input.empty()) {
                    FD_SET(session->master_fd, &write_fds);
                }
            }
#endif
            if (mdns_fd_ != INVALID_SOCKET) {
#ifndef _WIN32
                if (mdns_fd_ >= FD_SETSIZE) {
                    log_event("mdns_fd_too_high", std::to_string(mdns_fd_));
                    CLOSESOCK(mdns_fd_);
                    mdns_fd_ = INVALID_SOCKET;
                } else
#endif
                {
                FD_SET(mdns_fd_, &read_fds);
                if (mdns_fd_ > max_fd) max_fd = mdns_fd_;
                }
            }

            // v2.0.6: include pending TLS+Hello handshakes. A handshake may need
            // both read and write readiness depending on OpenSSL state, so include
            // pending sockets in both sets; advance_handshakes() is non-blocking.
            for (auto& ph : pending_handshakes_) {
                if (ph.sock_fd == INVALID_SOCKET) continue;
#ifndef _WIN32
                if (ph.sock_fd >= FD_SETSIZE) {
                    log_event("handshake_fd_too_high", std::to_string(ph.sock_fd));
                    if (ph.ssl) SSL_set_quiet_shutdown(ph.ssl.get(), 1);
                    CLOSESOCK(ph.sock_fd);
                    ph.sock_fd = INVALID_SOCKET;
                    ph.state = PendingHandshake::State::Failed;
                    continue;
                }
#endif
                if (ph.want_read) FD_SET(ph.sock_fd, &read_fds);
                if (ph.want_write) FD_SET(ph.sock_fd, &write_fds);
                if (ph.sock_fd > max_fd) max_fd = ph.sock_fd;
            }

            // 2. select(): POSIX PTYs wake this set immediately. Windows pipe
            // handles cannot participate in select(), so poll ConPTY at 20 Hz.
#ifdef _WIN32
            timeval tv{0, 50'000};
#else
            timeval tv{0, 100'000};
#endif
            int nfds = select(static_cast<int>(max_fd) + 1, &read_fds, &write_fds, nullptr, &tv);

            if (nfds < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
#endif
                break;
            }

            auto now = std::chrono::steady_clock::now();
            maybe_reload_config_seeds();
            maybe_prune_revoked_connections();
            if (config_.idle_timeout_hours > 0 &&
                now - last_session_prune_time_ >= std::chrono::minutes(1)) {
                sessions_.prune_idle(std::chrono::hours(config_.idle_timeout_hours));
                last_session_prune_time_ = now;
            }
#ifndef _WIN32
            if (g_config_reload_requested.exchange(false))
                reload_seeds_from_disk();
#endif

            // Service CLI IPC the moment a request arrives (event-driven).
            if (cli_listen_fd_ != INVALID_SOCKET && FD_ISSET(cli_listen_fd_, &read_fds)) {
                cli_ipc_accept_one();
                if (nfds > 0) --nfds;
            }

            // 3. Accept new connections
            if (nfds > 0 && FD_ISSET(listen_fd_, &read_fds)) {
                accept_inbound();
                --nfds;
            }
            // 3.5. mDNS read
            if (nfds > 0 && mdns_fd_ != INVALID_SOCKET && FD_ISSET(mdns_fd_, &read_fds)) {
                mdns_check();
                --nfds;
            }

            // 4. Read from established connections
            for (int i = 0; i < static_cast<int>(conns_.size()) && nfds > 0; ++i) {
                auto& conn = conns_[static_cast<size_t>(i)];
                if (conn.exec_busy && conn.exec_busy->load()) continue;
                if (conn.sock_fd != INVALID_SOCKET &&
                    FD_ISSET(conn.sock_fd, &read_fds)) {
                    check_conn_read(i);
                    --nfds;
                }
            }

            // 4.5. Advance non-blocking TLS + Hello handshakes.
            // Run unconditionally: progress may be driven by SSL buffered data too.
            advance_handshakes();

            // 5. Connect to seeds / discovered peers
            try_connect_to_seeds();

            // 6. Ping broadcast
            auto ping_interval = std::chrono::seconds(config_.ping_interval_secs);
            if (now - last_ping_time_ >= ping_interval) {
                broadcast_ping();
                last_ping_time_ = now;
            }

            // 7. Pong timeout check
            check_pong_timeouts();

            // 7b. Stale exec watchdog (BUG-1)
            check_stale_exec();

            // 8. Gossip broadcast
            auto gossip_interval = std::chrono::seconds(config_.gossip_interval_secs);
            if (now - last_gossip_time_ >= gossip_interval) {
                broadcast_gossip();
                last_gossip_time_ = now;
            }

            // 8.5. mDNS announce (every 30s)
            if (mdns_fd_ != INVALID_SOCKET && now - last_mdns_time_ >= std::chrono::seconds(30)) {
                mdns_announce();
                last_mdns_time_ = now;
            }
            // 9. Clean dead connections
            clean_dead_conns();

            // 9.5. Poll PTY output for all attached sessions
            pty_output_poller();

#ifndef _WIN32
            for (const auto& info : sessions_.list()) {
                Session* session = sessions_.get(info.name);
                if (!session || session->pending_input.empty() || session->master_fd < 0)
                    continue;
                if (FD_ISSET(session->master_fd, &write_fds))
                    (void)drain_pending_pty_input(*session);
            }
#endif

            // 10. Reap dead sessions
            sessions_.reap_dead();
        }
    }

    // ── mDNS LAN discovery ─────────────────────────────────────

    void mdns_init() {
        mdns_fd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (mdns_fd_ == INVALID_SOCKET) return;
        int yes = 1;
        setsockopt(mdns_fd_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        bind_addr.sin_port = htons(kMdnsPort);
        if (bind(mdns_fd_, (sockaddr*)&bind_addr, sizeof(bind_addr)) == SOCKET_ERROR) {
            CLOSESOCK(mdns_fd_); mdns_fd_ = INVALID_SOCKET; return;
        }
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = inet_addr(kMdnsGroup);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(mdns_fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char*)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
            CLOSESOCK(mdns_fd_); mdns_fd_ = INVALID_SOCKET; return;
        }
#ifdef _WIN32
        u_long mode = 1; ioctlsocket(mdns_fd_, FIONBIO, &mode);
#else
        int flags = fcntl(mdns_fd_, F_GETFL, 0); fcntl(mdns_fd_, F_SETFL, flags | O_NONBLOCK);
#endif
        log_event("mdns_init", std::string("listening on ") + kMdnsGroup + ":" + std::to_string(kMdnsPort));
    }

    void mdns_announce() {
        if (mdns_fd_ == INVALID_SOCKET) return;
        nlohmann::json j;
        j["name"] = config_.node_name; j["port"] = config_.listen_port; j["pubkey"] = our_pubkey_;
#ifndef BS_NO_NAT
        if (!external_addr_.empty()) {
            j["wan"] = external_addr_;
        }
#endif
        std::string payload = j.dump();
        sockaddr_in dest{};
        dest.sin_family = AF_INET; dest.sin_addr.s_addr = inet_addr(kMdnsGroup); dest.sin_port = htons(kMdnsPort);
        sendto(mdns_fd_, payload.data(), (int)payload.size(), 0, (sockaddr*)&dest, sizeof(dest));
    }

    void process_mdns_announcement(const std::string& name,
                                   const std::string& addr,
                                   const std::string& pubkey) {
        if (pubkey.empty() || pubkey == our_pubkey_) return;
        if (!is_trusted_pubkey(pubkey)) return;
        for (auto& s : config_.seeds) {
            if (!s.pubkey_hex.empty() && s.pubkey_hex == pubkey) {
                if (!addr.empty()) s.addr = addr;
                s.last_seen = now_unix_seconds();
                log_event("mdns_address_update", s.name + " " + addr);
                return;
            }
            if (peer_name_eq(s.name, name) && s.pubkey_hex != pubkey) return;
        }
        for (auto& d : config_.discovered) {
            if (d.pubkey_hex == pubkey) {
                if (!addr.empty()) d.addr = addr;
                if (!name.empty()) d.name = name;
                d.last_seen = now_unix_seconds();
                log_event("mdns_address_update", d.name + " " + addr);
                return;
            }
            if (peer_name_eq(d.name, name) && d.pubkey_hex != pubkey) return;
        }
        PeerEntry pe{name, addr, pubkey, now_unix_seconds()};
        config_.discovered.push_back(std::move(pe));
        log_event("mdns_discovered", name + " " + addr);
    }

    void mdns_check() {
        if (mdns_fd_ == INVALID_SOCKET) return;
        char buf[2048]; sockaddr_in from{}; socklen_t from_len = sizeof(from);
        int n = recvfrom(mdns_fd_, buf, sizeof(buf)-1, 0, (sockaddr*)&from, &from_len);
        if (n <= 0) return; buf[n] = '\0';
        try {
            auto j = nlohmann::json::parse(buf);
            if (!j.contains("name") || !j.contains("port") || !j.contains("pubkey")) return;
            std::string name = j["name"], pubkey = j["pubkey"];
            if (pubkey == our_pubkey_) return;
            char ip_str[64]; inet_ntop(AF_INET, &from.sin_addr, ip_str, sizeof(ip_str));
            std::string addr = std::string(ip_str) + ":" + std::to_string(j["port"].get<int>());
            process_mdns_announcement(name, addr, pubkey);
        } catch (...) {}
    }

    void mdns_shutdown() {
        if (mdns_fd_ == INVALID_SOCKET) return;
        ip_mreq mreq{}; mreq.imr_multiaddr.s_addr = inet_addr(kMdnsGroup); mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        setsockopt(mdns_fd_, IPPROTO_IP, IP_DROP_MEMBERSHIP, (const char*)&mreq, sizeof(mreq));
        CLOSESOCK(mdns_fd_); mdns_fd_ = INVALID_SOCKET;
    }

    // ── Common: resolve peer → addr ─────────────────────────────
    static bool peer_name_eq(const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
        }
        return true;
    }

    std::string find_peer_addr(const std::string& peer_name) const {
        for (auto& s : config_.seeds) if (peer_name_eq(s.name, peer_name)) return s.addr;
        for (auto& d : config_.discovered) if (peer_name_eq(d.name, peer_name)) return d.addr;
        return "";
    }

    // Read frames until Pong or deadline (handles Gossip/Hello interleaved on mesh link).
    bool wait_for_pong(SSL* ssl, SOCKET sfd, std::chrono::steady_clock::time_point deadline) {
        while (std::chrono::steady_clock::now() < deadline) {
            auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (remain.count() <= 0) break;
            int ms = static_cast<int>(std::min<int64_t>(remain.count(), 2000));
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(sfd, &read_fds);
            timeval tv{};
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
#ifdef _WIN32
            int sel = select(0, &read_fds, nullptr, nullptr, &tv);
#else
            int sel = select(static_cast<int>(sfd) + 1, &read_fds, nullptr, nullptr, &tv);
#endif
            if (sel > 0 || SSL_pending(ssl) > 0) {
                try {
                    Message resp = read_frame(ssl);
                    if (std::holds_alternative<PongMsg>(resp)) return true;
                } catch (...) {
                    return false;
                }
            }
        }
        return false;
    }

    // ── Common: TCP + TLS + Hello ────────────────────────────────
    struct SslConn {
        SslPtr ssl;
        SOCKET sfd = INVALID_SOCKET;
        HelloMsg hello{};
        ConnectFailReason fail = ConnectFailReason::None;
        std::string fail_detail;
    };
    static std::string connect_fail_string(ConnectFailReason r) {
        switch (r) {
        case ConnectFailReason::Refused: return "refused";
        case ConnectFailReason::Timeout: return "timeout";
        case ConnectFailReason::TlsRejected: return "tls_rejected";
        case ConnectFailReason::HelloRejected: return "hello_rejected";
        default: return "unknown";
        }
    }
    SslConn connect_and_hello(const std::string& addr,
                              const std::string& expected_pubkey = {}) {
        SslConn out;
        if (expected_pubkey.empty()) {
            out.fail = ConnectFailReason::TlsRejected;
            out.fail_detail = "peer key not pinned";
            return out;
        }
        sockaddr_in sa = resolve_addr(addr);
        SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd == INVALID_SOCKET) {
            out.fail = ConnectFailReason::Refused;
            out.fail_detail = "socket() failed";
            return out;
        }
        set_socket_timeouts(sfd, outbound_connect_timeout_ms_);
        { int o = 1; setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&o, sizeof(o)); }  // R3.6
        const auto connect_result = connect_socket_with_timeout(
            sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa),
            outbound_connect_timeout_ms_);
        if (!connect_result.connected) {
            out.fail = connect_result.timed_out
                ? ConnectFailReason::Timeout
                : ConnectFailReason::Refused;
            out.fail_detail = "connect errno=" + std::to_string(connect_result.error);
            ssl_close(nullptr, sfd);
            return out;
        }
        auto ssl = SslPtr(SSL_new(tls_connect_.get()));
        if (!ssl) { ssl_close(nullptr, sfd); out.fail = ConnectFailReason::TlsRejected; return out; }
        SSL_set_fd(ssl.get(), (int)sfd);
        {
            int rc = ssl_connect_blocking(ssl.get(), sfd, outbound_connect_timeout_ms_);
            if (rc <= 0) {
                int ssl_err = SSL_get_error(ssl.get(), rc);
                char errbuf[256] = {};
                unsigned long e = ERR_get_error();
                if (e) ERR_error_string_n(e, errbuf, sizeof(errbuf));
                out.fail = classify_ssl_connect_fail(ssl_err);
                out.fail_detail = "ssl_err=" + std::to_string(ssl_err) +
                                  (errbuf[0] ? std::string(" ") + errbuf : "");
                append_ssl_connect_error_detail(out.fail_detail, ssl_err);
                log_event("tls_connect_and_hello_failed", out.fail_detail);
                ssl_close(ssl.get(), sfd);
                return out;
            }
        }
        const std::string certificate_pubkey = peer_public_key_hex(ssl.get());
        if (!expected_pubkey.empty() &&
            !peer_identity_matches(expected_pubkey, certificate_pubkey)) {
            out.fail = ConnectFailReason::TlsRejected;
            out.fail_detail = "peer certificate fingerprint mismatch";
            log_event("tls_peer_identity_mismatch", addr);
            ssl_close(ssl.get(), sfd);
            return out;
        }
        try {
            write_frame(ssl.get(), build_hello(), CONTROL_STREAM_ID);
            Message msg = read_frame(ssl.get());
            if (!std::holds_alternative<HelloMsg>(msg)) {
                ssl_close(ssl.get(), sfd);
                out.fail = ConnectFailReason::HelloRejected;
                out.fail_detail = "expected HelloMsg";
                return out;
            }
            out.hello = std::get<HelloMsg>(msg);
            if (!certificate_pubkey.empty() &&
                out.hello.pubkey_hex != certificate_pubkey) {
                ssl_close(ssl.get(), sfd);
                out.fail = ConnectFailReason::HelloRejected;
                out.fail_detail = "Hello pubkey does not match TLS certificate";
                return out;
            }
        } catch (const std::exception& e) {
            ssl_close(ssl.get(), sfd);
            out.fail = ConnectFailReason::HelloRejected;
            out.fail_detail = e.what();
            log_event("tls_connect_and_hello_failed", out.fail_detail);
            return out;
        } catch (...) {
            ssl_close(ssl.get(), sfd);
            out.fail = ConnectFailReason::HelloRejected;
            out.fail_detail = "hello exchange failed";
            log_event("tls_connect_and_hello_failed", out.fail_detail);
            return out;
        }
        out.ssl = std::move(ssl);
        out.sfd = sfd;
        return out;
    }

    // ── Shutdown ───────────────────────────────────────────────

    void shutdown() { mdns_shutdown(); running_ = false; }

    void print_connect_failure(const std::string& peer_name, const SslConn& sc) const {
        if (sc.fail != ConnectFailReason::None)
            std::cerr << "Failed to connect to " << peer_name << ": " << connect_fail_string(sc.fail)
                      << (sc.fail_detail.empty() ? "" : " (" + sc.fail_detail + ")") << "\n";
        else
            std::cerr << "Failed to connect to " << peer_name << "\n";
    }

    // ── CLI: shell_peer ────────────────────────────────────────
    // Returns: 0 on success (interactive), session exit_code on non-interactive,
    //          255 on connection/peer failure.
    int shell_peer(const std::string& peer_name, const std::string& session_name,
                   const std::string& cmd, uint16_t cols, uint16_t rows, const std::string& term,
                   bool signal_forward = true, const std::string& signal_on_detach = "") {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) { std::cerr << "Peer not found: " << peer_name << "\n"; return 255; }
        const std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        const bool non_interactive = !shell_command_uses_interactive_mode(cmd, stdin_is_terminal());

        auto connect_with_startup_retries = [&]() {
            SslConn result;
            for (int attempt = 0; attempt < 3; ++attempt) {
                result = connect_and_hello(addr, expected_pubkey);
                if (result.ssl && result.sfd != INVALID_SOCKET) break;
                const bool retryable = result.fail == ConnectFailReason::Timeout ||
                                       result.fail == ConnectFailReason::Refused;
                if (!retryable || attempt == 2) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250 * (attempt + 1)));
            }
            return result;
        };

        if (non_interactive) {
            auto sc = connect_with_startup_retries();
            if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
                print_connect_failure(peer_name, sc);
                return 255;
            }
            int32_t exit_code = 0;
            bool running = true;
            bool transport_error = false;
            bool saw_session_end = false;
            try {
                AttachMsg am;
                am.session_name = session_name;
                am.cols = cols;
                am.rows = rows;
                am.term = term;
                am.command = cmd;
                am.signal_on_detach = signal_on_detach;
                write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
                while (running) {
                    fd_set read_fds;
                    FD_ZERO(&read_fds);
                    FD_SET((int)sc.sfd, &read_fds);
                    timeval tv{5, 0};
#ifdef _WIN32
                    int ready = select(0, &read_fds, nullptr, nullptr, &tv);
#else
                    int ready = select((int)sc.sfd + 1, &read_fds, nullptr, nullptr, &tv);
#endif
                    if (ready < 0) {
#ifndef _WIN32
                        if (errno == EINTR) continue;
#endif
                        throw std::runtime_error("select failed while waiting for noninteractive session");
                    }
                    if ((ready > 0 && FD_ISSET((int)sc.sfd, &read_fds)) ||
                        SSL_pending(sc.ssl.get()) > 0) {
                        running = process_noninteractive_response(
                            sc.ssl.get(), exit_code, &transport_error, &saw_session_end);
                    }
                }
                // v2.0.1: after SessionDied, drain late OutputMsg frames briefly.
                // Server may still push conhost-flushed text after death notice.
                if (saw_session_end && !transport_error && sc.ssl && sc.sfd != INVALID_SOCKET) {
                    const auto drain_deadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    while (std::chrono::steady_clock::now() < drain_deadline) {
                        if (SSL_pending(sc.ssl.get()) <= 0) {
                            fd_set rfds; FD_ZERO(&rfds); FD_SET((int)sc.sfd, &rfds);
                            timeval tv{0, 50'000};
#ifdef _WIN32
                            if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;
#else
                            if (select((int)sc.sfd + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
#endif
                        }
                        try {
                            Message resp = read_frame(sc.ssl.get());
                            if (std::holds_alternative<OutputMsg>(resp)) {
                                std::cout << strip_ansi_escapes(std::get<OutputMsg>(resp).data)
                                          << std::flush;
                            } else if (std::holds_alternative<PingMsg>(resp)) {
                                write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                            }
                        } catch (...) {
                            break;
                        }
                    }
                }
                if (transport_error) {
                    CLOSESOCK(sc.sfd);
                    sc.sfd = INVALID_SOCKET;
                    return 255;
                }
                ssl_close(sc.ssl.get(), sc.sfd);
                sc.sfd = INVALID_SOCKET;
                return static_cast<int>(exit_code);
            } catch (...) {
                ssl_close(sc.ssl.get(), sc.sfd);
                sc.sfd = INVALID_SOCKET;
                return 255;
            }
        }

        std::optional<InteractiveTerminalGuard> terminal_guard;
        try {
            terminal_guard.emplace(signal_forward);
        } catch (...) {
            return 255;
        }
        auto restore_local_terminal = [&]() {
            terminal_guard->restore();
        };

        std::string pending_input;
        auto wait_for_local_stop = [&](int wait_ms) {
            std::array<char, 256> input{};
#ifdef _WIN32
            HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
            if (WaitForSingleObject(hIn, static_cast<DWORD>(wait_ms)) != WAIT_OBJECT_0)
                return false;
            DWORD nread = 0;
            if (!ReadFile(hIn, input.data(), static_cast<DWORD>(input.size()), &nread, nullptr))
                return false;
            if (nread == 0) return false;
            return queue_disconnected_input(
                pending_input, std::string_view(input.data(), nread));
#else
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(STDIN_FILENO, &read_fds);
            timeval tv{wait_ms / 1000, (wait_ms % 1000) * 1000};
            int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &tv);
            if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) return false;
            ssize_t n = ::read(STDIN_FILENO, input.data(), input.size());
            if (n <= 0) return true;
            return queue_disconnected_input(
                pending_input, std::string_view(input.data(), static_cast<size_t>(n)));
#endif
        };

        bool local_stop = false;
        int reconnect_delay_ms = 100;
        try {
        while (!local_stop) {
            addr = find_peer_addr(peer_name);
            if (addr.empty()) {
                local_stop = wait_for_local_stop(reconnect_delay_ms);
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, 5000);
                continue;
            }

            auto sc = connect_and_hello(addr, expected_pubkey);
            if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
                // Authentication/certificate rejection is permanent until the
                // operator changes trust configuration. Retrying it silently
                // would look like a hung terminal and weaken failure visibility.
                if (sc.fail == ConnectFailReason::TlsRejected) {
                    restore_local_terminal();
                    print_connect_failure(peer_name, sc);
                    return 255;
                }
                local_stop = wait_for_local_stop(reconnect_delay_ms);
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, 5000);
                continue;
            }

            reconnect_delay_ms = 100;
            bool transport_alive = true;
            auto [last_cols, last_rows] = get_winsize();
            if (last_cols == 0 || last_rows == 0) {
                last_cols = cols;
                last_rows = rows;
            }
            try {
                AttachMsg am;
                am.session_name = session_name;
                am.cols = last_cols;
                am.rows = last_rows;
                am.term = term;
                am.command = cmd;
                am.signal_on_detach = signal_on_detach;
                write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
                if (!pending_input.empty()) {
                    KeystrokeMsg queued;
                    queued.data = pending_input;
                    write_frame(sc.ssl.get(), queued, CONTROL_STREAM_ID);
                    pending_input.clear();
                }

                auto forward_local_input = [&](std::string_view input) {
                    // Forward everything to the remote PTY — Ctrl-C (\x03)
                    // is a normal keystroke the remote child should receive as
                    // SIGINT.  Only disconnect is handled at the transport level
                    // (connection loss / remote Detach / SessionDied).
                    KeystrokeMsg km;
                    km.data.assign(input.data(), input.size());
                    try {
                        write_frame(sc.ssl.get(), km, CONTROL_STREAM_ID);
                    } catch (...) {
                        (void)queue_disconnected_input(pending_input, input);
                        transport_alive = false;
                    }
                };

                std::array<char, 4096> stdin_buf{};
                while (!local_stop && transport_alive) {
#ifdef _WIN32
                    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
                    if (WaitForSingleObject(hIn, 0) == WAIT_OBJECT_0) {
                        DWORD nread = 0;
                        if (ReadFile(hIn, stdin_buf.data(), static_cast<DWORD>(stdin_buf.size()), &nread, nullptr) && nread > 0) {
                            forward_local_input(std::string_view(stdin_buf.data(), nread));
                        }
                    }
                    fd_set sock_fds;
                    FD_ZERO(&sock_fds);
                    FD_SET(sc.sfd, &sock_fds);
                    timeval sock_tv{0, 50000};
                    if (!local_stop && transport_alive &&
                        (select(0, &sock_fds, nullptr, nullptr, &sock_tv) > 0 ||
                         SSL_pending(sc.ssl.get()) > 0))
                        transport_alive = process_shell_response(sc.ssl.get());
#else
                    fd_set read_fds;
                    FD_ZERO(&read_fds);
                    FD_SET(STDIN_FILENO, &read_fds);
                    FD_SET((int)sc.sfd, &read_fds);
                    int maxfd = std::max(STDIN_FILENO, (int)sc.sfd);
                    timeval tv{0, 50000};
                    int ready = select(maxfd + 1, &read_fds, nullptr, nullptr, &tv);
                    if (ready < 0 && errno != EINTR) {
                        transport_alive = false;
                    } else if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
                        ssize_t n = ::read(STDIN_FILENO, stdin_buf.data(), stdin_buf.size());
                        if (n <= 0) {
                            local_stop = true;
                        } else {
                            forward_local_input(std::string_view(
                                stdin_buf.data(), static_cast<size_t>(n)));
                        }
                    }
                    if (!local_stop && transport_alive &&
                        ((ready > 0 && FD_ISSET((int)sc.sfd, &read_fds)) || SSL_pending(sc.ssl.get()) > 0)) {
                        transport_alive = process_shell_response(sc.ssl.get());
                    }
#endif
                    if (!local_stop && transport_alive) {
                        auto [new_cols, new_rows] = get_winsize();
                        if (new_cols > 0 && new_rows > 0 &&
                            (new_cols != last_cols || new_rows != last_rows)) {
                            ResizeMsg resize;
                            resize.cols = new_cols;
                            resize.rows = new_rows;
                            write_frame(sc.ssl.get(), resize, CONTROL_STREAM_ID);
                            last_cols = new_cols;
                            last_rows = new_rows;
                        }
                    }
                }

                if (local_stop && sc.ssl && sc.sfd != INVALID_SOCKET) {
                    try { write_frame(sc.ssl.get(), DetachMsg{}, CONTROL_STREAM_ID); } catch (...) {}
                }
            } catch (...) {
                transport_alive = false;
            }

            if (local_stop) {
                ssl_close(sc.ssl.get(), sc.sfd);
            } else if (sc.sfd != INVALID_SOCKET) {
                // The transport already failed. Avoid SSL_shutdown writing to a
                // dead socket (SIGPIPE on POSIX); reconnect using a fresh TLS link.
                CLOSESOCK(sc.sfd);
            }
            sc.sfd = INVALID_SOCKET;
            if (!local_stop) {
                local_stop = wait_for_local_stop(reconnect_delay_ms);
                reconnect_delay_ms = std::min(reconnect_delay_ms * 2, 5000);
            }
        }
        } catch (...) {
            restore_local_terminal();
            return 255;
        }

        restore_local_terminal();
        return 0;
    }

    // Non-interactive response handler: writes OutputMsg data to stdout
    // (ANSI-stripped — ConPTY mode CSI is not useful for --cmd capture),
    // returns false on SessionDiedMsg (capturing exit_code).
    bool process_noninteractive_response(SSL* ssl, int32_t& exit_code,
                                         bool* transport_error = nullptr,
                                         bool* session_ended = nullptr) {
        if (transport_error) *transport_error = false;
        if (session_ended) *session_ended = false;
        try {
            Message resp = read_frame(ssl);
            if (std::holds_alternative<OutputMsg>(resp)) {
                std::cout << strip_ansi_escapes(std::get<OutputMsg>(resp).data) << std::flush;
            } else if (std::holds_alternative<SessionDiedMsg>(resp)) {
                exit_code = std::get<SessionDiedMsg>(resp).exit_code;
                if (session_ended) *session_ended = true;
                return false;
            } else if (std::holds_alternative<DetachMsg>(resp)) {
                if (session_ended) *session_ended = true;
                return false;
            } else if (std::holds_alternative<ExitCodeMsg>(resp)) {
                exit_code = std::get<ExitCodeMsg>(resp).code;
                if (session_ended) *session_ended = true;
                return false;
            } else if (std::holds_alternative<PingMsg>(resp)) {
                write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
            }
        } catch (const std::exception& e) {
            std::cerr << "Shell transport failed: " << e.what() << "\n";
            if (transport_error) *transport_error = true;
            return false;
        } catch (...) {
            std::cerr << "Shell transport failed: unknown transport error\n";
            if (transport_error) *transport_error = true;
            return false;
        }
        return true;
    }

    bool process_shell_response(SSL* ssl) {
        try {
            Message resp = read_frame(ssl);
            if (std::holds_alternative<OutputMsg>(resp)) { std::cout << std::get<OutputMsg>(resp).data << std::flush; }
            else if (std::holds_alternative<ScrollbackMsg>(resp)) { std::cout << std::get<ScrollbackMsg>(resp).data << std::flush; }
            else if (std::holds_alternative<SessionDiedMsg>(resp)) {
                return false; // reconnect and let the server resurrect the named PTY
            } else if (std::holds_alternative<DetachMsg>(resp)) {
                return false; // remote detach is not a local-client exit
            } else if (std::holds_alternative<PingMsg>(resp)) {
                write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
            }
        } catch (...) { return false; }
        return true;
    }

    // ── CLI: list_sessions ────────────────────────────────────
    void list_sessions(const std::string& peer_name, bool all) {
        (void)all;
        if (peer_name.empty()) {
            std::cout << sessions_.summary() << "\n";
            return;
        }
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) { std::cerr << "Peer not found: " << peer_name << "\n"; return; }
        auto sc = connect_and_hello(addr, trusted_peer_pubkey(config_, peer_name));
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) { print_connect_failure(peer_name, sc); return; }
        try {
            SessionListMsg req; write_frame(sc.ssl.get(), req, 0);
            fd_set read_fds; FD_ZERO(&read_fds); FD_SET(sc.sfd, &read_fds);
            timeval tv{5, 0};
#ifdef _WIN32
            if (select(0, &read_fds, nullptr, nullptr, &tv) > 0) {
#else
            if (select((int)sc.sfd+1, &read_fds, nullptr, nullptr, &tv) > 0) {
#endif
                Message resp = read_frame(sc.ssl.get());
                if (std::holds_alternative<SessionListMsg>(resp))
                    for (auto& si : std::get<SessionListMsg>(resp).sessions)
                        std::cout << si.name << "  " << si.state << "  uptime=" << si.uptime_seconds << "s\n";
            } else std::cerr << "Timeout\n";
            CLOSESOCK(sc.sfd);
        } catch (...) { if (sc.sfd != INVALID_SOCKET) CLOSESOCK(sc.sfd); }
    }

    // ── CLI: health_check ─────────────────────────────────────
    bool health_check(const std::string& peer_name, std::string* status_out = nullptr) {
        std::string nonce = "bs-health-" + sha256_hex(
            peer_name + ":" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))
            .substr(0, 16);
        std::string output;
        int ec = daemon_shell_via_ipc(peer_name, "health-" + nonce, "echo " + nonce, &output, 15000);
        if (ec >= 0) {
            output = strip_ansi_escapes(output);
            bool ok = (ec == 0 && output.find(nonce) != std::string::npos);
            if (status_out) {
                *status_out = ok ? "healthy (data-plane ok)"
                                 : "unhealthy (data-plane probe mismatch)";
            }
            return ok;
        }
        if (!output.empty() &&
            !should_fallback_to_direct_shell(ec, output)) {
            if (status_out) *status_out = "unhealthy (data-plane probe failed: " + output + ")";
            return false;
        }
        // No safe daemon relay: fall back to a direct data-plane probe rather
        // than a bare Ping/Pong, so CLI-only health does not report control-plane
        // liveness as peer health.
        int prev = outbound_connect_timeout_ms_;
        outbound_connect_timeout_ms_ = kHealthConnectTimeoutMs;
        struct TimeoutRestore { int& ref; int val; ~TimeoutRestore() { ref = val; } } restore{outbound_connect_timeout_ms_, prev};

        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) {
            if (status_out) *status_out = "unknown peer";
            return false;
        }
        auto sc = connect_and_hello(addr, trusted_peer_pubkey(config_, peer_name));
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            if (status_out) {
                if (sc.fail != ConnectFailReason::None)
                    *status_out = connect_fail_string(sc.fail);
                else
                    *status_out = "unreachable";
            }
            return false;
        }
        try {
            AttachMsg am;
            am.session_name = "health-" + nonce;
            am.command = "echo " + nonce;
            am.cols = 80;
            am.rows = 24;
            am.term = "xterm-256color";
            write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
            std::string stdout_buf;
            int32_t exit_code = -1;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            while (std::chrono::steady_clock::now() < deadline) {
                if (SSL_pending(sc.ssl.get()) <= 0) {
                    fd_set rfds; FD_ZERO(&rfds); FD_SET(sc.sfd, &rfds);
                    timeval tv{2, 0};
#ifdef _WIN32
                    if (select(0, &rfds, nullptr, nullptr, &tv) <= 0) continue;
#else
                    if (select(static_cast<int>(sc.sfd) + 1, &rfds, nullptr, nullptr, &tv) <= 0) continue;
#endif
                }
                Message resp = read_frame(sc.ssl.get());
                if (std::holds_alternative<OutputMsg>(resp)) {
                    stdout_buf += strip_ansi_escapes(std::get<OutputMsg>(resp).data);
                } else if (std::holds_alternative<ExitCodeMsg>(resp)) {
                    exit_code = std::get<ExitCodeMsg>(resp).code;
                    // v2.0.1: brief post-death drain for late Windows OutputMsg
                    const auto ddeadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    while (std::chrono::steady_clock::now() < ddeadline) {
                        if (SSL_pending(sc.ssl.get()) <= 0) {
                            fd_set drfds; FD_ZERO(&drfds); FD_SET(sc.sfd, &drfds);
                            timeval dtv{0, 50'000};
#ifdef _WIN32
                            if (select(0, &drfds, nullptr, nullptr, &dtv) <= 0) continue;
#else
                            if (select(static_cast<int>(sc.sfd) + 1, &drfds, nullptr, nullptr, &dtv) <= 0) continue;
#endif
                        }
                        try {
                            Message late = read_frame(sc.ssl.get());
                            if (std::holds_alternative<OutputMsg>(late))
                                stdout_buf += strip_ansi_escapes(std::get<OutputMsg>(late).data);
                            else if (std::holds_alternative<PingMsg>(late))
                                write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                        } catch (...) { break; }
                    }
                    break;
                } else if (std::holds_alternative<SessionDiedMsg>(resp)) {
                    exit_code = std::get<SessionDiedMsg>(resp).exit_code;
                    const auto ddeadline =
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    while (std::chrono::steady_clock::now() < ddeadline) {
                        if (SSL_pending(sc.ssl.get()) <= 0) {
                            fd_set drfds; FD_ZERO(&drfds); FD_SET(sc.sfd, &drfds);
                            timeval dtv{0, 50'000};
#ifdef _WIN32
                            if (select(0, &drfds, nullptr, nullptr, &dtv) <= 0) continue;
#else
                            if (select(static_cast<int>(sc.sfd) + 1, &drfds, nullptr, nullptr, &dtv) <= 0) continue;
#endif
                        }
                        try {
                            Message late = read_frame(sc.ssl.get());
                            if (std::holds_alternative<OutputMsg>(late))
                                stdout_buf += strip_ansi_escapes(std::get<OutputMsg>(late).data);
                            else if (std::holds_alternative<PingMsg>(late))
                                write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                        } catch (...) { break; }
                    }
                    break;
                } else if (std::holds_alternative<PingMsg>(resp)) {
                    write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                }
            }
            bool ok = (exit_code == 0 && stdout_buf.find(nonce) != std::string::npos);
            if (status_out) {
                *status_out = ok ? "healthy (data-plane ok)"
                                 : "unhealthy (data-plane probe failed)";
            }
            CLOSESOCK(sc.sfd);
            return ok;
        } catch (const std::exception& e) {
            if (status_out) *status_out = std::string("error: ") + e.what();
            if (sc.sfd != INVALID_SOCKET) CLOSESOCK(sc.sfd);
            return false;
        } catch (...) {
            if (status_out) *status_out = "error";
            if (sc.sfd != INVALID_SOCKET) CLOSESOCK(sc.sfd);
            return false;
        }
    }

    // ── CLI: file_send ──────────────────────────────────────────
    std::string file_send(const std::string& peer_name, const std::string& local_path,
                          bool wait_for_completion) {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path) || fs::is_directory(local_path)) {
            return "ERROR file not found or is a directory: " + local_path;
        }
        // Try daemon IPC first (reuses existing mesh conns)
        std::string ipc = daemon_send_via_ipc(peer_name, local_path, 120000, wait_for_completion);
        if (!ipc.empty()) {
            return ipc;
        }
        return "ERROR no daemon running — cannot send without daemon mesh connection";
    }

    // ── CLI: file_recv ──────────────────────────────────────────
    // ── CLI: file_recv ──────────────────────────────────────────
    std::string file_recv(const std::string& peer_name, const std::string& remote_path,
                          const std::string& local_dest, bool wait_for_completion) {
        std::string dest = local_dest.empty() ? "." : local_dest;
        std::string ipc = daemon_recv_via_ipc(peer_name, remote_path, dest, 120000, wait_for_completion);
        if (!ipc.empty()) return ipc;
        return "ERROR no daemon running";
    }

    // ── CLI: edit_peer ──────────────────────────────────────────
    void edit_peer(const std::string& target) {
        auto colon = target.find(':');
        if (colon == std::string::npos || colon == 0 || colon == target.size() - 1) {
            std::cerr << "usage: bridgesessions edit <peer>:<path> (e.g. dev:/etc/nginx.conf)\n";
            return;
        }
        std::string peer_name = target.substr(0, colon);
        std::string remote_path = target.substr(colon + 1);

        // Download via daemon IPC
        std::string dl_result = daemon_edit_dl_via_ipc(peer_name, remote_path, 120000);
        if (dl_result.empty()) {
            std::cerr << "no daemon running — edit requires running daemon\n";
            return;
        }
        if (dl_result.rfind("ERROR", 0) == 0) {
            std::cerr << dl_result << "\n";
            return;
        }
        std::cout << dl_result << "\n";
        // dl_result format: "OK <local-path> <checksum>"
        // Parse local path and checksum from response
        auto sp1 = dl_result.find(' ');
        if (sp1 == std::string::npos) return;
        std::string local_path = dl_result.substr(sp1 + 1);
        auto sp2 = local_path.rfind(' ');
        if (sp2 == std::string::npos) return;
        std::string checksum = local_path.substr(sp2 + 1);
        local_path = local_path.substr(0, sp2);

        std::cout << "downloaded to " << local_path << "\n";

        // Open editor
#ifdef _WIN32
        std::string editor = "notepad";
        const char* env_editor = std::getenv("EDITOR");
        if (env_editor && *env_editor) editor = env_editor;
        int ret = run_editor_process(editor, local_path);
        if (ret != 0) { std::cerr << "editor exited with code " << ret << "\n"; }
#else
        std::string editor = "vim";
        const char* env_editor = std::getenv("EDITOR");
        if (env_editor && *env_editor) editor = env_editor;
        int ret = run_editor_process(editor, local_path);
        if (ret != 0) { std::cerr << "editor exited with code " << ret << "\n"; }
#endif

        // Check for changes and upload
        std::ifstream infile(local_path, std::ios::binary);
        std::string new_content((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
        std::string new_checksum = sha256_hex(new_content);

        if (new_checksum == checksum) {
            std::cout << "no changes\n";
            return;
        }
        std::cout << "file changed, uploading via daemon...\n";
        std::string up_result = daemon_edit_up_via_ipc(peer_name, remote_path, local_path, 120000);
        std::cout << up_result << "\n";
    }

    // ── CLI: show_stats ───────────────────────────────────────
    void show_stats() const {
        auto now = std::chrono::steady_clock::now();
        std::cout << "=== bridgesessions stats ===\n";
        std::cout << "node: " << config_.node_name << "  pubkey: " << our_pubkey_.substr(0,16) << "...\n";
        std::cout << "listen: " << config_.listen_addr << ":" << config_.listen_port << "\n";
        std::cout << "connections: " << conns_.size() << " / " << config_.max_peers << "\n";
        for (auto& cn : conns_) {
            if (cn.sock_fd == INVALID_SOCKET) continue;
            auto pong_age = std::chrono::duration_cast<std::chrono::seconds>(now - cn.last_pong).count();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - cn.connected_at).count();
            std::cout << "  " << cn.peer_name << "  " << cn.peer_addr
                      << "  " << (cn.is_outbound ? "outbound" : "inbound")
                      << "  uptime=" << uptime << "s"
                      << "  last_pong=" << pong_age << "s ago"
                      << "  bytes_in=" << cn.bytes_in << "  bytes_out=" << cn.bytes_out;
            if (cn.attached_session) std::cout << "  session=" << cn.attached_session->name;
            std::cout << "\n";
        }
        std::cout << "sessions: " << sessions_.list().size() << "\n";
    }

    // ── CLI: show_peers_detail — live connection status ──────────
    void show_peers_detail(const std::string& peer_name = "") {
        auto now = std::chrono::steady_clock::now();
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            if (!peer_name.empty() && c.peer_name != peer_name) continue;
            auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.last_pong).count();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - c.connected_at).count();
            std::cout << c.peer_name << " " << c.peer_addr << " "
                      << (c.is_outbound ? "outbound" : "inbound")  << " "
                      << "latency=" << latency << "ms "
                      << "uptime=" << uptime << "s" << std::endl;
        }
    }

    // ── Accessors (for tests) ──────────────────────────────────

    SessionRegistry& sessions() { return sessions_; }
    const std::vector<Conn>& conns() const { return conns_; }
    size_t conn_count() const { return conns_.size(); }

    // ── Public connect (for tests/CLI) ──────────────────────────
    bool connect_to_peer(const std::string& addr) {
        return connect_to_peer_impl(addr);
    }

#ifdef BS_TESTING
    // ── Test helpers ─────────────────────────────────────────────

    // True if the given conn has not received a pong within pong_timeout_secs.
    bool is_pong_timed_out(const Conn& c) const {
        auto now = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(config_.pong_timeout_secs);
        return (now - c.last_pong > timeout);
    }

    // Inject a GossipMsg directly into the discovered peers list.
    void inject_gossip(const GossipMsg& g) { merge_peers(g.peers); }

    // Return a snapshot of all discovered (non-seed) peers.
    std::vector<PeerEntry> discovered_peers() const { return config_.discovered; }

    // This node's own ed25519 public key hex.
    std::string own_pubkey_hex() const { return our_pubkey_; }

    // Duplicate-connection resolution: true if this node should keep its
    // outbound connection when both sides raced to connect each other.
    // Rule: keep outbound when our pubkey is lexicographically less than peer's.
    static bool should_keep_outbound(const std::string& own_hex, const std::string& peer_hex) {
        return own_hex < peer_hex;
    }

    // Compute the next reconnect delay in milliseconds for the given attempt number.
    // Doubles from 100ms, capped at reconnect_backoff_max_secs * 1000.
    long next_backoff_ms(int attempt) const {
        long ms = 100;
        long cap = static_cast<long>(config_.reconnect_backoff_max_secs) * 1000;
        for (int i = 0; i < attempt; ++i) {
            ms = std::min(ms * 2, cap);
        }
        return ms;
    }

    // Trust-filter tests.
    bool is_trusted_pubkey_for_test(const std::string& pk) {
        return is_trusted_pubkey(pk);
    }
    std::string configured_peer_addr_for_test(const std::string& name) const {
        for (const auto& s : config_.seeds)
            if (peer_name_eq(s.name, name)) return s.addr;
        for (const auto& d : config_.discovered)
            if (peer_name_eq(d.name, name)) return d.addr;
        return {};
    }
    void process_mdns_announcement_for_test(const std::string& name,
                                            const std::string& addr,
                                            const std::string& pubkey) {
        process_mdns_announcement(name, addr, pubkey);
    }

    // Hello duplicate-policy tests.
    void test_set_initial_hello_for_test(Conn& c, const HelloMsg& h) {
        c.initial_hello = h;
    }
    bool test_handle_hello_for_test(Conn& c, const HelloMsg& h) {
        if (!c.initial_hello.has_value()) {
            c.initial_hello = h;
            c.peer_name = h.node_name;
            merge_peers(h.known_peers);
            return true;
        }
        if (*c.initial_hello == h) return true;
        c.close_requested = true;
        return false;
    }

    // IPC token tests.
    void set_ipc_token_for_test(const std::string& token) { ipc_token_ = token; }
    std::string ipc_token_for_test() const { return ipc_token_; }
    std::string ipc_token_path_for_test() const { return ipc_token_path_; }
    bool ipc_request_is_authorized_for_test(const std::string& line) const {
        return !ipc_token_.empty() && line.size() > ipc_token_.size() &&
               line.compare(0, ipc_token_.size(), ipc_token_) == 0 &&
               (line[ipc_token_.size()] == ' ' || line[ipc_token_.size()] == '\t');
    }
    bool another_daemon_running_for_test() { return another_daemon_running(); }
    void inject_file_meta_for_test(Conn& c, const FileMetaMsg& m) { handle_file_meta(c, m); }
    void inject_file_chunk_for_test(Conn& c, const FileChunkMsg& m) { handle_file_chunk(c, m); }
    const std::string& pending_recv_dir_for_test(const Conn& c) const { return c.pending_recv_dir; }
    bool begin_async_receive_for_test(Conn& c, const std::string& dir) {
        return begin_async_receive(c, dir);
    }
    const FileReceiveState& file_receive_for_test(const Conn& c) const { return c.file_receive; }
    bool write_pty_input_for_test(Session& s, const void* data, size_t len) {
        return write_pty_input(s, data, len);
    }
#ifndef _WIN32
    bool drain_pending_pty_input_for_test(Session& s) {
        return drain_pending_pty_input(s);
    }
    const std::string& pending_input_for_test(const Session& s) const {
        return s.pending_input;
    }
#endif
#endif
};

// 10. CLIPBOARD BRIDGE (Windows only)
// ────────────────────────────────────────────────────────────────────

#ifdef _WIN32

class ClipboardBridge {
    std::string last_seen_hash_;
    std::string last_acked_hash_;

    static std::string wide_to_utf8(const wchar_t* wstr) {
        if (!wstr) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return {};
        std::string result(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, result.data(), len, nullptr, nullptr);
        return result;
    }

public:
    ClipboardBridge() {
        if (OpenClipboard(nullptr)) {
            HANDLE h = GetClipboardData(CF_UNICODETEXT);
            if (h) {
                auto* wstr = static_cast<const wchar_t*>(GlobalLock(h));
                if (wstr) {
                    auto text = wide_to_utf8(wstr);
                    if (!text.empty()) {
                        last_seen_hash_ = sha256_hex(text);
                    }
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
    }

    std::optional<std::string> poll() {
        if (!OpenClipboard(nullptr)) return std::nullopt;
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (!h) { CloseClipboard(); return std::nullopt; }
        auto* wstr = static_cast<const wchar_t*>(GlobalLock(h));
        if (!wstr) { CloseClipboard(); return std::nullopt; }
        auto text = wide_to_utf8(wstr);
        GlobalUnlock(h);
        CloseClipboard();
        if (text.empty()) return std::nullopt;
        std::string hash = sha256_hex(text);
        if (hash == last_seen_hash_ || hash == last_acked_hash_) return std::nullopt;
        last_seen_hash_ = hash;
        return text;
    }

    void write_to_clipboard(const std::string& text) {
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        int wlen = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (wlen > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wlen + 1) * sizeof(wchar_t));
            if (hMem) {
                auto* wstr = static_cast<wchar_t*>(GlobalLock(hMem));
                MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wstr, wlen);
                wstr[wlen] = L'\0';
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }
        CloseClipboard();
        last_seen_hash_ = sha256_hex(text);
    }

    void ack_hash(const std::string& hash) { last_acked_hash_ = hash; }
};

#endif // _WIN32

// ────────────────────────────────────────────────────────────────────
// 11. IMAGE RENDER (cross-platform stubs)
// ────────────────────────────────────────────────────────────────────

namespace {

static constexpr size_t kMaxImageBytes = 50 * 1024 * 1024; // 50MB cap

[[nodiscard]] inline bool is_png_magic(std::span<const uint8_t> bytes) {
    static constexpr std::array<uint8_t, 8> kPngMagic{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return bytes.size() >= kPngMagic.size() &&
           std::equal(kPngMagic.begin(), kPngMagic.end(), bytes.begin());
}

[[nodiscard]] inline bool is_jpeg_magic(std::span<const uint8_t> bytes) {
    return bytes.size() >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
}

[[nodiscard]] inline bool is_gif_magic_alt(std::span<const uint8_t> bytes) {
    return bytes.size() >= 6 &&
           (std::memcmp(bytes.data(), "GIF87a", 6) == 0 || std::memcmp(bytes.data(), "GIF89a", 6) == 0);
}

} // anonymous namespace

[[nodiscard]] inline std::vector<uint8_t> read_binary_file(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("cannot stat " + path.string() + ": " + ec.message());
    if (size > kMaxImageBytes)
        throw std::runtime_error("image exceeds 50MB cap: " + path.string());

    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in) throw std::runtime_error("cannot read " + path.string());
    }
    return data;
}

[[nodiscard]] inline std::optional<uint8_t> detect_image_format(std::span<const uint8_t> bytes) {
    if (is_png_magic(bytes)) return static_cast<uint8_t>(0);   // PNG
    if (is_jpeg_magic(bytes)) return static_cast<uint8_t>(1);  // JPEG
    return std::nullopt;
}

struct GifMetadata {
    uint32_t delay_ms = 0;
    uint16_t loop_count = 0; // 0 = infinite
};

[[nodiscard]] inline GifMetadata parse_gif_metadata(std::span<const uint8_t> bytes) {
    GifMetadata meta{};
    if (!is_gif_magic_alt(bytes)) return meta;

    for (size_t i = 0; i + 17 < bytes.size(); ++i) {
        if (bytes[i] == 0x21 && bytes[i + 1] == 0xF9 && bytes[i + 2] == 0x04) {
            uint16_t delay_cs = static_cast<uint16_t>(bytes[i + 4]) | (static_cast<uint16_t>(bytes[i + 5]) << 8);
            if (meta.delay_ms == 0) meta.delay_ms = static_cast<uint32_t>(delay_cs) * 10u;
        }
        if (bytes[i] == 0x21 && bytes[i + 1] == 0xFF && bytes[i + 2] == 0x0B) {
            const char* app = reinterpret_cast<const char*>(bytes.data() + i + 3);
            if (std::memcmp(app, "NETSCAPE2.0", 11) == 0 || std::memcmp(app, "ANIMEXTS1.0", 11) == 0) {
                if (bytes[i + 14] == 0x03 && bytes[i + 15] == 0x01) {
                    meta.loop_count = static_cast<uint16_t>(bytes[i + 16]) |
                                      (static_cast<uint16_t>(bytes[i + 17]) << 8);
                }
            }
        }
    }
    return meta;
}

[[nodiscard]] inline ImageDataMsg make_image_data_message(const std::filesystem::path& path) {
    auto bytes = read_binary_file(path);
    auto format = detect_image_format(bytes);
    if (!format)
        throw std::runtime_error("unsupported image format for " + path.string() + " (expected PNG or JPEG)");
    ImageDataMsg msg;
    msg.format = *format;
    msg.name = path.filename().string();
    msg.data = std::move(bytes);
    return msg;
}

[[nodiscard]] inline ImageFrameMsg make_image_frame_message(const std::filesystem::path& path) {
    auto bytes = read_binary_file(path);
    if (!is_gif_magic_alt(bytes))
        throw std::runtime_error("unsupported animation format for " + path.string() + " (expected GIF)");
    auto meta = parse_gif_metadata(bytes);
    ImageFrameMsg msg;
    msg.format = 2;
    msg.delay_ms = meta.delay_ms;
    msg.loop_count = meta.loop_count;
    msg.data = std::move(bytes);
    return msg;
}

[[nodiscard]] inline bool render_image_message(const ImageDataMsg& msg, int output_fd) {
#ifdef _WIN32
    // Windows: print text placeholder
    std::string note = "[Image: " + msg.name + "]\r\n";
    HANDLE hOut = (output_fd == 1) ? GetStdHandle(STD_OUTPUT_HANDLE)
                 : reinterpret_cast<HANDLE>(_get_osfhandle(output_fd));
    DWORD written;
    WriteFile(hOut, note.data(), static_cast<DWORD>(note.size()), &written, nullptr);
    return true;
#else
    // POSIX: attempt chafa render via fork/exec
    const char* chafa_path = "/usr/bin/chafa";
    if (::access(chafa_path, X_OK) != 0) {
        std::string note = "[Image: " + msg.name + "]\n";
        const uint8_t* p = reinterpret_cast<const uint8_t*>(note.data());
        size_t remaining = note.size();
        while (remaining > 0) {
            ssize_t n = ::write(output_fd, p, remaining);
            if (n < 0) { if (errno == EINTR) continue; return false; }
            p += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }
    // Write image data to temp file, fork chafa
    std::string tmpl = "/tmp/bs-image-XXXXXX";
    int tmp_fd = ::mkstemp(tmpl.data());
    if (tmp_fd < 0) return false;
    if (!msg.data.empty()) {
        const uint8_t* p = msg.data.data();
        size_t remaining = msg.data.size();
        while (remaining > 0) {
            ssize_t n = ::write(tmp_fd, p, remaining);
            if (n < 0) { if (errno == EINTR) continue; ::close(tmp_fd); ::unlink(tmpl.c_str()); return false; }
            p += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
    }
    ::close(tmp_fd);

    pid_t pid = ::fork();
    if (pid < 0) { ::unlink(tmpl.c_str()); return false; }
    if (pid == 0) {
        if (output_fd != STDOUT_FILENO) ::dup2(output_fd, STDOUT_FILENO);
        ::execl(chafa_path, chafa_path, "--animate", "off", tmpl.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        ::unlink(tmpl.c_str());
        return false;
    }
    ::unlink(tmpl.c_str());
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

[[nodiscard]] inline bool render_image_message(const ImageFrameMsg& msg, int output_fd) {
#ifdef _WIN32
    // Windows: print text placeholder (GIF frame)
    std::string note = "[ImageFrame: delay=" + std::to_string(msg.delay_ms) + "ms]\r\n";
    HANDLE hOut = (output_fd == 1) ? GetStdHandle(STD_OUTPUT_HANDLE)
                 : reinterpret_cast<HANDLE>(_get_osfhandle(output_fd));
    DWORD written;
    WriteFile(hOut, note.data(), static_cast<DWORD>(note.size()), &written, nullptr);
    return true;
#else
    const char* chafa_path = "/usr/bin/chafa";
    if (::access(chafa_path, X_OK) != 0) {
        std::string note = "[ImageFrame: delay=" + std::to_string(msg.delay_ms) + "ms]\n";
        const uint8_t* p = reinterpret_cast<const uint8_t*>(note.data());
        size_t remaining = note.size();
        while (remaining > 0) {
            ssize_t n = ::write(output_fd, p, remaining);
            if (n < 0) { if (errno == EINTR) continue; return false; }
            p += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
        return true;
    }
    std::string tmpl = "/tmp/bs-frame-XXXXXX";
    int tmp_fd = ::mkstemp(tmpl.data());
    if (tmp_fd < 0) return false;
    if (!msg.data.empty()) {
        const uint8_t* p = msg.data.data();
        size_t remaining = msg.data.size();
        while (remaining > 0) {
            ssize_t n = ::write(tmp_fd, p, remaining);
            if (n < 0) { if (errno == EINTR) continue; ::close(tmp_fd); ::unlink(tmpl.c_str()); return false; }
            p += static_cast<size_t>(n);
            remaining -= static_cast<size_t>(n);
        }
    }
    ::close(tmp_fd);

    pid_t pid = ::fork();
    if (pid < 0) { ::unlink(tmpl.c_str()); return false; }
    if (pid == 0) {
        if (output_fd != STDOUT_FILENO) ::dup2(output_fd, STDOUT_FILENO);
        ::execl(chafa_path, chafa_path, tmpl.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        ::unlink(tmpl.c_str());
        return false;
    }
    ::unlink(tmpl.c_str());
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// Top-level CLI wrapper for image/anim commands
inline void render_image_to_terminal(const std::string& path_str) {
    std::filesystem::path path(path_str);
    if (!std::filesystem::exists(path)) { std::cerr << "File not found: " << path_str << "\n"; return; }
    auto bytes = read_binary_file(path);
    if (is_gif_magic_alt(bytes)) {
        auto frame = make_image_frame_message(path);
        if (!render_image_message(frame, STDOUT_FILENO))
            std::cerr << "failed to render image frame\n";
    } else {
        auto img = make_image_data_message(path);
        if (!render_image_message(img, STDOUT_FILENO))
            std::cerr << "failed to render image\n";
    }
}

// ────────────────────────────────────────────────────────────────────
// 12. PEER HELPERS
// ────────────────────────────────────────────────────────────────────

[[nodiscard]] inline std::vector<PeerEntry> load_peers_from_file(const std::string& path) {
    MeshConfig cfg = load_config(path);
    std::vector<PeerEntry> peers;
    peers.reserve(cfg.seeds.size() + cfg.discovered.size());
    peers.insert(peers.end(), cfg.seeds.begin(), cfg.seeds.end());
    peers.insert(peers.end(), cfg.discovered.begin(), cfg.discovered.end());
    return peers;
}

[[nodiscard]] inline std::optional<PeerEntry> find_peer(const std::vector<PeerEntry>& peers,
                                                         const std::string& name) {
    for (const auto& p : peers) {
        if (p.name == name) return p;
    }
    return std::nullopt;
}

} // namespace bs::mesh

// ────────────────────────────────────────────────────────────────────
// 2. MAIN — CLI + daemon (guarded for test builds)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_TESTING

#include <CLI/CLI.hpp>
#include <cstdlib>
#include <iostream>

namespace {

std::string resolve_home(const std::string& path) {
    return bs::mesh::expand_home(path);
}

// ── keygen: generate ed25519 keypair ──────────────────────────────
int cmd_keygen(const std::string& app_home) {
    std::string dir = app_home.empty() ? (resolve_home("~") + "/.bridgesessions") : app_home;
    if (dir.empty()) { std::cerr << "config dir empty / HOME not set\n"; return 1; }

    if (!bs::mesh::ensure_private_directory(dir)) {
        std::cerr << "cannot create private config directory: " << dir << "\n";
        return 1;
    }

    auto [cert, key] = bs::mesh::generate_cert_key_pair("bridgesessions");
    auto pubkey = bs::mesh::pubkey_hex_from_pem(key);

    std::string key_path  = dir + "/id_ed25519.pem";
    std::string cert_path = dir + "/id_ed25519-cert.pem";
    std::string pub_path  = dir + "/id_ed25519.pub";

    if (!bs::mesh::write_private_text_file(key_path, key) ||
        !bs::mesh::write_private_text_file(cert_path, cert) ||
        !bs::mesh::write_private_text_file(pub_path, pubkey + "\n")) {
        std::cerr << "cannot securely write generated identity\n";
        return 1;
    }

    std::cout << "Generated ed25519 keypair:\n"
              << "  Private key: " << key_path << "\n"
              << "  Certificate: " << cert_path << "\n"
              << "  Public key:  " << pub_path << "\n"
              << "  Pubkey hex:  " << pubkey << "\n";

    return 0;
}

// Free-function wrappers for IPC (used by main dispatch)
static std::string daemon_simple_ipc(const std::string& cmd, int wait_ms,
                                     const std::string& app_home) {
    std::string token = bs::mesh::load_ipc_token(app_home);
    if (token.empty()) return "";
    SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == INVALID_SOCKET) return "";
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(19980);
    // set_socket_timeouts inline
    int ms = wait_ms > 0 ? wait_ms : 5000;
#ifdef _WIN32
    DWORD to = ms;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&to, sizeof(to));
#else
    timeval tv{}; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) { CLOSESOCK(sfd); return ""; }
    std::string full = token + " " + cmd + "\n";
    send(sfd, full.data(), (int)full.size(), 0);
    char buf[4096] = {}; int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = recv(sfd, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n > 0) { total += n; buf[total] = '\0'; if (strchr(buf, '\n')) break; }
        else break;
    }
    CLOSESOCK(sfd);
    std::string line(buf);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    return line;
}

// ── authorize: register a hex-encoded ed25519 public key ──────────
int cmd_authorize(const char* hex_pubkey, const std::string& app_home) {
    if (!hex_pubkey || !*hex_pubkey) {
        std::cerr << "usage: bridgesessions authorize <hex-pubkey>\n";
        return 1;
    }

    std::string normalized(hex_pubkey);
    const auto decoded = bs::mesh::hex_decode(normalized);
    if (normalized.size() != 64 || decoded.size() != 32) {
        std::cerr << "invalid ed25519 public key: expected 64 hexadecimal characters\n";
        return 1;
    }
    for (char& c : normalized) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
    }

    std::string dir = app_home.empty() ? (resolve_home("~") + "/.bridgesessions") : app_home;
    if (dir.empty()) { std::cerr << "config dir empty / HOME not set\n"; return 1; }

    if (!bs::mesh::ensure_private_directory(dir)) {
        std::cerr << "cannot create private config directory: " << dir << "\n";
        return 1;
    }
    std::string path = dir + "/authorized_keys";

    // Check for duplicates
    {
        std::ifstream existing(path);
        std::string line;
        while (std::getline(existing, line)) {
            if (line == normalized) {
                std::cout << "Key already authorized: " << normalized << "\n";
                return 0;
            }
        }
    }

    if (!bs::mesh::append_private_text_file(path, normalized + "\n")) {
        std::cerr << "cannot securely update " << path << "\n";
        return 1;
    }

    std::cout << "Authorized key: " << normalized << "\n";
    std::cout << "Written to: " << path << "\n";
    return 0;
}

int cmd_doctor(const std::string& config_path, const std::string& app_home) {
    namespace fs = std::filesystem;
    std::string dir = app_home.empty() ? resolve_home("~/.bridgesessions") : app_home;
    int failures = 0;
    auto pass = [](const std::string& label, const std::string& detail = "") {
        std::cout << "[PASS] " << label;
        if (!detail.empty()) std::cout << ": " << detail;
        std::cout << "\n";
    };
    auto warn = [](const std::string& label, const std::string& detail = "") {
        std::cout << "[WARN] " << label;
        if (!detail.empty()) std::cout << ": " << detail;
        std::cout << "\n";
    };
    auto fail = [&](const std::string& label, const std::string& detail = "") {
        std::cout << "[FAIL] " << label;
        if (!detail.empty()) std::cout << ": " << detail;
        std::cout << "\n";
        ++failures;
    };

    std::cout << "bridgesessions doctor v" << bs::mesh::kBridgeSessionsVersion << "\n";
    if (fs::exists(dir) && fs::is_directory(dir)) pass("dir config", dir);
    else fail("dir config", dir);

    // Identity files live directly under ~/.bridgesessions. Older doctor builds
    // incorrectly checked ~/.bridgesessions/keys and sent operators chasing a
    // false cert failure while the daemon was using the correct files.
    for (const auto& name : {"id_ed25519.pem", "id_ed25519-cert.pem", "id_ed25519.pub"}) {
        fs::path p = fs::path(dir) / name;
        if (fs::exists(p) && fs::is_regular_file(p)) pass(name, p.string());
        else fail(name, p.string());
    }

    fs::path cfg(config_path);
    if (fs::exists(cfg) && fs::is_regular_file(cfg)) pass("config", cfg.string());
    else warn("config", cfg.string());

    for (const auto& name : {"logs", "state"}) {
        fs::path p = fs::path(dir) / name;
        if (fs::exists(p) && fs::is_directory(p)) pass(std::string("dir ") + name, p.string());
        else warn(std::string("dir ") + name, p.string());
    }

#ifndef _WIN32
    fs::path unit = fs::path(resolve_home("~/.config/systemd/user")) / "bridgesessions.service";
    if (fs::exists(unit) && fs::is_regular_file(unit)) pass("systemd unit", unit.string());
    else warn("systemd unit", unit.string());
#endif

    std::string ipc = daemon_simple_ipc("HEALTH __doctor_nonexistent__", 1500, app_home);
    if (!ipc.empty()) pass("daemon IPC", "port 19980 answered");
    else warn("daemon IPC", "port 19980 did not answer");

    return failures == 0 ? 0 : 1;
}

} // anonymous namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    CLI::App app{"bridgesessions — mesh terminal relay"};
    app.set_version_flag("--version,-V", std::string(bs::mesh::kBridgeSessionsVersion));

    // Global options
    std::string config_path = "";
    std::string config_dir = "";
    bool daemon_flag = false;
    app.add_option("--config", config_path, "Config file path (default: ~/.bridgesessions/config)");
    app.add_option("--config-dir", config_dir, "Config directory (default: ~/.bridgesessions)");
    app.add_flag("--daemon", daemon_flag, "Detach from terminal (daemonize)");

    // Fast path: `bs dev hermes` (the `bs` executable is a symlink to this binary).
    // Unknown peer names are resolved through `ssh -G` for address discovery only;
    // terminal data still travels exclusively over the BridgeSessions protocol.
    std::string quick_peer, quick_session = "shell";
    app.add_option("peer", quick_peer, "Peer name or SSH Host alias");
    app.add_option("session", quick_session, "Persistent server session name (default: shell)");

    // Subcommand: shell
    std::string shell_peer, shell_session = "default", shell_cmd;
    uint16_t shell_cols = 80, shell_rows = 24;
    bool shell_record = false, shell_signal_forward = true;
    std::string shell_signal_on_detach;
    auto* shell_cmd_app = app.add_subcommand("shell", "Open shell on a peer");
    shell_cmd_app->add_option("peer", shell_peer, "Peer name")->required();
    shell_cmd_app->add_option("-n,--name", shell_session, "Session name");
    shell_cmd_app->add_option("-x,--cmd", shell_cmd, "Command override");
    auto* shell_cols_opt = shell_cmd_app->add_option("--cols", shell_cols, "Terminal columns");
    auto* shell_rows_opt = shell_cmd_app->add_option("--rows", shell_rows, "Terminal rows");
    shell_cmd_app->add_flag("-r,--record", shell_record, "Record session output to file");
    shell_cmd_app->add_flag("--signal-forward{true}", shell_signal_forward, "Forward Ctrl-C to remote child (default: on)")->default_str("true");
    shell_cmd_app->add_option("--signal-on-detach", shell_signal_on_detach, "Send HUP/TERM/INT/QUIT/KILL to the child when the last peer detaches (default: none)")->check(CLI::IsMember({"HUP","TERM","INT","QUIT","KILL"}));

    // Subcommand: sessions
    std::string sessions_peer;
    bool sessions_all = false;
    auto* sessions_cmd_app = app.add_subcommand("sessions", "List sessions");
    sessions_cmd_app->add_option("peer", sessions_peer, "Peer name (omit for local)");
    sessions_cmd_app->add_flag("--all", sessions_all, "All peers");

    // Subcommand: keygen
    auto* keygen_cmd_app = app.add_subcommand("keygen", "Generate ed25519 keypair");

    // Subcommand: authorize
    std::string auth_pubkey;
    auto* auth_cmd_app = app.add_subcommand("authorize", "Authorize a peer public key");
    auth_cmd_app->add_option("pubkey", auth_pubkey, "Hex pubkey")->required();

    // Subcommand: doctor
    auto* doctor_cmd_app = app.add_subcommand("doctor", "Check local bridgesessions configuration");

    // Subcommand: peers
    auto* peers_cmd = app.add_subcommand("peers", "Manage peers");
    peers_cmd->require_subcommand(1);

    auto* peers_list = peers_cmd->add_subcommand("list", "List peers");
    std::string peer_add_name, peer_add_addr;
    auto* peers_add = peers_cmd->add_subcommand("add", "Add a seed peer");
    peers_add->add_option("name", peer_add_name)->required();
    peers_add->add_option("addr", peer_add_addr)->required();
    std::string peer_remove_name;
    auto* peers_remove = peers_cmd->add_subcommand("remove", "Remove a peer");
    peers_remove->add_option("name", peer_remove_name)->required();
    // health
    std::string health_peer;
    auto* health_cmd_app = app.add_subcommand("health", "Ping/pong health check against a peer");
    health_cmd_app->add_option("peer", health_peer, "Peer name")->required();
    // reconnect
    std::string reconnect_peer;
    auto* reconnect_cmd_app = app.add_subcommand("reconnect", "Tear down and re-handshake one peer via the running daemon");
    reconnect_cmd_app->add_option("peer", reconnect_peer, "Peer name")->required();
    // image
    std::string image_file;
    auto* image_cmd_app = app.add_subcommand("image", "Preview an image in the terminal");
    image_cmd_app->add_option("file", image_file, "Image file path")->required();
    // anim
    std::string anim_file;
    auto* anim_cmd_app = app.add_subcommand("anim", "Preview an animated GIF in the terminal");
    anim_cmd_app->add_option("file", anim_file, "GIF file path")->required();
    // stats
    auto* stats_cmd_app = app.add_subcommand("stats", "Show connection and session statistics");

    // file
    auto* file_cmd = app.add_subcommand("file", "File transfer operations");
    file_cmd->require_subcommand(1);
    std::string file_send_peer, file_send_path;
    bool file_send_wait = false;
    auto* file_send_app = file_cmd->add_subcommand("send", "Send file to a peer's receive directory");
    file_send_app->add_option("peer", file_send_peer, "Peer name")->required();
    file_send_app->add_option("local", file_send_path, "Local file path")->required();
    file_send_app->add_flag("--wait", file_send_wait, "Block until the peer acknowledges transfer completion");
    std::string file_recv_peer, file_recv_remote, file_recv_local, file_recv_to;
    bool file_recv_wait = false;
    auto* file_recv_app = file_cmd->add_subcommand("recv", "Receive file from a peer (run on target node)");
    file_recv_app->add_option("peer", file_recv_peer, "Peer name")->required();
    file_recv_app->add_option("remote", file_recv_remote, "Remote file path")->required();
    file_recv_app->add_option("local", file_recv_local, "Local directory (default: .)");
    file_recv_app->add_option("--to", file_recv_to, "Local destination path or directory");
    file_recv_app->add_flag("--wait", file_recv_wait, "Block until the transfer completes or fails");

    // vfolder
    auto* vfolder_cmd = app.add_subcommand("vfolder", "Manage virtual folder sync");
    vfolder_cmd->require_subcommand(1);
    std::string vfolder_name, vfolder_local, vfolder_peer, vfolder_remote, vfolder_dir;
    int vfolder_interval = 30;
    auto* vfolder_add = vfolder_cmd->add_subcommand("add", "Add a virtual folder mapping");
    vfolder_add->add_option("name", vfolder_name, "Mapping name")->required();
    vfolder_add->add_option("local", vfolder_local, "Local path")->required();
    vfolder_add->add_option("peer", vfolder_peer, "Remote peer")->required();
    vfolder_add->add_option("remote", vfolder_remote, "Remote path")->required();
    vfolder_add->add_option("--interval", vfolder_interval, "Sync interval (seconds)");
    vfolder_add->add_option("--dir", vfolder_dir, "Sync direction (push/pull/bidirectional)");
    auto* vfolder_sync = vfolder_cmd->add_subcommand("sync", "Sync a specific folder now");
    vfolder_sync->add_option("name", vfolder_name, "Mapping name")->required();
    auto* vfolder_list = vfolder_cmd->add_subcommand("list", "List active folder mappings");

    // edit
    std::string edit_target;
    auto* edit_cmd_app = app.add_subcommand("edit", "Edit a file on a remote peer");
    edit_cmd_app->add_option("target", edit_target, "Peer:path (e.g. dev:/etc/nginx.conf)")->required();

    // pane (BridgePanel publish from the mesh CLI)
    std::string pane_session = "default", pane_type = "documents", pane_title, pane_file;
    auto* pane_cmd_app = app.add_subcommand("pane", "Publish a file to the BridgePanel surface");
    pane_cmd_app->require_subcommand(1);
    auto* pane_publish = pane_cmd_app->add_subcommand("publish", "Copy a local file into a BridgePanel session");
    pane_publish->add_option("--session", pane_session, "Session name (default: default)");
    pane_publish->add_option("--type", pane_type, "documents | comms");
    pane_publish->add_option("--title", pane_title, "Display title / filename override");
    pane_publish->add_option("file", pane_file, "Local markdown file to publish")->required();

    CLI11_PARSE(app, argc, argv);

    // Resolve config path
    std::string home_dir;
    if (!config_dir.empty()) { home_dir = config_dir; }
    else { home_dir = resolve_home("~/.bridgesessions"); }
    if (config_path.empty()) { config_path = home_dir + "/config"; }
    // Ensure isolated app root exists for --config-dir runs
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(home_dir, ec);
        auto paths = bs::mesh::make_app_paths(home_dir);
        fs::create_directories(paths.received, ec);
        fs::create_directories(paths.logs, ec);
        fs::create_directories(paths.state, ec);
    }

    // Dispatch
    if (!quick_peer.empty()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        if (!bs::mesh::import_ssh_alias_peer(cfg, quick_peer)) {
            std::cerr << quick_peer
                      << " is not a configured BridgeSessions peer or valid SSH alias\n";
            return 2;
        }
        if (bs::mesh::trusted_peer_pubkey(cfg, quick_peer).empty()) {
            std::cerr << "Refusing untrusted first contact to " << quick_peer
                      << ": pair it once with a pinned BridgeSessions pubkey\n";
            return 2;
        }
        bs::mesh::bootstrap_identity(home_dir);
        auto [cols, rows] = bs::mesh::get_winsize();
        bs::mesh::MeshController mc(cfg, home_dir);
        return mc.shell_peer(quick_peer, quick_session, {}, cols, rows,
                             "xterm-256color");
    }
    if (shell_cmd_app->parsed()) {
        // Commands launched from a real terminal keep full PTY input/output. Piped or
        // automated commands use daemon IPC and capture a finite result.
        if (!shell_cmd.empty() && !bs::mesh::stdin_is_terminal()) {
            bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
            bs::mesh::bootstrap_identity(home_dir);
            bs::mesh::MeshController mc(cfg, home_dir);
            std::string output;
            int ec = mc.daemon_shell_via_ipc(shell_peer, shell_session, shell_cmd, &output);
            if (ec == -1) {
                // Shell execution intentionally uses an isolated direct TLS
                // connection; older/unavailable daemons take the same path.
                std::cerr << "Using direct TLS shell transport.\n";
                return mc.shell_peer(shell_peer, shell_session, shell_cmd,
                                     shell_cols, shell_rows, "xterm-256color");
            }
            if (ec < 0) {
                // timeout (-2) or other error — report, don't double-exec
                std::cerr << "Shell IPC error (code " << ec << ").\n";
                return 1;
            }
            if (!output.empty()) std::cout << output;
            return ec;
        }
        // Interactive shell: direct TLS (needs full terminal passthrough).
        // `shell -x` historically kept the parser defaults (80x24), unlike the
        // quick-connect path. That forces full-screen TUIs into a tiny remote PTY.
        auto [detected_cols, detected_rows] = bs::mesh::get_winsize();
        if (shell_cols_opt->count() == 0) shell_cols = detected_cols;
        if (shell_rows_opt->count() == 0) shell_rows = detected_rows;
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        return mc.shell_peer(shell_peer, shell_session, shell_cmd, shell_cols, shell_rows, "xterm-256color", shell_signal_forward, shell_signal_on_detach);
    }
    if (sessions_cmd_app->parsed()) {
        if (sessions_peer.empty()) {
            std::string ipc = daemon_simple_ipc("SESSIONS", 3000, home_dir);
            if (!ipc.empty() && ipc.rfind("ERROR", 0) != 0) {
                std::cout << ipc << "\n";
                return 0;
            }
        }
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::MeshController mc(cfg, home_dir);
        mc.list_sessions(sessions_peer, sessions_all);
        return 0;
    }
    if (keygen_cmd_app->parsed()) {
        return cmd_keygen(home_dir);
    }
    if (auth_cmd_app->parsed()) {
        return cmd_authorize(auth_pubkey.c_str(), home_dir);
    }
    if (doctor_cmd_app->parsed()) {
        return cmd_doctor(config_path, home_dir);
    }
    if (peers_list->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        // Show known peers from config
        std::cout << "=== Known peers ===\n";
        for (auto& p : cfg.seeds) std::cout << "  [seed] " << p.name << " " << p.addr << std::endl;
        for (auto& p : cfg.discovered) std::cout << "  [discovered] " << p.name << " " << p.addr << std::endl;
        // Show live connection status if daemon is running
        bs::mesh::MeshController mc(cfg, home_dir);
        mc.show_peers_detail();
        return 0;
    }
    if (peers_add->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        cfg.seeds.push_back({peer_add_name, peer_add_addr});
        (void)bs::mesh::save_config(config_path, cfg);
        std::cout << "added seed " << peer_add_name << " -> " << peer_add_addr << std::endl;
        return 0;
    }
    if (peers_remove->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        cfg.seeds.erase(std::remove_if(cfg.seeds.begin(), cfg.seeds.end(),
            [&](auto& p){ return p.name == peer_remove_name; }), cfg.seeds.end());
        (void)bs::mesh::save_config(config_path, cfg);
        std::cout << "removed seed " << peer_remove_name << std::endl;
        return 0;
    }

    if (health_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string status;
        bool ok = mc.health_check(health_peer, &status);
        std::cout << health_peer << " " << status << std::endl;
        return ok ? 0 : 1;
    }
    if (reconnect_cmd_app->parsed()) {
        std::string result = daemon_simple_ipc("RECONNECT " + reconnect_peer, 25000, home_dir);
        if (result.empty()) {
            std::cerr << "ERROR no daemon running\n";
            return 1;
        }
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (image_cmd_app->parsed()) {
        bs::mesh::render_image_to_terminal(image_file);
        return 0;
    }
    if (anim_cmd_app->parsed()) {
        bs::mesh::render_image_to_terminal(anim_file);
        return 0;
    }
    if (stats_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string ipc = daemon_simple_ipc("STATS", 3000, home_dir);
        if (!ipc.empty() && ipc.rfind("ERROR", 0) != 0) {
            std::cout << ipc << "\n";
            return 0;
        }
        mc.show_stats();
        return 0;
    }
    if (file_send_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string result = mc.file_send(file_send_peer, file_send_path, file_send_wait);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (file_recv_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        std::string dest = !file_recv_to.empty() ? file_recv_to : file_recv_local;
        std::string result = mc.file_recv(file_recv_peer, file_recv_remote, dest, file_recv_wait);
        std::cout << result << "\n";
        return result.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (edit_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(home_dir);
        bs::mesh::MeshController mc(cfg, home_dir);
        mc.edit_peer(edit_target);
        return 0;
    }
    if (pane_publish->parsed()) {
        // type whitelist
        if (pane_type != "comms" && pane_type != "documents") {
            std::cerr << "ERROR --type must be comms or documents\n";
            return 2;
        }
        // session / title are used as directory + file names -> restrict to a safe charset
        auto safe_name = [](const std::string& s) {
            return std::all_of(s.begin(), s.end(), [](unsigned char c) {
                return std::isalnum(c) || c == ' ' || c == '.' || c == '_' || c == '-';
            });
        };
        if (!safe_name(pane_session) || !safe_name(pane_title)) {
            std::cerr << "ERROR invalid --session or --title (use alnum, space, . _ -)\n";
            return 2;
        }
        // locate the bridgepanel helper on PATH (installed to ~/.local/bin)
        std::string bin = "bridgepanel";
        FILE* which = popen("command -v bridgepanel 2>/dev/null", "r");
        if (which) {
            char buf[512];
            std::string found;
            while (std::fgets(buf, sizeof(buf), which)) found += buf;
            pclose(which);
            auto nl = found.find('\n');
            if (nl != std::string::npos) found = found.substr(0, nl);
            if (!found.empty()) bin = found;
        }
        // single-quote escape for safe shell passing of the file path
        auto sq = [](const std::string& s) {
            std::string o = "'";
            for (char c : s) {
                if (c == '\'') o += "'\\''";
                else o += c;
            }
            return o + "'";
        };
        std::string cmd = bin + " publish --session " + sq(pane_session)
                          + " --type " + sq(pane_type)
                          + (pane_title.empty() ? std::string() : " --title " + sq(pane_title))
                          + " " + sq(pane_file);
        int rc = std::system(cmd.c_str());
        if (rc != 0) {
            std::cerr << "ERROR bridgepanel publish failed (rc=" << rc << ")\n";
            return 1;
        }
        return 0;
    }
    if (vfolder_sync->parsed()) {
        bs::mesh::bootstrap_identity(home_dir);
        std::string ipc = daemon_simple_ipc("VFOLDER_SYNC " + vfolder_name, 300000, home_dir);
        if (ipc.empty()) { std::cerr << "no daemon running\n"; return 1; }
        std::cout << ipc << "\n";
        return ipc.rfind("ERROR", 0) == 0 ? 1 : 0;
    }
    if (vfolder_list->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        std::cout << "=== Virtual folders ===\n";
        for (auto& v : cfg.vfolders) {
            std::cout << v.name << ": " << v.local_path << " <-> " << v.remote_peer << ":" << v.remote_path
                      << " (" << v.direction << ", every " << v.sync_interval_secs << "s)\n";
        }
        return 0;
    }
    if (vfolder_add->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::MeshConfig::VFolderEntry ve;
        ve.name = vfolder_name; ve.local_path = vfolder_local;
        ve.remote_peer = vfolder_peer; ve.remote_path = vfolder_remote;
        ve.sync_interval_secs = vfolder_interval;
        if (!vfolder_dir.empty()) ve.direction = vfolder_dir;
        cfg.vfolders.push_back(ve);
        if (!bs::mesh::save_config(config_path, cfg)) {
            std::cerr << "failed to write config: " << config_path << "\n";
            return 1;
        }
        std::cout << "added vfolder " << vfolder_name << "\n";
        return 0;
    }
    // Default: daemon mode
    bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
    bs::mesh::bootstrap_identity(home_dir);
#ifdef _WIN32
    if (daemon_flag) {
        FreeConsole();
        FILE* nul = fopen("nul", "w");
        if (nul) { fclose(stdout); _dup2(_fileno(nul), _fileno(stdout)); fclose(nul); }
    }
#else
    if (daemon_flag) {
        pid_t pid = fork();
        if (pid < 0) { std::cerr << "fork failed\n"; return 1; }
        if (pid > 0) { std::cout << pid << std::endl; return 0; }
        setsid();
        freopen("/dev/null", "r", stdin);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
    }
#endif
    bs::mesh::MeshController mc(cfg, home_dir);
    mc.run();
    return 0;
}

#endif
