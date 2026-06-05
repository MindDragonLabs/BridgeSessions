// bridgesessions.cpp — Mesh peer-to-peer terminal sharing
// Single-file architecture: all protocol, TLS, session, and mesh logic in one file.
// Namespace: bs::mesh

#define NOMINMAX
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
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
#include <stdexcept>
#include <functional>
#ifdef _WIN32
#include <windows.h>
#endif
#include <zstd.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <filesystem>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#endif
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

// ────────────────────────────────────────────────────────────────────
// 1. MESSAGE TYPES (ported from bs-protocol, namespace bs::mesh)
// ────────────────────────────────────────────────────────────────────

namespace bs::mesh {

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
};

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
    uint16_t cols = 80;
    uint16_t rows = 24;
    std::string term = "xterm-256color";
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
    enum class SignalType : uint8_t { CtrlC = 0, CtrlZ = 1, CtrlBackslash = 2 };
    SignalType signal = SignalType::CtrlC;
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
    std::string name;        // "shadow", "linux-a", etc.
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
    GossipMsg           // 20 — NEW
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
    FLAG_COMPRESSED = 0x01,
    FLAG_CONTROL    = 0x02,
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
    MessageType::ProcExited,    // 13  (was 0x0E cast)
    MessageType::ScrollbackAck, // 14
    MessageType::SessionDied,   // 15
    MessageType::ImageData,     // 16
    MessageType::ImageFrame,    // 17
    MessageType::ImageAck,      // 18
    MessageType::Hello,         // 19 — NEW
    MessageType::Gossip,        // 20 — NEW
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
void serialize_msg(Serializer& s, const OutputMsg&       m) { s.str(m.data); }
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
void serialize_msg(Serializer& s, const AttachMsg&       m) { s.u16(m.cols); s.u16(m.rows); s.str_prefixed(m.term); s.str_prefixed(m.session_name); }
void serialize_msg(Serializer& s, const DetachMsg&)        {}
void serialize_msg(Serializer& s, const PingMsg&)          {}
void serialize_msg(Serializer& s, const PongMsg&)          {}
void serialize_msg(Serializer& s, const ScrollbackAckMsg&) {}
void serialize_msg(Serializer& s, const SessionListMsg&  m) { for (auto& si : m.sessions) { s.str_prefixed(si.name); s.str_prefixed(si.state); s.u32be(si.uptime_seconds); } }
void serialize_msg(Serializer& s, const ServerInfoMsg&   m) { s.str(m.hostname); s.u8('\n'); s.str(m.version); s.u8('\n'); s.bytes(reinterpret_cast<const uint8_t*>(&m.load), 8); }
void serialize_msg(Serializer& s, const ScrollbackMsg&   m) { s.u32be(m.total_lines); s.u32be(m.chunk_index); s.str(m.data); }
void serialize_msg(Serializer& s, const SignalMsg&       m) { s.u8(static_cast<uint8_t>(m.signal)); }
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

    [[nodiscard]] bool ok(size_t need) const { return p + need <= end; }

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
    case 0x02: { OutputMsg    m; m.data = d.str_size(payload.size()); return m; }
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
    case 0x06: { AttachMsg m; m.cols = d.u16(); m.rows = d.u16(); m.term = d.str_prefixed(); m.session_name = d.str_prefixed(); return m; }
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
        auto nl1 = std::find(d.p, d.end, static_cast<uint8_t>('\n'));
        if (nl1 == d.end) throw std::runtime_error("ServerInfo truncated");
        m.hostname = d.str_size(static_cast<size_t>(nl1 - d.p));
        d.u8();
        auto nl2 = std::find(d.p, d.end, static_cast<uint8_t>('\n'));
        if (nl2 == d.end) throw std::runtime_error("ServerInfo truncated");
        m.version = d.str_size(static_cast<size_t>(nl2 - d.p));
        d.u8();
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
    case 0x0D: { SignalMsg m; m.signal = static_cast<SignalMsg::SignalType>(d.u8()); return m; }
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
    std::vector<uint8_t> raw;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        unsigned int byte = 0;
        std::stringstream ss;
        ss << std::hex << hex.substr(i, 2);
        ss >> byte;
        raw.push_back(static_cast<uint8_t>(byte));
    }
    return raw;
}

// ── AuthorizedKeys ────────────────────────────────────────────

struct AuthorizedKeys {
    std::vector<std::vector<uint8_t>> keys;

    void load_from_file(const std::string& path) {
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

    bool contains(const std::vector<uint8_t>& key) const {
        for (auto& k : keys) if (k == key) return true;
        return false;
    }
};

// ── Custom cert verify callbacks ──────────────────────────────

// Server: verifies client's ed25519 raw public key against authorized_keys
int server_cert_verify_cb(X509_STORE_CTX* ctx, void* arg) {
    auto* auth = static_cast<AuthorizedKeys*>(arg);
    X509* cert = X509_STORE_CTX_get0_cert(ctx);
    if (!cert) return 0;
    EVP_PKEY* pk = X509_get0_pubkey(cert);
    if (!pk) return 0;
    auto raw = extract_raw_pubkey(pk);
    if (raw.empty()) return 0;
    if (auth->contains(raw)) {
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
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
        X509_STORE_CTX_set_error(ctx, X509_V_OK);
        return 1;
    }
    return 0;
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

// ── Public: bootstrap_identity ───────────────────────────────────
// Auto-generate ed25519 keypair on first run into ~/.bridgesessions/

void bootstrap_identity(const std::string& home_dir) {
    namespace fs = std::filesystem;
    fs::path dir(home_dir);

    // If the standard identity already exists, nothing to do
    fs::path id_key   = dir / "id_ed25519.pem";
    fs::path id_cert  = dir / "id_ed25519-cert.pem";
    fs::path id_pub   = dir / "id_ed25519.pub";

    if (fs::exists(id_key)) return;

    // Migration: if legacy _bs_autocert.pem + _bs_autokey.pem exist, copy them
    fs::path legacy_cert = dir / "_bs_autocert.pem";
    fs::path legacy_key  = dir / "_bs_autokey.pem";

    if (fs::exists(legacy_cert) && fs::exists(legacy_key)) {
        fs::copy_file(legacy_cert, id_cert, fs::copy_options::overwrite_existing);
        fs::copy_file(legacy_key, id_key, fs::copy_options::overwrite_existing);

        // Also generate the .pub file from the migrated key
        std::ifstream kf(id_key);
        if (kf.is_open()) {
            std::stringstream buf;
            buf << kf.rdbuf();
            kf.close();
            std::string hex = pubkey_hex_from_pem(buf.str());
            if (!hex.empty()) {
                std::ofstream pf(id_pub);
                pf << hex;
                pf.close();
            }
        }
        return;
    }

    // Fresh bootstrap: generate keypair
    fs::create_directories(dir);

    auto [cert_pem, key_pem] = generate_cert_key_pair("bridgesessions");
    std::string pubkey_hex = pubkey_hex_from_pem(key_pem);

    // Write key
    {
        std::ofstream f(id_key);
        if (!f) throw std::runtime_error("cannot write " + id_key.string());
        f << key_pem;
        f.close();
    }

    // Write cert
    {
        std::ofstream f(id_cert);
        if (!f) throw std::runtime_error("cannot write " + id_cert.string());
        f << cert_pem;
        f.close();
    }

    // Write pubkey
    {
        std::ofstream f(id_pub);
        if (!f) throw std::runtime_error("cannot write " + id_pub.string());
        f << pubkey_hex;
        f.close();
    }

    // Set file permissions: owner read+write only (0600 equivalent)
#ifdef _WIN32
    ::_chmod(id_key.string().c_str(), _S_IREAD | _S_IWRITE);
    ::_chmod(id_cert.string().c_str(), _S_IREAD | _S_IWRITE);
    ::_chmod(id_pub.string().c_str(), _S_IREAD | _S_IWRITE);
#else
    ::chmod(id_key.string().c_str(), S_IRUSR | S_IWUSR);
    ::chmod(id_cert.string().c_str(), S_IRUSR | S_IWUSR);
    ::chmod(id_pub.string().c_str(), S_IRUSR | S_IWUSR);
#endif
}

// ── Public: unified create_node_tls ──────────────────────────────

SslCtxPtr create_node_tls(const NodeTlsConfig& cfg, TlsMode mode) {
    SslCtxPtr ctx;

    if (mode == TlsMode::Listen) {
        ctx = SslCtxPtr(SSL_CTX_new(TLS_server_method()));
        if (!ctx) throw std::runtime_error("TLS_server_method failed");
    } else {
        ctx = SslCtxPtr(SSL_CTX_new(TLS_client_method()));
        if (!ctx) throw std::runtime_error("TLS_client_method failed");
    }

    // Both modes: TLS 1.3 only
    SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION);

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

        auto* auth = new AuthorizedKeys{};
        if (!cfg.authorized_keys_file.empty())
            auth->load_from_file(cfg.authorized_keys_file);
        SSL_CTX_set_cert_verify_callback(ctx.get(), server_cert_verify_cb, auth);

        // TLS session cache — reuse sessions across reconnects
        SSL_CTX_set_session_cache_mode(ctx.get(), SSL_SESS_CACHE_SERVER);
        SSL_CTX_sess_set_cache_size(ctx.get(), 256);
    } else {
        // Client: verify server cert via TOFU
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);

        auto* cb = new std::function<bool(const std::string&)>(cfg.tofu_cb);
        SSL_CTX_set_cert_verify_callback(ctx.get(), client_cert_verify_cb, cb);
    }

    return ctx;
}

// ────────────────────────────────────────────────────────────────────
// 4. FRAME I/O (ssl_check, read_frame, write_frame)
// ────────────────────────────────────────────────────────────────────

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
            int ret = SSL_read_ex(ssl, raw.data() + FRAME_HEADER_SIZE + total, length - total, &n);
            ssl_check(ret, ssl, "SSL_read payload");
            total += n;
        }
    }

    return decode(raw);
}

void write_frame(SSL* ssl, const Message& msg, uint16_t stream_id) {
    auto frame = encode(msg, stream_id);

    size_t total = 0;
    while (total < frame.size()) {
        size_t n = 0;
        int ret = SSL_write_ex(ssl, frame.data() + total, frame.size() - total, &n);
        ssl_check(ret, ssl, "SSL_write");
        total += n;
    }
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
    std::string peer_id;  // pubkey hex of remote peer attached (empty if detached)
    std::string command;
#ifdef _WIN32
    HANDLE master_fd = nullptr;     // ConPTY output read handle (child stdout -> server)
    HANDLE child_pid = nullptr;     // process handle
    HANDLE write_handle = nullptr;  // ConPTY input write handle (server -> child stdin)
    HPCON hpcon = nullptr;          // for ResizePseudoConsole
#else
    int master_fd = -1;
    int child_pid = -1;
#endif
    SessionState state = SessionState::Created;

    RingBuffer<kDefaultRingBufferSize> scrollback;

    std::chrono::steady_clock::time_point created_at;
    std::chrono::steady_clock::time_point last_output_at;
    std::chrono::steady_clock::time_point last_attach_at;

    bool auto_restart = false;
    int restart_failures = 0;
    std::chrono::steady_clock::time_point restart_window_start;

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
#else
    bool is_valid() const { return master_fd >= 0; }
#endif
};

// ── Session implementation ────────────────────────────────────────

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
    , peer_id(std::move(other.peer_id))
    , command(std::move(other.command))
    , master_fd(other.master_fd)
    , child_pid(other.child_pid)
    , state(other.state)
    , scrollback(std::move(other.scrollback))
    , created_at(other.created_at)
    , last_output_at(other.last_output_at)
    , last_attach_at(other.last_attach_at)
    , auto_restart(other.auto_restart)
    , restart_failures(other.restart_failures)
    , restart_window_start(other.restart_window_start)
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
        peer_id = std::move(other.peer_id);
        command = std::move(other.command);
        master_fd = other.master_fd;
        child_pid = other.child_pid;
        state = other.state;
        scrollback = std::move(other.scrollback);
        created_at = other.created_at;
        last_output_at = other.last_output_at;
        last_attach_at = other.last_attach_at;
        auto_restart = other.auto_restart;
        restart_failures = other.restart_failures;
        restart_window_start = other.restart_window_start;
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

[[nodiscard]] std::expected<Session, PtyError> create_session(
    const std::string& name, const std::string& command,
    uint16_t cols, uint16_t rows, const std::string& term)
{
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

    // Per MSDN: CreatePseudoConsole takes ownership of the handles passed
    // to it. After a successful call, the caller MUST close the inbound read
    // and outbound write ends.
    CloseHandle(hPipeInRead);
    CloseHandle(hPipeOutWrite);
    hPipeInRead = nullptr;
    hPipeOutWrite = nullptr;

    // Set up STARTUPINFOEX for the child process
    STARTUPINFOEXA siEx{};
    siEx.StartupInfo.cb = sizeof(siEx);
    siEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    siEx.StartupInfo.hStdInput = nullptr;
    siEx.StartupInfo.hStdOutput = nullptr;
    siEx.StartupInfo.hStdError = nullptr;

    // Add the ConPTY to the process attribute list
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    siEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attrSize));
    if (!siEx.lpAttributeList || !InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attrSize)) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{"InitializeProcThreadAttributeList failed"});
    }
    UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPC, sizeof(HPCON), nullptr, nullptr);

    // Build command line: cmd.exe /c <command>
    std::string cmdline = "cmd.exe /c \"" + command + "\"";

    // Set TERM environment
    SetEnvironmentVariableA("TERM", term.c_str());

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessA(
        nullptr,                    // app name
        const_cast<LPSTR>(cmdline.c_str()),
        nullptr, nullptr,           // process/thread security
        TRUE,                       // inherit handles
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,                    // environment (use parent's)
        nullptr,                    // current directory
        &siEx.StartupInfo,
        &pi);

    HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);

    if (!created) {
        ClosePseudoConsole(hPC);
        CloseHandle(hPipeInWrite);
        CloseHandle(hPipeOutRead);
        return std::unexpected(PtyError{"CreateProcess failed: " + std::to_string(GetLastError())});
    }

    CloseHandle(pi.hThread);

    Session s;
    s.name = name;
    s.command = command;
    s.master_fd = hPipeOutRead;     // read: child stdout -> server
    s.write_handle = hPipeInWrite;  // write: server -> child stdin
    s.child_pid = pi.hProcess;      // process handle
    s.hpcon = hPC;                  // for ResizePseudoConsole
    s.state = SessionState::Running;
    return s;
}

[[nodiscard]] std::expected<void, PtyError> resize_pty(intptr_t handle, uint16_t cols, uint16_t rows) {
    HPCON hPC = reinterpret_cast<HPCON>(handle);
    COORD size = {static_cast<SHORT>(cols), static_cast<SHORT>(rows)};
    if (SUCCEEDED(ResizePseudoConsole(hPC, size)))
        return {};
    return std::unexpected(PtyError{"ResizePseudoConsole failed"});
}

#endif // _WIN32

// ────────────────────────────────────────────────────────────────────
// CONFIG PARSER — key=value config file parser
// ────────────────────────────────────────────────────────────────────

struct PeerEntry {
    std::string name;
    std::string addr;       // "host:port"
    std::string pubkey_hex; // learned via Hello, empty until then
    uint64_t last_seen = 0;
};

struct MeshConfig {
    std::string node_name = "unnamed";
    std::string listen_addr = "0.0.0.0";
    uint16_t listen_port = 19948;
    size_t max_peers = 50;
    int gossip_interval_secs = 30;
    int reconnect_backoff_max_secs = 30;
    int ping_interval_secs = 5;
    int pong_timeout_secs = 30;
    std::vector<PeerEntry> seeds;
    std::vector<PeerEntry> discovered;
    std::string authorized_keys_path = "~/.bridgesessions/authorized_keys";
    std::string persistence_path = "~/.bridgesessions/sessions.json";
    int scrollback_lines = 2000;
    int idle_timeout_hours = 168;
    std::string default_shell;
    std::string terminal = "xterm-256color";

    MeshConfig() {
#ifdef _WIN32
        default_shell = "cmd.exe";
#else
        default_shell = "/bin/bash -l";
#endif
    }
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

    std::ifstream f(resolved);
    if (!f.is_open()) return cfg; // missing file = all defaults

    std::string line;
    while (std::getline(f, line)) {
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
        // ── seed <name> <addr> ───────────────────────────────
        else if (key_str == "seed") {
            // val contains "<name> <addr>"
            std::string_view v2(val);
            size_t sp2 = std::string::npos;
            for (size_t i = 0; i < v2.size(); ++i) {
                if (v2[i] == ' ' || v2[i] == '\t') {
                    sp2 = i;
                    break;
                }
            }
            if (sp2 == std::string::npos) continue; // malformed seed line

            std::string seed_name = std::string(trim(v2.substr(0, sp2)));
            std::string seed_addr = std::string(trim(v2.substr(sp2 + 1)));
            if (seed_name.empty() || seed_addr.empty()) continue;

            // Deduplicate by name: if a seed with this name already exists, update addr
            bool found = false;
            for (auto& s : cfg.seeds) {
                if (s.name == seed_name) {
                    s.addr = seed_addr;
                    found = true;
                    break;
                }
            }
            if (!found) {
                PeerEntry p;
                p.name = std::move(seed_name);
                p.addr = std::move(seed_addr);
                cfg.seeds.push_back(std::move(p));
            }
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
    f << "\n";

    // Seeds
    f << "# ── Bootstrap peers ────────────────────────────────\n";
    for (const auto& s : cfg.seeds) {
        write_peer_line(f, "seed", s);
    }
    f << "\n";

    // Discovered
    f << "# ── Discovered peers (auto-populated) ──────────────\n";
    for (const auto& d : cfg.discovered) {
        write_peer_line(f, "discovered", d);
    }
    f << "\n";

    // Sessions
    f << "# ── Session defaults ───────────────────────────────\n";
    f << "sessions.scrollback_lines " << cfg.scrollback_lines << "\n";
    f << "sessions.idle_timeout_hours " << cfg.idle_timeout_hours << "\n";
    f << "sessions.default_shell " << cfg.default_shell << "\n";
    f << "sessions.terminal " << cfg.terminal << "\n";
    f << "sessions.persistence_path " << cfg.persistence_path << "\n";
    f << "sessions.authorized_keys_path " << cfg.authorized_keys_path << "\n";

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

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << j.dump(2) << '\n';
        f.flush();
        if (!f) {
            std::filesystem::remove(tmp);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return false;
    }
    return true;
}

// Load session list from JSON
inline std::vector<SessionMeta> load_sessions(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    try {
        auto j = nlohmann::json::parse(f);
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

// Thread-safe JSON logger
inline std::shared_ptr<spdlog::logger> get_logger() {
    static auto logger = []() {
        const char* home = getenv("HOME");
#ifdef _WIN32
        if (!home) home = getenv("USERPROFILE");
#endif
        std::string path = home ? std::string(home) + "/.bridgesessions/bs-mesh.log"
                                : "/tmp/bs-mesh.log";
        // Create parent directory if needed
        namespace fs = std::filesystem;
        auto parent = fs::path(path).parent_path();
        std::error_code ec;
        fs::create_directories(parent, ec);

        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            path, 1'048'576, 3);  // 1 MB, 3 rotated files
        file_sink->set_pattern("%v");  // raw JSON lines

        auto l = std::make_shared<spdlog::logger>("bs-mesh", file_sink);
        l->set_level(spdlog::level::info);
        l->flush_on(spdlog::level::info);
        spdlog::register_logger(l);
        return l;
    }();
    return logger;
}

// Log a structured event as a single JSON line
inline void log_event(const std::string& event, const std::string& detail = "") {
    auto* l = get_logger().get();
    // Build JSON line manually to avoid fmt compile-time format string issues
    std::string line = "{\"ts\":\"" + 
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
        "\",\"event\":\"" + event + "\"";
    if (!detail.empty()) {
        line += ",\"detail\":\"" + detail + "\"";
    }
    line += "}";
    l->info(line);
}

// ────────────────────────────────────────────────────────────────────
// 10. SESSION REGISTRY — thread-safe session lifecycle manager
// ────────────────────────────────────────────────────────────────────

class SessionRegistry {
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::string persistence_path_;

    static std::string resolve_command(const std::string& from_client) {
        if (!from_client.empty()) return from_client;
#ifdef _WIN32
        return "cmd.exe";
#else
        return "/bin/bash -l";
#endif
    }

public:
    SessionRegistry() = default;

    void set_persistence_path(const std::string& path) {
        persistence_path_ = path;
    }

    // ── Attach / Create ─────────────────────────────────────────
    // Returns session pointer. Returns nullptr on error.
    Session* attach(const std::string& name, const std::string& cmd,
                    uint16_t cols, uint16_t rows, const std::string& term) {
        std::unique_lock lock(mutex_);

        auto it = sessions_.find(name);
        if (it != sessions_.end()) {
            auto* s = it->second.get();

            if (s->state == SessionState::Running || s->state == SessionState::Detached) {
                // Reattach — resize PTY to match new client
                s->state = SessionState::Attached;
                s->last_attach_at = std::chrono::steady_clock::now();
#ifdef _WIN32
                if (s->hpcon)
                    (void)resize_pty(reinterpret_cast<intptr_t>(s->hpcon), cols, rows);
#endif
                return s;
            }

            if (s->state == SessionState::Attached) {
                // Replace existing attachment
                s->state = SessionState::Attached;
                s->last_attach_at = std::chrono::steady_clock::now();
#ifdef _WIN32
                if (s->hpcon)
                    (void)resize_pty(reinterpret_cast<intptr_t>(s->hpcon), cols, rows);
#endif
                return s;
            }

            // Session is Died/Exited/Killed/Recoverable — cannot attach
            return nullptr;
        }

        // Create new session
        auto resolved_cmd = resolve_command(cmd);
        auto session_result = create_session(name, resolved_cmd, cols, rows, term);
        if (!session_result) return nullptr;

        auto s = std::make_unique<Session>(std::move(*session_result));
        s->command = resolved_cmd;
        s->state = SessionState::Attached;

        auto* ptr = s.get();
        sessions_[name] = std::move(s);
        log_event("session_attach", name);
        return ptr;
    }

    // ── Detach ──────────────────────────────────────────────────
    void detach(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it != sessions_.end() && it->second->state == SessionState::Attached) {
            it->second->state = SessionState::Detached;
            log_event("session_detach", name);
        }
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
            it->second->state = SessionState::Killed;
            sessions_.erase(it);
            log_event("session_kill", name);
        }
    }

    // ── Reap dead children ──────────────────────────────────────
    void reap_dead() {
        std::unique_lock lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            auto* s = it->second.get();
            if (s->state == SessionState::Running || s->state == SessionState::Attached ||
                s->state == SessionState::Detached) {
#ifdef _WIN32
                if (s->child_pid && WaitForSingleObject(s->child_pid, 0) == WAIT_OBJECT_0) {
                    auto prev_state = s->state;
                    s->state = SessionState::Died;
                    CloseHandle(s->child_pid);
                    s->child_pid = nullptr;
#else
                // POSIX would use waitpid here
#endif

                    // Auto-restart logic
                    if (s->auto_restart) {
                        auto now = std::chrono::steady_clock::now();
                        auto window = std::chrono::seconds(60);
                        if (now - s->restart_window_start > window) {
                            s->reset_restart_failures();
                        }
                        if (s->restart_failures < 3) {
                            ++s->restart_failures;
                            // Respawn
                            auto new_session = create_session(
                                s->name, s->command, 80, 24, "xterm-256color");
                            if (new_session) {
                                auto fresh = std::make_unique<Session>(std::move(*new_session));
                                fresh->command = s->command;
                                fresh->auto_restart = true;
                                fresh->state = SessionState::Detached;
                                fresh->restart_failures = s->restart_failures;
                                fresh->restart_window_start = s->restart_window_start;
                                it->second = std::move(fresh);
                                log_event("session_auto_restart", s->name + " attempt=" + std::to_string(s->restart_failures));
                                ++it;
                                continue;
                            }
                        }
                        // Too many failures — mark as Exited
                        s->state = SessionState::Exited;
                        log_event("session_circuit_breaker", s->name + " failures=" + std::to_string(s->restart_failures));
                    }
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

        auto session_result = create_session(name, s->command, cols, rows, term);
        if (!session_result) return nullptr;

        auto fresh = std::make_unique<Session>(std::move(*session_result));
        fresh->command = s->command;
        fresh->state = SessionState::Attached;
        auto* ptr = fresh.get();
        sessions_[name] = std::move(fresh);
        log_event("session_resurrect", name);
        return ptr;
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
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSESOCK close
#else
// windows.h / winsock2.h already included at top
#define CLOSESOCK closesocket
#endif

class MeshController {
public:
    struct Conn {
        std::string peer_name;
        std::string peer_pubkey;
        std::string peer_addr;
        SslPtr ssl;
        SOCKET sock_fd = INVALID_SOCKET;
        bool is_outbound = false;
        std::chrono::steady_clock::time_point last_pong;
        Session* attached_session = nullptr;
        std::string remote_session;
    };

private:
    MeshConfig config_;
    SessionRegistry sessions_;
    SslCtxPtr tls_listen_;
    SslCtxPtr tls_connect_;
    std::string our_pubkey_;
    std::string home_dir_;

    std::vector<Conn> conns_;
    static constexpr size_t kMaxConnections = 64;

    // Backoff state per seed
    struct Backoff {
        int delay_ms = 100;
        int max_ms = 30000;
        int attempt = 0;
    };
    std::unordered_map<std::string, Backoff> backoffs_;

    // Shutdown flag for event loop
    std::atomic<bool> running_{false};

    // Last gossip/ping broadcast times
    std::chrono::steady_clock::time_point last_ping_time_;
    std::chrono::steady_clock::time_point last_gossip_time_;

    // Listen socket
    SOCKET listen_fd_ = INVALID_SOCKET;

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
            if (c.peer_pubkey == pubkey_hex && c.sock_fd != INVALID_SOCKET)
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

    // Find conn index by sock_fd
    int find_conn_index(SOCKET fd) const {
        for (size_t i = 0; i < conns_.size(); ++i) {
            if (conns_[i].sock_fd == fd) return static_cast<int>(i);
        }
        return -1;
    }

    // Remove a connection and clean up
    void remove_conn(size_t index) {
        if (index >= conns_.size()) return;
        auto& c = conns_[index];
        if (c.sock_fd != INVALID_SOCKET) {
            CLOSESOCK(c.sock_fd);
        }
        // Remove backoff for this peer so it can be reconnected
        if (!c.peer_addr.empty()) {
            backoffs_.erase(c.peer_addr);
        }
        conns_.erase(conns_.begin() + static_cast<ptrdiff_t>(index));
    }

    // ── Hello exchange ─────────────────────────────────────────

    // Build a HelloMsg with our info + all known peers
    HelloMsg build_hello() const {
        HelloMsg h;
        h.node_name = config_.node_name;
        h.version = "1.0.0";
        h.pubkey_hex = our_pubkey_;

        // Add seeds as known peers
        for (auto& s : config_.seeds) {
            PeerInfo pi;
            pi.name = s.name;
            pi.addr = s.addr;
            pi.pubkey_hex = s.pubkey_hex;
            pi.last_seen = static_cast<uint64_t>(
                std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            h.known_peers.push_back(std::move(pi));
        }

        // Add discovered peers
        for (auto& d : config_.discovered) {
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

    // Merge peers from Hello or Gossip into discovered
    void merge_peers(const std::vector<PeerInfo>& peers) {
        bool changed = false;
        for (auto& p : peers) {
            // Skip self
            if (p.pubkey_hex == our_pubkey_) continue;
            // Skip already known seeds
            bool is_seed = false;
            for (auto& s : config_.seeds) {
                if (s.name == p.name || (!p.pubkey_hex.empty() && s.pubkey_hex == p.pubkey_hex)) {
                    is_seed = true;
                    // Update pubkey if missing
                    if (s.pubkey_hex.empty() && !p.pubkey_hex.empty()) {
                        s.pubkey_hex = p.pubkey_hex;
                        changed = true;
                    }
                    break;
                }
            }
            if (is_seed) continue;

            // Check if already in discovered
            bool found = false;
            for (auto& d : config_.discovered) {
                if (d.name == p.name || (!p.pubkey_hex.empty() && d.pubkey_hex == p.pubkey_hex)) {
                    found = true;
                    if (!p.addr.empty() && d.addr.empty()) {
                        d.addr = p.addr;
                        changed = true;
                    }
                    if (!p.pubkey_hex.empty() && d.pubkey_hex.empty()) {
                        d.pubkey_hex = p.pubkey_hex;
                        changed = true;
                    }
                    d.last_seen = static_cast<uint64_t>(
                        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                    break;
                }
            }
            if (!found) {
                PeerEntry pe;
                pe.name = p.name;
                pe.addr = p.addr;
                pe.pubkey_hex = p.pubkey_hex;
                pe.last_seen = static_cast<uint64_t>(
                    std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                config_.discovered.push_back(std::move(pe));
                changed = true;
            }
        }
        if (changed) {
            save_config(home_dir_ + "/.bridgesessions/config", config_);
        }
    }

    // Duplicate resolution: if two conns have same pubkey, keep the one
    // where OUR pubkey < PEER pubkey lexicographically
    void resolve_duplicates() {
        for (size_t i = 0; i < conns_.size(); ++i) {
            if (conns_[i].sock_fd == INVALID_SOCKET) continue;
            for (size_t j = i + 1; j < conns_.size(); ++j) {
                if (conns_[j].sock_fd == INVALID_SOCKET) continue;
                if (conns_[i].peer_pubkey == conns_[j].peer_pubkey &&
                    !conns_[i].peer_pubkey.empty()) {
                    // Keep the connection where our_pubkey < peer_pubkey
                    bool keep_i = our_pubkey_ < conns_[i].peer_pubkey;
                    if (keep_i) {
                        remove_conn(j);
                    } else {
                        remove_conn(i);
                    }
                    // Restart the check after removal
                    resolve_duplicates();
                    return;
                }
            }
        }
    }

    // ── Accept new inbound connection ──────────────────────────

    void accept_inbound() {
        sockaddr_in peer_addr{};
        socklen_t addr_len = sizeof(peer_addr);
        SOCKET cfd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer_addr), &addr_len);
        if (cfd == INVALID_SOCKET) return;

        if (conns_.size() >= kMaxConnections) {
            CLOSESOCK(cfd);
            return;
        }

        // TLS handshake (server side)
        auto ssl = SslPtr(SSL_new(tls_listen_.get()));
        if (!ssl) { CLOSESOCK(cfd); return; }
        SSL_set_fd(ssl.get(), static_cast<int>(cfd));

        int ret = SSL_accept(ssl.get());
        if (ret <= 0) { CLOSESOCK(cfd); return; }

        // Get peer's pubkey
        std::string peer_pk = peer_public_key_hex(ssl.get());
        if (peer_pk.empty()) { CLOSESOCK(cfd); return; }

        // Read Hello from peer
        try {
            Message msg = read_frame(ssl.get());
            if (!std::holds_alternative<HelloMsg>(msg)) {
                CLOSESOCK(cfd);
                return;
            }
            auto& hello = std::get<HelloMsg>(msg);

            Conn c;
            c.peer_name = hello.node_name;
            c.peer_pubkey = peer_pk;
            c.peer_addr = std::string(inet_ntoa(peer_addr.sin_addr)) + ":" +
                          std::to_string(ntohs(peer_addr.sin_port));
            c.ssl = std::move(ssl);
            c.sock_fd = cfd;
            c.is_outbound = false;
            c.last_pong = std::chrono::steady_clock::now();

            // Merge known peers from Hello
            merge_peers(hello.known_peers);

            // Send our Hello back
            write_frame(c.ssl.get(), build_hello(), CONTROL_STREAM_ID);

            // Check duplicate resolution
            conns_.push_back(std::move(c));
            resolve_duplicates();

            log_event("mesh_peer_connected", hello.node_name + " pubkey=" + peer_pk.substr(0, 16) + "...");
        } catch (...) {
            CLOSESOCK(cfd);
        }
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

            if (connect(sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
                CLOSESOCK(sfd);
                return false;
            }

            // TLS handshake (client side)
            auto ssl = SslPtr(SSL_new(tls_connect_.get()));
            if (!ssl) { CLOSESOCK(sfd); return false; }
            SSL_set_fd(ssl.get(), static_cast<int>(sfd));

            int ret = SSL_connect(ssl.get());
            if (ret <= 0) { CLOSESOCK(sfd); return false; }

            // Get peer's pubkey
            std::string peer_pk = peer_public_key_hex(ssl.get());
            if (peer_pk.empty()) { CLOSESOCK(sfd); return false; }

            // Send our Hello
            write_frame(ssl.get(), build_hello(), CONTROL_STREAM_ID);

            // Read Hello from peer
            Message msg = read_frame(ssl.get());
            if (!std::holds_alternative<HelloMsg>(msg)) {
                CLOSESOCK(sfd);
                return false;
            }
            auto& hello = std::get<HelloMsg>(msg);

            Conn c;
            c.peer_name = hello.node_name;
            c.peer_pubkey = peer_pk;
            c.peer_addr = addr;
            c.ssl = std::move(ssl);
            c.sock_fd = sfd;
            c.is_outbound = true;
            c.last_pong = std::chrono::steady_clock::now();

            // Merge known peers from Hello
            merge_peers(hello.known_peers);

            conns_.push_back(std::move(c));
            resolve_duplicates();

            // Reset backoff on success
            backoffs_.erase(addr);

            log_event("mesh_peer_connected_outbound", hello.node_name + " addr=" + addr);
            return true;
        } catch (...) {
            return false;
        }
    }

    // ── Build Gossip message ───────────────────────────────────

    GossipMsg build_gossip() const {
        GossipMsg g;
        for (auto& s : config_.seeds) {
            PeerInfo pi;
            pi.name = s.name;
            pi.addr = s.addr;
            pi.pubkey_hex = s.pubkey_hex;
            pi.last_seen = s.last_seen;
            g.peers.push_back(std::move(pi));
        }
        for (auto& d : config_.discovered) {
            PeerInfo pi;
            pi.name = d.name;
            pi.addr = d.addr;
            pi.pubkey_hex = d.pubkey_hex;
            pi.last_seen = d.last_seen;
            g.peers.push_back(std::move(pi));
        }
        return g;
    }

    // ── Dispatch a received message ────────────────────────────

    void dispatch_message(int conn_idx, Message& msg) {
        auto& c = conns_[static_cast<size_t>(conn_idx)];

        if (std::holds_alternative<PingMsg>(msg)) {
            try {
                write_frame(c.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
            } catch (...) {}
        }
        else if (std::holds_alternative<PongMsg>(msg)) {
            c.last_pong = std::chrono::steady_clock::now();
        }
        else if (std::holds_alternative<HelloMsg>(msg)) {
            // Duplicate Hello — update peer info
            auto& h = std::get<HelloMsg>(msg);
            c.peer_name = h.node_name;
            merge_peers(h.known_peers);
        }
        else if (std::holds_alternative<GossipMsg>(msg)) {
            auto& g = std::get<GossipMsg>(msg);
            merge_peers(g.peers);
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
    // ────────────────────────────────────────────────────────────────
    // Phase 6: Session message handlers (public for tests)
    // ────────────────────────────────────────────────────────────────

    // write_all — write all bytes to a HANDLE (Windows) or fd (POSIX)
    // For PTY writes (child stdin). Stripped-down version for ConPTY.
    bool write_all(void* handle, const void* data, size_t len) {
        if (!handle || !data || len == 0) return true;
#ifdef _WIN32
        HANDLE h = reinterpret_cast<HANDLE>(handle);
        const char* p = static_cast<const char*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            DWORD written = 0;
            if (!WriteFile(h, p, static_cast<DWORD>(std::min(remaining, size_t(4096))),
                           &written, nullptr)) {
                return false;
            }
            p += written;
            remaining -= written;
        }
        return true;
#else
        int fd = reinterpret_cast<intptr_t>(handle);
        const char* p = static_cast<const char*>(data);
        size_t remaining = len;
        while (remaining > 0) {
            ssize_t n = write(fd, p, remaining);
            if (n <= 0) return false;
            p += n;
            remaining -= static_cast<size_t>(n);
        }
        return true;
#endif
    }

    // 1. handle_inbound_session — messages from a remote peer
    //    operating on OUR local sessions
    void handle_inbound_session(Conn& conn, Message& msg) {
        // AttachMsg — peer wants to attach to one of our sessions
        if (std::holds_alternative<AttachMsg>(msg)) {
            auto& a = std::get<AttachMsg>(msg);
            log_event("session_attach_request",
                      a.session_name + " from " + conn.peer_name);

            auto* s = sessions_.attach(a.session_name,
                                       config_.default_shell,
                                       a.cols, a.rows, a.term);
            if (s) {
                conn.attached_session = s;
                s->peer_id = conn.peer_pubkey;

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
#ifdef _WIN32
                write_all(conn.attached_session->write_handle,
                          ks.data.data(), ks.data.size());
#else
                write_all(reinterpret_cast<void*>(
                    static_cast<intptr_t>(conn.attached_session->master_fd)),
                    ks.data.data(), ks.data.size());
#endif
            }
            return;
        }

        // ResizeMsg — peer resized their terminal
        if (std::holds_alternative<ResizeMsg>(msg)) {
            auto& r = std::get<ResizeMsg>(msg);
            if (conn.attached_session && conn.attached_session->is_valid()) {
#ifdef _WIN32
                if (conn.attached_session->hpcon) {
                    resize_pty(reinterpret_cast<intptr_t>(conn.attached_session->hpcon),
                               r.cols, r.rows);
                }
#else
                // POSIX would use TIOCSWINSZ
#endif
            }
            return;
        }

        // DetachMsg — peer wants to detach from session
        if (std::holds_alternative<DetachMsg>(msg)) {
            if (conn.attached_session) {
                sessions_.detach(conn.attached_session->name);
                conn.attached_session = nullptr;
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
                    int sig = (sig.signal == SignalMsg::SignalType::CtrlC) ? SIGINT :
                              (sig.signal == SignalMsg::SignalType::CtrlZ) ? SIGTSTP :
                              SIGQUIT;
                    kill(conn.attached_session->child_pid, sig);
                }
#endif
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
#ifdef _WIN32
                write_all(conn.attached_session->write_handle,
                          paste.data(), paste.size());
#else
                write_all(reinterpret_cast<void*>(
                    static_cast<intptr_t>(conn.attached_session->master_fd)),
                    paste.data(), paste.size());
#endif
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
        for (auto& conn : conns_) {
            auto* s = conn.attached_session;
            if (!s || !s->is_valid()) continue;

#ifdef _WIN32
            // PeekNamedPipe to check for available data
            DWORD bytes_avail = 0;
            if (!PeekNamedPipe(s->master_fd, nullptr, 0, nullptr, &bytes_avail, nullptr))
                continue;
            if (bytes_avail == 0) continue;

            // Read available data
            std::string buf;
            buf.resize(bytes_avail);
            DWORD bytes_read = 0;
            if (!ReadFile(s->master_fd, &buf[0], bytes_avail, &bytes_read, nullptr))
                continue;
            buf.resize(bytes_read);
#else
            // POSIX: non-blocking read or poll
            char tmp[4096];
            ssize_t n = read(s->master_fd, tmp, sizeof(tmp));
            if (n <= 0) continue;
            std::string buf(tmp, static_cast<size_t>(n));
#endif

            if (buf.empty()) continue;

            // Write to ring buffer
            s->scrollback.write(std::string_view(buf));
            s->touch_output();

            // OSC 52 scan
            auto osc = scan_osc52(buf);
            if (osc.clipboard_text && !osc.clipboard_text->empty()) {
                ClipboardMsg cb;
                cb.text = *osc.clipboard_text;
                cb.hash = sha256_hex(cb.text);
                try {
                    write_frame(conn.ssl.get(), cb, 0);
                } catch (...) {}
            }

            // Send OutputMsg with cleaned text
            if (!osc.cleaned_text.empty()) {
                OutputMsg om;
                om.data = std::move(osc.cleaned_text);
                try {
                    write_frame(conn.ssl.get(), om, 0);
                } catch (...) {}
            }

            // Check child exit
#ifdef _WIN32
            if (s->child_pid &&
                WaitForSingleObject(s->child_pid, 0) == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                GetExitCodeProcess(s->child_pid, &exit_code);
                CloseHandle(s->child_pid);
                s->child_pid = nullptr;
                s->state = SessionState::Died;

                SessionDiedMsg sdm;
                sdm.exit_code = static_cast<int32_t>(exit_code);
                sdm.signal_num = 0;
                try {
                    write_frame(conn.ssl.get(), sdm, 0);
                } catch (...) {}
                log_event("session_died", s->name + " exit_code=" + std::to_string(exit_code));
            }
#else
            if (s->child_pid > 0) {
                int status = 0;
                pid_t result = waitpid(s->child_pid, &status, WNOHANG);
                if (result == s->child_pid) {
                    s->child_pid = -1;
                    s->state = SessionState::Died;
                    SessionDiedMsg sdm;
                    if (WIFEXITED(status)) {
                        sdm.exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        sdm.signal_num = WTERMSIG(status);
                    }
                    try {
                        write_frame(conn.ssl.get(), sdm, 0);
                    } catch (...) {}
                }
            }
#endif
        }
    }

private:

    void check_conn_read(int conn_idx) {
        auto& c = conns_[static_cast<size_t>(conn_idx)];
        try {
            Message msg = read_frame(c.ssl.get());
            dispatch_message(conn_idx, msg);
        } catch (...) {
            // Read failure — mark for removal
            c.sock_fd = INVALID_SOCKET;
        }
    }

    // ── Try to connect to seeds/discovered ─────────────────────

    void try_connect_to_seeds() {
        auto now = std::chrono::steady_clock::now();

        // Try seeds first
        for (auto& s : config_.seeds) {
            if (has_conn_for_addr(s.addr)) continue;
            if (conns_.size() >= config_.max_peers) break;

            auto& bo = backoffs_[s.addr];
            if (bo.attempt > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_ping_time_);
                // Check if we've waited enough
                // Simple: try once per loop iteration with backoff
            }

            // Attempt connect
            bool ok = connect_to_peer_impl(s.addr);
            if (ok) {
                bo.attempt = 0;
                bo.delay_ms = 100;
            } else {
                bo.attempt++;
                bo.delay_ms = std::min(bo.delay_ms * 2, bo.max_ms);
            }
        }

        // Try discovered peers too (if we have room)
        if (conns_.size() < config_.max_peers) {
            for (auto& d : config_.discovered) {
                if (d.addr.empty()) continue;
                if (has_conn_for_addr(d.addr)) continue;
                if (conns_.size() >= config_.max_peers) break;

                auto& bo = backoffs_[d.addr];
                bool ok = connect_to_peer_impl(d.addr);
                if (ok) {
                    bo.attempt = 0;
                    bo.delay_ms = 100;
                } else {
                    bo.attempt++;
                    bo.delay_ms = std::min(bo.delay_ms * 2, bo.max_ms);
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
            try {
                write_frame(c.ssl.get(), g, CONTROL_STREAM_ID);
            } catch (...) {}
        }
    }

    // ── Send Ping to all connections ───────────────────────────

    void broadcast_ping() {
        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            try {
                write_frame(c.ssl.get(), PingMsg{}, CONTROL_STREAM_ID);
            } catch (...) {
                c.sock_fd = INVALID_SOCKET;
            }
        }
    }

    // ── Check pong timeout ─────────────────────────────────────

    void check_pong_timeouts() {
        auto now = std::chrono::steady_clock::now();
        auto timeout = std::chrono::seconds(config_.pong_timeout_secs);

        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            if (now - c.last_pong > timeout) {
                log_event("mesh_pong_timeout", c.peer_name + " " + c.peer_addr);
                CLOSESOCK(c.sock_fd);
                c.sock_fd = INVALID_SOCKET;
            }
        }
    }

    // ── Clean up dead connections ──────────────────────────────

    void clean_dead_conns() {
        conns_.erase(
            std::remove_if(conns_.begin(), conns_.end(),
                [](const Conn& c) { return c.sock_fd == INVALID_SOCKET; }),
            conns_.end());
    }

    // ── Backoff sleep for seeds trying to reconnect ────────────

    void sleep_backoffs() {
        // Sleep for the minimum backoff among pending seeds
        int min_sleep = 1000; // default 1 second
        auto now = std::chrono::steady_clock::now();

        for (auto& [addr, bo] : backoffs_) {
            if (bo.attempt > 0 && bo.delay_ms < min_sleep) {
                min_sleep = bo.delay_ms;
            }
        }

        // Actually sleep via select timeout in run() loop
        // This method just returns the suggested sleep
    }

public:
    // ── Constructor ───────────────────────────────────────────

    MeshController(const MeshConfig& cfg)
        : config_(cfg)
    {
        // Determine home directory
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (home) home_dir_ = home;

        // Bootstrap identity if needed
        std::string bs_dir = home_dir_ + "/.bridgesessions";
        bootstrap_identity(home_dir_);

        // Read our pubkey
        std::string pub_path = bs_dir + "/id_ed25519.pub";
        std::ifstream pf(pub_path);
        if (pf.is_open()) {
            std::getline(pf, our_pubkey_);
            pf.close();
        }

        // Read key and cert for TLS
        std::string key_path = bs_dir + "/id_ed25519.pem";
        std::string cert_path = bs_dir + "/id_ed25519-cert.pem";

        // Create listen TLS context
        NodeTlsConfig listen_cfg;
        listen_cfg.cert_file = cert_path;
        listen_cfg.key_file = key_path;
        listen_cfg.authorized_keys_file = expand_home(config_.authorized_keys_path);
        tls_listen_ = create_node_tls(listen_cfg, TlsMode::Listen);

        // Create connect TLS context with TOFU callback
        NodeTlsConfig connect_cfg;
        connect_cfg.cert_file = cert_path;
        connect_cfg.key_file = key_path;
        connect_cfg.tofu_cb = [](const std::string&) { return true; }; // Accept all for now
        tls_connect_ = create_node_tls(connect_cfg, TlsMode::Connect);

        // Set persistence path for sessions
        sessions_.set_persistence_path(expand_home(config_.persistence_path));
    }

    // ── Destructor ────────────────────────────────────────────

    ~MeshController() {
        running_ = false;
        for (auto& c : conns_) {
            if (c.sock_fd != INVALID_SOCKET) {
                CLOSESOCK(c.sock_fd);
            }
        }
        conns_.clear();
        if (listen_fd_ != INVALID_SOCKET) {
            CLOSESOCK(listen_fd_);
            listen_fd_ = INVALID_SOCKET;
        }
    }

    // ── Main event loop ───────────────────────────────────────

    void run() {
        running_ = true;

        // Create listen socket
        listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ == INVALID_SOCKET) {
            log_event("mesh_listen_socket_failed");
            return;
        }

        int opt = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = inet_addr(config_.listen_addr.c_str());
        if (sa.sin_addr.s_addr == INADDR_NONE) {
            sa.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        sa.sin_port = htons(config_.listen_port);

        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
            log_event("mesh_listen_bind_failed");
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

        log_event("mesh_listening", config_.listen_addr + ":" + std::to_string(config_.listen_port));

        last_ping_time_ = std::chrono::steady_clock::now();
        last_gossip_time_ = std::chrono::steady_clock::now();

        while (running_) {
            // 1. Build fd_set for select()
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(listen_fd_, &read_fds);

            SOCKET max_fd = listen_fd_;
            for (auto& c : conns_) {
                if (c.sock_fd != INVALID_SOCKET) {
                    FD_SET(c.sock_fd, &read_fds);
                    if (c.sock_fd > max_fd) max_fd = c.sock_fd;
                }
            }

            // 2. select() with 1 second timeout
            timeval tv{1, 0}; // 1 second
            int nfds = select(static_cast<int>(max_fd) + 1, &read_fds, nullptr, nullptr, &tv);

            if (nfds < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
#endif
                break;
            }

            auto now = std::chrono::steady_clock::now();

            // 3. Accept new connections
            if (nfds > 0 && FD_ISSET(listen_fd_, &read_fds)) {
                accept_inbound();
                --nfds;
            }

            // 4. Read from established connections
            for (int i = 0; i < static_cast<int>(conns_.size()) && nfds > 0; ++i) {
                if (conns_[static_cast<size_t>(i)].sock_fd != INVALID_SOCKET &&
                    FD_ISSET(conns_[static_cast<size_t>(i)].sock_fd, &read_fds)) {
                    check_conn_read(i);
                    --nfds;
                }
            }

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

            // 8. Gossip broadcast
            auto gossip_interval = std::chrono::seconds(config_.gossip_interval_secs);
            if (now - last_gossip_time_ >= gossip_interval) {
                broadcast_gossip();
                last_gossip_time_ = now;
            }

            // 9. Clean dead connections
            clean_dead_conns();

            // 9.5. Poll PTY output for all attached sessions
            pty_output_poller();

            // 10. Reap dead sessions
            sessions_.reap_dead();
        }
    }

    // ── Shutdown ───────────────────────────────────────────────

    void shutdown() {
        running_ = false;
    }

    // ── Stubs for Phase 6 ──────────────────────────────────────

    void shell_peer(const std::string& peer_name, const std::string& session_name,
                    const std::string& cmd, uint16_t cols, uint16_t rows, const std::string& term) {
        (void)peer_name; (void)session_name; (void)cmd; (void)cols; (void)rows; (void)term;
        // Phase 6: send AttachMsg to peer
    }

    void list_sessions(const std::string& peer_name, bool all) {
        (void)peer_name; (void)all;
        // Phase 6: query sessions from peer or list local
    }

    bool health_check(const std::string& peer_name) {
        (void)peer_name;
        // Phase 6: ping specific peer
        return false;
    }

    // ── Accessors (for tests) ──────────────────────────────────

    SessionRegistry& sessions() { return sessions_; }
    const std::vector<Conn>& conns() const { return conns_; }
    size_t conn_count() const { return conns_.size(); }

    // ── Public connect (for tests/CLI) ──────────────────────────
    bool connect_to_peer(const std::string& addr) {
        return connect_to_peer_impl(addr);
    }
};

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
int cmd_keygen() {
    std::string home = resolve_home("~");
    if (home.empty()) { std::cerr << "HOME/USERPROFILE not set\n"; return 1; }

    std::string dir = home + "/.bridgesessions";
    std::filesystem::create_directories(dir);

    auto [cert, key] = bs::mesh::generate_cert_key_pair("bridgesessions");
    auto pubkey = bs::mesh::pubkey_hex_from_pem(key);

    std::string key_path  = dir + "/id_ed25519.pem";
    std::string cert_path = dir + "/id_ed25519-cert.pem";
    std::string pub_path  = dir + "/id_ed25519.pub";

    // Write key
    {
        std::ofstream f(key_path);
        f << key;
    }
    // Write cert
    {
        std::ofstream f(cert_path);
        f << cert;
    }
    // Write public key
    {
        std::ofstream f(pub_path);
        f << pubkey << "\n";
    }

    std::cout << "Generated ed25519 keypair:\n"
              << "  Private key: " << key_path << "\n"
              << "  Certificate: " << cert_path << "\n"
              << "  Public key:  " << pub_path << "\n"
              << "  Pubkey hex:  " << pubkey << "\n";

    return 0;
}

// ── authorize: register a hex-encoded ed25519 public key ──────────
int cmd_authorize(const char* hex_pubkey) {
    if (!hex_pubkey || !*hex_pubkey) {
        std::cerr << "usage: bridgesessions authorize <hex-pubkey>\n";
        return 1;
    }

    std::string home = resolve_home("~");
    if (home.empty()) { std::cerr << "HOME/USERPROFILE not set\n"; return 1; }

    std::string dir = home + "/.bridgesessions";
    std::filesystem::create_directories(dir);
    std::string path = dir + "/authorized_keys";

    // Check for duplicates
    {
        std::ifstream existing(path);
        std::string line;
        while (std::getline(existing, line)) {
            if (line == hex_pubkey) {
                std::cout << "Key already authorized: " << hex_pubkey << "\n";
                return 0;
            }
        }
    }

    // Append
    {
        std::ofstream f(path, std::ios::app);
        f << hex_pubkey << "\n";
    }

    std::cout << "Authorized key: " << hex_pubkey << "\n";
    std::cout << "Written to: " << path << "\n";
    return 0;
}

} // anonymous namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    CLI::App app{"bridgesessions — mesh terminal relay"};
    app.set_version_flag("--version,-V", "1.0.0-mesh");

    // Global options
    std::string config_path = "";
    app.add_option("--config", config_path, "Config file path");

    // Subcommand: shell
    std::string shell_peer, shell_session = "default", shell_cmd;
    uint16_t shell_cols = 80, shell_rows = 24;
    auto* shell_cmd_app = app.add_subcommand("shell", "Open shell on a peer");
    shell_cmd_app->add_option("peer", shell_peer, "Peer name")->required();
    shell_cmd_app->add_option("-n,--name", shell_session, "Session name");
    shell_cmd_app->add_option("-x,--cmd", shell_cmd, "Command override");
    shell_cmd_app->add_option("--cols", shell_cols, "Terminal columns");
    shell_cmd_app->add_option("--rows", shell_rows, "Terminal rows");

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

    CLI11_PARSE(app, argc, argv);

    // Resolve config path
    if (config_path.empty()) {
        config_path = resolve_home("~/.bridgesessions/config");
    }

    // Dispatch
    if (shell_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::bootstrap_identity(resolve_home("~/.bridgesessions"));
        bs::mesh::MeshController mc(cfg);
        mc.shell_peer(shell_peer, shell_session, shell_cmd, shell_cols, shell_rows, "xterm-256color");
        return 0;
    }
    if (sessions_cmd_app->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        bs::mesh::MeshController mc(cfg);
        mc.list_sessions(sessions_peer, sessions_all);
        return 0;
    }
    if (keygen_cmd_app->parsed()) {
        return cmd_keygen();
    }
    if (auth_cmd_app->parsed()) {
        return cmd_authorize(auth_pubkey.c_str());
    }
    if (peers_list->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        for (auto& p : cfg.seeds) std::cout << p.name << " " << p.addr << std::endl;
        for (auto& p : cfg.discovered) std::cout << "[d] " << p.name << " " << p.addr << std::endl;
        return 0;
    }
    if (peers_add->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        cfg.seeds.push_back({peer_add_name, peer_add_addr});
        bs::mesh::save_config(config_path, cfg);
        std::cout << "added seed " << peer_add_name << " -> " << peer_add_addr << std::endl;
        return 0;
    }
    if (peers_remove->parsed()) {
        bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
        cfg.seeds.erase(std::remove_if(cfg.seeds.begin(), cfg.seeds.end(),
            [&](auto& p){ return p.name == peer_remove_name; }), cfg.seeds.end());
        bs::mesh::save_config(config_path, cfg);
        std::cout << "removed seed " << peer_remove_name << std::endl;
        return 0;
    }

    // Default: daemon mode
    bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
    bs::mesh::bootstrap_identity(resolve_home("~/.bridgesessions"));
    bs::mesh::MeshController mc(cfg);
    mc.run();
    return 0;
}

#endif
