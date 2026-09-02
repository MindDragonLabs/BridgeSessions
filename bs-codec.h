// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bs-codec.h — Message types, serializer, decoder, transfer validation
// Extracted from bs-protocol.h (R6 structural refactor, 2026-09-02)
// Designed for inclusion inside `namespace bs::mesh { ... }`
// Does NOT open its own namespace — parent file provides it.
#pragma once

// ── Upgrade tag validation (shared with tests) ────────────────────
// W4-P1 guard: only [A-Za-z0-9._-] allowed so a malicious --tag cannot
// break out of single-quoted curl/system commands in the upgrade path.
inline bool bs_upgrade_tag_valid(const std::string& tag) {
    if (tag == "latest") return true;
    if (tag.empty()) return false;
    for (char c : tag) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// Normalize a user-supplied --tag for the download path, which prepends "v".
// Accepts both "26.09.19-beta5" and "v26.09.19-beta5"; "latest" is untouched.
// Caller must validate with bs_upgrade_tag_valid() first.
inline std::string bs_upgrade_tag_normalize(const std::string& tag) {
    if (tag != "latest" && tag.size() > 1 && tag[0] == 'v') return tag.substr(1);
    return tag;
}

// Fleet/CLI names interpolated into shells: same alphabet as upgrade tags.
inline bool bs_peer_name_shell_safe(std::string_view name) {
    if (name.empty() || name.size() > 128) return false;
    for (unsigned char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

inline bool const_time_token_match(std::string_view line, std::string_view token) {
    if (token.empty()) return false;
    const size_t n = token.size();
    unsigned char acc = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char lc = (i < line.size()) ? static_cast<unsigned char>(line[i]) : 0;
        acc |= static_cast<unsigned char>(lc ^ static_cast<unsigned char>(token[i]));
    }
    unsigned char sep = 0;
    if (line.size() > n) {
        unsigned char c = static_cast<unsigned char>(line[n]);
        sep = (c == ' ' || c == '\t') ? 1 : 0;
    }
    const unsigned char long_enough = (line.size() > n) ? 1 : 0;
    return acc == 0 && sep && long_enough;
}

inline std::string transfer_path_basename(std::string_view path) {
    std::string s(path);
    for (char& c : s) if (c == '\\') c = '/';
    auto pos = s.find_last_of('/');
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

// Identity / trust-store / PEM files: refuse serve+overwrite unless opted in.
inline bool is_sensitive_mesh_path(std::string_view path) {
    if (path.empty()) return false;
    std::string base = transfer_path_basename(path);
    for (char& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (base == "authorized_keys" || base == "ipc-token" ||
        base == "cua-helper-token")
        return true;
    if (base.rfind("id_ed25519", 0) == 0) return true;
    if (base.size() >= 4 &&
        (base.compare(base.size() - 4, 4, ".pem") == 0 ||
         base.compare(base.size() - 4, 4, ".key") == 0))
        return true;
    if (base == "config") {
        std::string n(path);
        for (char& c : n) if (c == '\\') c = '/';
        for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (n.find("/received/") != std::string::npos) return false;
        if (n.find(".bridgesessions/") != std::string::npos) return true;
    }
    return false;
}

inline std::string redact_secrets(std::string text) {
    auto redact_value = [&](std::string_view marker) {
        std::string lower = text;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        std::string needle(marker);
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        size_t pos = 0;
        while ((pos = lower.find(needle, pos)) != std::string::npos) {
            const size_t value_begin = pos + marker.size();
            size_t value_end = value_begin;
            while (value_end < text.size() && !std::isspace(
                       static_cast<unsigned char>(text[value_end])) &&
                   text[value_end] != '&' && text[value_end] != ';' &&
                   text[value_end] != ',' && text[value_end] != '"' &&
                   text[value_end] != '\'') {
                ++value_end;
            }
            text.replace(value_begin, value_end - value_begin, "[REDACTED]");
            lower.replace(value_begin, value_end - value_begin, "[redacted]");
            pos = value_begin + 10;
        }
    };
    for (std::string_view marker : {"token=", "password=", "passwd=", "api_key=", "secret=",
                                    "authorization: bearer "})
        redact_value(marker);
    const std::string begin = "-----BEGIN PRIVATE KEY-----";
    const std::string end = "-----END PRIVATE KEY-----";
    size_t pos = 0;
    while ((pos = text.find(begin, pos)) != std::string::npos) {
        size_t finish = text.find(end, pos + begin.size());
        finish = finish == std::string::npos ? text.size() : finish + end.size();
        text.replace(pos, finish - pos, "[REDACTED PRIVATE KEY]");
        pos += 22;
    }
    return text;
}

[[nodiscard]] inline std::string private_tmp_dir(const std::string& app_home = {});
[[nodiscard]] inline std::string create_private_temp_file(
    const std::string& prefix,
    const std::string& suffix,
    const std::string& app_home = {});

// ── Message Type Enum ─────────────────────────────────────────────
// Original types + mesh types (41 total as of v26.08.10 — see variant below)

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
    DirectoryEnroll       = 0x2C,  // bidirectional: signed mesh-directory entry (bootstrap: auto-trust new member)
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
    double load = 0.0;                 // 1-min load average (Unix) or cpu_pct/100 (Windows)
    std::string sessions_summary_json; // 2.0.8: trailing, optional. JSON array of
                                        // {name,state,command,bytes} for this node's
                                        // sessions. Capped (~4 KiB); empty = no data.
    // Optional trailing (v26.08.12+): compact host metrics JSON for `bs fleet`.
    // Keys: cpu, mem, disk (pct), load, os, arch, ncpu, mem_mb, disk_gb.
    std::string host_stats_json;
};

struct ScrollbackMsg {
    std::string data;      // replay chunk
    uint32_t total_lines = 0;
    uint32_t chunk_index = 0;
};

struct SignalMsg {
    enum class SignalType : uint8_t { CtrlC = 0, CtrlZ = 1, CtrlBackslash = 2, Restart = 3, Kill = 4 };
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
    // Requested node name (empty = host auto-assigns "node-<token8>"). The
    // host uses this name when it vouches for the joiner via DirectoryEnrollMsg,
    // so the mesh directory reflects the operator's chosen identity.
    std::string node_name;
    // Joiner's own reachable listen addr ("ip:port"). The host only sees the
    // ephemeral source port, so the joiner advertises its real endpoint here —
    // the Tailscale-style endpoint-advertisement that makes auto-enroll possible.
    std::string listen_addr;
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

// ── Bootstrap: signed mesh-directory enrollment ──────────────────────
// A trusted member vouches for a NEW member by signing {name, pubkey, addr}
// with its own ed25519 key. Receivers verify the signature against a pubkey
// they already trust, then auto-append the new member to authorized_keys and
// seed it — so a freshly `bs join`-ed host is reachable mesh-wide with NO
// manual key copying on any peer.
struct DirectoryEnrollMsg {
    std::string name;          // new member node name
    std::string pubkey_hex;    // new member ed25519 pubkey (64 hex)
    std::string addr;          // new member reachable addr "host:port"
    std::string issuer_pubkey; // vouching member's ed25519 pubkey (64 hex)
    uint64_t issued_at = 0;    // unix seconds
    std::vector<uint8_t> signature; // ed25519 signature over the canonical payload
    bool operator==(const DirectoryEnrollMsg&) const = default;

    // Canonical bytes signed = name || '\0' || pubkey_hex || '\0' || addr ||
    // '\0' || issuer_pubkey || '\0' || decimal(issued_at). Deterministic and
    // unambiguous so the verifier reconstructs exactly what the issuer signed.
    std::string signed_payload() const {
        std::string p;
        p += name;           p.push_back('\0');
        p += pubkey_hex;     p.push_back('\0');
        p += addr;           p.push_back('\0');
        p += issuer_pubkey;  p.push_back('\0');
        p += std::to_string(issued_at);
        return p;
    }
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
    std::string filename;       // basename only (compat / display)
    uint64_t filesize = 0;      // total file size in bytes
    std::string checksum;       // SHA-256 hex of entire file
    uint32_t total_chunks = 0;  // total number of chunks
    // Optional trailing field (v26.08.10+): raw chunk size in bytes.
    // 0 = peer omitted field → use kTransferChunkRawSizeDefault.
    // Enables mixed-fleet negotiation without compile-time lockstep.
    uint32_t chunk_size = 0;
    // Optional trailing field (v26.08.12+): remote destination path for
    // scp-style file send. Empty = default receive_dir/basename behavior.
    // Relative paths are under receive_dir; absolute/~ are constrained.
    std::string dest_path;
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
    CuaVideoCaptureResultMsg, // 40 — 2.0.12-alpha5
    DirectoryEnrollMsg        // 41 — bootstrap: signed mesh-directory entry
>;

// ── Frame ──────────────────────────────────────────────────────────
// Wire format (legacy / small):
//   [stream_id: u16][type: u8][flags: u8][length: u16][data]
// Wire format (large, FLAG_LENGTH_U32):
//   [stream_id: u16][type: u8][flags: u8][length: u32be][data]
// Capability: Hello.version contains "+frm2" when a peer can send/recv u32 length.
// Peers without frm2 must only ever see u16-length frames (≤65535 payload).

constexpr uint16_t CONTROL_STREAM_ID = 0;
constexpr size_t   FRAME_HEADER_SIZE_U16 = 6;   // stream_id + type + flags + length(u16)
constexpr size_t   FRAME_HEADER_SIZE_U32 = 8;   // stream_id + type + flags + length(u32)
constexpr size_t   FRAME_HEADER_SIZE     = FRAME_HEADER_SIZE_U16; // min header / legacy alias
constexpr uint32_t MAX_FRAME_PAYLOAD_U16 = 65535u;
constexpr uint32_t MAX_FRAME_PAYLOAD_U32 = 4u * 1024u * 1024u; // 4 MiB single-frame cap
constexpr uint16_t MAX_FRAME_SIZE        = 65535; // legacy alias (= MAX_FRAME_PAYLOAD_U16)
constexpr uint16_t COMPRESSION_THRESHOLD = 256;
constexpr size_t   MAX_IMAGE_BYTES       = 50ull * 1024ull * 1024ull;

// Hello.version capability tags (appended as +tag[+tag...]).
inline constexpr std::string_view kCapFrm2 = "frm2"; // u32 frame length support

enum FrameFlags : uint8_t {
    FLAG_COMPRESSED      = 0x01,
    FLAG_CONTROL         = 0x02,
    FLAG_RENDER_MARKDOWN = 0x04,
    FLAG_LENGTH_U32      = 0x08, // length field is u32be; header is 8 bytes
    // FLAG_FRAGMENT      = 0x10, // reserved: multi-frame logical fragmentation
};

// Parse "+cap" tags from a version string like "26.08.10-beta2+frm2+txc".
[[nodiscard]] inline bool version_has_cap(std::string_view version, std::string_view cap) {
    if (cap.empty()) return false;
    auto plus = version.find('+');
    if (plus == std::string_view::npos) return false;
    std::string_view rest = version.substr(plus + 1);
    while (!rest.empty()) {
        auto next = rest.find('+');
        std::string_view tag = (next == std::string_view::npos) ? rest : rest.substr(0, next);
        if (tag == cap) return true;
        if (next == std::string_view::npos) break;
        rest.remove_prefix(next + 1);
    }
    return false;
}

[[nodiscard]] inline std::string version_string_with_local_caps() {
    return std::string(kBridgeSessionsVersion) + "+" + std::string(kCapFrm2);
}

// Strip capability tags: "26.08.12-beta3+frm2" → "26.08.12-beta3"
[[nodiscard]] inline std::string_view version_core(std::string_view v) {
    auto plus = v.find('+');
    return plus == std::string_view::npos ? v : v.substr(0, plus);
}

// Parsed calendar tag: YY.MM.DD or YYYY.MM.DD, optional -betaN (or other suffix).
// Two-digit years are 2000+YY so 26.08.24 and 2026.08.24 compare equal.
struct VersionParts {
    int year = 0;
    int month = 0;
    int day = 0;
    std::string rest;
    bool ok = false;
};

[[nodiscard]] inline VersionParts parse_version_tag(std::string_view raw) {
    VersionParts p;
    const auto v = version_core(raw);
    if (v.empty()) return p;
    size_t i = 0;
    int year = 0;
    size_t year_digits = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])) && year_digits < 4) {
        year = year * 10 + (v[i] - '0');
        ++i;
        ++year_digits;
    }
    if (year_digits == 0 || year_digits == 3 || i >= v.size() || v[i] != '.') return p;
    if (year_digits == 2) year += 2000;
    ++i;
    int month = 0;
    size_t month_digits = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])) && month_digits < 2) {
        month = month * 10 + (v[i] - '0');
        ++i;
        ++month_digits;
    }
    if (month_digits == 0 || i >= v.size() || v[i] != '.') return p;
    ++i;
    int day = 0;
    size_t day_digits = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i])) && day_digits < 2) {
        day = day * 10 + (v[i] - '0');
        ++i;
        ++day_digits;
    }
    if (day_digits == 0 || month < 1 || month > 12 || day < 1 || day > 31) return p;
    p.year = year;
    p.month = month;
    p.day = day;
    p.rest = std::string(v.substr(i));
    p.ok = true;
    return p;
}

// True if `remote` is strictly older than `local` for date-based tags
// YY.MM.DD[-betaN] or YYYY.MM.DD[-betaN]. Lexicographic compare is unsafe
// across the two schemes: "2026.08.24-beta7" < "26.09.19-beta6" as ASCII.
[[nodiscard]] inline bool version_is_older(std::string_view remote, std::string_view local) {
    auto r = parse_version_tag(remote);
    auto l = parse_version_tag(local);
    if (!r.ok || !l.ok) {
        auto rc = version_core(remote);
        auto lc = version_core(local);
        if (rc.empty() || lc.empty() || rc == lc) return false;
        return rc < lc;
    }
    if (r.year != l.year) return r.year < l.year;
    if (r.month != l.month) return r.month < l.month;
    if (r.day != l.day) return r.day < l.day;
    if (r.rest == l.rest) return false;
    return r.rest < l.rest;
}

struct Frame {
    uint16_t stream_id = 0;
    MessageType type = MessageType::Ping;
    uint8_t flags = 0;
    std::vector<uint8_t> data;
};

// ── Type mapping (variant index → MessageType byte) ──────────
// Must match the variant ordering exactly. 41 alternatives = 41 entries.

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
    MessageType::CuaVideoCaptureResult, // 40 — 2.0.12-alpha5
    MessageType::DirectoryEnroll      // 41 — bootstrap: signed mesh-directory entry
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

uint32_t read_u32be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void write_u32be(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v & 0xFF);
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
    void u64be(uint64_t v) { for (int i=7; i>=0; --i) out.push_back(static_cast<uint8_t>(v>>(i*8))); }
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
void serialize_msg(Serializer& s, const AttachMsg&       m) { s.u16(m.cols); s.u16(m.rows); s.str_prefixed(m.term); s.str_prefixed(m.session_name); s.str_prefixed(m.routing); s.str_prefixed_u16(m.command); s.str_prefixed_u16(m.signal_on_detach); s.u32be(m.client_instance_id); s.u8(m.spectator ? 1 : 0); }
void serialize_msg(Serializer& s, const DetachMsg&)        {}
void serialize_msg(Serializer& s, const PingMsg&)          {}
void serialize_msg(Serializer& s, const PongMsg&)          {}
void serialize_msg(Serializer& s, const ScrollbackAckMsg&) {}
void serialize_msg(Serializer& s, const SessionListMsg& m) {
    for (const auto& si : m.sessions) {
        s.str_prefixed(si.name);
        s.str_prefixed(si.state);
        s.u32be(static_cast<uint32_t>(std::min<uint64_t>(
            si.uptime_seconds, std::numeric_limits<uint32_t>::max())));
    }
}
void serialize_msg(Serializer& s, const ServerInfoMsg&   m) {
    s.str_prefixed_u16(m.hostname);
    s.str_prefixed_u16(m.version);
    s.bytes(reinterpret_cast<const uint8_t*>(&m.load), 8);
    s.str_prefixed_u16(m.sessions_summary_json);
    // Optional host metrics (new peers only; old peers stop after sessions).
    if (!m.host_stats_json.empty()) s.str_prefixed_u16(m.host_stats_json);
}
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
    // Always emit chunk_size so new receivers can negotiate; old peers stop
    // reading after total_chunks and ignore trailing payload bytes.
    s.u32be(m.chunk_size);
    // Optional scp-style dest (new peers only; old peers ignore trailing).
    if (!m.dest_path.empty()) s.str_prefixed_u16(m.dest_path);
}
void serialize_msg(Serializer& s, const FileChunkMsg& m) {
    s.u32be(m.chunk_index);
    s.u32be(m.total_chunks);
    if (m.data.size() > MAX_FRAME_PAYLOAD_U32)
        throw std::runtime_error("file chunk payload exceeds MAX_FRAME_PAYLOAD_U32");
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

// ── 2.0.8-alpha3 serialize dispatchers ──────────────────────────
void serialize_msg(Serializer& s, const AttachAckMsg& m) {
    s.u32be(m.attach_id); s.str_prefixed(m.session_name);
    s.u16(m.cols); s.u16(m.rows);
}
void serialize_msg(Serializer& s, const OutputGapMsg& m) {
    s.u64be(m.dropped_bytes);
}
void serialize_msg(Serializer& s, const ConversationAppendMsg& m) {
    s.str_prefixed(m.conv_id); s.u64be(m.seq); s.u64be(m.ts);
    // body is u16-prefixed (2.0.8-alpha3 final): u8 capped messages at 255B,
    // throwing inside serialize for normal chat-sized text (MoA P1).
    s.str_prefixed(m.agent_id); s.u8(m.role); s.str_prefixed_u16(m.body);
}
void serialize_msg(Serializer& s, const ConversationQueryMsg& m) {
    s.str_prefixed(m.conv_id); s.u64be(m.since_seq);
}
void serialize_msg(Serializer& s, const ConversationBatchMsg& m) {
    s.str_prefixed(m.conv_id);
    for (auto& msg : m.messages) serialize_msg(s, msg);
}
void serialize_msg(Serializer& s, const CuaRequestMsg& m) {
    s.u32be(m.request_id); s.u8(m.action);
    s.u16(static_cast<uint16_t>(m.x)); s.u16(static_cast<uint16_t>(m.y));
    s.u8(m.button); s.u32be(m.hid_key); s.u8(m.modifiers);
    s.str_prefixed(m.text);
}
void serialize_msg(Serializer& s, const CuaResponseMsg& m) {
    s.u32be(m.request_id); s.u8(m.status); s.str_prefixed(m.error);
    s.u32be(m.screen_w); s.u32be(m.screen_h); s.u8(m.format);
    s.u32be(static_cast<uint32_t>(m.data.size()));
    if (!m.data.empty()) s.bytes(std::span<const uint8_t>(m.data.data(), m.data.size()));
}
void serialize_msg(Serializer& s, const JoinRequestMsg& m) {
    s.str_prefixed(m.token);
    s.str_prefixed(m.node_name);    // optional requested name (bootstrap auto-enroll)
    s.str_prefixed(m.listen_addr);  // joiner's advertised reachable endpoint
}
void serialize_msg(Serializer& s, const CuaVideoCaptureMsg& m) {
    s.u32be(m.request_id); s.u8(m.fps); s.u16(m.duration_sec);
    s.u8(m.quality); s.u16(m.max_width);
}
void serialize_msg(Serializer& s, const CuaVideoCaptureResultMsg& m) {
    s.u32be(m.request_id); s.u8(m.status); s.str_prefixed(m.error);
    s.str_prefixed(m.file_path); s.u16(m.duration_sec);
    s.u16(m.width); s.u16(m.height); s.u8(m.format);
}
void serialize_msg(Serializer& s, const JoinReplyMsg& m) {
    s.u8(m.ok ? 1 : 0);
    if (m.ok) {
        s.str_prefixed(m.node_name);
        s.str_prefixed_u16(m.seeds_csv); // u16 — fleet with >3 seeds exceeds 255B
        s.str_prefixed(m.host_pubkey);
        s.str_prefixed(m.host_addr);
        s.str_prefixed_u16(m.peer_pubkeys_json); // 2.0.20 — u16-prefixed (was u8, exceeded 255B with >3 seeds)
    } else {
        s.str_prefixed(m.error);
    }
}
void serialize_msg(Serializer& s, const DirectoryEnrollMsg& m) {
    s.str_prefixed(m.name);
    s.str_prefixed(m.pubkey_hex);
    s.str_prefixed(m.addr);
    s.str_prefixed(m.issuer_pubkey);
    s.u64be(m.issued_at);
    s.bytes(std::span<const uint8_t>(m.signature.data(), m.signature.size()));
}

// ── Zstd ──────────────────────────────────────────────────────
// P2: reuse zstd contexts instead of creating/destroying per call.
// Thread-local so each thread gets its own — zstd contexts are not thread-safe.
inline ZSTD_CCtx* get_zstd_cctx() {
    thread_local std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> ctx(
        ZSTD_createCCtx(), &ZSTD_freeCCtx);
    return ctx.get();
}
inline ZSTD_DCtx* get_zstd_dctx() {
    thread_local std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> ctx(
        ZSTD_createDCtx(), &ZSTD_freeDCtx);
    return ctx.get();
}

std::vector<uint8_t> zstd_compress(std::span<const uint8_t> data) {
    std::vector<uint8_t> out(ZSTD_compressBound(data.size()));
    size_t sz = ZSTD_compressCCtx(get_zstd_cctx(), out.data(), out.size(), data.data(), data.size(), 3);
    if (ZSTD_isError(sz)) throw std::runtime_error(std::string("zstd compress: ") + ZSTD_getErrorName(sz));
    out.resize(sz);
    return out;
}

std::vector<uint8_t> zstd_decompress(std::span<const uint8_t> data) {
    uint64_t bound = ZSTD_getFrameContentSize(data.data(), data.size());
    if (bound == ZSTD_CONTENTSIZE_ERROR) throw std::runtime_error("zstd: invalid frame");
    if (bound == ZSTD_CONTENTSIZE_UNKNOWN) throw std::runtime_error("zstd: unknown decompressed size");
    // Cap matches largest dual-frame payload (frm2 / u32 length).
    if (bound > MAX_FRAME_PAYLOAD_U32)
        throw std::runtime_error("zstd: decompressed frame exceeds MAX_FRAME_PAYLOAD_U32");
    std::vector<uint8_t> out(static_cast<size_t>(bound));
    size_t sz = ZSTD_decompressDCtx(get_zstd_dctx(), out.data(), out.size(), data.data(), data.size());
    if (ZSTD_isError(sz)) throw std::runtime_error(std::string("zstd decompress: ") + ZSTD_getErrorName(sz));
    if (sz > MAX_FRAME_PAYLOAD_U32)
        throw std::runtime_error("zstd: decoded frame exceeds MAX_FRAME_PAYLOAD_U32");
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
// Idle stall budget: long enough for Wi‑Fi blackouts (~60s) without aborting.
// Progress (any FileAck / successful write) resets this deadline.
constexpr int kTransferIdleTimeoutSec = 300;
constexpr int kTransferProgressIntervalSec = 10;
// How many times direct-TLS send/recv reconnects after a transport error.
constexpr int kTransferReconnectMax = 12;
// Default raw chunk size for legacy (u16-only) peers: uncompressible data must
// still fit MAX_FRAME_PAYLOAD_U16 after framing. With +frm2, prefer large.
constexpr size_t kTransferChunkRawSizeDefault = 48 * 1024;
constexpr size_t kTransferChunkRawSizeLarge   = 256 * 1024; // frm2 peers
constexpr size_t kTransferChunkRawSizeMin     = 4 * 1024;
constexpr size_t kTransferChunkRawSizeMax     = 256 * 1024; // clamp upper
constexpr size_t kTransferChunkRawSize = kTransferChunkRawSizeDefault; // alias
constexpr int    kTransferPipelineSize = 32;

// Clamp a peer-declared chunk size into the safe wire range.
// declared==0 means "legacy peer omitted field" → always 48 KiB default
// (must not invent 256 KiB or chunk-count validation breaks).
[[nodiscard]] inline size_t effective_transfer_chunk_size(
        uint32_t declared, size_t max_allowed = kTransferChunkRawSizeMax) {
    size_t max_c = std::min(max_allowed, kTransferChunkRawSizeMax);
    if (max_c < kTransferChunkRawSizeMin) max_c = kTransferChunkRawSizeMin;
    if (declared == 0)
        return std::min(kTransferChunkRawSizeDefault, max_c);
    size_t v = static_cast<size_t>(declared);
    if (v < kTransferChunkRawSizeMin) v = kTransferChunkRawSizeMin;
    if (v > max_c) v = max_c;
    return v;
}

// Chunk size to offer a peer based on Hello.version capability tags.
[[nodiscard]] inline size_t transfer_chunk_size_for_peer(std::string_view remote_version) {
    if (version_has_cap(remote_version, kCapFrm2))
        return kTransferChunkRawSizeLarge;
    return kTransferChunkRawSizeDefault;
}

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
    // P2 audit fix: deque gives O(1) front removal (vector::erase(begin) was O(n)).
    std::deque<TransferTelemetryEntry> entries;
    mutable std::mutex mutex;

    void append(TransferTelemetryEntry e) {
        std::lock_guard lock(mutex);
        if (entries.size() >= kMaxEntries)
            entries.pop_front();
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
    uint64_t filesize, uint64_t max_bytes, size_t chunk_raw = kTransferChunkRawSizeDefault) {
    if (max_bytes > 0 && filesize > max_bytes) {
        return {false, 0, "file exceeds transfer.max_bytes"};
    }
    chunk_raw = effective_transfer_chunk_size(static_cast<uint32_t>(chunk_raw));
    uint64_t expected = filesize / chunk_raw;
    if (filesize % chunk_raw != 0) ++expected;
    if (expected == 0) expected = 1;  // zero-byte files still carry one empty chunk
    if (expected > std::numeric_limits<uint32_t>::max()) {
        return {false, 0, "file requires too many chunks"};
    }
    const auto expected_u32 = static_cast<uint32_t>(expected);
    return {true, expected_u32, {}};
}

[[nodiscard]] inline TransferMetadataValidation validate_transfer_metadata(
    uint64_t filesize, uint32_t total_chunks, uint64_t max_bytes,
    size_t chunk_raw = kTransferChunkRawSizeDefault) {
    auto result = calculate_transfer_metadata(filesize, max_bytes, chunk_raw);
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
    size_t decompressed_size,
    size_t chunk_raw = kTransferChunkRawSizeDefault) {
    if (chunk_total_chunks != expected_total_chunks) {
        return {false, "chunk total does not match transfer metadata"};
    }
    if (chunk_index != expected_index || chunk_index >= expected_total_chunks) {
        return {false, "unexpected chunk index"};
    }
    if (received_bytes > expected_size) {
        return {false, "received byte count already exceeds declared size"};
    }
    chunk_raw = effective_transfer_chunk_size(static_cast<uint32_t>(chunk_raw));
    const uint64_t remaining = expected_size - received_bytes;
    const uint64_t expected_chunk_size =
        std::min<uint64_t>(remaining, chunk_raw);
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
    constexpr size_t kMaxPendingIpcLine = 1024u * 1024u;
    if (chunk.size() > kMaxPendingIpcLine -
                           std::min(pending.size(), kMaxPendingIpcLine)) {
        pending.clear();
        return std::string("ERROR IPC response exceeds 1 MiB");
    }
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

// allow_large: emit FLAG_LENGTH_U32 frames when compressed payload > 65535.
// Only true for peers advertising +frm2 (or local tests).
std::vector<uint8_t> encode(const Message& msg, uint16_t stream_id,
                            bool allow_large = false) {
    std::vector<uint8_t> payload;
    Serializer s{payload};
    std::visit([&](const auto& m) { serialize_msg(s, m); }, msg);

    const uint32_t logical_cap = allow_large ? MAX_FRAME_PAYLOAD_U32 : MAX_FRAME_PAYLOAD_U16;
    if (payload.size() > logical_cap)
        throw std::runtime_error("logical payload exceeds frame capacity");

    bool compress = payload.size() > COMPRESSION_THRESHOLD;
    if (compress) payload = zstd_compress(payload);

    uint8_t flags = 0;
    if (compress) flags |= FLAG_COMPRESSED;
    if (stream_id == CONTROL_STREAM_ID) flags |= FLAG_CONTROL;

    const bool use_u32 = allow_large && payload.size() > MAX_FRAME_PAYLOAD_U16;
    if (use_u32) {
        if (payload.size() > MAX_FRAME_PAYLOAD_U32)
            throw std::runtime_error("encoded payload exceeds MAX_FRAME_PAYLOAD_U32");
        flags |= FLAG_LENGTH_U32;
        std::vector<uint8_t> frame(FRAME_HEADER_SIZE_U32 + payload.size());
        write_u16(frame.data(), stream_id);
        frame[2] = static_cast<uint8_t>(message_type(msg));
        frame[3] = flags;
        write_u32be(frame.data() + 4, static_cast<uint32_t>(payload.size()));
        std::copy(payload.begin(), payload.end(), frame.begin() + FRAME_HEADER_SIZE_U32);
        return frame;
    }

    if (payload.size() > MAX_FRAME_PAYLOAD_U16)
        throw std::runtime_error("encoded payload exceeds MAX_FRAME_PAYLOAD_U16 (peer lacks +frm2)");

    std::vector<uint8_t> frame(FRAME_HEADER_SIZE_U16 + payload.size());
    write_u16(frame.data(), stream_id);
    frame[2] = static_cast<uint8_t>(message_type(msg));
    frame[3] = flags;
    write_u16(frame.data() + 4, static_cast<uint16_t>(payload.size()));
    std::copy(payload.begin(), payload.end(), frame.begin() + FRAME_HEADER_SIZE_U16);
    return frame;
}

// Parse length + header size from a buffer that already has ≥6 bytes.
// Returns {header_size, payload_length} or throws on protocol error.
[[nodiscard]] inline std::pair<size_t, uint32_t> frame_header_layout(const uint8_t* data, size_t available) {
    if (available < FRAME_HEADER_SIZE_U16)
        throw std::runtime_error("frame header truncated");
    const uint8_t flags = data[3];
    if (flags & FLAG_LENGTH_U32) {
        if (available < FRAME_HEADER_SIZE_U32)
            throw std::runtime_error("frame u32 header truncated");
        uint32_t length = read_u32be(data + 4);
        if (length > MAX_FRAME_PAYLOAD_U32)
            throw std::runtime_error("frame payload exceeds MAX_FRAME_PAYLOAD_U32");
        return {FRAME_HEADER_SIZE_U32, length};
    }
    uint32_t length = read_u16(data + 4);
    return {FRAME_HEADER_SIZE_U16, length};
}

Message decode(std::span<const uint8_t> raw) {
    if (raw.size() < FRAME_HEADER_SIZE_U16) throw std::runtime_error("frame too short");

    uint8_t type_byte = raw[2];
    uint8_t flags     = raw[3];
    auto [hdr, length] = frame_header_layout(raw.data(), raw.size());

    if (raw.size() < hdr + length) throw std::runtime_error("frame truncated");

    auto payload = raw.subspan(hdr, length);

    // Decompress if needed
    std::vector<uint8_t> decompressed;
    if ((flags & FLAG_COMPRESSED) && length > 0) {
        decompressed = zstd_decompress(payload);
        payload = decompressed;
    }
    if (payload.size() > MAX_FRAME_PAYLOAD_U32)
        throw std::runtime_error("decoded payload exceeds MAX_FRAME_PAYLOAD_U32");

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
                /* host_stats_json optional (v26.08.12+). */
                m.host_stats_json = d.ok(2) ? d.str_prefixed_u16() : std::string{};
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
        /* host_stats_json optional (v26.08.12+). */
        m.host_stats_json = d.ok(2) ? d.str_prefixed_u16() : std::string{};
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
        // Optional trailing chunk_size (new peers). Absent → 0 → default.
        m.chunk_size = d.ok(4) ? d.u32be() : 0u;
        // Optional scp-style dest_path (v26.08.12+). Absent → empty.
        m.dest_path = d.ok(2) ? d.str_prefixed_u16() : std::string{};
        return m;
    }
    case 0x1D: {
        FileChunkMsg m;
        m.chunk_index = d.u32be();
        m.total_chunks = d.u32be();
        uint32_t sz = d.u32be();
        if (sz > MAX_FRAME_PAYLOAD_U32)
            throw std::runtime_error("file chunk size exceeds MAX_FRAME_PAYLOAD_U32");
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
        // Cap message count so a hostile peer cannot force unbounded RAM.
        static constexpr size_t kMaxConversationBatch = 512;
        while (d.ok(1) && m.messages.size() < kMaxConversationBatch) {
            ConversationAppendMsg am;
            am.conv_id = d.str_prefixed();
            am.seq = d.u64be(); am.ts = d.u64be();
            am.agent_id = d.str_prefixed(); am.role = d.u8();
            am.body = d.str_prefixed_u16(); // matches serialize (2.0.8-alpha3 final)
            m.messages.push_back(std::move(am));
        }
        if (d.ok(1))
            throw std::runtime_error("ConversationBatchMsg exceeds 512 messages");
        return m;
    }
    case 0x26: {
        CuaRequestMsg m;
        m.request_id = d.u32be(); m.action = d.u8();
        m.x = static_cast<int16_t>(d.u16()); m.y = static_cast<int16_t>(d.u16());
        m.button = d.u8(); m.hid_key = d.u32be(); m.modifiers = d.u8();
        m.text = d.str_prefixed();
        if (m.text.size() > 64 * 1024)
            throw std::runtime_error("CUA request text exceeds 64KB");
        return m;
    }
    case 0x27: {
        CuaResponseMsg m;
        m.request_id = d.u32be(); m.status = d.u8();
        m.error = d.str_prefixed();
        m.screen_w = d.u32be(); m.screen_h = d.u32be(); m.format = d.u8();
        uint32_t data_size = d.u32be();
        if (data_size > MAX_IMAGE_BYTES)
            throw std::runtime_error("CUA response data exceeds 50MB cap");
        if (data_size > 0) {
            m.data = d.bytes_size(data_size);
        }
        return m;
    }
    case 0x28: {
        JoinRequestMsg m;
        m.token = d.str_prefixed();
        if (d.ok(1)) m.node_name = d.str_prefixed();    // optional requested name
        if (d.ok(1)) m.listen_addr = d.str_prefixed();  // optional advertised endpoint
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
            m.node_name = d.str_prefixed();
            m.seeds_csv = d.str_prefixed_u16(); // u16 — fleet with >3 seeds exceeds 255B
            m.host_pubkey = d.str_prefixed(); m.host_addr = d.str_prefixed();
            m.peer_pubkeys_json = d.str_prefixed_u16(); // 2.0.20 — u16 (was u8, exceeded 255B)
        } else {
            m.error = d.str_prefixed();
        }
        return m;
    }
    case 0x2C: {
        DirectoryEnrollMsg m;
        m.name = d.str_prefixed();
        m.pubkey_hex = d.str_prefixed();
        m.addr = d.str_prefixed();
        m.issuer_pubkey = d.str_prefixed();
        m.issued_at = d.u64be();
        // Remaining bytes are the fixed 64-byte ed25519 signature.
        const size_t sig_len = static_cast<size_t>(d.end - d.p);
        m.signature = d.bytes_size(sig_len);
        return m;
    }
    }

    throw std::runtime_error("unknown message type: " + std::to_string(type_byte));
}

size_t max_encoded_size(const Message&) {
    return MAX_FRAME_SIZE;
}

