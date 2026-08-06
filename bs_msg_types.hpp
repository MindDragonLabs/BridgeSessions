// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs_msg_types.hpp — Message types, serialization, codec (extracted from bs-protocol.h)
// DO NOT include directly — include bs-protocol.h instead.
#pragma once

namespace bs::mesh {

#include "bs-session.h"
#ifndef BS_VERSION
#define BS_VERSION "0.0.0-dev"
#endif
inline constexpr std::string_view kBridgeSessionsVersion = BS_VERSION;


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
    // ── 2.0.8-alpha3 wire types (0x20 reserved) ──
    AttachAck      = 0x21,  // server → client: {attach_id, session_name, cols, rows}
    OutputGap      = 0x22,  // server → client: {dropped_bytes} per-conn queue overrun
    ConversationAppend = 0x23, // both: {conv_id, seq, ts, agent_id, role, body}
    ConversationQuery   = 0x24, // client → server: {conv_id, since_seq}
    ConversationBatch   = 0x25, // server → client: ordered message run
    CuaRequest     = 0x26,  // client → server: computer-use action (HID usage IDs on wire)
    CuaResponse    = 0x27,  // server → client: {status, error, screen_w/h, format}
    JoinRequest    = 0x28,  // client → server: {token}
    JoinReply      = 0x29,  // server → client: {ok, node_name, seeds_csv, host_pubkey, error}
    CuaVideoCapture       = 0x2A,  // client → server: {fps, duration, quality, max_width}
    CuaVideoCaptureResult = 0x2B,  // server → client: {status, file_path, duration, width, height, format}
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
    uint32_t client_instance_id = 0; // per-connection instance id, distinct from client pubkey (2.0.8 multi-attach)
    bool spectator = false; // 2.0.8: read-only attach (receives Output; Keystroke/CuaRequest rejected)
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
    std::string sessions_summary_json; // 2.0.8: trailing, optional. JSON array of
                                        // {name,state,command,bytes} for this node's
                                        // sessions. Capped (~4 KiB); empty = no data.
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

// ── 2.0.8-alpha3 Message Structs ────────────────────────────────

struct AttachAckMsg {
    uint32_t attach_id = 0;     // server-assigned canonical attach id
    std::string session_name;
    uint16_t cols = 80;
    uint16_t rows = 24;
    bool operator==(const AttachAckMsg&) const = default;
};

struct OutputGapMsg {
    uint64_t dropped_bytes = 0; // emitted when a per-connection output queue overruns
    bool operator==(const OutputGapMsg&) const = default;
};

struct ConversationAppendMsg {
    std::string conv_id;
    uint64_t seq = 0;           // assigned by store
    uint64_t ts = 0;            // unix ms
    std::string agent_id;       // pubkey hex, or "human"
    uint8_t role = 0;           // 0=system 1=user 2=agent 3=tool
    std::string body;
    bool operator==(const ConversationAppendMsg&) const = default;
};

struct ConversationQueryMsg {
    std::string conv_id;
    uint64_t since_seq = 0;
    bool operator==(const ConversationQueryMsg&) const = default;
};

struct ConversationBatchMsg {
    std::string conv_id;
    std::vector<ConversationAppendMsg> messages;
    bool operator==(const ConversationBatchMsg&) const = default;
};

// CUA action enum (mirrors wire action byte)
struct CuaRequestMsg {
    uint32_t request_id = 0;
    uint8_t action = 0;         // 0=screen_info 1=key 2=text 3=mouse_move 4=mouse_button 5=wheel 6=capture
    int16_t x = 0;
    int16_t y = 0;
    uint8_t button = 0;
    uint32_t hid_key = 0;       // USB HID usage ID (never platform keycode)
    uint8_t modifiers = 0;      // bitmask: 1=ctrl 2=shift 4=alt 8=meta
    std::string text;
    bool operator==(const CuaRequestMsg&) const = default;
};

struct CuaResponseMsg {
    uint32_t request_id = 0;
    uint8_t status = 0;         // 0=ok 1=error
    std::string error;
    uint32_t screen_w = 0;
    uint32_t screen_h = 0;
    uint8_t format = 0;         // 0=none 1=png 2=jpeg (capture result format)
    std::vector<uint8_t> data;  // capture payload (empty for non-capture responses)
    bool operator==(const CuaResponseMsg&) const = default;
};

// v2.0.12: Video capture — remote ffmpeg, file transfer, local vision analysis
struct CuaVideoCaptureMsg {
    uint32_t request_id = 0;
    uint8_t  fps = 2;           // frames per second
    uint16_t duration_sec = 15; // capture duration
    uint8_t  quality = 70;      // JPEG/MP4 quality (1-100)
    uint16_t max_width = 1280;  // downscale width (0 = native)
    bool operator==(const CuaVideoCaptureMsg&) const = default;
};

struct CuaVideoCaptureResultMsg {
    uint32_t request_id = 0;
    uint8_t  status = 0;        // 0=ok 1=error
    std::string error;
    std::string file_path;      // temp file on remote (for file_recv)
    uint16_t duration_sec = 0;  // actual captured duration
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t  format = 0;        // 1=mp4 2=gif
    bool operator==(const CuaVideoCaptureResultMsg&) const = default;
};

struct JoinRequestMsg {
    std::string token;
    bool operator==(const JoinRequestMsg&) const = default;
};

struct JoinReplyMsg {
    bool ok = false;
    std::string node_name;
    std::string seeds_csv;
    std::string host_pubkey;
    std::string host_addr;
    std::string peer_pubkeys_json; // 2.0.20: [{name,addr,pubkey_hex}] for all configured seeds
    std::string error;
    bool operator==(const JoinReplyMsg&) const = default;
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
    FileRequestMsg,     // 29 — P1 file transfer
    AttachAckMsg,       // 30 — 2.0.8-alpha3
    OutputGapMsg,       // 31 — 2.0.8-alpha3
    ConversationAppendMsg, // 32 — 2.0.8-alpha3
    ConversationQueryMsg,  // 33 — 2.0.8-alpha3
    ConversationBatchMsg,  // 34 — 2.0.8-alpha3
    CuaRequestMsg,     // 35 — 2.0.8-alpha3
    CuaResponseMsg,    // 36 — 2.0.8-alpha3
    JoinRequestMsg,    // 37 — 2.0.9-alpha5
    JoinReplyMsg,      // 38 — 2.0.9-alpha5
    CuaVideoCaptureMsg,       // 39 — 2.0.12-alpha5
    CuaVideoCaptureResultMsg  // 40 — 2.0.12-alpha5
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
    MessageType::FileMeta,        // 26 — P1 file transfer
    MessageType::FileChunk,       // 27 — P1 file transfer
    MessageType::FileAck,         // 28 — P1 file transfer
    MessageType::FileRequest,     // 29 — P1 file transfer
    MessageType::AttachAck,         // 30 — 2.0.8-alpha3
    MessageType::OutputGap,         // 31 — 2.0.8-alpha3
    MessageType::ConversationAppend,// 32 — 2.0.8-alpha3
    MessageType::ConversationQuery, // 33 — 2.0.8-alpha3
    MessageType::ConversationBatch, // 34 — 2.0.8-alpha3
    MessageType::CuaRequest,        // 35 — 2.0.8-alpha3
    MessageType::CuaResponse,       // 36 — 2.0.8-alpha3
    MessageType::JoinRequest,       // 37 — 2.0.9-alpha5
    MessageType::JoinReply,          // 38 — 2.0.9-alpha5
    MessageType::CuaVideoCapture,    // 39 — 2.0.12-alpha5
    MessageType::CuaVideoCaptureResult // 40 — 2.0.12-alpha5
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
    void u32be(uint32_t v) { for (int i=3; i>=0; --i) out.push_back(v>>(i*8))

... [OUTPUT TRUNCATED - 8,887 chars omitted out of 58,815 total] ...

(ZSTD_isError(sz)) throw std::runtime_error(std::string("zstd compress: ") + ZSTD_getErrorName(sz));
    out.resize(sz);
    return out;
}

std::vector<uint8_t> zstd_decompress(std::span<const uint8_t> data) {
    uint64_t bound = ZSTD_getFrameContentSize(data.data(), data.size());
    if (bound == ZSTD_CONTENTSIZE_ERROR) throw std::runtime_error("zstd: invalid frame");
    if (bound == ZSTD_CONTENTSIZE_UNKNOWN) throw std::runtime_error("zstd: unknown decompressed size");
    if (bound > MAX_FRAME_SIZE) throw std::runtime_error("zstd: decompressed frame exceeds MAX_FRAME_SIZE");
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    size_t sz = ZSTD_decompressDCtx(get_zstd_dctx(), out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(sz)) throw std::runtime_error(std::string("zstd decompress: ") + ZSTD_getErrorName(sz));
    if (sz > MAX_FRAME_SIZE) throw std::runtime_error("zstd: decoded frame exceeds MAX_FRAME_SIZE");
    out.resize(sz);
    return out;
}

// File-chunk payloads are ambiguous across versions: v2.0.14+ senders pass raw
// bytes to write_frame() and let encode() compress at the frame layer, while
// pre-2.0.14 senders manually zstd_compress()ed each chunk first (double
// compression). Sniff the zstd magic (0xFD2FB528, little-endian on the wire)
// and decompress only when it is present; otherwise treat payload as raw.
// The end-to-end sha256 check guards integrity if a raw chunk happens to start
// with the magic bytes (decompress failure falls back to raw, hash then fails
// loudly rather than corrupting silently).
std::vector<uint8_t> decompress_chunk_payload(std::span<const uint8_t> data) {
    if (data.size() >= 4 &&
        data[0] == 0x28 && data[1] == 0xB5 && data[2] == 0x2F && data[3] == 0xFD) {
        try {
            return zstd_decompress(data);
        } catch (...) {
            // Magic collision on raw data — fall through and treat as raw.
        }
    }
    return std::vector<uint8_t>(data.begin(), data.end());
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

    uint64_t u64be() {
        ensure(8);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
        p += 8;
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
constexpr size_t kTransferChunkRawSize = 48 * 1024;   // 48KB (must match across peers)
constexpr int    kTransferPipelineSize = 16;            // 768KB pipeline (was 8)

// ── Transfer telemetry ──────────────────────────────────────────────
// Accumulates per-chunk wall-clock timings during a transfer.  Emit a
// summary line after the last chunk so operators can see exactly where
// time is going (select / write / drain / overhead) on slow links.
// Set BS_NOXFER_TIMING=1 to suppress.
struct TransferChunkTiming {
    int64_t select_us   = 0;
    int64_t write_us    = 0;
    int64_t drain_us    = 0;
    int64_t total_us    = 0;
    int64_t select_min  = INT64_MAX;
    int64_t select_max  = 0;
    int64_t write_min   = INT64_MAX;
    int64_t write_max   = 0;
    int64_t drain_min   = INT64_MAX;
    int64_t drain_max   = 0;
    int64_t total_min   = INT64_MAX;
    int64_t total_max   = 0;
    uint32_t count      = 0;

    void record(int64_t s, int64_t w, int64_t d, int64_t t) {
        select_us += s; write_us += w; drain_us += d; total_us += t;
        select_min = std::min(select_min, s); select_max = std::max(select_max, s);
        write_min  = std::min(write_min,  w); write_max  = std::max(write_max,  w);
        drain_min  = std::min(drain_min,  d); drain_max  = std::max(drain_max,  d);
        total_min  = std::min(total_min,  t); total_max  = std::max(total_max,  t);
        count++;
    }

    [[nodiscard]] std::string format(const std::string& file, uint64_t bytes) const {
        if (count == 0) return {};
        const auto mean = [&](int64_t sum) { return static_cast<double>(sum) / count; };
        const auto us2ms = [](int64_t us) { return static_cast<double>(us) / 1000.0; };
        int64_t overhead_us = total_us - write_us;
        double overhead_pct = total_us > 0
            ? 100.0 * static_cast<double>(overhead_us) / static_cast<double>(total_us) : 0.0;
        char buf[640];
        snprintf(buf, sizeof(buf),
            "XFER_TIMING file=%s bytes=%llu chunks=%u total_wall_ms=%.0f "
            "select(total=%.0fms mean=%.0fus min=%.0fus max=%.0fus) "
            "write(total=%.0fms mean=%.0fus min=%.0fus max=%.0fus) "
            "drain(total=%.0fms mean=%.0fus min=%.0fus max=%.0fus) "
            "overhead_pct=%.1f",
            file.c_str(),
            static_cast<unsigned long long>(bytes), count,
            us2ms(total_us),
            us2ms(select_us), mean(select_us), static_cast<double>(select_min), static_cast<double>(select_max),
            us2ms(write_us),    mean(write_us),    static_cast<double>(write_min),    static_cast<double>(write_max),
            us2ms(drain_us),    mean(drain_us),    static_cast<double>(drain_min),    static_cast<double>(drain_max),
            overhead_pct);
        return std::string(buf);
    }
};

[[nodiscard]] inline TransferChunkTiming make_transfer_timing() {
    TransferChunkTiming t{};
    if (const char* e = getenv("BS_NOXFER_TIMING"); e && (e[0] == '1')) {
        t.count = 0; // sentinel — format() short-circuits
    }
    return t;
}

// ── Transfer telemetry ring buffer ───────────────────────────────────
// Bounded in-memory ring of recent transfer completions.  Exported via
// `bs telemetry --json` (IPC "TELEMETRY") so a fleet controller can
// poll peers without extra ports.
struct TransferTelemetryEntry {
    std::string file;        // filename (basename)
    uint64_t bytes = 0;
    uint32_t chunks = 0;
    int64_t total_wall_ms = 0;
    double rate_mibs = 0.0;
    double overhead_pct = 0.0;
    int64_t select_total_ms = 0;
    int64_t write_total_ms = 0;
    int64_t drain_total_ms = 0;
    double select_mean_us = 0.0;
    double write_mean_us = 0.0;
    double drain_mean_us = 0.0;
    int64_t timestamp_s = 0; // unix epoch second at completion
    std::string direction;   // "send" or "recv"
    std::string peer;        // remote peer name (empty if local)
};

struct TransferTelemetryRing {
    static constexpr size_t kMaxEntries = 256;
    std::vector<TransferTelemetryEntry> entries;
    mutable std::mutex mutex;

    void append(TransferTelemetryEntry e) {
        std::lock_guard lock(mutex);
        if (entries.size() >= kMaxEntries)
            entries.erase(entries.begin());
        entries.push_back(std::move(e));
    }

    [[nodiscard]] std::string to_json() const {
        std::lock_guard lock(mutex);
        nlohmann::json j = nlohmann::json::array();
        for (auto& e : entries) {
            nlohmann::json je;
            je["file"] = e.file;
            je["bytes"] = e.bytes;
            je["chunks"] = e.chunks;
            je["total_wall_ms"] = e.total_wall_ms;
            je["rate_mibs"] = e.rate_mibs;
            je["overhead_pct"] = e.overhead_pct;
            je["select_total_ms"] = e.select_total_ms;
            je["write_total_ms"] = e.write_total_ms;
            je["drain_total_ms"] = e.drain_total_ms;
            je["select_mean_us"] = e.select_mean_us;
            je["write_mean_us"] = e.write_mean_us;
            je["drain_mean_us"] = e.drain_mean_us;
            je["ts"] = e.timestamp_s;
            je["dir"] = e.direction;
            je["peer"] = e.peer;
            j.push_back(je);
        }
        return j.dump();
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard lock(mutex);
        return entries.size();
    }
};

// Helper: build a telemetry entry from TransferChunkTiming + context.
[[nodiscard]] inline TransferTelemetryEntry make_telemetry_entry(
        const TransferChunkTiming& t, const std::string& filename,
        uint64_t filesize, const std::string& peer,
        const std::string& direction) {
    TransferTelemetryEntry e;
    const auto us2ms = [](int64_t us) { return static_cast<double>(us) / 1000.0; };
    e.file = filename;
    e.bytes = filesize;
    e.chunks = t.count;
    e.total_wall_ms = static_cast<int64_t>(us2ms(t.total_us));
    double elapsed_s = std::max(0.001, us2ms(t.total_us) / 1000.0);
    e.rate_mibs = elapsed_s > 0.0
        ? (static_cast<double>(filesize) / elapsed_s) / (1024.0 * 1024.0) : 0.0;
    e.overhead_pct = t.total_us > 0
        ? 100.0 * static_cast<double>(t.total_us - t.write_us) / static_cast<double>(t.total_us) : 0.0;
    e.select_total_ms = static_cast<int64_t>(us2ms(t.select_us));
    e.write_total_ms  = static_cast<int64_t>(us2ms(t.write_us));
    e.drain_total_ms  = static_cast<int64_t>(us2ms(t.drain_us));
    e.select_mean_us = t.count > 0 ? static_cast<double>(t.select_us) / t.count : 0.0;
    e.write_mean_us  = t.count > 0 ? static_cast<double>(t.write_us) / t.count : 0.0;
    e.drain_mean_us  = t.count > 0 ? static_cast<double>(t.drain_us) / t.count : 0.0;
    e.timestamp_s = static_cast<int64_t>(std::time(nullptr));
    e.direction = direction;
    e.peer = peer;
    return e;
}

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
    case 0x06: { AttachMsg m; m.cols = d.u16(); m.rows = d.u16(); m.term = d.str_prefixed(); m.session_name = d.str_prefixed(); m.routing = d.str_prefixed(); /* command is optional (v1.7+, str_prefixed_u16). v1.6 clients don't send it. */ m.command = d.ok(2) ? d.str_prefixed_u16() : std::string{}; /* signal_on_detach is optional (v2.0.7+, str_prefixed_u16). */ m.signal_on_detach = d.ok(2) ? d.str_prefixed_u16() : std::string{}; /* client_instance_id is optional (2.0.8+, u32be). Legacy 2.0.7 clients don't send it. */ m.client_instance_id = d.ok(4) ? d.u32be() : 0u; /* spectator is optional (2.0.8+, u8). Defaults to interactive (false). */ m.spectator = d.ok(1) ? (d.u8() != 0) : false; return m; }
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
                if (d.ok(8)) { std::memcpy(&m.load, d.p, 8); d.p += 8; }
                /* sessions_summary_json is optional (2.0.8+, str_prefixed_u16). Legacy peers don't send it. */
                m.sessions_summary_json = d.ok(2) ? d.str_prefixed_u16() : std::string{};
                return m;
            }
        }
        // Prefixed-u16 format (v2.0.7+).
        d.p = saved;
        m.hostname = d.str_prefixed_u16();
        m.version = d.str_prefixed_u16();
        if (d.ok(8)) { std::memcpy(&m.load, d.p, 8); d.p += 8; }
        /* sessions_summary_json is optional (2.0.8+, str_prefixed_u16). Legacy peers don't send it. */
        m.sessions_summary_json = d.ok(2) ? d.str_prefixed_u16() : std::string{};
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
    // ── 2.0.8-alpha3 decode cases ──
    case 0x21: {
        AttachAckMsg m;
        m.attach_id = d.u32be();
        m.session_name = d.str_prefixed();
        m.cols = d.u16(); m.rows = d.u16();
        return m;
    }
    case 0x22: {
        OutputGapMsg m;
        m.dropped_bytes = d.u64be();
        return m;
    }
    case 0x23: {
        ConversationAppendMsg m;
        m.conv_id = d.str_prefixed();
        m.seq = d.u64be(); m.ts = d.u64be();
        m.agent_id = d.str_prefixed(); m.role = d.u8();
        m.body = d.str_prefixed_u16(); // matches serialize (2.0.8-alpha3 final)
        return m;
    }
    case 0x24: {
        ConversationQueryMsg m;
        m.conv_id = d.str_prefixed();
        m.since_seq = d.u64be();
        return m;
    }
    case 0x25: {
        ConversationBatchMsg m;
        m.conv_id = d.str_prefixed();
        // remaining bytes are a run of ConversationAppendMsg (no count prefix → parse until end)
        while (d.ok(1)) {
            ConversationAppendMsg am;
            am.conv_id = d.str_prefixed();
            am.seq = d.u64be(); am.ts = d.u64be();
            am.agent_id = d.str_prefixed(); am.role = d.u8();
            am.body = d.str_prefixed_u16(); // matches serialize (2.0.8-alpha3 final)
            m.messages.push_back(std::move(am));
        }
        return m;
    }
    case 0x26: {
        CuaRequestMsg m;
        m.request_id = d.u32be(); m.action = d.u8();
        m.x = static_cast<int16_t>(d.u16()); m.y = static_cast<int16_t>(d.u16());
        m.button = d.u8(); m.hid_key = d.u32be(); m.modifiers = d.u8();
        m.text = d.str_prefixed();
        return m;
    }
    case 0x27: {
        CuaResponseMsg m;
        m.request_id = d.u32be(); m.status = d.u8();
        m.error = d.str_prefixed();
        m.screen_w = d.u32be(); m.screen_h = d.u32be(); m.format = d.u8();
        uint32_t data_size = d.u32be();
        if (data_size > 0) {
            m.data = d.bytes_size(data_size);
        }
        return m;
    }
    case 0x28: {
        JoinRequestMsg m;
        m.token = d.str_prefixed();
        return m;
    }
    case 0x2A: {
        CuaVideoCaptureMsg m;
        m.request_id = d.u32be(); m.fps = d.u8(); m.duration_sec = d.u16();
        m.quality = d.u8(); m.max_width = d.u16();
        return m;
    }
    case 0x2B: {
        CuaVideoCaptureResultMsg m;
        m.request_id = d.u32be(); m.status = d.u8();
        m.error = d.str_prefixed();
        m.file_path = d.str_prefixed();
        m.duration_sec = d.u16(); m.width = d.u16(); m.height = d.u16(); m.format = d.u8();
        return m;
    }
    case 0x29: {
        JoinReplyMsg m;
        m.ok = (d.u8() != 0);
        if (m.ok) {
            m.node_name = d.str_prefixed(); m.seeds_csv = d.str_prefixed();
            m.host_pubkey = d.str_prefixed(); m.host_addr = d.str_prefixed();
            m.peer_pubkeys_json = d.str_prefixed(); // 2.0.20
        } else {
            m.error = d.str_prefixed();
        }
        return m;
    }
    }

    throw std::runtime_error("unknown message type: " + std::to_string(type_byte));
}

size_t max_encoded_size(const Message&) {
    return MAX_FRAME_SIZE;
}
} // namespace bs::mesh
