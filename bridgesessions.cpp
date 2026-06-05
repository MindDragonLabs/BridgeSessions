// bridgesessions.cpp — Mesh peer-to-peer terminal sharing
// Single-file architecture: all protocol, TLS, session, and mesh logic in one file.
// Namespace: bs::mesh

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <cstring>
#include <algorithm>
#include <span>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <functional>
#include <zstd.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/bio.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

// ────────────────────────────────────────────────────────────────────
// 1. MESSAGE TYPES (ported from bs-protocol, namespace bs::mesh)
// ────────────────────────────────────────────────────────────────────

namespace bs::mesh {

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
    return auth->contains(raw) ? 1 : 0;
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
    return (*cb)(fp) ? 1 : 0;
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

} // namespace bs::mesh

// ────────────────────────────────────────────────────────────────────
// 2. MAIN (guarded for test builds)
// ────────────────────────────────────────────────────────────────────

#ifndef BS_TESTING
#include <cstdio>
int main() { return 0; }
#endif
