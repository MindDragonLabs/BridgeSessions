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
#include <sys/termios.h>
#include <sys/socket.h>
#include <sys/un.h>    // AF_UNIX, sockaddr_un — CUA helper Unix socket (P2)
#include <netinet/in.h>
#include <netinet/tcp.h>  // TCP_NODELAY — critical for interactive shell performance
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#endif
#ifdef __APPLE__
#include <util.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#else
#ifndef _WIN32
#include <pty.h>
#endif
#endif
#ifndef _WIN32
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <net/if.h>
#endif
#endif
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <deque>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <span>
#include <optional>
#include <expected>
#include <chrono>
#include <random>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <array>
#include <utility>
#include <stdexcept>
#include <functional>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <fcntl.h>
#include <process.h>
// MSVC does not provide POSIX pid_t; MinGW (sys/types.h) does.
#if defined(_MSC_VER) && !defined(_PID_T_DEFINED)
typedef int pid_t;
#define _PID_T_DEFINED
#endif
// POSIX-compatible pipe spawn aliases (MSVC CRT)
#ifndef BS_POPEN
#define BS_POPEN _popen
#define BS_PCLOSE _pclose
#endif
#else
#ifndef BS_POPEN
#define BS_POPEN popen
#define BS_PCLOSE pclose
#endif
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
#include <unordered_set>
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

#include "bs-session.h"
#ifndef BS_VERSION
#define BS_VERSION "0.0.0-dev"
#endif
inline constexpr std::string_view kBridgeSessionsVersion = BS_VERSION;


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
    CuaVideoCaptureResultMsg  // 40 — 2.0.12-alpha5
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

// True if `remote` is strictly older than `local` for date-based tags
// YY.MM.DD[-betaN] (lexicographic works for zero-padded YY.MM.DD).
[[nodiscard]] inline bool version_is_older(std::string_view remote, std::string_view local) {
    auto r = version_core(remote);
    auto l = version_core(local);
    if (r.empty() || l.empty()) return false;
    if (r == l) return false;
    // Prefer pure string compare for our scheme (26.08.06-beta1 < 26.08.12-beta3).
    return r < l;
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
void serialize_msg(Serializer& s, const SessionListMsg&  m) { for (auto& si : m.sessions) { s.str_prefixed(si.name); s.str_prefixed(si.state); s.u32be(si.uptime_seconds); } }
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
    std::filesystem::file_time_type last_mtime_{};  // P2 audit fix: reload cache

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
            // Strip optional "pubkey " prefix (written by join handler)
            // Also handle leading whitespace
            while (!line.empty() && (line[0] == ' ' || line[0] == '\t')) line = line.substr(1);
            if (line.starts_with("pubkey ") || line.starts_with("pubkey\t")) {
                line = line.substr(7);
            }
            if (!line.empty()) {
                auto raw = hex_decode(line);
                if (raw.size() == 32) keys.push_back(std::move(raw));
            }
        }
    }

    // R4.1: reload from disk — called per-accept so revocations take effect
    // immediately. P2 audit fix: only re-read when the file mtime changed
    // (avoids disk I/O + TOCTOU on every accept under connection storms).
    void reload() {
        if (file_path.empty()) return;
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(file_path, ec);
        if (ec) return;
        if (mtime == last_mtime_) return;
        last_mtime_ = mtime;
        load_from_file(file_path);
    }

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

// When true (set by `bs invite`), the server accepts TLS from unknown peers
// so they can send a JoinRequest. The JoinRequest handler validates the invite
// token and adds the peer to authorized_keys. Without this, the TLS handshake
// blocks unknown peers before they can present their invite token.
inline std::atomic<bool> g_allow_join_connections{false};

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
    // Join window open: accept unknown peers so they can present an invite token.
    // The JoinRequest handler validates the token; without a valid token the
    // connection is dropped after the join exchange (no session is created).
    if (g_allow_join_connections.load(std::memory_order_relaxed)) {
        log_event("tls_verify_server", pk_hex + " result=accept (join window)");
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
    // P2 audit note (2026-08-10): TLS 1.3 is disabled because the static Windows
    // PE links OpenSSL 3.3.2 while Linux/macOS link 3.6.x — TLS 1.3 data-plane
    // breaks between them (fleet RCA 2026-08-05). TODO: once the Windows PE is
    // rebuilt against OpenSSL >= 3.6, remove this max pin to re-enable TLS 1.3.
    SSL_CTX_set_max_proto_version(ctx.get(), TLS1_2_VERSION);  // force 1.2: OpenSSL 3.3.2 vs 3.6.3 TLS1.3 data-plane breaks

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

// Bounded WANT_READ/WANT_WRITE tolerance for frame reads. select() readiness
// only guarantees *some* bytes, not a complete TLS record — large chunk frames
// split across records can surface WANT_READ mid-frame (observed pulling 1 MiB
// from a Windows peer: transfer died on the first record boundary). Retry
// briefly instead of tearing the connection down; the budget caps the stall.
inline bool ssl_want_retry(SSL* ssl, int ret, int& budget) {
    int err = SSL_get_error(ssl, ret);
    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) return false;
    if (--budget <= 0) return false;
    // Plain sleep (no select): this header's SOCKET/select compat layer is
    // defined further down, and 25 ms granularity is plenty for frame I/O.
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    return true;
}

Message read_frame(SSL* ssl) {
    int want_budget = 400;  // 400 x 25 ms = 10 s worst-case mid-frame stall
    // Read minimum header (6 bytes); extend to 8 if FLAG_LENGTH_U32.
    uint8_t header[FRAME_HEADER_SIZE_U32];
    size_t total = 0;
    while (total < FRAME_HEADER_SIZE_U16) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_read_ex(ssl, header + total, FRAME_HEADER_SIZE_U16 - total, &n);
        if (ret <= 0 && ssl_want_retry(ssl, ret, want_budget)) continue;
        ssl_check(ret, ssl, "SSL_read header");
        total += n;
    }
    size_t hdr_size = FRAME_HEADER_SIZE_U16;
    uint32_t length = 0;
    if (header[3] & FLAG_LENGTH_U32) {
        while (total < FRAME_HEADER_SIZE_U32) {
            size_t n = 0;
            clear_stale_ssl_errors_before_io();
            int ret = SSL_read_ex(ssl, header + total, FRAME_HEADER_SIZE_U32 - total, &n);
            if (ret <= 0 && ssl_want_retry(ssl, ret, want_budget)) continue;
            ssl_check(ret, ssl, "SSL_read u32 header");
            total += n;
        }
        hdr_size = FRAME_HEADER_SIZE_U32;
        length = read_u32be(header + 4);
        if (length > MAX_FRAME_PAYLOAD_U32)
            throw std::runtime_error("frame payload exceeds MAX_FRAME_PAYLOAD_U32");
    } else {
        length = read_u16(header + 4);
    }

    std::vector<uint8_t> raw(hdr_size + length);
    std::memcpy(raw.data(), header, hdr_size);

    if (length > 0) {
        total = 0;
        while (total < length) {
            size_t n = 0;
            clear_stale_ssl_errors_before_io();
            int ret = SSL_read_ex(ssl, raw.data() + hdr_size + total, length - total, &n);
            if (ret <= 0 && ssl_want_retry(ssl, ret, want_budget)) continue;
            ssl_check(ret, ssl, "SSL_read payload");
            total += n;
        }
    }

    return decode(raw);
}

// Payload length only (not including header). Requires ≥6 bytes available.
inline uint32_t detect_frame_length(const uint8_t* data, size_t available) {
    auto [hdr, length] = frame_header_layout(data, available);
    (void)hdr;
    return length;
}

[[nodiscard]] inline bool buffered_bytes_hold_complete_frame(
    std::span<const uint8_t> bytes) {
    if (bytes.size() < FRAME_HEADER_SIZE_U16) return false;
    try {
        auto [hdr, length] = frame_header_layout(bytes.data(), bytes.size());
        if (bytes.size() < hdr) return false; // need more header bytes
        return bytes.size() >= hdr + length;
    } catch (...) {
        return true; // let decode surface the protocol error
    }
}

[[nodiscard]] inline std::vector<Message> drain_complete_frames(
    std::vector<uint8_t>& buffered) {
    std::vector<Message> messages;
    size_t consumed = 0;
    while (buffered.size() - consumed >= FRAME_HEADER_SIZE_U16) {
        const uint8_t* start = buffered.data() + consumed;
        size_t avail = buffered.size() - consumed;
        size_t hdr = 0;
        uint32_t length = 0;
        try {
            auto layout = frame_header_layout(start, avail);
            hdr = layout.first;
            length = layout.second;
        } catch (...) {
            throw;
        }
        if (avail < hdr) break;
        const size_t frame_size = hdr + length;
        if (avail < frame_size) break;
        messages.push_back(decode(std::span<const uint8_t>(start, frame_size)));
        consumed += frame_size;
    }
    if (consumed > 0)
        buffered.erase(buffered.begin(), buffered.begin() + static_cast<std::ptrdiff_t>(consumed));
    return messages;
}

[[nodiscard]] inline bool ssl_has_complete_buffered_frame(SSL* ssl) {
    const int pending = SSL_pending(ssl);
    if (pending < static_cast<int>(FRAME_HEADER_SIZE_U16)) return false;
    std::array<uint8_t, FRAME_HEADER_SIZE_U32> header{};
    size_t peeked = 0;
    if (SSL_peek_ex(ssl, header.data(), FRAME_HEADER_SIZE_U16, &peeked) <= 0 ||
        peeked < FRAME_HEADER_SIZE_U16) return false;
    if (header[3] & FLAG_LENGTH_U32) {
        if (pending < static_cast<int>(FRAME_HEADER_SIZE_U32)) return false;
        size_t peeked2 = 0;
        if (SSL_peek_ex(ssl, header.data(), FRAME_HEADER_SIZE_U32, &peeked2) <= 0 ||
            peeked2 < FRAME_HEADER_SIZE_U32) return false;
    }
    return buffered_bytes_hold_complete_frame(
        std::span<const uint8_t>(header.data(),
            (header[3] & FLAG_LENGTH_U32) ? FRAME_HEADER_SIZE_U32 : FRAME_HEADER_SIZE_U16))
        || pending >= static_cast<int>(
            ((header[3] & FLAG_LENGTH_U32) ? FRAME_HEADER_SIZE_U32 : FRAME_HEADER_SIZE_U16)
            + detect_frame_length(header.data(),
                (header[3] & FLAG_LENGTH_U32) ? FRAME_HEADER_SIZE_U32 : FRAME_HEADER_SIZE_U16));
}

void write_frame(SSL* ssl, const Message& msg, uint16_t stream_id,
                 bool allow_large = false) {
    auto frame = encode(msg, stream_id, allow_large);

    size_t total = 0;
    int retries = 0;
    const int max_retries = 1000;
    // P2: exponential backoff instead of yield() busy-loop on stalled connections.
    auto backoff = std::chrono::microseconds(1000);      // start 1ms
    const auto max_backoff = std::chrono::microseconds(100000); // cap 100ms
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (total < frame.size() && retries < max_retries) {
        size_t n = 0;
        clear_stale_ssl_errors_before_io();
        int ret = SSL_write_ex(ssl, frame.data() + total, frame.size() - total, &n);
        int err = SSL_get_error(ssl, ret);
        if (ret > 0) {
            total += n;
            retries = 0;
            continue;
        }
        // v2.0.12c: retry on WANT_WRITE/WANT_READ — Windows/MinGW returns these
        // even on blocking sockets for large writes.
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
            // P2: bounded by 30s deadline — fail instead of looping forever.
            if (std::chrono::steady_clock::now() >= deadline) break;
            ++retries;
            std::this_thread::sleep_for(backoff);
            backoff = std::min(backoff * 2, max_backoff);
            continue;
        }
        ssl_check(ret, ssl, "SSL_write");
    }
    if (total < frame.size()) {
        throw std::runtime_error("SSL_write failed after retries/deadline");
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
    // P2 audit fix: 256-byte reverse lookup table instead of strchr (O(n*64)).
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static int8_t kRev[256] = {};
    static bool kRevInit = false;
    if (!kRevInit) {
        for (int i = 0; i < 256; ++i) kRev[i] = -1;
        for (size_t i = 0; i < 64; ++i) kRev[(uint8_t)kTable[i]] = (int8_t)i;
        kRevInit = true;
    }

    std::string result;
    result.reserve(b64.size() * 3 / 4);

    int acc = 0, bits = 0;
    for (char c : b64) {
        if (c == '=') break;
        int8_t v = kRev[(uint8_t)c];
        if (v < 0) continue;  // skip whitespace / invalid chars
        acc = (acc << 6) | v;
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
        // Mark this as a BridgeSessions shell session so user profiles can
        // detect and skip interactive-only features (starship, tmux auto-attach,
        // zellij, mouse tracking, etc.) that produce escape-sequence garbage.
        setenv("BS_SESSION", "1", 1);
        // Clear env vars that trigger interactive terminal features which
        // corrupt BS shell sessions with escape-sequence noise.
        unsetenv("FORCE_STARSHIP");
        unsetenv("ZELLIJ_AUTO_ATTACH");
        // Close inherited daemon FDs before exec.
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

// ── 2.0.8 P5: Cross-platform Computer Use dispatch ─────────────────────
// Dispatches CuaRequestMsg to the appropriate OS backend.

#ifndef _WIN32
[[nodiscard]] std::optional<std::string> find_binary(std::string_view name) {
    if (name.find('/') != std::string_view::npos) {
        std::filesystem::path p{name};
        if (::access(p.c_str(), X_OK) == 0) return p.string();
        return std::nullopt;
    }
    const char* path_env = std::getenv("PATH");
    if (!path_env || !*path_env) return std::nullopt;
    std::string path_str(path_env);
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(':', start);
        std::string_view part = end == std::string::npos
            ? std::string_view(path_str).substr(start)
            : std::string_view(path_str).substr(start, end - start);
        std::filesystem::path dir = part.empty() ? std::filesystem::path(".") : std::filesystem::path(part);
        std::filesystem::path candidate = dir / name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate.string();
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}
#endif // _WIN32

// CUA helper constants and helpers (used by bs-cua-helper.h)
inline constexpr uint16_t kCuaHelperPort = 19986; // Windows-only (POSIX uses Unix socket)
[[nodiscard]] inline std::string cua_helper_token_path(const std::string& app_home) {
    return (std::filesystem::path(app_home) / "cua-helper-token").string();
}

// Windows: Session-1 helper writes the token as the interactive user; the mesh
// daemon often runs as SYSTEM (Session 0). Default creator-owner ACLs can block
// the daemon from reading the token → "ERROR: auth". Grant SYSTEM + Admins read.
[[nodiscard]] inline bool grant_local_system_read(const std::string& path) {
#ifdef _WIN32
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD st = GetNamedSecurityInfoA(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, &old_dacl, nullptr, &sd);
    if (st != ERROR_SUCCESS) return false;
    PSID system_sid = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&nt_auth, 1, SECURITY_LOCAL_SYSTEM_RID,
                                  0, 0, 0, 0, 0, 0, 0, &system_sid)) {
        if (sd) LocalFree(sd);
        return false;
    }
    EXPLICIT_ACCESSA ea{};
    ea.grfAccessPermissions = FILE_GENERIC_READ;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPSTR>(system_sid);
    PACL new_dacl = nullptr;
    st = SetEntriesInAclA(1, &ea, old_dacl, &new_dacl);
    bool ok = false;
    if (st == ERROR_SUCCESS && new_dacl) {
        st = SetNamedSecurityInfoA(const_cast<char*>(path.c_str()), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
        ok = (st == ERROR_SUCCESS);
        LocalFree(new_dacl);
    }
    FreeSid(system_sid);
    if (sd) LocalFree(sd);
    return ok;
#else
    (void)path;
    return true;
#endif
}

// Write CUA helper IPC token (owner-private + SYSTEM read on Windows).
[[nodiscard]] inline bool write_cua_helper_token(const std::string& app_home,
                                                 std::string_view token) {
    const std::string path = cua_helper_token_path(app_home);
    if (!write_private_text_file(path, token)) return false;
#ifdef _WIN32
    (void)grant_local_system_read(path);
#endif
    return true;
}

// P2: Unix domain socket path for CUA helper (POSIX). Windows keeps TCP loopback.
[[nodiscard]] inline std::string cua_helper_socket_path(const std::string& app_home) {
    return (std::filesystem::path(app_home) / "cua-helper.sock").string();
}

// Cross-platform socket close helper for cua_helper_rpc
inline void close_socket(int fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

// CUA helper RPC: call the user-session helper at localhost:19986
// The helper runs in the interactive desktop session (where capture/input work).
// Returns {status=1, error} if helper not running — caller falls back.
[[nodiscard]] inline CuaResponseMsg cua_helper_rpc(const CuaRequestMsg& req, const std::string& app_home) {
    CuaResponseMsg resp;
    resp.status = 1;

    // Load auth token from app home (path helper; SYSTEM must be able to read)
    std::string token_path = cua_helper_token_path(app_home);
    std::ifstream tf(token_path);
    if (!tf) { resp.error = "no cua-helper token"; return resp; }
    std::string token((std::istreambuf_iterator<char>(tf)), std::istreambuf_iterator<char>());
    while (!token.empty() && (token.back() == '\n' || token.back() == '\r')) token.pop_back();
    if (token.empty()) { resp.error = "empty cua-helper token"; return resp; }

    // P2 security: POSIX uses Unix domain socket (filesystem perms protect token).
    // Windows: TCP loopback (named pipes are the alternative but need separate code path).
#ifdef _WIN32
    int sfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) { resp.error = "socket failed"; return resp; }
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(kCuaHelperPort);
    if (connect(sfd, (sockaddr*)&sa, sizeof(sa)) < 0) {
        close_socket(sfd);
        resp.error = "cua-helper not running (connect failed)";
        return resp;
    }
#else
    std::string sock_path = cua_helper_socket_path(app_home);
    int sfd = (int)socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { resp.error = "socket failed"; return resp; }
    sockaddr_un su{};
    su.sun_family = AF_UNIX;
    if (sock_path.size() >= sizeof(su.sun_path)) {
        close_socket(sfd);
        resp.error = "cua-helper socket path too long";
        return resp;
    }
    std::strncpy(su.sun_path, sock_path.c_str(), sizeof(su.sun_path) - 1);
    if (connect(sfd, (sockaddr*)&su, sizeof(su)) < 0) {
        close_socket(sfd);
        resp.error = "cua-helper not running (connect failed)";
        return resp;
    }
#endif
    // Recv timeout: capture (action 6) may take PowerShell+GDI ~2–8s; allow 20s.
    // Input actions stay snappy with the same budget (fail closed if helper wedged).
#ifdef _WIN32
    DWORD rcv_timeout_ms = 20000;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&rcv_timeout_ms, sizeof(rcv_timeout_ms));
#else
    struct timeval rcv_tv { 20, 0 };
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
#endif

    // Send request as JSON line
    nlohmann::json j;
    j["action"] = req.action;
    j["x"] = req.x;
    j["y"] = req.y;
    j["button"] = req.button;
    j["hid_key"] = req.hid_key;
    j["modifiers"] = req.modifiers;
    j["text"] = req.text;
    // Token as space-prefix: helper expects "TOKEN {json}\n"
    std::string line = token + " " + j.dump() + "\n";
    if (send(sfd, line.data(), (int)line.size(), 0) <= 0) {
        close_socket(sfd);
        resp.error = "cua-helper send failed";
        return resp;
    }

    // Read response
    std::string acc;
    char buf[65536];
    while (true) {
        int n = (int)recv(sfd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        acc.append(buf, n);
        if (acc.find('\n') != std::string::npos) break;
    }
    close_socket(sfd);

    if (acc.empty()) { resp.error = "cua-helper no response"; return resp; }
    try {
        auto r = nlohmann::json::parse(acc);
        resp.status = r.value("status", 1);
        resp.error = r.value("error", "");
        resp.screen_w = r.value("screen_w", 0);
        resp.screen_h = r.value("screen_h", 0);
        resp.format = r.value("format", 0);
        if (r.contains("data_b64") && r["data_b64"].is_string()) {
            // Base64 decode inline (helper sends base64-encoded image data as "data_b64")
            // P2 audit fix: 256-byte reverse lookup table instead of find() (O(n*64))
            // for large screenshots (up to 50MB).
            std::string b64 = r["data_b64"].get<std::string>();
            static const std::string b64chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            static int8_t kRev[256] = {};
            static bool kRevInit = false;
            if (!kRevInit) {
                for (int i = 0; i < 256; ++i) kRev[i] = -1;
                for (size_t i = 0; i < b64chars.size(); ++i) kRev[(uint8_t)b64chars[i]] = (int8_t)i;
                kRevInit = true;
            }
            int val = 0, valb = -8;
            for (char c : b64) {
                if (c == '=') break;
                int8_t v = kRev[(uint8_t)c];
                if (v < 0) continue;
                val = (val << 6) | v;
                valb += 6;
                if (valb >= 0) {
                    resp.data.push_back((uint8_t)((val >> valb) & 0xFF));
                    valb -= 8;
                }
            }
        }
    } catch (const std::exception& e) {
        resp.error = std::string("cua-helper parse error: ") + e.what();
    }
    return resp;
}

// Forward declarations for macOS CUA (definitions in macos-capture.mm)
#ifdef __APPLE__
#include <CoreGraphics/CoreGraphics.h>
#ifdef BS_TESTING
inline int bs_macos_capture_png(const char*, unsigned, char* error, size_t capacity) {
    if (error && capacity) std::snprintf(error, capacity, "ScreenCaptureKit test stub");
    return 0;
}
#else
extern "C" int bs_macos_capture_png(const char*, unsigned, char*, size_t);
#endif
#endif

[[nodiscard]] CuaResponseMsg cua_execute(const CuaRequestMsg& req, const std::string& app_home = "") {
    CuaResponseMsg resp;
    resp.status = 0;

#ifdef _WIN32
    // Prefer cua-helper for ALL actions (including capture). Session 0 daemons
    // cannot reliably GDI-capture the interactive desktop; the helper (user
    // session) owns BitBlt + BMP encode (no GDI+ — crashes on recent Win11).
    {
        auto helper_resp = cua_helper_rpc(req, app_home);
        if (helper_resp.status == 0) return helper_resp;
        // P1: always retain helper error for the outer message
        if (!helper_resp.error.empty()) resp.error = helper_resp.error;
    }
    // Fallback: PowerShell GDI capture for action 6 only, and only when this
    // process is already in an interactive session (Session != 0). Session 0
    // PowerShell cannot see the desktop — skipping avoids a misleading path.
    if (req.action == 6) {
        DWORD sid = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &sid);
        if (sid == 0) {
            resp.status = 1;
            if (resp.error.empty()) {
                resp.error = "windows CUA capture requires --cua-helper in Session 1 "
                             "(daemon is Session 0; PowerShell GDI fallback skipped)";
            } else {
                resp.error += "; PowerShell GDI fallback skipped (Session 0)";
            }
            return resp;
        }
        std::string tmp_path = create_private_temp_file("bsc", ".png", app_home);
        if (tmp_path.empty()) {
            resp.status = 1;
            resp.error = "failed to allocate private capture temp file";
            return resp;
        }
        // P2: load System.Windows.Forms (VirtualScreen) + System.Drawing.
        // Escape single quotes in path for PowerShell single-quoted -Command.
        std::string ps_path = tmp_path;
        for (size_t i = 0; i < ps_path.size(); ++i) {
            if (ps_path[i] == '\'') { ps_path.insert(i, "'"); ++i; }
        }
        std::string ps_cmd =
            "powershell -NoProfile -Command \""
            "Add-Type -AssemblyName System.Drawing,System.Windows.Forms;"
            "$b=[System.Drawing.Rectangle]::FromLTRB("
            "[System.Windows.Forms.SystemInformation]::VirtualScreen.Left,"
            "[System.Windows.Forms.SystemInformation]::VirtualScreen.Top,"
            "[System.Windows.Forms.SystemInformation]::VirtualScreen.Right,"
            "[System.Windows.Forms.SystemInformation]::VirtualScreen.Bottom);"
            "$img=New-Object System.Drawing.Bitmap($b.Width,$b.Height);"
            "$g=[System.Drawing.Graphics]::FromImage($img);"
            "$g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size);"
            "$g.Dispose();$img.Save('" + ps_path + "',[System.Drawing.Imaging.ImageFormat]::Png);"
            "$img.Dispose()\" 2>nul";
        int rc = std::system(ps_cmd.c_str());
        if (rc != 0 || !std::filesystem::exists(tmp_path)) {
            resp.status = 1;
            // P1: never drop the helper error under a generic message
            if (resp.error.empty()) {
                resp.error = "windows screen capture failed (helper unreachable; "
                             "PowerShell GDI fallback also failed)";
            } else {
                resp.error += "; PowerShell GDI fallback also failed";
            }
            return resp;
        }
        std::ifstream cap(tmp_path, std::ios::binary | std::ios::ate);
        if (!cap) {
            resp.status = 1;
            resp.error = (resp.error.empty() ? "" : resp.error + "; ") +
                         "failed to open capture file";
            ::unlink(tmp_path.c_str()); return resp;
        }
        auto cap_size = cap.tellg();
        if (cap_size > 0 && static_cast<size_t>(cap_size) <= MAX_IMAGE_BYTES) {
            resp.data.resize(static_cast<size_t>(cap_size));
            cap.seekg(0);
            cap.read(reinterpret_cast<char*>(resp.data.data()), cap_size);
            resp.format = 0; resp.status = 0;  // PNG
            resp.screen_w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            resp.screen_h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            resp.error.clear();
        } else {
            resp.status = 1;
            resp.error = (resp.error.empty() ? "" : resp.error + "; ") +
                         "capture empty or exceeds 50MB";
        }
        ::unlink(tmp_path.c_str());
        return resp;
    }
    resp.status = 1;
    if (resp.error.empty())
        resp.error = "windows CUA requires cua-helper (run: bridgesessions --cua-helper in user session)";
    return resp;
#elif defined(__APPLE__)
    // macOS: route through cua-helper (runs in user session for desktop access).
    // The helper uses CGEvent for input injection and ScreenCaptureKit for capture.
    {
        auto helper_resp = cua_helper_rpc(req, app_home);
        if (helper_resp.status == 0) return helper_resp;
        // Fall through to in-process fallback
    }
    // In-process fallback (daemon running in user session)
    if (req.action == 0) {
        // Screen dimensions via CoreGraphics (no subprocess needed)
        auto r = CGDisplayBounds(CGMainDisplayID());
        resp.screen_w = (int16_t)r.size.width;
        resp.screen_h = (int16_t)r.size.height;
        resp.status = 0;
        return resp;
    }
    if (req.action == 6) {
        // Capture via ScreenCaptureKit (macos-capture.mm), with screencapture(1)
        // fallback when TCC blocks SCK ("user declined" / no shareable displays).
        // Default to failure — do not leave status=0 with empty data (CLI then
        // prints "no capture data returned" with a false success).
        resp.status = 1;
        std::string tmp_owned = create_private_temp_file("cua", ".png", app_home);
        if (tmp_owned.empty()) {
            resp.error = "macOS capture failed: cannot create private temp file";
            return resp;
        }
        const char* tmp = tmp_owned.c_str();
        char err[1024] = {};
        bool got_png = bs_macos_capture_png(tmp, 0, err, sizeof(err));
        if (!got_png) {
            std::string sc = "/usr/sbin/screencapture -x -t png '";
            sc += tmp;
            sc += "' 2>/dev/null";
            if (std::system(sc.c_str()) == 0 && std::filesystem::exists(tmp) &&
                std::filesystem::file_size(tmp) > 0) {
                got_png = true;
                err[0] = '\0';
            }
        }
        if (got_png) {
            std::ifstream cap(tmp, std::ios::binary | std::ios::ate);
            auto sz = cap.tellg();
            if (sz > 0 && (size_t)sz <= MAX_IMAGE_BYTES) {
                resp.data.resize((size_t)sz);
                cap.seekg(0);
                cap.read((char*)resp.data.data(), sz);
                resp.format = 1;
                resp.status = 0;
                resp.error.clear();
                auto r = CGDisplayBounds(CGMainDisplayID());
                resp.screen_w = (int16_t)r.size.width;
                resp.screen_h = (int16_t)r.size.height;
            } else {
                resp.error = "macOS capture empty/oversize";
            }
        } else {
            resp.error = std::string("macOS capture failed (grant Screen Recording to "
                                     "BridgeSessions.app if needed): ") +
                         (err[0] ? err : "ScreenCaptureKit / screencapture failed");
        }
        ::unlink(tmp);
        return resp;
    }
    resp.status = 1;
    resp.error = "macos CUA input requires cua-helper (run: bridgesessions --cua-helper in user session)";
    return resp;
#else
    // Linux: dispatch via xdotool (ubiquitous on X11 desktops).
    std::string cmd;
    switch (req.action) {
        case 0: { // screen info
            std::string out;
            FILE* p = popen("xdpyinfo 2>/dev/null | grep dimensions", "r");
            if (p) { char buf[256]; while (fgets(buf, sizeof(buf), p)) out += buf; pclose(p); }
            if (!out.empty()) {
                // Parse "  dimensions:    1920x1080 pixels" (or "1920 x 1080").
                // IMPORTANT: do not search for 'x' from offset 0 — that hits the
                // letter x inside the word "dimensions".
                auto dim = out.find("dimensions");
                auto px = (dim == std::string::npos) ? std::string::npos
                                                     : out.find('x', dim);
                if (px != std::string::npos && px > 0) {
                    // Walk back over spaces to the start of the width number
                    size_t w_end = px;
                    while (w_end > 0 && out[w_end - 1] == ' ') --w_end;
                    size_t w_start = w_end;
                    while (w_start > 0 && std::isdigit(static_cast<unsigned char>(out[w_start - 1])))
                        --w_start;
                    size_t h_start = px + 1;
                    while (h_start < out.size() && out[h_start] == ' ') ++h_start;
                    size_t h_end = h_start;
                    while (h_end < out.size() && std::isdigit(static_cast<unsigned char>(out[h_end])))
                        ++h_end;
                    if (w_end > w_start && h_end > h_start) {
                        resp.screen_w = static_cast<int16_t>(
                            std::stoul(out.substr(w_start, w_end - w_start)));
                        resp.screen_h = static_cast<int16_t>(
                            std::stoul(out.substr(h_start, h_end - h_start)));
                        resp.status = 0;
                        return resp;
                    }
                }
            }
            resp.status = 1; resp.error = "cannot determine screen size";
            return resp;
        }
        case 1: // key press
            cmd = "xdotool key --delay 0 " + std::to_string(req.hid_key) + " 2>/dev/null";
            break;
        case 2: { // text entry
            // POSIX single-quote escape: close, escaped-quote, reopen ('\'').
            // A backslash-prefix does NOT escape inside single quotes — the
            // old escaping allowed full shell injection via req.text
            // (2.0.8 MoA P0, two-lane consensus).
            std::string escaped = "'";
            for (char ch : req.text) {
                if (ch == '\'') escaped += "'\\''";
                else escaped += ch;
            }
            escaped += "'";
            cmd = "xdotool type --delay 0 " + escaped + " 2>/dev/null";
            break;
        }
        case 3: // mouse move
            cmd = "xdotool mousemove " + std::to_string(req.x) + " "
                + std::to_string(req.y) + " 2>/dev/null";
            break;
        case 4: // mouse button
            cmd = std::string("xdotool click ") + std::to_string(req.button) + " 2>/dev/null";
            break;
        case 5: // mouse wheel
            cmd = std::string("xdotool click ") + ((req.button == 0) ? "4" : "5") + " 2>/dev/null";
            break;
        case 6: { // screen capture — v2.0.11 P5c
            // Tool order matters: ImageMagick `import` often "succeeds" with a
            // 1-bit grayscale black PNG under XFCE/Xvfb (~263B for 1280x800) while
            // scrot returns a real RGB framebuffer. Prefer grim (Wayland) then
            // scrot, and only then import as last resort.
            // Ensure DISPLAY for X11 tools when daemon lacks a session env.
            if (const char* d = std::getenv("DISPLAY"); !d || !*d) {
                setenv("DISPLAY", ":0", 0);
                // Common agent desktop paths (bs-qa-ubuntu XFCE uses :1)
                if (std::filesystem::exists("/tmp/.X11-unix/X1"))
                    setenv("DISPLAY", ":1", 1);
                else if (std::filesystem::exists("/tmp/.X11-unix/X0"))
                    setenv("DISPLAY", ":0", 1);
            }
            std::string tmp_path;
            const char* tools[] = {"grim", "scrot", "import", nullptr};
            for (int i = 0; tools[i]; ++i) {
                auto bin = find_binary(tools[i]);
                if (!bin) continue;
                tmp_path = create_private_temp_file("cap", ".png", app_home);
                if (tmp_path.empty()) continue;
                std::string capture_cmd;
                if (std::string(tools[i]) == "grim") {
                    capture_cmd = *bin + " '" + tmp_path + "' 2>/dev/null";
                } else if (std::string(tools[i]) == "import") {
                    capture_cmd = *bin + " -window root '" + tmp_path + "' 2>/dev/null";
                } else {
                    capture_cmd = *bin + " '" + tmp_path + "' 2>/dev/null";
                }
                if (std::system(capture_cmd.c_str()) != 0) {
                    ::unlink(tmp_path.c_str());
                    tmp_path.clear();
                    continue;
                }
                // Reject tiny / 1-bit "success" captures and try next tool.
                std::error_code ec;
                auto sz = std::filesystem::file_size(tmp_path, ec);
                if (ec || sz < 1024) {
                    ::unlink(tmp_path.c_str());
                    tmp_path.clear();
                    continue;
                }
                break;
            }
            if (tmp_path.empty()) {
                resp.status = 1;
                resp.error = "no screen capture tool available (install grim, scrot, or imagemagick)";
                return resp;
            }
            // Prefer JPEG for mesh (macOS path does the same): smaller frames,
            // reliable +frm2 transport. Fall back to PNG if convert missing.
            std::string out_path = tmp_path;
            uint8_t out_format = 1;  // PNG
            {
                std::string jpg = tmp_path + ".jpg";
                std::string conv;
                if (auto c = find_binary("convert")) {
                    conv = *c + " '" + tmp_path +
                           "' -colorspace sRGB -type TrueColor -resize '1280x1280>' -quality 40 '" +
                           jpg + "' 2>/dev/null";
                } else if (auto m = find_binary("magick")) {
                    conv = *m + " '" + tmp_path +
                           "' -colorspace sRGB -type TrueColor -resize '1280x1280>' -quality 40 '" +
                           jpg + "' 2>/dev/null";
                }
                if (!conv.empty() && std::system(conv.c_str()) == 0 &&
                    std::filesystem::exists(jpg) &&
                    std::filesystem::file_size(jpg) > 0) {
                    ::unlink(tmp_path.c_str());
                    out_path = jpg;
                    out_format = 2;  // JPEG
                }
            }
            std::ifstream cap(out_path, std::ios::binary | std::ios::ate);
            if (!cap) {
                resp.status = 1;
                resp.error = "failed to open capture temp file";
                ::unlink(out_path.c_str());
                return resp;
            }
            auto cap_size = cap.tellg();
            if (cap_size > 0 && static_cast<size_t>(cap_size) <= MAX_IMAGE_BYTES) {
                resp.data.resize(static_cast<size_t>(cap_size));
                cap.seekg(0);
                cap.read(reinterpret_cast<char*>(resp.data.data()), cap_size);
                resp.format = out_format;
                resp.status = 0;
                FILE* xr = popen(
                    "xdpyinfo 2>/dev/null | grep dimensions", "r");
                if (xr) {
                    char dims[256] = {};
                    if (fgets(dims, sizeof(dims), xr)) {
                        // Parse after "dimensions" so we don't match the 'x' in the word.
                        std::string out = dims;
                        auto dim = out.find("dimensions");
                        auto px = (dim == std::string::npos) ? std::string::npos
                                                             : out.find('x', dim);
                        if (px != std::string::npos && px > 0) {
                            int w = 0, h = 0;
                            // walk back for width digits
                            size_t i = px;
                            while (i > 0 && std::isdigit(static_cast<unsigned char>(out[i - 1]))) --i;
                            w = std::atoi(out.c_str() + i);
                            h = std::atoi(out.c_str() + px + 1);
                            if (w > 0 && h > 0) {
                                resp.screen_w = (int16_t)w;
                                resp.screen_h = (int16_t)h;
                            }
                        }
                    }
                    pclose(xr);
                }
            } else {
                resp.status = 1;
                resp.error = "capture file empty or exceeds size limit";
            }
            ::unlink(out_path.c_str());
            return resp;
        }
        default:
            resp.status = 1;
            resp.error = "unknown CUA action " + std::to_string(req.action);
            return resp;
    }
    if (!cmd.empty() && std::system(cmd.c_str()) != 0) {
        resp.status = 1;
        resp.error = "xdotool failed";
    }
    return resp;
#endif
}

// ── 2.0.12: Video capture via ffmpeg ─────────────────────────────
// (bs_macos_capture_png forward declaration moved to top of file near cua_execute)
[[nodiscard]] CuaVideoCaptureResultMsg video_capture_execute(const CuaVideoCaptureMsg& req) {
    CuaVideoCaptureResultMsg result;
    result.request_id = req.request_id;
    result.status = 1;

#ifdef _WIN32
    std::string tmp_path = create_private_temp_file("bsv", ".mp4");
    if (tmp_path.empty()) {
        result.error = "failed to allocate private video temp file";
        return result;
    }
    std::string cmd =
        "ffmpeg -y -f gdigrab -framerate " + std::to_string(req.fps) +
        " -t " + std::to_string(req.duration_sec) +
        " -i desktop -c:v libx264 -preset ultrafast -crf 28 -pix_fmt yuv420p \"" +
        tmp_path + "\" 2>nul";
#elif defined(__APPLE__)
    std::string tmp_path = create_private_temp_file("bsv", ".mp4");
    if (tmp_path.empty()) {
        result.error = "failed to allocate private video temp file";
        return result;
    }
    const std::string frames_dir = tmp_path + ".frames";
    std::error_code frames_ec;
    std::filesystem::create_directories(frames_dir, frames_ec);
    if (frames_ec) {
        result.error = "failed to create macOS capture frame directory";
        return result;
    }

    const uint32_t fps = (std::max)(uint32_t{1}, static_cast<uint32_t>(req.fps));
    const uint32_t duration = (std::max)(uint32_t{1}, static_cast<uint32_t>(req.duration_sec));
    const uint32_t max_width = req.max_width == 0 ? 1920u : static_cast<uint32_t>(req.max_width);
    const uint64_t frame_count = static_cast<uint64_t>(fps) * duration;
    const auto interval = std::chrono::duration<double>(1.0 / static_cast<double>(fps));
    auto next_frame = std::chrono::steady_clock::now();
    for (uint64_t frame = 0; frame < frame_count; ++frame) {
        char filename[64]{};
        std::snprintf(filename, sizeof(filename), "frame-%06llu.png",
                      static_cast<unsigned long long>(frame));
        const std::string frame_path =
            (std::filesystem::path(frames_dir) / filename).string();
        char capture_error[1024]{};
        if (!bs_macos_capture_png(frame_path.c_str(), max_width,
                                  capture_error, sizeof(capture_error))) {
            std::filesystem::remove_all(frames_dir, frames_ec);
            result.error = "ScreenCaptureKit failed: " + std::string(capture_error);
            return result;
        }
        next_frame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        std::this_thread::sleep_until(next_frame);
    }

    auto ffmpeg = find_binary("/opt/homebrew/bin/ffmpeg");
    if (!ffmpeg) ffmpeg = find_binary("/usr/local/bin/ffmpeg");
    if (!ffmpeg) ffmpeg = find_binary("ffmpeg");
    std::string cmd;
    if (ffmpeg) {
        cmd = *ffmpeg + " -hide_banner -loglevel error -y -framerate " +
              std::to_string(fps) + " -i '" + frames_dir +
              "/frame-%06d.png' -c:v libx264 -preset ultrafast -crf 28" +
              " -pix_fmt yuv420p '" + tmp_path + "' 2>'" +
              (private_tmp_dir() + "/bs-video-ffmpeg.log") + "'";
    }
#else
    std::string tmp_path = create_private_temp_file("bsv", ".mp4");
    if (tmp_path.empty()) {
        result.error = "failed to allocate private video temp file";
        return result;
    }
    auto ffmpeg = find_binary("ffmpeg");
    std::string cmd;
    if (ffmpeg) {
        cmd = *ffmpeg + " -y -f x11grab -framerate " + std::to_string(req.fps) +
              " -t " + std::to_string(req.duration_sec) +
              " -i :0.0 -c:v libx264 -preset ultrafast -crf 28 -pix_fmt yuv420p '" +
              tmp_path + "' 2>/dev/null";
    }
#endif
    if (cmd.empty()) {
        result.error = "ffmpeg not available for video capture";
        return result;
    }
    int rc = std::system(cmd.c_str());
    if (rc != 0 || !std::filesystem::exists(tmp_path)) {
#ifdef __APPLE__
        result.error = "macOS video capture failed (ffmpeg exit " + std::to_string(rc) +
                       "); verify Screen Recording permission for bridgesessions/ffmpeg";
#else
        result.error = "video capture failed (ffmpeg exit " + std::to_string(rc) + ")";
#endif
        return result;
    }
    result.status = 0;
    result.file_path = tmp_path;
    result.duration_sec = req.duration_sec;
    result.format = 1; // mp4
#ifdef __APPLE__
    // P2 audit fix: frames_dir (~450MB of PNGs per capture) leaked on success.
    // Remove the intermediate frame directory now that the mp4 is produced.
    std::error_code cleanup_ec;
    std::filesystem::remove_all(frames_dir, cleanup_ec);
#endif
    return result;
}

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
    int startup_wait_secs = 30;  // boot-time network readiness gate (0 = skip)
    std::string receive_dir_override;  // override default received/ path (for SYSTEM daemons)
    int ping_interval_secs = 5;
    int pong_timeout_secs = 30;
    // When true, offer `bridgesessions upgrade` to peers that reconnect with an
    // older Hello.version than this node (cooldown-limited). Peers that were
    // offline during a fleet upgrade catch up automatically when they return.
    bool auto_upgrade = true;
    // Minimum seconds between auto-upgrade attempts for the same peer.
    int auto_upgrade_cooldown_secs = 3600;
    // When true (default), outbound seed/discovered dials require pubkey= pin and
    // post-handshake cert/Hello identity binding. TLS fingerprint TOFU alone is
    // not sufficient for mesh trust (independent review 2026-07-16 P0-1).
    bool require_seed_pins = true;
    // Seconds a discovered (runtime-learned) peer may stay silent before it is
    // pruned. Durable `seed` peers are exempt — they persist offline and are
    // expected to return. 0 = keep discovered peers indefinitely (no pruning).
    int discovered_ttl_secs = 900;
    // Hard cap on inbound file transfer size (bytes). 0 = unlimited.
    // Default 8 GiB so 500MB+ agent artifacts work; override with transfer.max_bytes.
    uint64_t transfer_max_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    // Serve/overwrite identity, authorized_keys, tokens, *.pem/*.key.
    bool allow_sensitive_paths = false;
    // scp --dest may target $HOME / temp. Default is receive_dir only.
    bool dest_allow_home = false;
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
        } else if (key_str == "mesh.startup_wait_secs") {
            auto v = parse_int(val);
            if (v.has_value() && *v >= 0) cfg.startup_wait_secs = *v;
        } else if (key_str == "receive_dir") {
            cfg.receive_dir_override = val;
        } else if (key_str == "mesh.ping_interval_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.ping_interval_secs = *v;
        } else if (key_str == "mesh.pong_timeout_secs") {
            auto v = parse_int(val);
            if (v.has_value()) cfg.pong_timeout_secs = *v;
        } else if (key_str == "mesh.auto_upgrade") {
            std::string s(val);
            for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            cfg.auto_upgrade = !(s == "false" || s == "0" || s == "no" || s == "off");
        } else if (key_str == "mesh.auto_upgrade_cooldown_secs") {
            auto v = parse_int(val);
            if (v.has_value() && *v >= 60) cfg.auto_upgrade_cooldown_secs = *v;
        } else if (key_str == "mesh.require_seed_pins") {
            std::string s(val);
            for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            cfg.require_seed_pins = !(s == "false" || s == "0" || s == "no" || s == "off");
        } else if (key_str == "mesh.discovered_ttl_secs") {
            auto v = parse_int(val);
            if (v.has_value() && *v >= 0) cfg.discovered_ttl_secs = *v;
        } else if (key_str == "mesh.mdns_enabled") {
            std::string_view t = trim(val);
            cfg.mdns_enabled = (t == "true" || t == "1" || t == "yes");
        } else if (key_str == "transfer.max_bytes") {
            try {
                cfg.transfer_max_bytes = static_cast<uint64_t>(std::stoull(std::string(val)));
            } catch (...) {}
        } else if (key_str == "transfer.allow_sensitive_paths") {
            std::string_view t = trim(val);
            cfg.allow_sensitive_paths =
                (t == "true" || t == "1" || t == "yes" || t == "on");
        } else if (key_str == "file.dest_allow_home") {
            std::string_view t = trim(val);
            cfg.dest_allow_home =
                (t == "true" || t == "1" || t == "yes" || t == "on");
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
            // P3 audit fix: split from the RIGHT so vfolder names containing
            // dots (e.g. 'vfolder.my.app.local_path') parse correctly.
            // The key is the segment after the last dot.
            auto dot = rest.rfind('.');
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
        // ── unknown keys: log a warning so config typos are not silent ──
        else {
            // P3 audit fix: surface typos (e.g. 'nodge.name') instead of
            // silently ignoring them. log_event is forward-declared above.
            static std::once_flag once;
            std::call_once(once, [&]{
                log_event("config_unknown_key", key_str);
            });
        }
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

// ── Peer name resolution: fuzzy matching helpers ──────────────
// NO-FALLBACK CONTRACT: bs resolves peer names, connects, and either
// succeeds or fails with diagnostics. It NEVER invokes ssh/winrm/telnet
// as a fallback transport. There is no code path that does so. This is
// a design invariant — see AUDIT-MOA-2026-08-06.md.

// Classic two-row Wagner-Fischer. Returns edit distance.
[[nodiscard]] inline size_t levenshtein(const std::string& a,
                                        const std::string& b) {
    const auto m = a.size(), n = b.size();
    if (m == 0) return n;
    if (n == 0) return m;
    std::vector<size_t> prev(n + 1), curr(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = j;
    for (size_t i = 1; i <= m; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= n; ++j) {
            size_t cost = std::tolower(static_cast<unsigned char>(a[i - 1])) !=
                          std::tolower(static_cast<unsigned char>(b[j - 1]));
            curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
        }
        prev.swap(curr);
    }
    return prev[n];
}

// Case-insensitive suffix/prefix check for tier-3 matching.
[[nodiscard]] inline bool name_has_segment(const std::string& name,
                                           const std::string& query) {
    if (query.empty() || name.size() < query.size()) return false;
    auto ic_eq = [](unsigned char a, unsigned char b) {
        return std::tolower(a) == std::tolower(b);
    };
    // Exact suffix: "shadow" matches "lab-shadow"
    if (name.size() > query.size() &&
        name[name.size() - query.size() - 1] == '-' &&
        std::equal(query.rbegin(), query.rend(), name.rbegin(), ic_eq))
        return true;
    // Exact prefix: "shadow" matches "shadow-df8uluc8"
    if (name.size() > query.size() &&
        name[query.size()] == '-' &&
        std::equal(query.begin(), query.end(), name.begin(), ic_eq))
        return true;
    return false;
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

    // Only copy pubkey when exactly ONE peer has that addr (multi-peer shared
    // host must not blindly inherit another peer's key — name collision fix).
    auto copy_identity_for_addr = [&](const std::vector<PeerEntry>& peers) {
        const PeerEntry* match = nullptr;
        int count = 0;
        for (const auto& peer : peers) {
            if (peer.addr == resolved_addr && !peer.pubkey_hex.empty()) {
                match = &peer;
                ++count;
            }
        }
        return (count == 1) ? match->pubkey_hex : std::string{};
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
    // 2.0.8 MoA fix: reject control chars (log/IPC injection via node name).
    for (unsigned char ch : hello_name)
        if (ch < 0x20 || ch == 0x7f)
            return {false, "Hello node name contains control characters"};
    // Name is an operational label; identity is the pinned key. Allow rename
    // when the pin already matched (e.g. seed "windows-peer" announces as
    // "windows-peer-2"). Reject only when there is no pin to bind identity.
    if (!expected_name.empty() &&
        !config_peer_name_eq(expected_name, hello_name)) {
        if (expected_pubkey.empty() ||
            !peer_identity_matches(expected_pubkey, cert_pubkey)) {
            return {false, "Hello node name does not match expected peer name"};
        }
        // pinned key OK — accept Hello name (conn.peer_name becomes hello_name)
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
    // 2.0.8 MoA fix: reject control chars in node names — a \n-bearing name
    // forges log lines (log injection) and breaks line-oriented IPC replies.
    for (unsigned char ch : hello_name)
        if (ch < 0x20 || ch == 0x7f)
            return {false, "Hello node name contains control characters"};

    auto check_peer = [&](const PeerEntry& peer) -> std::optional<std::string> {
        // Name collision fix: do NOT reject when cert_pubkey matches a trusted
        // key that belongs to a differently-named peer. Multiple peers may share
        // a host/key legitimately. The name→key binding check below still guards
        // against a different key claiming an existing name.
        if (!peer.pubkey_hex.empty() &&
            config_peer_name_eq(peer.name, hello_name) &&
            peer.pubkey_hex != cert_pubkey) {
            return "Hello node name is pinned to a different certificate key";
        }
        return std::nullopt;
    };
    bool matched_authoritative_seed = false;
    for (const auto& peer : cfg.seeds) {
        if (auto reason = check_peer(peer)) return {false, *reason};
        if (!peer.pubkey_hex.empty() && peer.pubkey_hex == cert_pubkey &&
            config_peer_name_eq(peer.name, hello_name)) {
            matched_authoritative_seed = true;
        }
    }
    // Explicit seed pins are operator-controlled and authoritative. Gossip may
    // retain a peer's previous key after a legitimate rotation; once the seed
    // name/key pair matches, stale discovered entries must not poison that
    // identity indefinitely.
    if (matched_authoritative_seed) return {true, {}};
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

// Resolve a remote FileRequest path to a local filesystem path.
// Relative requests are tried under receive_dir; common client mistakes that
// re-prefix `.bridgesessions/received/` or `received/` are stripped so
// meshmon-style probes do not double-nest (see test_file_path_sanitization).
[[nodiscard]] inline std::string resolve_file_request_path(
        const std::string& path, const std::string& receive_dir) {
    namespace fs = std::filesystem;
    if (path.empty()) return {};

    auto exists_file = [](const std::string& p) -> bool {
        std::error_code ec;
        return !p.empty() && fs::exists(p, ec) && !fs::is_directory(p, ec);
    };

    std::vector<std::string> candidates;
    candidates.reserve(8);

    const std::string expanded = expand_home(path);
    candidates.push_back(expanded);

    fs::path as_path(expanded);
    if (!as_path.is_absolute()) {
        // Basename-only under receive_dir (most common agent/meshmon form).
        candidates.push_back((fs::path(receive_dir) / as_path.filename()).string());
        // Full relative path under receive_dir.
        candidates.push_back((fs::path(receive_dir) / as_path).string());

        // Strip accidental receive-dir prefixes clients re-send.
        auto strip_and_join = [&](std::string_view prefix) {
            std::string rel = expanded;
            // Accept either POSIX or Windows separators in the prefix match.
            if (rel.size() >= prefix.size()) {
                bool match = true;
                for (size_t i = 0; i < prefix.size(); ++i) {
                    char a = rel[i];
                    char b = prefix[i];
                    if (a == '\\') a = '/';
                    if (b == '\\') b = '/';
                    if (a != b) { match = false; break; }
                }
                if (match) {
                    rel = rel.substr(prefix.size());
                    while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
                        rel.erase(rel.begin());
                    if (!rel.empty())
                        candidates.push_back((fs::path(receive_dir) / rel).string());
                }
            }
        };
        strip_and_join(".bridgesessions/received/");
        strip_and_join("bridgesessions/received/");
        strip_and_join("received/");
        strip_and_join(".bridgesessions\\received\\");
        strip_and_join("received\\");
    }

    for (const auto& c : candidates) {
        if (exists_file(c)) return c;
    }
    // Fallback for error messages: keep the relative tree under receive_dir
    // (basename-only is only preferred when that candidate actually exists).
    if (!as_path.is_absolute())
        return (fs::path(receive_dir) / as_path).string();
    return expanded;
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

// Resolve scp-style file-send destination on the receiver.
// - empty → nullopt (caller uses receive_dir/basename)
// - relative → receive_dir / dest
// - ~/… or absolute → expand, then require containment under receive_dir
//   (default) or, when allow_home_tmp, also $HOME / temp.
// Rejects ".." segments and escapes.
[[nodiscard]] inline std::optional<std::string> resolve_file_send_dest(
    std::string_view dest_req,
    const std::string& receive_dir,
    bool allow_home_tmp = false) {
    if (dest_req.empty()) return std::nullopt;
    if (dest_req.size() > 1024) return std::nullopt;
    for (unsigned char c : dest_req) {
        if (c < 32 || c == 127) return std::nullopt;
    }
    {
        std::string scan(dest_req);
        for (char& c : scan) if (c == '\\') c = '/';
        size_t i = 0;
        while (i < scan.size()) {
            while (i < scan.size() && scan[i] == '/') ++i;
            size_t j = i;
            while (j < scan.size() && scan[j] != '/') ++j;
            std::string_view part(scan.data() + i, j - i);
            if (part == "..") return std::nullopt;
            i = j;
        }
    }
    namespace fs = std::filesystem;
    std::string dest(dest_req);
    std::string candidate;
    const bool abs =
        (!dest.empty() && (dest[0] == '/' || dest[0] == '\\')) ||
        (dest.size() >= 2 && std::isalpha(static_cast<unsigned char>(dest[0])) &&
         dest[1] == ':');
    if (!dest.empty() && dest[0] == '~') {
        candidate = expand_home(dest);
    } else if (abs) {
        candidate = dest;
    } else {
        candidate = (fs::path(receive_dir) / dest).lexically_normal().string();
    }
    std::vector<std::string> roots;
    roots.push_back(expand_home(receive_dir));
    if (allow_home_tmp) {
        roots.push_back(expand_home("~"));
#ifdef _WIN32
        if (const char* t = std::getenv("TEMP")) roots.emplace_back(t);
        if (const char* t = std::getenv("TMP")) roots.emplace_back(t);
        roots.emplace_back("C:\\Windows\\Temp");
#else
        roots.emplace_back("/tmp");
#endif
    }
    for (const auto& root : roots) {
        if (root.empty()) continue;
        if (path_is_inside_directory(candidate, root)) return candidate;
    }
    return std::nullopt;
}

// ── Host metrics (for `bs fleet` + ServerInfo gossip) ─────────────
struct HostStats {
    double load1 = -1.0;       // 1-min loadavg; -1 = N/A
    double cpu_pct = -1.0;     // 0–100; -1 until second sample
    double mem_pct = -1.0;
    double disk_pct = -1.0;
    uint64_t mem_used_mb = 0;
    uint64_t mem_total_mb = 0;
    uint64_t disk_used_gb = 0;
    uint64_t disk_total_gb = 0;
    int ncpu = 0;
    std::string os;
    std::string arch;
    bool cua_helper = false;   // local CUA helper reachable
    std::string primary_addr;  // best non-loopback advertise IP (no port)
};

// Best-effort primary IPv4 for fleet "self" row (prefer 100.x Tailscale).
[[nodiscard]] inline std::string primary_advertise_ip() {
#if defined(_WIN32)
    // Prefer GetAdaptersAddresses; fall back to empty (CLI shows listen).
    return {};
#else
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0 || !ifa) return {};
    std::string ts, other;
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN] = {};
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        if (!inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;
        std::string ip(buf);
        if (ip.rfind("100.", 0) == 0) { ts = ip; break; }
        if (other.empty() && ip.rfind("127.", 0) != 0) other = ip;
    }
    freeifaddrs(ifa);
    return !ts.empty() ? ts : other;
#endif
}

// True when local CUA helper token+socket/port responds (quick connect probe).
[[nodiscard]] inline bool cua_helper_reachable(const std::string& app_home) {
    if (app_home.empty()) return false;
    namespace fs = std::filesystem;
#ifdef _WIN32
    // Token file present + TCP connect to helper port.
    if (!fs::exists(cua_helper_token_path(app_home))) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(kCuaHelperPort);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    bool ok = false;
    if (rc == 0) ok = true;
    else if (WSAGetLastError() == WSAEWOULDBLOCK || WSAGetLastError() == WSAEINPROGRESS) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(s, &wfds);
        timeval tv{0, 200000};
        ok = select(0, nullptr, &wfds, nullptr, &tv) > 0;
    }
    closesocket(s);
    return ok;
#else
    std::string sock = cua_helper_socket_path(app_home);
    if (!fs::exists(sock)) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    if (sock.size() >= sizeof(sa.sun_path)) { close(fd); return false; }
    std::strncpy(sa.sun_path, sock.c_str(), sizeof(sa.sun_path) - 1);
    // Short connect timeout via nonblock + select
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    bool ok = false;
    if (rc == 0) ok = true;
    else if (errno == EINPROGRESS) {
        fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
        timeval tv{0, 200000};
        ok = select(fd + 1, nullptr, &wfds, nullptr, &tv) > 0;
    }
    close(fd);
    return ok;
#endif
}

[[nodiscard]] inline std::string host_stats_platform_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unix";
#endif
}

[[nodiscard]] inline std::string host_stats_platform_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

// Compact JSON for wire + fleet table. Omits unknown (-1) pct fields as null.
[[nodiscard]] inline std::string host_stats_to_json(const HostStats& h) {
    auto num_or_null = [](double v) -> std::string {
        if (v < 0) return "null";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", v);
        return buf;
    };
    std::ostringstream o;
    o << "{\"cpu\":" << num_or_null(h.cpu_pct)
      << ",\"mem\":" << num_or_null(h.mem_pct)
      << ",\"disk\":" << num_or_null(h.disk_pct)
      << ",\"load\":" << num_or_null(h.load1)
      << ",\"os\":\"" << h.os << "\""
      << ",\"arch\":\"" << h.arch << "\""
      << ",\"ncpu\":" << h.ncpu
      << ",\"mem_mb\":" << h.mem_total_mb
      << ",\"disk_gb\":" << h.disk_total_gb
      << ",\"cua\":" << (h.cua_helper ? "true" : "false")
      << "}";
    return o.str();
}

// Sample host CPU / mem / disk. CPU uses a static previous tick so the first
// call may return cpu_pct=-1; subsequent calls (gossip ~30s) are accurate.
// app_home: optional path for CUA helper probe (default ~/.bridgesessions).
[[nodiscard]] inline HostStats collect_host_stats(const std::string& app_home = {}) {
    HostStats h;
    h.os = host_stats_platform_os();
    h.arch = host_stats_platform_arch();
    h.ncpu = static_cast<int>(std::thread::hardware_concurrency());
    h.primary_addr = primary_advertise_ip();
    {
        std::string home = app_home.empty() ? expand_home("~/.bridgesessions") : app_home;
        h.cua_helper = cua_helper_reachable(home);
    }

#if !defined(_WIN32)
    {
        double la[3] = {0, 0, 0};
        if (getloadavg(la, 3) > 0) h.load1 = la[0];
    }
    // Memory
#if defined(__APPLE__)
    {
        mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
        vm_statistics64_data_t vm;
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                              reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS) {
            int64_t pagesize = sysconf(_SC_PAGESIZE);
            if (pagesize <= 0) pagesize = 4096;
            uint64_t total_pages = 0;
            size_t len = sizeof(total_pages);
            int mib[2] = {CTL_HW, HW_MEMSIZE};
            uint64_t memsize = 0;
            size_t mlen = sizeof(memsize);
            if (sysctl(mib, 2, &memsize, &mlen, nullptr, 0) == 0 && memsize > 0) {
                // free + inactive + speculative ≈ available
                uint64_t free_pages = static_cast<uint64_t>(vm.free_count) +
                                      static_cast<uint64_t>(vm.inactive_count) +
                                      static_cast<uint64_t>(vm.speculative_count);
                uint64_t free_b = free_pages * static_cast<uint64_t>(pagesize);
                h.mem_total_mb = memsize / (1024 * 1024);
                h.mem_used_mb = (memsize > free_b)
                    ? (memsize - free_b) / (1024 * 1024) : 0;
                if (h.mem_total_mb > 0)
                    h.mem_pct = 100.0 * static_cast<double>(h.mem_used_mb) /
                                static_cast<double>(h.mem_total_mb);
            }
            (void)total_pages;
        }
        // CPU via host_statistics. Tick delta needs two samples; a zero-delta
        // call (FLEET + gossip racing, or two back-to-back FLEET seeds) must
        // not wipe a previously good reading.
        {
            host_cpu_load_info_data_t cpuinfo{};
            mach_msg_type_number_t ccount = HOST_CPU_LOAD_INFO_COUNT;
            if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                                reinterpret_cast<host_info_t>(&cpuinfo),
                                &ccount) == KERN_SUCCESS) {
                static std::mutex cpu_mu;
                static uint64_t prev_user = 0, prev_system = 0, prev_idle = 0, prev_nice = 0;
                uint64_t user = cpuinfo.cpu_ticks[CPU_STATE_USER];
                uint64_t system = cpuinfo.cpu_ticks[CPU_STATE_SYSTEM];
                uint64_t idle = cpuinfo.cpu_ticks[CPU_STATE_IDLE];
                uint64_t nice = cpuinfo.cpu_ticks[CPU_STATE_NICE];
                uint64_t total = user + system + idle + nice;
                std::lock_guard<std::mutex> lk(cpu_mu);
                uint64_t prev_total = prev_user + prev_system + prev_idle + prev_nice;
                if (prev_total > 0 && total > prev_total) {
                    uint64_t d_total = total - prev_total;
                    uint64_t d_idle = idle - prev_idle;
                    if (d_total > 0)
                        h.cpu_pct = 100.0 * (1.0 - static_cast<double>(d_idle) /
                                                     static_cast<double>(d_total));
                }
                prev_user = user; prev_system = system; prev_idle = idle; prev_nice = nice;
            }
        }
    }
#elif defined(__linux__)
    {
        std::ifstream mf("/proc/meminfo");
        uint64_t mem_total_kb = 0, mem_avail_kb = 0, mem_free_kb = 0;
        std::string key;
        uint64_t val = 0;
        std::string unit;
        while (mf >> key >> val >> unit) {
            if (key == "MemTotal:") mem_total_kb = val;
            else if (key == "MemAvailable:") mem_avail_kb = val;
            else if (key == "MemFree:") mem_free_kb = val;
        }
        if (mem_avail_kb == 0) mem_avail_kb = mem_free_kb;
        if (mem_total_kb > 0) {
            h.mem_total_mb = mem_total_kb / 1024;
            h.mem_used_mb = (mem_total_kb > mem_avail_kb)
                ? (mem_total_kb - mem_avail_kb) / 1024 : 0;
            h.mem_pct = 100.0 * static_cast<double>(mem_total_kb - mem_avail_kb) /
                        static_cast<double>(mem_total_kb);
        }
        // CPU from /proc/stat
        std::ifstream sf("/proc/stat");
        std::string cpu_label;
        uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0,
                 irq = 0, softirq = 0, steal = 0;
        if (sf >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal) {
            static uint64_t prev_idle = 0, prev_total = 0;
            uint64_t idle_all = idle + iowait;
            uint64_t non_idle = user + nice + system + irq + softirq + steal;
            uint64_t total = idle_all + non_idle;
            if (prev_total > 0 && total > prev_total) {
                uint64_t d_total = total - prev_total;
                uint64_t d_idle = idle_all - prev_idle;
                if (d_total > 0)
                    h.cpu_pct = 100.0 * (1.0 - static_cast<double>(d_idle) /
                                                 static_cast<double>(d_total));
            }
            prev_idle = idle_all;
            prev_total = total;
        }
    }
#endif
    // Disk: root filesystem
    {
        struct statvfs st{};
        if (statvfs("/", &st) == 0 && st.f_blocks > 0) {
            uint64_t total = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
            uint64_t freeb = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
            uint64_t used = total > freeb ? total - freeb : 0;
            h.disk_total_gb = total / (1024ULL * 1024ULL * 1024ULL);
            h.disk_used_gb = used / (1024ULL * 1024ULL * 1024ULL);
            h.disk_pct = 100.0 * static_cast<double>(used) / static_cast<double>(total);
        }
    }
#else  // _WIN32
    {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            h.mem_total_mb = ms.ullTotalPhys / (1024ULL * 1024ULL);
            h.mem_used_mb = (ms.ullTotalPhys - ms.ullAvailPhys) / (1024ULL * 1024ULL);
            h.mem_pct = static_cast<double>(ms.dwMemoryLoad);
        }
        ULARGE_INTEGER freeb{}, total{}, dummy{};
        if (GetDiskFreeSpaceExA("C:\\", &freeb, &total, &dummy) && total.QuadPart > 0) {
            uint64_t used = total.QuadPart > freeb.QuadPart
                ? total.QuadPart - freeb.QuadPart : 0;
            h.disk_total_gb = total.QuadPart / (1024ULL * 1024ULL * 1024ULL);
            h.disk_used_gb = used / (1024ULL * 1024ULL * 1024ULL);
            h.disk_pct = 100.0 * static_cast<double>(used) /
                         static_cast<double>(total.QuadPart);
        }
        // CPU via GetSystemTimes
        FILETIME idle_ft{}, kernel_ft{}, user_ft{};
        if (GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
            auto ft64 = [](const FILETIME& ft) -> uint64_t {
                return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            };
            static uint64_t prev_idle = 0, prev_kernel = 0, prev_user = 0;
            uint64_t idle = ft64(idle_ft), kernel = ft64(kernel_ft), user = ft64(user_ft);
            // kernel includes idle on Windows
            uint64_t total = kernel + user;
            uint64_t prev_total = prev_kernel + prev_user;
            if (prev_total > 0 && total > prev_total) {
                uint64_t d_total = total - prev_total;
                uint64_t d_idle = idle - prev_idle;
                if (d_total > 0)
                    h.cpu_pct = 100.0 * (1.0 - static_cast<double>(d_idle) /
                                                 static_cast<double>(d_total));
            }
            prev_idle = idle; prev_kernel = kernel; prev_user = user;
        }
        // Approximate "load" as cpu fraction so existing load field is useful
        if (h.cpu_pct >= 0) h.load1 = h.cpu_pct / 100.0 * std::max(1, h.ncpu);
    }
#endif
    if (h.cpu_pct > 100.0) h.cpu_pct = 100.0;
    if (h.mem_pct > 100.0) h.mem_pct = 100.0;
    if (h.disk_pct > 100.0) h.disk_pct = 100.0;
    if (h.cpu_pct < 0 && h.cpu_pct != -1.0) h.cpu_pct = 0;
    // Reuse last good CPU when this sample had no tick delta (common on
    // macOS: FLEET IPC and gossip both call collect_host_stats; a zero-delta
    // sample used to publish cpu_pct=null for `self` forever).
    {
        static std::mutex last_cpu_mu;
        static double last_good_cpu = -1.0;
        std::lock_guard<std::mutex> lk(last_cpu_mu);
        if (h.cpu_pct >= 0.0) last_good_cpu = h.cpu_pct;
        else if (last_good_cpu >= 0.0) h.cpu_pct = last_good_cpu;
    }
    return h;
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

[[nodiscard]] inline std::string private_tmp_dir(const std::string& app_home) {
    const std::string root = app_home.empty()
        ? expand_home("~/.bridgesessions") : app_home;
    const std::string dir =
        (std::filesystem::path(make_app_paths(root).root) / "tmp").string();
    (void)ensure_private_directory(dir);
    return dir;
}

// Unique file under ~/.bridgesessions/tmp (0700). Empty on failure.
[[nodiscard]] inline std::string create_private_temp_file(
        const std::string& prefix,
        const std::string& suffix,
        const std::string& app_home) {
    const std::string dir = private_tmp_dir(app_home);
#ifdef _WIN32
    char tmpl[MAX_PATH]{};
    std::string pfx = prefix.empty() ? "bs" : prefix.substr(0, 3);
    if (GetTempFileNameA(dir.c_str(), pfx.c_str(), 0, tmpl) == 0) return {};
    std::string created = tmpl;
    if (suffix.empty()) return created;
    std::string renamed = created + suffix;
    if (std::rename(created.c_str(), renamed.c_str()) != 0) {
        ::unlink(created.c_str());
        return {};
    }
    return renamed;
#else
    std::string tmpl = dir + "/" + (prefix.empty() ? "bs" : prefix) +
                       "XXXXXX" + suffix;
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemps(buf.data(), static_cast<int>(suffix.size()));
    if (fd < 0) return {};
    ::close(fd);
    return std::string(buf.data());
#endif
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
    f << "mesh.auto_upgrade " << (cfg.auto_upgrade ? "true" : "false") << "\n";
    f << "mesh.auto_upgrade_cooldown_secs " << cfg.auto_upgrade_cooldown_secs << "\n";
    f << "mesh.require_seed_pins " << (cfg.require_seed_pins ? "true" : "false") << "\n";
    f << "mesh.discovered_ttl_secs " << cfg.discovered_ttl_secs << "\n";
    f << "mesh.mdns_enabled " << (cfg.mdns_enabled ? "true" : "false") << "\n";
    f << "transfer.max_bytes " << cfg.transfer_max_bytes << "\n";
    f << "transfer.allow_sensitive_paths "
      << (cfg.allow_sensitive_paths ? "true" : "false") << "\n";
    f << "file.dest_allow_home " << (cfg.dest_allow_home ? "true" : "false") << "\n";
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
    // P3 audit fix: also include wall-clock ISO time so logs correlate with
    // system/peer logs across processes (steady_clock has no defined epoch).
    {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &tt);
#else
        gmtime_r(&tt, &tm);
#endif
        char tbuf[32]{};
        std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        j["wall"] = tbuf;
    }
    j["event"] = event;
    if (!detail.empty()) j["detail"] = detail;
    l->info(j.dump());
}

// ────────────────────────────────────────────────────────────────────
// 10. SESSION REGISTRY — thread-safe session lifecycle manager
// ────────────────────────────────────────────────────────────────────

// Agent/health/unnamed-tty names. Explicit `bs <peer> <name>` is not ephemeral.
[[nodiscard]] inline bool is_ephemeral_session_name(std::string_view name) {
    return name.rfind("health-", 0) == 0
        || name.rfind("cmd-", 0) == 0
        || name.rfind("oneshot-", 0) == 0
        || name.rfind("script-", 0) == 0
        || name.rfind("hcheck-", 0) == 0
        || name.rfind("vcheck-", 0) == 0
        || name.rfind("tty-", 0) == 0;
}

// Session origin class: `probe` (internal one-shot — health checks, --cmd
// relays, script runs), `harness` (agent/automation spawns carrying an explicit
// command override), or `user` (interactive operator shell). The panel uses
// this to stop reporting probe traffic as real workload.
[[nodiscard]] inline const char* session_kind_str(Session::Kind k) {
    switch (k) {
        case Session::Kind::User:    return "user";
        case Session::Kind::Harness: return "harness";
        case Session::Kind::Probe:   return "probe";
    }
    return "user";
}

[[nodiscard]] inline Session::Kind classify_session_kind(
        std::string_view name, SessionCommandSource source) {
    // One-shot internal probes win over everything — they are pure noise.
    if (name.rfind("health-", 0) == 0 || name.rfind("cmd-", 0) == 0
        || name.rfind("oneshot-", 0) == 0 || name.rfind("script-", 0) == 0
        || name.rfind("hcheck-", 0) == 0 || name.rfind("vcheck-", 0) == 0)
        return Session::Kind::Probe;
    // Harness/agent shells carry an explicit client command override (-x/--cmd).
    if (source == SessionCommandSource::ClientOverride)
        return Session::Kind::Harness;
    return Session::Kind::User;
}

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
    std::atomic<uint32_t> next_attach_id_{1};
    std::vector<SessionHistoryEntry> recent_;
    std::string persistence_path_;
    static constexpr size_t kMaxRecentSessions = 50;

    // P0 UAF fix: fires before a session is erased from the map so callers
    // (MeshController) can null raw attached_session pointers that would dangle.
    std::function<void(const std::string&)> on_session_erased_;

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
        const Session::Kind kind = target.kind;

        // P2 audit fix: the manual destructor + placement-new on a
        // unique_ptr-owned object is a double-free hazard if the Session move
        // constructor throws (the unique_ptr still owns a destructed object).
        // Construct into a temporary first; only destroy the old object after
        // the new one is fully built.
        Session replacement(std::move(spawned));
        // Now safe to destroy the old object and placement-new the replacement.
        target.~Session();
        new (&target) Session(std::move(replacement));
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
        target.kind = kind;
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

    // P0 UAF fix: allow MeshController to register a callback that fires
    // before any session is erased, so it can null dangling attached_session pointers.
    void set_on_session_erased(std::function<void(const std::string&)> cb) {
        on_session_erased_ = std::move(cb);
    }

    void set_persistence_path(const std::string& path) {
        persistence_path_ = path;
    }

    // ── Attach / Create ─────────────────────────────────────────
    // Connection-path attach (2.0.8): registers a per-connection Attachment,
    // returns the server-assigned attach_id, and reports the effective
    // (min-wins) geometry across all current attachments. Returns 0 on error.
    uint32_t attach_connection(const std::string& name,
                               const ResolvedSessionCommand& resolved,
                               uint16_t cols, uint16_t rows, const std::string& term,
                               const std::string& peer_pubkey,
                               uint32_t client_instance_id, bool spectator,
                               uint16_t& out_eff_cols, uint16_t& out_eff_rows) {
        std::unique_lock lock(mutex_);
        Session* s = nullptr;

        // 2.0.8 MoA fix: session names cross the line-oriented IPC protocol
        // (SCROLLBACK <name> <offset>) and log lines — whitespace/control
        // chars make a session unaddressable and forge protocol fields.
        if (name.empty()) return 0;
        for (unsigned char ch : name)
            if (ch <= 0x20 || ch == 0x7f) return 0;

        auto it = sessions_.find(name);
        if (it != sessions_.end()) {
            s = it->second.get();

            // Client --cmd overrides must always spawn a new process. Reattaching
            // to a live "default" interactive shell silently ignored --cmd and
            // hung Hermes/agent one-shots (health OK via unique session names).
            const bool live = s->state == SessionState::Running
                           || s->state == SessionState::Detached
                           || s->state == SessionState::Attached;
            const bool force_respawn =
                resolved.source == SessionCommandSource::ClientOverride
                && !resolved.command.empty();

            if (live && !force_respawn) {
                s->state = SessionState::Attached;
                s->last_attach_at = std::chrono::steady_clock::now();
                if (!peer_pubkey.empty()
                    && std::find(s->peer_ids.begin(), s->peer_ids.end(), peer_pubkey)
                           == s->peer_ids.end())
                    s->peer_ids.push_back(peer_pubkey);
            } else {
                // Died/Exited/Killed/Recoverable — or ClientOverride force-respawn.
                log_event(force_respawn && live ? "session_cmd_override_replace"
                                                : "session_resurrect_replace",
                          name + (force_respawn ? " cmd_override" : ""));
                record_history_locked(*s, -1, session_state_str(s->state));
                // Drop stale attachments: their PTYs are being replaced.
                s->attachments.clear();
                const std::string spawn_command = prepare_session_command(resolved);
                auto session_result = create_session(name, spawn_command, cols, rows, term);
                if (!session_result) return 0;
                session_result->kind = classify_session_kind(name, resolved.source);
                install_spawned_runtime(*s, std::move(*session_result), SessionState::Attached);
                if (!peer_pubkey.empty()
                    && std::find(s->peer_ids.begin(), s->peer_ids.end(), peer_pubkey)
                           == s->peer_ids.end())
                    s->peer_ids.push_back(peer_pubkey);
            }
        } else {
            // Create new session
            const std::string spawn_command = prepare_session_command(resolved);
            auto session_result = create_session(name, spawn_command, cols, rows, term);
            if (!session_result) return 0;
            auto news = std::make_unique<Session>(std::move(*session_result));
            news->state = SessionState::Attached;
            news->kind = classify_session_kind(name, resolved.source);
            if (!peer_pubkey.empty()) news->peer_ids.push_back(peer_pubkey);
            s = news.get();
            sessions_[name] = std::move(news);
        }

        // Register this connection's Attachment and compute effective geometry.
        // Reserve 0 as the error sentinel: skip it if the counter ever wraps.
        uint32_t aid = next_attach_id_.fetch_add(1, std::memory_order_relaxed);
        if (aid == 0) aid = next_attach_id_.fetch_add(1, std::memory_order_relaxed);
        Session::Attachment att;
        att.attach_id = aid;
        att.cols = cols; att.rows = rows;
        att.spectator = spectator;
        att.pubkey = peer_pubkey;
        s->attachments[aid] = att;

        // MIN-wins geometry across all attachments (narrowest pane drives the PTY).
        uint16_t min_c = cols, min_r = rows;
        for (auto& kv : s->attachments) {
            min_c = std::min(min_c, kv.second.cols);
            min_r = std::min(min_r, kv.second.rows);
        }
        out_eff_cols = min_c; out_eff_rows = min_r;
        apply_min_geometry_locked(*s);
        log_event("session_attach", name + " attach_id=" + std::to_string(aid)
                  + " spectator=" + (spectator ? "1" : "0")
                  + " eff=" + std::to_string(min_c) + "x" + std::to_string(min_r));
        return aid;
    }

    // Programmatic / test-facing attach: returns the Session* (registers a
    // default interactive attachment). Preserves the pre-2.0.8 call shape.
    Session* attach(const std::string& name,
                    const ResolvedSessionCommand& resolved,
                    uint16_t cols, uint16_t rows, const std::string& term,
                    const std::string& peer_pubkey = "") {
        uint16_t ec = 0, er = 0;
        uint32_t aid = attach_connection(name, resolved, cols, rows, term,
                                          peer_pubkey, 0, false, ec, er);
        if (aid == 0) return nullptr;
        return get(name);
    }

    // Look up a Session by its attach_id (for detach-by-id bookkeeping).
    Session* session_by_attach_id(uint32_t attach_id) {
        std::shared_lock lock(mutex_);
        for (auto& kv : sessions_) {
            if (kv.second->attachments.count(attach_id)) return kv.second.get();
        }
        return nullptr;
    }

    // Recompute the effective (MIN-wins) geometry across all attachments and
    // resize the PTY accordingly. Caller must hold mutex_. Used by
    // attach_connection, detach(uint32_t), and set_attachment_geometry.
    void apply_min_geometry_locked(Session& s) {
        if (s.attachments.empty()) return;
        uint16_t min_c = UINT16_MAX, min_r = UINT16_MAX;
        for (auto& kv : s.attachments) {
            min_c = std::min(min_c, kv.second.cols);
            min_r = std::min(min_r, kv.second.rows);
        }
#ifndef _WIN32
        if (s.master_fd >= 0) (void)resize_pty(static_cast<intptr_t>(s.master_fd), min_c, min_r);
#else
        if (s.hpcon) (void)resize_pty(reinterpret_cast<intptr_t>(s.hpcon), min_c, min_r);
#endif
    }

    // Update one attachment's geometry and re-apply MIN-wins (called from the
    // ResizeMsg path). No-op if the attach_id is unknown.
    void set_attachment_geometry(uint32_t attach_id, uint16_t cols, uint16_t rows) {
        std::unique_lock lock(mutex_);
        // 2.0.8 MoA fix: floor the geometry. Spectators may legitimately
        // resize (min-wins policy), but a 0x0/1x1 resize from ANY attachment
        // collapses the shared PTY for every viewer — a read-only role must
        // not be able to DoS the session display.
        cols = std::max<uint16_t>(cols, 20);
        rows = std::max<uint16_t>(rows, 5);
        // Inline lookup — session_by_attach_id() acquires its own shared_lock,
        // which would deadlock against our unique_lock on a non-recursive mutex.
        Session* s = nullptr;
        for (auto& kv : sessions_) {
            if (kv.second->attachments.count(attach_id)) { s = kv.second.get(); break; }
        }
        if (!s) return;
        auto it = s->attachments.find(attach_id);
        if (it == s->attachments.end()) return;
        it->second.cols = cols;
        it->second.rows = rows;
        apply_min_geometry_locked(*s);
        log_event("session_geometry", s->name
                  + " attach_id=" + std::to_string(attach_id)
                  + " eff=" + std::to_string(cols) + "x" + std::to_string(rows));
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

    // Detach a single connection by its server-assigned attach_id. Removes
    // only that attachment; the session survives until the last attachment is
    // gone. Returns true if the session still has attachments, false if empty.
    bool detach(uint32_t attach_id) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.begin();
        for (; it != sessions_.end(); ++it) {
            if (it->second->attachments.count(attach_id)) break;
        }
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        const std::string name = s->name;
        auto ait = s->attachments.find(attach_id);
        const std::string pubkey = (ait != s->attachments.end()) ? ait->second.pubkey : "";
        s->attachments.erase(attach_id);
        // Re-apply MIN-wins geometry: if the detached pane was the narrowest,
        // the PTY must grow back to the next-smallest remaining pane.
        if (!s->attachments.empty()) apply_min_geometry_locked(*s);
        if (!pubkey.empty()) {
            s->peer_ids.erase(
                std::remove(s->peer_ids.begin(), s->peer_ids.end(), pubkey),
                s->peer_ids.end());
        }
        if (s->attachments.empty() &&
            (s->state == SessionState::Attached || s->state == SessionState::Running)) {
            if (!s->detach_signal.empty()) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0 && s->child_pid) {
#ifdef _WIN32
                    if (sig == 0) {
                        GenerateConsoleCtrlEvent(CTRL_C_EVENT, GetProcessId(s->child_pid));
                    } else {
                        TerminateProcess(s->child_pid, 1);
                    }
                    log_event("session_detach_signal", name + " -> " + s->detach_signal);
#else
                    ::kill(s->child_pid, sig);
                    log_event("session_detach_signal", name + " -> " + s->detach_signal);
#endif
                } else if (s->child_pid) {
                    log_event("session_detach_signal_unknown", name + " signal=" + s->detach_signal);
                }
            }
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return !s->attachments.empty();
    }

    // Backward-compat (used by existing tests + any caller that tracked only
    // pubkey, not attach_id): detach every attachment belonging to a pubkey.
    bool detach(const std::string& name, const std::string& peer_pubkey) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        std::vector<uint32_t> to_erase;
        for (auto& kv : s->attachments)
            if (kv.second.pubkey == peer_pubkey) to_erase.push_back(kv.first);
        for (uint32_t id : to_erase) s->attachments.erase(id);
        if (!peer_pubkey.empty()) {
            s->peer_ids.erase(
                std::remove(s->peer_ids.begin(), s->peer_ids.end(), peer_pubkey),
                s->peer_ids.end());
        }
        if (s->attachments.empty() &&
            (s->state == SessionState::Attached || s->state == SessionState::Running)) {
            if (!s->detach_signal.empty()) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0 && s->child_pid) {
#ifdef _WIN32
                    if (sig == 0) GenerateConsoleCtrlEvent(CTRL_C_EVENT, GetProcessId(s->child_pid));
                    else TerminateProcess(s->child_pid, 1);
#else
                    ::kill(s->child_pid, sig);
#endif
                }
            }
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return !s->attachments.empty();
    }

    // Detach ALL attachments from a session (legacy fallback for old callers
    // that wired Conn.attached_session directly without the AttachMsg path).
    // Returns false if the session was not found.
    bool detach_all(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        s->attachments.clear();
        s->peer_ids.clear();
        if (s->state == SessionState::Attached || s->state == SessionState::Running) {
            if (!s->detach_signal.empty() && s->child_pid) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0) {
#ifdef _WIN32
                    if (sig == 0) GenerateConsoleCtrlEvent(CTRL_C_EVENT, GetProcessId(s->child_pid));
                    else TerminateProcess(s->child_pid, 1);
#else
                    ::kill(s->child_pid, sig);
#endif
                }
            }
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return false; // no attachments remain
    }

    // Backward-compat: detach every attachment for a session name (used by
    // legacy callers/tests that don't track attach_id). Returns true if
    // attachments remain (should be false after a full detach).
    bool detach(const std::string& name) {
        std::unique_lock lock(mutex_);
        auto it = sessions_.find(name);
        if (it == sessions_.end()) return false;
        auto* s = it->second.get();
        s->attachments.clear();
        s->peer_ids.clear();
        if (s->state == SessionState::Attached || s->state == SessionState::Running) {
            if (!s->detach_signal.empty()) {
                int sig = resolve_detach_signal(s->detach_signal);
                if (sig >= 0 && s->child_pid) {
#ifdef _WIN32
                    if (sig == 0) {
                        GenerateConsoleCtrlEvent(CTRL_C_EVENT, GetProcessId(s->child_pid));
                    } else {
                        TerminateProcess(s->child_pid, 1);
                    }
#else
                    ::kill(s->child_pid, sig);
#endif
                }
            }
            s->state = SessionState::Detached;
            log_event("session_detach", name);
        }
        return false;
    }

    // ── List ────────────────────────────────────────────────────
    bool empty() const {
        std::shared_lock lock(mutex_);
        return sessions_.empty();
    }

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
        auto now = std::chrono::steady_clock::now();
        struct Row {
            std::string scope, name, state, pid, peer, up, bytes, exitc;
        };
        std::vector<Row> rows;
        for (auto& [key, s] : sessions_) {
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                now - s->created_at).count();
            Row r;
            r.scope = "live";
            r.name = s->name;
            r.state = session_state_str(s->state);
            r.pid = session_pid_string(*s);
            r.peer = s->peer_ids.empty() ? "-" : s->peer_ids.front().substr(0, 12);
            r.up = std::to_string(uptime) + "s";
            r.bytes = std::to_string(s->scrollback.total_written());
            r.exitc = "-";
            rows.push_back(std::move(r));
        }
        for (auto it = recent_.rbegin(); it != recent_.rend(); ++it) {
            Row r;
            r.scope = "recent";
            r.name = it->name;
            r.state = it->state;
            r.pid = it->pid;
            r.peer = it->peer.empty() ? "-" : (it->peer.size() > 12 ? it->peer.substr(0, 12) : it->peer);
            r.up = std::to_string(it->runtime_seconds) + "s";
            r.bytes = std::to_string(it->bytes);
            r.exitc = std::to_string(it->exit_code);
            rows.push_back(std::move(r));
        }
        if (rows.empty()) return "No sessions.\n";
        size_t w_sc = 5, w_nm = 4, w_st = 5, w_pid = 3, w_peer = 4, w_up = 2,
               w_b = 5, w_ex = 4;
        for (const auto& r : rows) {
            w_sc = std::max(w_sc, r.scope.size());
            w_nm = std::max(w_nm, r.name.size());
            w_st = std::max(w_st, r.state.size());
            w_pid = std::max(w_pid, r.pid.size());
            w_peer = std::max(w_peer, r.peer.size());
            w_up = std::max(w_up, r.up.size());
            w_b = std::max(w_b, r.bytes.size());
            w_ex = std::max(w_ex, r.exitc.size());
        }
        auto pad = [](const std::string& s, size_t w) {
            return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
        };
        auto rpad = [](const std::string& s, size_t w) {
            return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
        };
        std::ostringstream out;
        out << pad("SCOPE", w_sc) << "  " << pad("NAME", w_nm) << "  "
            << pad("STATE", w_st) << "  " << pad("PID", w_pid) << "  "
            << pad("PEER", w_peer) << "  " << rpad("UP", w_up) << "  "
            << rpad("BYTES", w_b) << "  " << rpad("EXIT", w_ex) << "\n";
        size_t total = w_sc + w_nm + w_st + w_pid + w_peer + w_up + w_b + w_ex + 2 * 7;
        out << std::string(total, '-') << "\n";
        for (const auto& r : rows) {
            out << pad(r.scope, w_sc) << "  " << pad(r.name, w_nm) << "  "
                << pad(r.state, w_st) << "  " << pad(r.pid, w_pid) << "  "
                << pad(r.peer, w_peer) << "  " << rpad(r.up, w_up) << "  "
                << rpad(r.bytes, w_b) << "  " << rpad(r.exitc, w_ex) << "\n";
        }
        out << rows.size() << " session row(s)\n";
        return out.str();
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
            // P0 UAF fix: fire callback before erasing so Conn::attached_session can be nulled
            if (on_session_erased_) on_session_erased_(killed_name);
            sessions_.erase(it);
            log_event("session_kill", killed_name);
        }
    }

    // ── Reap dead children ──────────────────────────────────────
    void reap_dead(bool include_attached = true) {
        std::unique_lock lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            auto* s = it->second.get();
            if (!include_attached && s->state == SessionState::Attached) {
                ++it;
                continue;
            }
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
                s->release_exited_runtime();
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
                    if (on_session_erased_) on_session_erased_(s->name);
                    it = sessions_.erase(it);
                    continue;
                }
            }
            ++it;
        }
    }

    // Drop finished agent/health/unnamed-tty sessions so they cannot pile up
    // as detached PTYs. Named sessions (bs <peer> <name>) are not pruned.
    void prune_ephemeral_sessions(std::chrono::seconds max_age = std::chrono::seconds(90)) {
        auto is_ephemeral = [](const std::string& name) {
            return is_ephemeral_session_name(name);
        };
        std::unique_lock lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end(); ) {
            auto* s = it->second.get();
            if (!is_ephemeral(s->name)) { ++it; continue; }
            const bool terminal =
                s->state == SessionState::Died
                || s->state == SessionState::Exited
                || s->state == SessionState::Killed
                || s->state == SessionState::Detached;
            const bool aged = (now - s->last_attach_at) > max_age
                           || (now - s->last_output_at) > max_age;
            // Also reap live attached ephemerals with zero attachments that
            // finished long ago (child dead but state lagging).
            bool child_gone = false;
#ifdef _WIN32
            if (s->child_pid && WaitForSingleObject(s->child_pid, 0) == WAIT_OBJECT_0)
                child_gone = true;
#else
            if (s->child_pid <= 0) child_gone = true;
#endif
            if ((terminal && aged) || (child_gone && s->attachments.empty() && aged)) {
                log_event("session_prune_ephemeral", s->name);
                record_history_locked(*s, -1, "ephemeral_pruned");
                if (on_session_erased_) on_session_erased_(s->name);
                it = sessions_.erase(it);
                continue;
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
            s->created_at_sys = std::chrono::system_clock::now();
            // Restore original wall-clock from persisted data if available
            if (!m.created_at.empty()) {
                try {
                    auto epoch_secs = std::stoll(m.created_at);
                    s->created_at_sys = std::chrono::system_clock::from_time_t(
                        static_cast<time_t>(epoch_secs));
                } catch (...) {}  // fallback to now() set above
            }
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
                        s->created_at_sys.time_since_epoch()).count());
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

// Disable Nagle's algorithm on a socket — critical for interactive shells.
// Without this, the TCP stack batches small packets (individual keystrokes)
// for up to 40ms, making typing feel sluggish compared to SSH.
inline void set_tcp_nodelay(SOCKET fd) {
    if (fd == INVALID_SOCKET) return;
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&flag, sizeof(flag));
}

inline bool socket_selectable(SOCKET fd) {
    if (fd == INVALID_SOCKET) return false;
#ifdef _WIN32
    return true;
#else
    // Main event loop uses poll() (no FD_SETSIZE ceiling). Many helper paths
    // still use select(); reject fds >= FD_SETSIZE so FD_SET stays defined.
    return fd >= 0 && static_cast<int>(fd) < FD_SETSIZE;
#endif
}

// ── poll() abstraction (main mesh loop — no FD_SETSIZE ceiling) ────
#ifdef _WIN32
using bs_pollfd = WSAPOLLFD;
// Winsock2 uses POLLRDNORM/POLLWRNORM; some SDKs also define POLLIN/POLLOUT.
#ifndef POLLIN
#define POLLIN  POLLRDNORM
#endif
#ifndef POLLOUT
#define POLLOUT POLLWRNORM
#endif
[[nodiscard]] inline int bs_poll(bs_pollfd* fds, unsigned long nfds, int timeout_ms) {
    return WSAPoll(fds, nfds, timeout_ms);
}
#else
using bs_pollfd = pollfd;
[[nodiscard]] inline int bs_poll(bs_pollfd* fds, nfds_t nfds, int timeout_ms) {
    return poll(fds, nfds, timeout_ms);
}
#endif
[[nodiscard]] inline bool pollfd_readable(const bs_pollfd& p) {
    return (p.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
}
[[nodiscard]] inline bool pollfd_writable(const bs_pollfd& p) {
    return (p.revents & POLLOUT) != 0;
}
// Lookup helper: find poll result for an fd (linear scan; N is small).
[[nodiscard]] inline const bs_pollfd* find_pollfd(const std::vector<bs_pollfd>& pfds, SOCKET fd) {
    for (const auto& p : pfds) {
        if (static_cast<SOCKET>(p.fd) == fd) return &p;
    }
    return nullptr;
}
[[nodiscard]] inline bool poll_fd_readable(const std::vector<bs_pollfd>& pfds, SOCKET fd) {
    const auto* p = find_pollfd(pfds, fd);
    return p && pollfd_readable(*p);
}
[[nodiscard]] inline bool poll_fd_writable(const std::vector<bs_pollfd>& pfds, SOCKET fd) {
    const auto* p = find_pollfd(pfds, fd);
    return p && pollfd_writable(*p);
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
    std::string_view command, bool /*stdin_tty*/) {
    // An explicit command is always finite, even when invoked from a PTY.
    // Only an empty command requests an attachable interactive shell.
    return command.empty();
}

// One-shot --cmd shells must not collide on the shared "default" session name.
// Hermes/agents always use -n default (CLI default); unique names isolate each
// invocation even before server-side force-respawn.
[[nodiscard]] inline std::string make_ephemeral_session_name(std::string_view prefix) {
    static std::atomic<uint32_t> seq{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const unsigned pid = static_cast<unsigned>(GetCurrentProcessId());
#else
    const unsigned pid = static_cast<unsigned>(::getpid());
#endif
    return std::string(prefix) + std::to_string(pid) + "-"
        + std::to_string(now & 0xffffff) + "-"
        + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] inline std::string make_ephemeral_cmd_session_name() {
    return make_ephemeral_session_name("cmd-");
}

// `bs <peer>` with no session argument: unique interactive PTY each time.
[[nodiscard]] inline std::string make_ephemeral_shell_session_name() {
    return make_ephemeral_session_name("tty-");
}

// Empty/omitted name → new tty-* session. An explicit name reattaches.
[[nodiscard]] inline std::string resolve_quick_connect_session_name(
        std::string_view requested) {
    if (requested.empty()) return make_ephemeral_shell_session_name();
    return std::string(requested);
}

// Bound noninteractive shell waits so agents fail loud instead of spinning
// until their outer timeout (Hermes 20s/60s/180s). Override with BS_SHELL_TIMEOUT_SEC.
[[nodiscard]] inline int noninteractive_shell_timeout_sec() {
    const char* env = std::getenv("BS_SHELL_TIMEOUT_SEC");
    if (env && *env) {
        try {
            int v = std::stoi(env);
            if (v >= 5 && v <= 7200) return v;
        } catch (...) {}
    }
    return 120;  // default 2 minutes
}

// Health commands can report process exit before their final OutputMsg reaches
// the stream. A successful probe therefore drains until the nonce is observed
// or the transport reaches EOF; failed probes can complete immediately.
[[nodiscard]] inline bool health_probe_drain_complete(
        int32_t exit_code, std::string_view output,
        std::string_view nonce, bool transport_eof) {
    if (exit_code != 0) return true;
    return transport_eof || output.find(nonce) != std::string_view::npos;
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
            // Sort by XOR distance from target.
            // P2 audit fix: iterate i=0..31 (byte 0 = MSB, matches pubkey_to_node_id
            // and xor_leading_zeros). Was iterating 31..0, inverting distance order.
            for (int i = 0; i < 32; ++i) {
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
    uint16_t mapped_port_ = 0;  // P2 audit fix: track for cleanup

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

        if (ret == UPNPCOMMAND_SUCCESS) mapped_port_ = port;
        return ret == UPNPCOMMAND_SUCCESS;
    }

    const std::string& external_ip() const { return external_ip_; }
    bool is_initialized() const { return initialized_; }

    void cleanup() {
        if (urls_.controlURL) {
            // P2 audit fix: delete the port mapping we set up (was leaked —
            // mappings accumulated on the router across daemon restarts).
            if (mapped_port_ != 0) {
                std::string port_str = std::to_string(mapped_port_);
                UPNP_DeletePortMapping(
                    urls_.controlURL, data_.first.servicetype,
                    port_str.c_str(), "TCP", nullptr);
                mapped_port_ = 0;
            }
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
        size_t chunk_raw_size = kTransferChunkRawSizeDefault;
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
        // 2.0.8 multi-attach: per-connection attach id assigned by the server,
        // plus the spectator flag. detach() is keyed by attach_id so N connections
        // from one pubkey detach independently.
        uint32_t attach_id = 0;
        bool spectator = false;
        // Delta gossip: generation this connection last received. 0 = needs
        // full snapshot on next broadcast.
        uint32_t last_gossip_generation = 0;
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
        // Remote peer version, populated from Hello or ServerInfo gossip.
        std::string remote_version;
        // Last host metrics from ServerInfo (cpu/mem/disk JSON); empty if peer
        // has not advertised them yet (older builds).
        std::string remote_host_stats_json;
        double remote_load = -1.0;
        // ── 2.0.8 P3 per-connection output queue ──────────────────────
        // When a fanout write fails (slow client, full socket buffer), the
        // OutputMsg is enqueued here instead of silently dropped. The event
        // loop drains queues after PTY polling. If the queue exceeds the
        // high-water mark, oldest messages are dropped and an OutputGap is
        // emitted so the client knows data was lost.
        static constexpr size_t kOutputQueueHighWater = 256;
        struct QueuedOutput { std::string data; bool render_markdown = false; };
        std::deque<QueuedOutput> output_queue;
        uint64_t output_dropped_bytes = 0;
        bool output_gap_pending = false;

        // ── Non-blocking control-plane TX queue ──────────────────────
        // Encoded frames (ping/gossip/acks/output) wait here instead of
        // blocking the event loop in write_frame() up to 30s. Drained when
        // select() reports the socket writable (or immediately on enqueue).
        static constexpr size_t kTxQueueHighWaterBytes = 2u * 1024u * 1024u;
        struct QueuedTxFrame {
            std::vector<uint8_t> data;
            size_t offset = 0;
        };
        std::deque<QueuedTxFrame> tx_queue;
        size_t tx_queue_bytes = 0;
        bool want_write = false;
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

    // Test accessors for invite/join regression tests
    size_t test_pending_invite_count() const {
        std::lock_guard lock(invite_mutex_);
        return pending_invites_.size();
    }
    bool test_has_unclaimed_invite(const std::string& token) const {
        std::lock_guard lock(invite_mutex_);
        return std::any_of(pending_invites_.begin(), pending_invites_.end(),
            [&](const auto& p) { return p.token == token && p.claimed_by.empty(); });
    }
    void test_add_invite(const std::string& token) {
        std::lock_guard lock(invite_mutex_);
        PendingInvite pi;
        pi.token = token;
        pi.created_at = std::chrono::steady_clock::now();
        pending_invites_.push_back(std::move(pi));
        g_allow_join_connections.store(true, std::memory_order_relaxed);
    }
    JoinReplyMsg test_process_join(const JoinRequestMsg& jr, const std::string& peer_pk) {
        return process_join_request(jr, peer_pk);
    }
    void test_close_join_window() { maybe_close_join_window(); }
#endif

private:
    MeshConfig config_;
    SessionRegistry sessions_;
    // ── 2.0.8 P4 conversation store ──────────────────────────────
    std::unordered_map<std::string, std::vector<ConversationAppendMsg>> conversations_;
    std::mutex conversations_mutex_;
    uint64_t next_conv_seq_ = 1;
    // ── 2.0.9 join/invite ────────────────────────────────────────
    struct PendingInvite {
        std::string token;
        std::chrono::steady_clock::time_point created_at;
        std::string claimed_by;
    };
    std::vector<PendingInvite> pending_invites_;
    mutable std::mutex invite_mutex_;
    // ── BridgePanel v3 mesh plane ────────────────────────────────
    // Pre-rendered JSON arrays of session summaries per peer, populated by
    // session gossip (ServerInfoMsg trailing field). Empty until gossip lands.
    std::unordered_map<std::string, std::string> gossip_sessions_json_;
    std::shared_mutex gossip_sessions_mutex_;
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
    // Seeded RNG for reconnect jitter (P2 audit fix — replaces global rand()).
    std::mt19937 rng_{std::random_device{}()};
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
            ReadJoinRequest, // server-side join window: read JoinRequest after Hello reply
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
        std::string contact_addr;         // inbound: peer ip:port (for JoinReply host_addr)
        bool hello_written = false;       // join path: Hello reply written to peer
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
    std::chrono::steady_clock::time_point last_discovered_prune_time_{};
    std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();
    // mDNS LAN discovery
    SOCKET mdns_fd_ = INVALID_SOCKET;
    static constexpr const char* kMdnsGroup = "224.0.0.252";
    static constexpr uint16_t kMdnsPort = 19949;

    // Listen socket
    SOCKET listen_fd_ = INVALID_SOCKET;
    std::atomic<uint16_t> actual_listen_port_{0};

    std::string config_file_path_;
    // Delta gossip: bumped whenever the peer set changes so build_gossip
    // can send only what changed since a connection's last snapshot.
    std::atomic<uint32_t> gossip_generation_{1};
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

    // Auto-upgrade: last attempt time per peer (cooldown).
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> auto_upgrade_last_;

    // ── Transfer telemetry ────────────────────────────────────────────
    TransferTelemetryRing transfer_telemetry_;

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

        // P1 fix: use getaddrinfo instead of gethostbyname (thread-safe, non-blocking)
        if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) <= 0) {
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            int gai_rc = getaddrinfo(host.c_str(), nullptr, &hints, &res);
            if (gai_rc != 0 || !res) {
#ifdef _WIN32
                // MinGW with -municode maps gai_strerror to the wide-char version.
                // Use gai_strerrorA explicitly for ANSI output.
                std::string err = gai_rc != 0 ? gai_strerrorA(gai_rc) : "no results";
#else
                std::string err = gai_rc != 0 ? gai_strerror(gai_rc) : "no results";
#endif
                throw std::runtime_error("DNS resolution failed for '" + host + "': " + err);
            }
            sa.sin_addr = reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        }
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
        if (!c.peer_name.empty()) {
            std::unique_lock lock(gossip_sessions_mutex_);
            gossip_sessions_json_.erase(c.peer_name);
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
        // Advertise wire capabilities (+frm2 = u32 frame length / large chunks).
        h.version = version_string_with_local_caps();
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
        // Skip pruning when join window is open — joining peers are not yet
        // in authorized_keys. The JoinRequest handler adds them on success.
        if (g_allow_join_connections.load(std::memory_order_relaxed)) {
            log_event("prune_skip_join_window", "g_allow_join_connections=true");
            return;
        }
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
        bool changed = false;
        for (auto& p : peers) {
            if (p.pubkey_hex.empty()) continue;          // require identity
            if (p.pubkey_hex == our_pubkey_) continue;   // skip self

            if (!is_trusted_pubkey(p.pubkey_hex)) continue;

            // A seed is trusted only when its configured pin exactly matches.
            // Never learn a missing seed pin from gossip/Hello.
            // Do NOT overwrite seed.addr from gossip/Hello: inbound peers often
            // present an ephemeral source port, which used to corrupt the
            // configured listen address (broke fleet display + redials).
            bool is_seed = false;
            for (auto& s : config_.seeds) {
                if (!s.pubkey_hex.empty() && s.pubkey_hex == p.pubkey_hex) {
                    is_seed = true;
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
                    if (!p.addr.empty() && d.addr != p.addr) { d.addr = p.addr; changed = true; }
                    if (!p.name.empty() && d.name != p.name) { d.name = p.name; changed = true; }
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
            changed = true;  // new peer added
        }
        if (changed) gossip_generation_.fetch_add(1, std::memory_order_relaxed);
    }

    // Prune runtime-learned (discovered) peers that have been silent past
    // mesh.discovered_ttl_secs. Durable `seed` peers are exempt — they stay in
    // the directory offline and reconnect when they return. This is for
    // ephemeral rentals (4090/5090) that appear, do work, then are destroyed.
    void prune_stale_discovered_peers() {
        if (config_.discovered_ttl_secs <= 0) return;  // 0 = keep forever
        const uint64_t now = now_unix_seconds();
        const uint64_t cutoff = static_cast<uint64_t>(config_.discovered_ttl_secs);
        auto has_live_conn = [&](const std::string& pubkey) {
            for (auto& c : conns_) {
                if (c.sock_fd == INVALID_SOCKET) continue;
                if (c.peer_pubkey == pubkey) return true;
            }
            return false;
        };
        auto& d = config_.discovered;
        size_t before = d.size();
        d.erase(std::remove_if(d.begin(), d.end(),
            [&](const PeerEntry& p) {
                // Never prune a peer with an active connection, even if its
                // last_seen is stale (last_seen updates on Hello/gossip only).
                if (has_live_conn(p.pubkey_hex)) return false;
                if (p.last_seen == 0) return false;  // never merged properly
                return (now - p.last_seen) > cutoff;
            }),
            d.end());
        if (d.size() != before) {
            log_event("prune_stale_discovered",
                      "removed=" + std::to_string(before - d.size()));
            gossip_generation_.fetch_add(1, std::memory_order_relaxed);
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
        // Iterative scan (P2 audit fix): originally recursed after each drop,
        // which could stack-overflow under reconnect storms with many duplicates.
        bool restarted = true;
        while (restarted) {
            restarted = false;
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
                    // Restart scan from the top — the conns_ vector changed.
                    restarted = true;
                    break;
                }
            }
            if (restarted) break;
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
        set_tcp_nodelay(cfd);  // interactive shell performance (server side)
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
        c.remote_version = hello.version;
        c.ssl = std::move(ph.ssl);
        c.sock_fd = ph.sock_fd;
        c.rx_buffer = std::move(ph.rx_buffer);
        c.is_outbound = !ph.server_side;
        c.purpose = ConnectionPurpose::Unknown;
        c.last_pong = std::chrono::steady_clock::now();
        // Steady-state recv timeout for established links.
        set_socket_timeouts(ph.sock_fd, kPeerRecvTimeoutMs);
        set_tcp_nodelay(ph.sock_fd);  // interactive shell performance (mesh connections)
        // v2.0.12c: increase socket buffers for large file transfers
        { int sz = 262144; setsockopt(ph.sock_fd, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
          setsockopt(ph.sock_fd, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz)); }

        merge_peers(hello.known_peers);
        conns_.push_back(std::move(c));
        resolve_duplicates();
        clear_accept_only_for(hello.node_name, ph.expected_addr, ph.peer_pk);

        log_event(ph.server_side ? "mesh_peer_connected" : "mesh_peer_connected_outbound",
                  hello.node_name + " addr=" + (ph.server_side ? "inbound" : ph.expected_addr) +
                  " pubkey=" + ph.peer_pk.substr(0, 16) + "...");

        maybe_schedule_auto_upgrade(hello.node_name, hello.version);

        // Handshake object will be erased; mark fd moved so ssl_close isn't called twice.
        ph.sock_fd = INVALID_SOCKET;
        ph.state = PendingHandshake::State::Done;
    }

    // If peer is behind us, fire-and-forget a remote `bridgesessions upgrade`
    // so hosts that were offline during a fleet cut catch up when they return.
    void maybe_schedule_auto_upgrade(const std::string& peer, const std::string& remote_ver) {
        if (!config_.auto_upgrade) return;
        if (peer.empty() || peer == config_.node_name) return;
        if (!version_is_older(remote_ver, kBridgeSessionsVersion)) return;
        const auto now = std::chrono::steady_clock::now();
        const auto cooldown = std::chrono::seconds(
            std::max(60, config_.auto_upgrade_cooldown_secs));
        auto it = auto_upgrade_last_.find(peer);
        if (it != auto_upgrade_last_.end() && now - it->second < cooldown) return;
        auto_upgrade_last_[peer] = now;
        log_event("auto_upgrade_dispatch",
                  peer + " remote=" + remote_ver +
                  " local=" + std::string(kBridgeSessionsVersion));
        // Detached: must not block the mesh event loop. Prefer `bs`/`bridgesessions`
        // on PATH of the operator host that is running this daemon.
        std::thread([peer]() {
            // shell + upgrade; remote daemon may stop mid-command (expected).
            std::string cmd =
                "bridgesessions shell " + peer +
                " --cmd 'bridgesessions upgrade' >/dev/null 2>&1";
            std::system(cmd.c_str());
        }).detach();
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
                            // Join window: allow unknown peers through Hello
                            // verification so they can send a JoinRequest with
                            // a valid invite token. The TLS cert verify
                            // callback already accepted them (g_allow_join_connections).
                            // The JoinRequest handler validates the token and
                            // adds them to authorized_keys; without a valid
                            // token the connection is dropped after join.
                            if (!g_allow_join_connections.load(std::memory_order_relaxed)) {
                                log_event("hello_identity_rejected",
                                          hello.node_name + " reason=" + identity.reason);
                                ph.state = PendingHandshake::State::Failed;
                                to_erase.push_back(i);
                                break;
                            }
                            log_event("hello_identity_accept_join_window",
                                      hello.node_name + " pubkey=" + hello.pubkey_hex.substr(0, 16) + "...");
                        }
                        // Send Hello reply. For join-window connections, write
                        // the Hello here (so the client gets it immediately) but
                        // transition to ReadJoinRequest (not promote) so the
                        // connection stays in pending_handshakes_ (immune to prune).
                        ph.outbound_hello = build_hello();
                        bool join_window = g_allow_join_connections.load(std::memory_order_relaxed) &&
                            !is_trusted_pubkey(ph.peer_pk);
                        int write_want = SSL_ERROR_WANT_WRITE;
                        bool hello_written = write_frame_nonblocking(ph.ssl.get(), ph.outbound_hello,
                                                    CONTROL_STREAM_ID, ph.tx_buffer, &write_want);
                        if (hello_written) {
                            ph.tx_buffer.clear();
                            if (join_window) {
                                // Hello sent; wait for JoinRequest in next state.
                                ph.state = PendingHandshake::State::ReadJoinRequest;
                                ph.hello_written = true;
                                ph.want_read = true;
                                ph.want_write = false;
                            } else {
                                promote_handshake_to_conn(ph, ph.peer_hello);
                                to_erase.push_back(i);
                            }
                        } else {
                            // Partial write — stay in WriteHello for non-join,
                            // or transition to ReadJoinRequest for join (which
                            // will finish the write on the next iteration).
                            if (join_window) {
                                ph.state = PendingHandshake::State::ReadJoinRequest;
                                ph.want_read = write_want == SSL_ERROR_WANT_READ;
                                ph.want_write = write_want == SSL_ERROR_WANT_WRITE;
                            } else {
                                ph.state = PendingHandshake::State::WriteHello;
                                ph.want_read = write_want == SSL_ERROR_WANT_READ;
                                ph.want_write = write_want == SSL_ERROR_WANT_WRITE;
                            }
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
                case PendingHandshake::State::ReadJoinRequest: {
                    // Server-side join window: write Hello reply, then read
                    // JoinRequest inline so prune can't kill the connection.
                    // Step 1: write Hello reply if not yet written.
                    if (!ph.outbound_hello.pubkey_hex.empty() && ph.tx_buffer.empty() && !ph.hello_written) {
                        ph.tx_buffer = encode(ph.outbound_hello, CONTROL_STREAM_ID);
                        ph.hello_written = true;
                    }
                    if (!ph.tx_buffer.empty()) {
                        int write_want = SSL_ERROR_WANT_WRITE;
                        if (write_frame_nonblocking(ph.ssl.get(), ph.outbound_hello,
                                                    CONTROL_STREAM_ID, ph.tx_buffer, &write_want)) {
                            ph.tx_buffer.clear();
                            ph.want_read = true;
                            ph.want_write = false;
                        } else {
                            ph.want_read = write_want == SSL_ERROR_WANT_READ;
                            ph.want_write = write_want == SSL_ERROR_WANT_WRITE;
                            break;
                        }
                    }
                    // Read JoinRequest
                    int want = SSL_ERROR_WANT_READ;
                    auto msg_opt = read_frame_nonblocking(ph.ssl.get(), ph.rx_buffer, &want);
                    ph.want_read = want == SSL_ERROR_WANT_READ;
                    ph.want_write = want == SSL_ERROR_WANT_WRITE;
                    if (!msg_opt) break;
                    if (std::holds_alternative<JoinRequestMsg>(*msg_opt)) {
                        auto& jr = std::get<JoinRequestMsg>(*msg_opt);
                        log_event("join_request_received", ph.peer_pk.substr(0, 16) + "...");
                        JoinReplyMsg reply = process_join_request(jr, ph.peer_pk);
                        bool reply_sent = false;
                        try {
                            write_frame(ph.ssl.get(), reply, CONTROL_STREAM_ID);
                            reply_sent = true;
                        } catch (const std::exception& e) {
                            log_event("join_reply_write_failed", e.what());
                        }
                        if (reply.ok && reply_sent) {
                            log_event("join_success", ph.peer_pk.substr(0, 16) + "...");
                            // Flush JoinReply to the wire before promoting —
                            // otherwise the event loop floods gossip frames
                            // that the client reads before JoinReply.
                            BIO_flush(SSL_get_wbio(ph.ssl.get()));
                            promote_handshake_to_conn(ph, ph.peer_hello);
                        } else if (reply.ok && !reply_sent) {
                            log_event("join_promote_without_reply", ph.peer_pk.substr(0, 16) + "...");
                        }
                    } else {
                        log_event("join_expected_join_request",
                                  "got message type index=" + std::to_string(msg_opt->index()));
                    }
                    to_erase.push_back(i);
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
            set_tcp_nodelay(sfd);  // interactive shell performance
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
            c.remote_version = hello.version;
            std::string subj_out = peer_cert_subject_oneline(ssl.get());  // R1.4 before move
            c.ssl = std::move(ssl);
            c.sock_fd = sfd;
            c.is_outbound = true;
            c.last_pong = std::chrono::steady_clock::now();
            // Steady-state recv timeout (see kPeerRecvTimeoutMs): bound mid-frame
            // stalls to drop+reconnect instead of a single-threaded loop freeze.
            set_socket_timeouts(sfd, kPeerRecvTimeoutMs);
            // v2.0.12c: increase socket buffers for large file transfers
            { int sz = 262144; setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (const char*)&sz, sizeof(sz));
              setsockopt(sfd, SOL_SOCKET, SO_RCVBUF, (const char*)&sz, sizeof(sz)); }

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
    // min_generation > 0: only meaningful when peers changed (delta). When
    // gossip_generation_ <= min_generation the caller already has current
    // state, so we return an empty peers list (nothing to send).
    // min_generation == 0: full snapshot (backward compat / first contact).
    GossipMsg build_gossip(uint32_t min_generation = 0) const {
        GossipMsg g;
        if (min_generation > 0 && gossip_generation_.load() <= min_generation)
            return g;  // caller is current — nothing changed

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
        const size_t chunk_raw = effective_transfer_chunk_size(m.chunk_size);
        const auto metadata = validate_transfer_metadata(
            m.filesize, m.total_chunks, config_.transfer_max_bytes, chunk_raw);
        if (!metadata.ok) {
            const std::string& err = metadata.reason;
            log_event("file_recv_rejected",
                      *safe_name + " size=" + std::to_string(m.filesize) +
                      " chunks=" + std::to_string(m.total_chunks) +
                      " chunk_size=" + std::to_string(chunk_raw) +
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

        // Starting a new transfer on the same connection aborts its old partial
        // unless the new FileMeta resumes the same checksum (Wi‑Fi reconnect).
        auto& state = c.file_receive;
        if (state.active) {
            state.file.close();
            if (state.checksum != m.checksum) {
                fs::remove(state.path + ".part", ec);
            }
            state.active = false;
        }

        // scp-style dest: optional absolute/relative path from FileMeta.dest_path.
        // Empty → classic receive_dir/basename behavior.
        std::string out_path;
        if (!m.dest_path.empty()) {
            auto resolved = resolve_file_send_dest(
                m.dest_path, recv_dir, config_.dest_allow_home);
            if (!resolved) {
                std::string err = "rejected dest path (escape or invalid)";
                log_event("file_recv_rejected", m.dest_path + " reason=dest_escape");
                try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
                return;
            }
            out_path = *resolved;
            // If dest is a directory (or ends with separator), append basename.
            if (!out_path.empty() &&
                (out_path.back() == '/' || out_path.back() == '\\' ||
                 (fs::exists(out_path) && fs::is_directory(out_path)))) {
                out_path = (fs::path(out_path) / *safe_name).string();
            }
            std::error_code pec;
            auto parent = fs::path(out_path).parent_path();
            if (!parent.empty()) fs::create_directories(parent, pec);
            if (pec) {
                std::string err = "cannot create dest parent directory";
                log_event("file_recv_failed", out_path + " reason=" + pec.message());
                try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
                return;
            }
            if (is_sensitive_mesh_path(out_path) && !config_.allow_sensitive_paths) {
                std::string err = "refused sensitive dest path";
                log_event("file_recv_rejected", "reason=sensitive_dest");
                try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
                return;
            }
        } else {
            out_path = (fs::path(recv_dir) / *safe_name).string();
            if (!path_is_inside_directory(out_path, recv_dir)) {
                std::string err = "path escapes receive directory";
                log_event("file_recv_rejected", *safe_name + " reason=path_escape");
                try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
                return;
            }
        }
        // Resume candidate: existing .part for this basename with matching sidecar
        // checksum. Prefer that path over inventing suffix.N (avoids restart-from-0
        // after a Wi‑Fi drop mid-transfer).
        std::string part_path = out_path + ".part";
        std::string meta_path = out_path + ".part.bsmeta";
        bool resume = false;
        uint32_t resume_chunks = 0;
        uint64_t resume_bytes = 0;
        if (fs::exists(part_path) && fs::exists(meta_path)) {
            std::ifstream mf(meta_path);
            std::string line;
            std::string meta_sum;
            uint64_t meta_size = 0;
            uint32_t meta_total = 0;
            size_t meta_chunk = 0;
            while (std::getline(mf, line)) {
                if (line.rfind("checksum=", 0) == 0) meta_sum = line.substr(9);
                else if (line.rfind("size=", 0) == 0) meta_size = std::strtoull(line.c_str() + 5, nullptr, 10);
                else if (line.rfind("total_chunks=", 0) == 0) meta_total = static_cast<uint32_t>(std::strtoul(line.c_str() + 13, nullptr, 10));
                else if (line.rfind("chunk_raw=", 0) == 0) meta_chunk = static_cast<size_t>(std::strtoul(line.c_str() + 10, nullptr, 10));
            }
            std::error_code pec;
            auto psz = fs::file_size(part_path, pec);
            if (!pec && meta_sum == m.checksum && meta_size == m.filesize &&
                meta_total == m.total_chunks && meta_chunk == chunk_raw &&
                psz > 0 && psz < m.filesize && (psz % chunk_raw == 0 || psz == m.filesize)) {
                resume = true;
                resume_bytes = static_cast<uint64_t>(psz);
                resume_chunks = static_cast<uint32_t>(resume_bytes / chunk_raw);
                // last partial chunk: if not aligned, truncate to whole chunks
                if (resume_bytes % chunk_raw != 0) {
                    resume_bytes = static_cast<uint64_t>(resume_chunks) * chunk_raw;
                    fs::resize_file(part_path, resume_bytes, pec);
                }
            }
        }
        if (!resume) {
            // Only auto-suffix collisions under default receive_dir (inbox) mode.
            // scp-style dest overwrites/resumes the exact path requested.
            if (m.dest_path.empty()) {
                int suffix = 1;
                while (fs::exists(out_path) || fs::exists(out_path + ".part")) {
                    std::string alt = (fs::path(recv_dir) /
                                       (*safe_name + "." + std::to_string(suffix))).string();
                    if (!path_is_inside_directory(alt, recv_dir)) break;
                    out_path = alt;
                    ++suffix;
                }
            }
            part_path = out_path + ".part";
            meta_path = out_path + ".part.bsmeta";
        }

        state = FileReceiveState{};
        state.filename = *safe_name;
        state.path = out_path;
        state.checksum = m.checksum;
        state.expected_size = m.filesize;
        state.total_chunks = m.total_chunks;
        state.chunk_raw_size = chunk_raw;
        if (resume) {
            state.received_bytes = resume_bytes;
            state.received_chunks = resume_chunks;
            state.file.open(part_path, std::ios::binary | std::ios::app);
            log_event("file_recv_resume", *safe_name + " from_chunk=" + std::to_string(resume_chunks) +
                      " bytes=" + std::to_string(resume_bytes));
        } else {
            state.file.open(part_path, std::ios::binary | std::ios::trunc);
            // Sidecar for cross-connection resume after Wi‑Fi drops
            {
                std::ofstream mf(meta_path, std::ios::trunc);
                if (mf) {
                    mf << "checksum=" << m.checksum << "\n"
                       << "size=" << m.filesize << "\n"
                       << "total_chunks=" << m.total_chunks << "\n"
                       << "chunk_raw=" << chunk_raw << "\n";
                }
            }
            log_event("file_recv_start", *safe_name + " -> " + out_path);
        }
        state.active = state.file.is_open();
        if (!state.active) {
            std::string err = "cannot open " + out_path + ".part";
            log_event("file_recv_failed", err);
            try { write_frame(c.ssl.get(), FileAckMsg{0, 0, true, err}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        // next_requested = first chunk we still need (0 for fresh, N for resume).
        // error_msg carries path=<resolved> so senders can confirm scp-style dest
        // (legacy peers leave this empty → sender warns that dest was not confirmed).
        try {
            write_frame(c.ssl.get(),
                        FileAckMsg{resume ? (resume_chunks > 0 ? resume_chunks - 1 : 0)
                                          : 0,
                                   resume_chunks, false, "path=" + out_path},
                        CONTROL_STREAM_ID);
        } catch (...) {}
    }

    void handle_file_chunk(Conn& c, const FileChunkMsg& m) {
        auto& state = c.file_receive;
        if (!state.active) {
            log_event("file_chunk_orphan", "no active receive for chunk " + std::to_string(m.chunk_index));
            try { write_frame(c.ssl.get(), FileAckMsg{m.chunk_index, 0, true, "no active receive"}, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
        // Decompress chunk data (zstd magic sniff — v2.0.16: handles both
        // pre-2.0.14 double-compressed and v2.0.14+ raw senders)
        std::vector<uint8_t> decompressed;
        if (!m.data.empty()) {
            decompressed = decompress_chunk_payload(std::span<const uint8_t>(m.data.data(), m.data.size()));
        }
        const auto chunk_valid = validate_transfer_chunk(
            state.expected_size, state.received_bytes, state.received_chunks,
            state.total_chunks, m.chunk_index, m.total_chunks, decompressed.size(),
            state.chunk_raw_size);
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
            // Drop resume sidecar either way (mismatch already removed .part)
            {
                std::error_code mec;
                fs::remove(final_path + ".part.bsmeta", mec);
            }
            state.active = false;
            // Final ack: on success include path= so sender can prove scp dest.
            std::string final_msg = complete_ok ? ("path=" + final_path) : final_error;
            try { write_frame(c.ssl.get(), FileAckMsg{m.chunk_index, m.total_chunks, !complete_ok, final_msg}, CONTROL_STREAM_ID); } catch (...) {}
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
        // Find the connection (allow file transfer even on busy conns)
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
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
        meta.checksum = checksum; meta.total_chunks = total_chunks; meta.chunk_size = static_cast<uint32_t>(kTransferChunkRawSizeDefault);
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
            // v2.0.12c: let encode() handle compression — manual zstd_compress here
            // causes double compression which breaks on Windows/MinGW.
            FileChunkMsg chunk;
            chunk.chunk_index = ci; chunk.total_chunks = total_chunks;
            if (chunk_sz > 0) {
                chunk.data.assign(raw.data(), raw.data() + chunk_sz);
            }
            try { write_frame(target->ssl.get(), chunk, CONTROL_STREAM_ID); } catch (...) { return false; }
        }
        log_event("file_send_complete", filename + " " + std::to_string(filesize) + " bytes " + std::to_string(total_chunks) + " chunks");
        std::cout << "sent " << filename << " (" << filesize << " bytes, " << total_chunks << " chunks, sha256:" << checksum.substr(0, 12) << "...)\\n";
        return true;
    }

    // ── Direct TLS file send (no daemon required) ──────────────
    // Opens a direct TLS connection via connect_and_hello, then sends the file
    // using file_send_wait_on_transport (same FileMetaMsg/FileChunkMsg/FileAckMsg
    // protocol as daemon_file_send). Falls back when daemon IPC is unavailable.
    std::string direct_connect_file_send(const std::string& peer_name,
                                         const std::string& local_path,
                                         bool wait_for_completion = true,
                                         const std::string& dest_path = {}) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return "ERROR peer not found: " + peer_name;
        std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);

        // Wi‑Fi-resilient: on transport errors, reconnect and resume from the
        // last FileAck.next_requested (receiver keeps .part + .bsmeta sidecar).
        uint32_t start_chunk = 0;
        std::string last_err = "ERROR transfer failed";
        for (int attempt = 0; attempt <= kTransferReconnectMax; ++attempt) {
            if (attempt > 0) {
                // Exponential backoff capped at 30s (survives multi-minute flaky Wi‑Fi
                // when combined with 300s idle budget across attempts).
                int backoff_ms = std::min(30000, 500 * (1 << std::min(attempt - 1, 6)));
                std::cerr << "RETRY phase=send peer=" << peer_name
                          << " attempt=" << attempt
                          << " from_chunk=" << start_chunk
                          << " backoff_ms=" << backoff_ms << "\n";
#ifdef _WIN32
                Sleep(static_cast<DWORD>(backoff_ms));
#else
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
#endif
            }
            auto sc = connect_and_hello(addr, expected_pubkey);
            if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
                std::string detail = sc.fail_detail.empty()
                    ? connect_fail_string(sc.fail) : sc.fail_detail;
                last_err = "ERROR failed to connect to " + peer_name + ": " + detail;
                continue;
            }
            struct SslCloseGuard {
                SslPtr* ssl; SOCKET* sfd;
                ~SslCloseGuard() {
                    if (*sfd != INVALID_SOCKET) {
                        ssl_close(ssl->get(), *sfd);
                        *sfd = INVALID_SOCKET;
                    }
                }
            } guard{&sc.ssl, &sc.sfd};

            uint32_t resume_hint = start_chunk;
            std::string result = file_send_wait_on_transport(
                sc.ssl.get(), sc.sfd, local_path, {}, {}, nullptr, peer_name,
                sc.hello.version, start_chunk, &resume_hint, dest_path);
            if (result.rfind("ERROR", 0) != 0) return result;  // success

            last_err = result;
            // Only retry transport/ack failures — not validation or cancel.
            const bool retryable =
                result.find("send chunk") != std::string::npos ||
                result.find("transfer ack") != std::string::npos ||
                result.find("idle timeout") != std::string::npos ||
                result.find("SSL") != std::string::npos ||
                result.find("connect") != std::string::npos;
            if (!retryable) return result;
            if (resume_hint > start_chunk) start_chunk = resume_hint;
        }
        return last_err + " (after " + std::to_string(kTransferReconnectMax) +
               " reconnects; last resume chunk=" + std::to_string(start_chunk) + ")";
    }

    // Direct TLS file recv (no daemon required) — sends FileRequestMsg, receives file
    std::string direct_connect_file_recv(const std::string& peer_name,
                                         const std::string& remote_path,
                                         const std::string& local_dest) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return "ERROR peer not found: " + peer_name;
        std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        auto sc = connect_and_hello(addr, expected_pubkey);
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            std::string detail = sc.fail_detail.empty()
                ? connect_fail_string(sc.fail) : sc.fail_detail;
            return "ERROR failed to connect to " + peer_name + ": " + detail;
        }
        // P2: RAII guard — ensures ssl_close runs even if recv throws
        struct SslCloseGuard {
            SslPtr* ssl; SOCKET* sfd;
            ~SslCloseGuard() { if (*sfd != INVALID_SOCKET) { ssl_close(ssl->get(), *sfd); *sfd = INVALID_SOCKET; } }
        } guard{&sc.ssl, &sc.sfd};
        // Use file_recv_wait_on_transport: sends request, receives meta + chunks, writes file
        return file_recv_wait_on_transport(
            sc.ssl.get(), sc.sfd, remote_path, local_dest, receive_dir_, {}, {});
    }

    // v2.0.6: transport-agnostic file send-wait. Runs on the event loop or a
    // worker thread; caller must ensure exclusive SSL transport access.
    // start_chunk: skip [0, start_chunk) after FileMeta handshake (resume after Wi‑Fi drop).
    // On transport error returns "ERROR …" and *resume_out is set to the first unacked chunk
    // so the caller can reconnect and continue.
    std::string file_send_wait_on_transport(
            SSL* ssl, SOCKET sock_fd, const std::string& local_path,
            const std::function<bool()>& is_cancelled = {},
            const std::function<void(const std::string&)>& on_progress = {},
            TransferTelemetryRing* telemetry_ring = nullptr,
            const std::string& peer_name = {},
            std::string_view peer_version = {},
            uint32_t start_chunk = 0,
            uint32_t* resume_out = nullptr,
            const std::string& dest_path = {}) {
        if (!socket_selectable(sock_fd)) return "ERROR socket exceeds select limit";
        // v2.0.12c: temporarily set blocking mode for the duration of the transfer.
        // Mesh sockets are non-blocking; SSL_write_ex on non-blocking sockets
        // returns SSL_ERROR_WANT_WRITE and fails on Windows/MinGW.
        struct BlockingGuard {
            SOCKET fd;
#ifdef _WIN32
            u_long orig;
            explicit BlockingGuard(SOCKET f) : fd(f), orig(0) {
                ioctlsocket(f, FIONBIO, &orig);
            }
            ~BlockingGuard() {
                u_long restore = 1;
                ioctlsocket(fd, FIONBIO, &restore);
            }
#else
            int orig;
            explicit BlockingGuard(SOCKET f) : fd(f), orig(fcntl(f, F_GETFL, 0)) {
                fcntl(f, F_SETFL, orig & ~O_NONBLOCK);
            }
            ~BlockingGuard() {
                fcntl(fd, F_SETFL, orig);
            }
#endif
        } guard{sock_fd};
        namespace fs = std::filesystem;
        auto emit = [&](const std::string& line) {
            if (on_progress) on_progress(line);
            else std::cerr << line << "\n";
        };
        if (!fs::exists(local_path) || fs::is_directory(local_path))
            return "ERROR file not found or is a directory: " + local_path;

        // Resolve peer capability (+frm2 → 256KB chunks + u32 frames).
        std::string ver{peer_version};
        if (ver.empty() && !peer_name.empty()) {
            for (const auto& c : conns_) {
                if (is_live_mesh_transport_for(c, peer_name, false)) {
                    ver = c.remote_version;
                    break;
                }
            }
        }
        const size_t chunk_raw = transfer_chunk_size_for_peer(ver);
        const bool allow_large = version_has_cap(ver, kCapFrm2);

        uint64_t filesize = static_cast<uint64_t>(fs::file_size(local_path));
        const auto shape = calculate_transfer_metadata(
            filesize, config_.transfer_max_bytes, chunk_raw);
        if (!shape.ok) return "ERROR " + shape.reason;
        std::string filename = fs::path(local_path).filename().string();
        std::string checksum = sha256_file_stream(local_path);
        if (checksum.empty()) return "ERROR cannot hash " + local_path;
        const uint32_t total_chunks = shape.expected_chunks;

        try {
            FileMetaMsg meta;
            meta.filename = filename; meta.filesize = filesize;
            meta.checksum = checksum; meta.total_chunks = total_chunks;
            meta.chunk_size = static_cast<uint32_t>(chunk_raw);
            meta.dest_path = dest_path;
            write_frame(ssl, meta, CONTROL_STREAM_ID, allow_large);
        } catch (const std::exception& e) {
            return "ERROR send meta: " + std::string(e.what());
        }

        auto overall_deadline = std::chrono::steady_clock::now() + transfer_overall_timeout(filesize);
        auto idle_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(kTransferIdleTimeoutSec);

        uint32_t last_acked = start_chunk;  // first chunk not yet fully acked
        // Remote path confirmed by peer via FileAck.error_msg "path=…".
        // Empty after transfer + non-empty dest_path ⇒ peer likely ignored dest.
        std::string remote_path_confirmed;

        auto mark_resume = [&](uint32_t at) {
            if (resume_out) *resume_out = at;
        };

        auto note_ack_path = [&](const FileAckMsg& ack) {
            if (ack.error) return;
            if (ack.error_msg.rfind("path=", 0) == 0 && ack.error_msg.size() > 5)
                remote_path_confirmed = ack.error_msg.substr(5);
        };

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
                        note_ack_path(ack);
                        if (ack.next_requested >= expected_next) {
                            last_acked = ack.next_requested;
                            idle_deadline = std::chrono::steady_clock::now() +
                                            std::chrono::seconds(kTransferIdleTimeoutSec);
                            return "OK";
                        }
                        // Peer ahead of us on resume: snap forward
                        if (ack.next_requested > last_acked)
                            last_acked = ack.next_requested;
                    } else if (std::holds_alternative<PingMsg>(resp)) {
                        write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID, allow_large);
                    }
                } catch (const std::exception& e) {
                    mark_resume(last_acked);
                    return "ERROR transfer ack: " + std::string(e.what());
                } catch (...) {
                    mark_resume(last_acked);
                    return "ERROR transfer ack failed";
                }
            }
            mark_resume(last_acked);
            if (std::chrono::steady_clock::now() >= overall_deadline)
                return "ERROR transfer overall timeout";
            return "ERROR transfer idle timeout waiting for ack";
        };

        // Initial handshake ack: next_requested may be >0 if peer resumes a .part
        std::string ack = wait_ack(0);
        if (ack.rfind("ERROR", 0) == 0) return ack;
        if (last_acked > start_chunk) start_chunk = last_acked;

        std::ifstream infile(local_path, std::ios::binary);
        if (!infile) return "ERROR cannot open " + local_path;
        if (start_chunk > 0) {
            infile.seekg(static_cast<std::streamoff>(
                static_cast<uint64_t>(start_chunk) * chunk_raw), std::ios::beg);
            if (!infile) return "ERROR seek for resume failed at chunk " + std::to_string(start_chunk);
            emit("RESUME phase=send file=" + filename + " from_chunk=" +
                 std::to_string(start_chunk) + "/" + std::to_string(total_chunks));
        }
        auto t0 = std::chrono::steady_clock::now();
        auto last_progress = t0;
        uint64_t bytes_sent = static_cast<uint64_t>(start_chunk) * chunk_raw;
        if (bytes_sent > filesize) bytes_sent = filesize;
        std::vector<char> raw(chunk_raw);
        auto timing = make_transfer_timing();

        for (uint32_t ci = start_chunk; ci < total_chunks; /* incremented in batch */) {
            uint32_t batch_end = std::min(ci + kTransferPipelineSize, total_chunks);

            // ── Send batch: write all chunks without waiting for acks ──
            for (; ci < batch_end; ++ci) {
                if (is_cancelled && is_cancelled()) return "ERROR cancelled";
                infile.read(raw.data(), static_cast<std::streamsize>(chunk_raw));
                size_t chunk_sz = static_cast<size_t>(infile.gcount());
                // v2.0.12c: let encode() handle compression — manual zstd_compress here
                // causes double compression which breaks on Windows/MinGW.
                std::vector<uint8_t> raw_chunk;
                if (chunk_sz > 0) {
                    raw_chunk.assign(reinterpret_cast<const uint8_t*>(raw.data()),
                                     reinterpret_cast<const uint8_t*>(raw.data()) + chunk_sz);
                }
                FileChunkMsg chunk;
                chunk.chunk_index = ci;
                chunk.total_chunks = total_chunks;
                chunk.data = std::move(raw_chunk);

                auto chunk_t0 = std::chrono::steady_clock::now();
                try { write_frame(ssl, chunk, CONTROL_STREAM_ID, allow_large); }
                catch (const std::exception& e) {
                    mark_resume(last_acked > 0 ? last_acked : ci);
                    return "ERROR send chunk: " + std::string(e.what());
                }
                bytes_sent += chunk_sz;
                auto after_write = std::chrono::steady_clock::now();

                // Per-chunk write timing (ack wait measured at batch boundary)
                {
                    int64_t w_us = std::chrono::duration_cast<std::chrono::microseconds>(after_write - chunk_t0).count();
                    timing.record(0, w_us, 0, w_us);
                }
            }

            // ── Batch boundary: drain acks (no artificial sleep — acks pace us) ──
            // Pipeline of kTransferPipelineSize chunks reduces ack round-trips ~8x.
            if (batch_end < total_chunks) {
                ack = wait_ack(batch_end);
                if (ack.rfind("ERROR", 0) == 0) return ack;
            }

            auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= std::chrono::seconds(kTransferProgressIntervalSec) ||
                batch_end == total_chunks) {
                last_progress = now;
                double elapsed = std::max(0.001, std::chrono::duration<double>(now - t0).count());
                double rate = (static_cast<double>(bytes_sent) / elapsed) / (1024.0 * 1024.0);
                int eta = 0;
                if (rate > 0.001 && filesize > bytes_sent)
                    eta = static_cast<int>((static_cast<double>(filesize - bytes_sent) /
                                           (rate * 1024.0 * 1024.0)));
                emit(format_transfer_progress("send", filename, batch_end, total_chunks,
                                              bytes_sent, filesize, rate, eta));
            }
        }
        // Final ack (may already be acked; harmless to ask again)
        ack = wait_ack(total_chunks);
        if (ack.rfind("ERROR", 0) == 0) return ack;
        auto tel = timing.format(filename, filesize);
        if (!tel.empty()) emit(tel);
        if (telemetry_ring && timing.count > 0)
            telemetry_ring->append(make_telemetry_entry(timing, filename, filesize,
                peer_name, "send"));
        log_event("file_send_wait_complete", filename + " " + std::to_string(filesize) + " bytes");
        std::string ok = "OK sent " + filename + " " + std::to_string(filesize)
                       + " bytes sha256:" + checksum;
        if (!dest_path.empty()) {
            if (remote_path_confirmed.empty()) {
                // Peer never echoed path= — almost always an older build that
                // ignored FileMeta.dest_path and wrote to receive_dir/basename.
                ok += " WARNING dest not confirmed by peer"
                      " (likely landed in receive_dir; upgrade peer for scp-style dest)";
            } else {
                ok += " dest=" + remote_path_confirmed;
            }
        } else if (!remote_path_confirmed.empty()) {
            ok += " dest=" + remote_path_confirmed;
        }
        return ok;
    }

    // v2.0.6: transport-agnostic remote file request fulfillment. Peer asked us
    // to send <path>; this runs on a worker thread with exclusive SSL access.
    std::string file_request_on_transport(
            SSL* ssl, SOCKET sock_fd, const std::string& path,
            const std::function<bool()>& is_cancelled = {},
            const std::function<void(const std::string&)>& on_progress = {},
            TransferTelemetryRing* telemetry_ring = nullptr,
            const std::string& peer_name = {}) {
        if (!socket_selectable(sock_fd)) return "ERROR socket exceeds select limit";
        // v2.0.12c: temporarily set blocking mode for the duration of the transfer.
        // Mesh sockets are non-blocking; SSL_write_ex on non-blocking sockets
        // returns SSL_ERROR_WANT_WRITE and fails on Windows/MinGW.
        struct BlockingGuard {
            SOCKET fd;
#ifdef _WIN32
            u_long orig;
            explicit BlockingGuard(SOCKET f) : fd(f), orig(0) {
                ioctlsocket(f, FIONBIO, &orig);
            }
            ~BlockingGuard() {
                u_long restore = 1;
                ioctlsocket(fd, FIONBIO, &restore);
            }
#else
            int orig;
            explicit BlockingGuard(SOCKET f) : fd(f), orig(fcntl(f, F_GETFL, 0)) {
                fcntl(f, F_SETFL, orig & ~O_NONBLOCK);
            }
            ~BlockingGuard() {
                fcntl(fd, F_SETFL, orig);
            }
#endif
        } guard{sock_fd};
        namespace fs = std::filesystem;
        auto emit = [&](const std::string& line) {
            if (on_progress) on_progress(line);
            else std::cerr << line << "\n";
        };

        // Resolve relative paths against receive_dir_ without double-nesting
        // client-supplied `.bridgesessions/received/` / `received/` prefixes.
        std::string resolved_path = resolve_file_request_path(path, receive_dir_);
        if (!resolved_path.empty() && is_sensitive_mesh_path(resolved_path) &&
            !config_.allow_sensitive_paths) {
            log_event("file_request_error", "refused sensitive path");
            try {
                write_frame(ssl, FileAckMsg{0, 0, true, "refused sensitive path"},
                            CONTROL_STREAM_ID);
            } catch (...) {}
            return "ERROR refused sensitive path";
        }
        if (resolved_path.empty() || !fs::exists(resolved_path) || fs::is_directory(resolved_path)) {
            log_event("file_request_error", "not found: " + resolved_path + " (receive_dir=" + receive_dir_ + ")");
            try { write_frame(ssl, FileAckMsg{0, 0, true, "file not found: " + path + " (looked in " + resolved_path + ")"}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR file not found: " + path + " (looked in " + resolved_path + ", receive_dir=" + receive_dir_ + ")";
        }
        uint64_t filesize = static_cast<uint64_t>(fs::file_size(resolved_path));
        // peer_name param identifies the requester; use their Hello caps.
        const size_t chunk_raw = transfer_chunk_size_for_peer(
            [&]() -> std::string {
                for (const auto& c : conns_) {
                    if (is_live_mesh_transport_for(c, peer_name, false))
                        return c.remote_version;
                }
                return {};
            }());
        const bool allow_large = chunk_raw > kTransferChunkRawSizeDefault;
        const auto shape = calculate_transfer_metadata(
            filesize, config_.transfer_max_bytes, chunk_raw);
        if (!shape.ok) {
            try { write_frame(ssl, FileAckMsg{0, 0, true, shape.reason}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR " + shape.reason;
        }
        std::string filename = fs::path(resolved_path).filename().string();
        std::string checksum = sha256_file_stream(resolved_path);
        if (checksum.empty()) {
            log_event("file_request_error", "cannot hash " + resolved_path);
            try { write_frame(ssl, FileAckMsg{0, 0, true, "cannot hash file"}, CONTROL_STREAM_ID); } catch (...) {}
            return "ERROR cannot hash file";
        }
        const uint32_t total_chunks = shape.expected_chunks;

        try {
            FileMetaMsg meta;
            meta.filename = filename; meta.filesize = filesize;
            meta.checksum = checksum; meta.total_chunks = total_chunks;
            meta.chunk_size = static_cast<uint32_t>(chunk_raw);
            write_frame(ssl, meta, CONTROL_STREAM_ID, allow_large);
        } catch (const std::exception& e) {
            return "ERROR send meta: " + std::string(e.what());
        }
        log_event("file_request_sending", filename + " " + std::to_string(total_chunks) + " chunks");

        std::ifstream infile(resolved_path, std::ios::binary);
        if (!infile) { log_event("file_request_error", "cannot open " + resolved_path); return "ERROR cannot open " + path; }
        std::vector<char> raw(chunk_raw);

        auto overall_deadline = std::chrono::steady_clock::now() + transfer_overall_timeout(filesize);
        auto idle_deadline = std::chrono::steady_clock::now() +
                             std::chrono::seconds(kTransferIdleTimeoutSec);

        uint64_t bytes_sent = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto last_progress = t0;
        auto timing = make_transfer_timing();

        for (uint32_t ci = 0; ci < total_chunks; /* incremented in batch */) {
            uint32_t batch_end = std::min(ci + kTransferPipelineSize, total_chunks);
            if (is_cancelled && is_cancelled()) {
                try { write_frame(ssl, FileAckMsg{ci, ci, true, "cancelled"}, CONTROL_STREAM_ID); } catch (...) {}
                return "ERROR cancelled";
            }
            if (std::chrono::steady_clock::now() >= overall_deadline)
                return "ERROR transfer overall timeout at chunk " + std::to_string(ci);
            if (std::chrono::steady_clock::now() >= idle_deadline)
                return "ERROR transfer idle timeout at chunk " + std::to_string(ci);

            // ── Send batch: write all chunks, one select+breather between ──
            for (; ci < batch_end; ++ci) {
                infile.read(raw.data(), static_cast<std::streamsize>(chunk_raw));
                size_t chunk_sz = static_cast<size_t>(infile.gcount());
                // v2.0.12c: let encode() handle compression — manual zstd_compress here
                // causes double compression which breaks on Windows/MinGW.
                FileChunkMsg chunk;
                chunk.chunk_index = ci; chunk.total_chunks = total_chunks;
                if (chunk_sz > 0) {
                    chunk.data.assign(raw.data(), raw.data() + chunk_sz);
                }

                auto chunk_t0 = std::chrono::steady_clock::now();

                // Brief write+readiness check every 4 chunks (was every chunk)
                if ((ci % 4) == 0) {
                    fd_set wfds; FD_ZERO(&wfds); FD_SET(sock_fd, &wfds);
                    fd_set rfds; FD_ZERO(&rfds); FD_SET(sock_fd, &rfds);
                    timeval tv{2, 0};
                    int sel = select(static_cast<int>(sock_fd) + 1, &rfds, &wfds, nullptr, &tv);
                    if (sel == 0) return "ERROR transfer idle timeout at chunk " + std::to_string(ci);
                }

                try { write_frame(ssl, chunk, CONTROL_STREAM_ID, allow_large); }
                catch (const std::exception& e) {
                    log_event("file_request_error", "send chunk failed " + std::to_string(ci));
                    return "ERROR send chunk " + std::to_string(ci) + ": " + e.what();
                }

                auto after_write = std::chrono::steady_clock::now();
                bytes_sent += chunk_sz;

                {
                    int64_t t_us = std::chrono::duration_cast<std::chrono::microseconds>(after_write - chunk_t0).count();
                    timing.record(0, t_us, 0, t_us);
                }
            }

            // ── Batch boundary: drain pending acks ──
            {
                auto drain_start = std::chrono::steady_clock::now();
                // Drain ALL pending frames before next batch
                while (SSL_pending(ssl) > 0) {
                    try {
                        Message resp = read_frame(ssl);
                        if (std::holds_alternative<PingMsg>(resp)) {
                            write_frame(ssl, PongMsg{}, CONTROL_STREAM_ID);
                        } else if (std::holds_alternative<FileAckMsg>(resp)) {
                            auto& ack = std::get<FileAckMsg>(resp);
                            if (ack.error) return "ERROR remote: " + ack.error_msg;
                        }
                    } catch (...) { break; }
                }
                // Also check socket-level readability for frames not yet in SSL buffer.
                {
                    fd_set rfds2; FD_ZERO(&rfds2); FD_SET(sock_fd, &rfds2);
                    timeval tv2{0, 0};
                    if (select(static_cast<int>(sock_fd) + 1, &rfds2, nullptr, nullptr, &tv2) > 0) {
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
                }
            }

            idle_deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(kTransferIdleTimeoutSec);

            auto now = std::chrono::steady_clock::now();
            if (now - last_progress >= std::chrono::seconds(kTransferProgressIntervalSec) ||
                batch_end == total_chunks) {
                last_progress = now;
                double elapsed = std::max(0.001, std::chrono::duration<double>(now - t0).count());
                double rate = (static_cast<double>(bytes_sent) / elapsed) / (1024.0 * 1024.0);
                int eta = 0;
                if (rate > 0.001 && filesize > bytes_sent)
                    eta = static_cast<int>((static_cast<double>(filesize - bytes_sent) /
                                           (rate * 1024.0 * 1024.0)));
                emit(format_transfer_progress("send", filename, batch_end, total_chunks,
                                              bytes_sent, filesize, rate, eta));
            }
        }
        auto tel = timing.format(filename, filesize);
        if (!tel.empty()) emit(tel);
        if (telemetry_ring && timing.count > 0)
            telemetry_ring->append(make_telemetry_entry(timing, filename, filesize,
                peer_name, "send"));
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
        return file_send_wait_on_transport(target->ssl.get(), target->sock_fd, local_path, {}, on_progress,
                                           &transfer_telemetry_, peer_name, {}, 0, nullptr, {});
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
                task.ssl, task.sock_fd, task.path1, is_cancelled, progress_to_ipc,
                &transfer_telemetry_, task.peer_name, {}, 0, nullptr, task.path2);
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
                task.ssl, task.sock_fd, task.path1, is_cancelled, progress_to_ipc,
                &transfer_telemetry_, task.peer_name);
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

        const size_t recv_chunk_raw = effective_transfer_chunk_size(meta->chunk_size);
        const auto metadata = validate_transfer_metadata(
            meta->filesize, meta->total_chunks, config_.transfer_max_bytes, recv_chunk_raw);
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
                        data = decompress_chunk_payload(std::span<const uint8_t>(chunk.data.data(), chunk.data.size()));
                    const auto chunk_valid = validate_transfer_chunk(
                        meta->filesize, bytes_recv, chunks_recv, meta->total_chunks,
                        chunk.chunk_index, chunk.total_chunks, data.size(), recv_chunk_raw);
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

        std::vector<bs_pollfd> pfds;
        auto add = [&](SOCKET fd, short events) {
            if (fd == INVALID_SOCKET) return;
            bs_pollfd p{};
            p.fd = static_cast<decltype(p.fd)>(fd);
            p.events = events;
            pfds.push_back(p);
        };
        add(listen_fd_, POLLIN);
        for (const auto& c : conns_) {
            if (c.exec_busy && c.exec_busy->load()) continue;
            if (c.sock_fd == INVALID_SOCKET) continue;
            add(c.sock_fd, POLLIN);
        }
        for (auto& ph : pending_handshakes_) {
            if (ph.sock_fd == INVALID_SOCKET) continue;
            short ev = 0;
            if (ph.want_read) ev = static_cast<short>(ev | POLLIN);
            if (ph.want_write) ev = static_cast<short>(ev | POLLOUT);
            if (ev) add(ph.sock_fd, ev);
        }

        int n = pfds.empty() ? 0 : bs_poll(pfds.data(),
#ifdef _WIN32
            static_cast<unsigned long>(pfds.size()),
#else
            static_cast<nfds_t>(pfds.size()),
#endif
            timeout_ms);
        if (n <= 0) return;

        if (poll_fd_readable(pfds, listen_fd_))
            accept_inbound();
        for (int i = 0; i < static_cast<int>(conns_.size()); ++i) {
            if (conns_[static_cast<size_t>(i)].sock_fd != INVALID_SOCKET &&
                poll_fd_readable(pfds, conns_[static_cast<size_t>(i)].sock_fd)) {
                check_conn_read(i);
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
        struct PRow {
            std::string name, addr, dir, up, pong, bin, bout, sess;
        };
        std::vector<PRow> prows;
        auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                          ? config_.pong_timeout_secs : 30);
        for (const auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            ++live_conns;
            bytes_in += c.bytes_in;
            bytes_out += c.bytes_out;
            PRow r;
            r.name = c.peer_name.empty() ? "-" : c.peer_name;
            r.addr = peer_listen_addr_for(c.peer_name, c.peer_pubkey);
            if (r.addr.empty()) r.addr = c.peer_addr;
            r.dir = c.is_outbound ? "out" : "in";
            auto up_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - c.connected_at).count();
            auto pong_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - c.last_pong).count();
            r.up = std::to_string(up_s) + "s";
            r.pong = (now - c.last_pong) <= fresh
                ? (std::to_string(pong_s) + "s") : ("stale " + std::to_string(pong_s) + "s");
            r.bin = std::to_string(c.bytes_in);
            r.bout = std::to_string(c.bytes_out);
            r.sess = c.attached_session ? c.attached_session->name : "-";
            prows.push_back(std::move(r));
        }
        auto live_sessions = sessions_.list();
        std::ostringstream out;
        out << "node     " << config_.node_name << "\n"
            << "uptime   " << uptime << "s\n"
            << "peers    " << live_conns << " / " << config_.max_peers << "\n"
            << "sessions " << live_sessions.size() << "\n"
            << "bytes_in " << bytes_in << "\n"
            << "bytes_out " << bytes_out << "\n";
        if (prows.empty()) {
            out << "(no live connections)\n";
            return out.str();
        }
        size_t w_n = 4, w_a = 7, w_d = 3, w_u = 2, w_p = 4, w_i = 2, w_o = 3, w_s = 4;
        for (const auto& r : prows) {
            w_n = std::max(w_n, r.name.size());
            w_a = std::max(w_a, r.addr.size());
            w_d = std::max(w_d, r.dir.size());
            w_u = std::max(w_u, r.up.size());
            w_p = std::max(w_p, r.pong.size());
            w_i = std::max(w_i, r.bin.size());
            w_o = std::max(w_o, r.bout.size());
            w_s = std::max(w_s, r.sess.size());
        }
        auto pad = [](const std::string& s, size_t w) {
            return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
        };
        auto rpad = [](const std::string& s, size_t w) {
            return s.size() >= w ? s : std::string(w - s.size(), ' ') + s;
        };
        out << "\n"
            << pad("PEER", w_n) << "  " << pad("ADDRESS", w_a) << "  "
            << pad("DIR", w_d) << "  " << rpad("UP", w_u) << "  "
            << pad("PONG", w_p) << "  " << rpad("IN", w_i) << "  "
            << rpad("OUT", w_o) << "  " << pad("SESS", w_s) << "\n";
        size_t total = w_n + w_a + w_d + w_u + w_p + w_i + w_o + w_s + 2 * 7;
        out << std::string(total, '-') << "\n";
        for (const auto& r : prows) {
            out << pad(r.name, w_n) << "  " << pad(r.addr, w_a) << "  "
                << pad(r.dir, w_d) << "  " << rpad(r.up, w_u) << "  "
                << pad(r.pong, w_p) << "  " << rpad(r.bin, w_i) << "  "
                << rpad(r.bout, w_o) << "  " << pad(r.sess, w_s) << "\n";
        }
        return out.str();
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

        // Acquire exec_busy guard before touching target->ssl — same pattern as
        // daemon_file_recv_wait (line 8684). Without it, broadcast_ping/gossip/
        // check_conn_read can race this blocking TLS exchange on the event loop.
        if (target->exec_busy->exchange(true)) return "ERROR peer busy with another transfer, retry";
        target->exec_completed->store(false);
        struct BusyGuard {
            std::shared_ptr<std::atomic<bool>> busy;
            std::shared_ptr<std::atomic<bool>> completed;
            ~BusyGuard() {
                if (completed) completed->store(true);
                if (busy) busy->store(false);
            }
        } busy_guard{target->exec_busy, target->exec_completed};

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
                            if (!chunk.data.empty()) d = decompress_chunk_payload(std::span<const uint8_t>(chunk.data.data(), chunk.data.size()));
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
        std::error_code rename_ec;
        fs::rename(part_path, local_path, rename_ec);
        if (rename_ec) {
            // P2 audit fix: rename across filesystems / permission failure
            // used to throw an uncaught filesystem_error, crashing the daemon.
            return "ERROR rename to " + local_path + ": " + rename_ec.message();
        }
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

        // Acquire exec_busy guard before touching target->ssl (same class as
        // daemon_edit_dl / daemon_file_recv_wait). Prevents TLS record-stream
        // corruption from concurrent event-loop access during the upload.
        if (target->exec_busy->exchange(true)) return "ERROR peer busy with another transfer, retry";
        target->exec_completed->store(false);
        struct BusyGuard {
            std::shared_ptr<std::atomic<bool>> busy;
            std::shared_ptr<std::atomic<bool>> completed;
            ~BusyGuard() {
                if (completed) completed->store(true);
                if (busy) busy->store(false);
            }
        } busy_guard{target->exec_busy, target->exec_completed};

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
        meta.filename = filename; meta.filesize = filesize; meta.checksum = checksum; meta.total_chunks = total_chunks; meta.chunk_size = static_cast<uint32_t>(kTransferChunkRawSizeDefault);
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
            // v2.0.12c: let encode() handle compression — avoid double-compress
            FileChunkMsg c; c.chunk_index = ci; c.total_chunks = total_chunks;
            c.data.assign(reinterpret_cast<const uint8_t*>(raw.data()),
                          reinterpret_cast<const uint8_t*>(raw.data()) + raw.size());
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
        Conn* target = nullptr;
        for (auto& c : conns_) {
            if (is_live_mesh_transport_for(c, vf->remote_peer)) { target = &c; break; }
        }
        if (!target) return "ERROR no conn to " + vf->remote_peer;

        // Hold exec_busy for the whole multi-file sync (not per file).
        if (target->exec_busy->exchange(true)) return "ERROR peer busy with another transfer, retry";
        target->exec_completed->store(false);
        struct BusyGuard {
            std::shared_ptr<std::atomic<bool>> busy;
            std::shared_ptr<std::atomic<bool>> completed;
            ~BusyGuard() {
                if (completed) completed->store(true);
                if (busy) busy->store(false);
            }
        } busy_guard{target->exec_busy, target->exec_completed};

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

            // Send FileMeta
            const size_t kChunkRawSize = 48 * 1024;
            size_t total = content.size();
            uint32_t total_chunks = static_cast<uint32_t>((total + kChunkRawSize - 1) / kChunkRawSize);
            if (total_chunks == 0) total_chunks = 1;
            FileMetaMsg meta;
            meta.filename = fs::path(remote_file).filename().string();
            meta.filesize = static_cast<uint64_t>(total);
            meta.checksum = checksum; meta.total_chunks = total_chunks; meta.chunk_size = static_cast<uint32_t>(kTransferChunkRawSizeDefault);
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
                // v2.0.12c: let encode() handle compression — avoid double-compress
                FileChunkMsg c; c.chunk_index = ci; c.total_chunks = total_chunks;
                c.data.assign(reinterpret_cast<const uint8_t*>(raw.data()),
                              reinterpret_cast<const uint8_t*>(raw.data()) + raw.size());
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
            (void)enqueue_frame(c, PongMsg{}, CONTROL_STREAM_ID);
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
        else if (std::holds_alternative<ServerInfoMsg>(msg)) {
            auto& info = std::get<ServerInfoMsg>(msg);
            if (!info.version.empty()) c.remote_version = info.version;
            c.remote_load = info.load;
            if (!info.host_stats_json.empty()) {
                // Object shape; sessions use array shape. Both MoA-gated.
                if (host_stats_json_shape_ok(info.host_stats_json))
                    c.remote_host_stats_json = std::move(info.host_stats_json);
                else
                    log_event("gossip_host_stats_rejected_bad_json", c.peer_name);
            }
            if (!info.sessions_summary_json.empty() && !c.peer_name.empty()) {
                // 2.0.8 MoA fix: validate at the trust boundary. The payload is
                // re-interpolated VERBATIM into MESH_TREE output — a malformed
                // or envelope-breaking value from a peer would corrupt the
                // panel's JSON.parse. Authenticated ≠ safe (defense-in-depth).
                if (gossip_json_shape_ok(info.sessions_summary_json)) {
                    std::unique_lock lock(gossip_sessions_mutex_);
                    gossip_sessions_json_[c.peer_name] = std::move(info.sessions_summary_json);
                } else {
                    log_event("gossip_sessions_rejected_bad_json", c.peer_name);
                }
            }
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
    // 2.0.8 MoA: minimal JSON-array shape validator for peer-supplied
    // sessions_summary_json (re-interpolated verbatim into MESH_TREE).
    // Checks: non-empty, '[' ... ']', and bracket/brace balance outside
    // string literals with backslash-escape awareness. Not a full JSON parse —
    // sufficient to guarantee the value composes as one JSON value.
    // Public: pure static, exercised directly by the test suite.
    static bool gossip_json_shape_ok(const std::string& v) {
        if (v.size() < 2 || v.front() != '[' || v.back() != ']') return false;
        int depth_square = 0, depth_curly = 0;
        bool in_str = false, esc = false;
        for (size_t i = 0; i < v.size(); ++i) {
            char ch = v[i];
            if (in_str) {
                if (esc) { esc = false; continue; }
                if (ch == '\\') { esc = true; continue; }
                if (ch == '"') in_str = false;
                continue;
            }
            switch (ch) {
                case '"': in_str = true; break;
                case '[': ++depth_square; break;
                case ']':
                    if (--depth_square < 0) return false;
                    if (depth_square == 0) {
                        // Outermost array closed: only trailing whitespace may
                        // follow. Anything else is a second value trying to
                        // break the MESH_TREE envelope (MoA gossip finding).
                        for (size_t j = i + 1; j < v.size(); ++j)
                            if (!std::isspace(static_cast<unsigned char>(v[j]))) return false;
                        return depth_curly == 0;
                    }
                    break;
                case '{': ++depth_curly; break;
                case '}': if (--depth_curly < 0) return false; break;
                default: break;
            }
        }
        return false; // outermost array never closed
    }

    // Object-shape validator for ServerInfo.host_stats_json ({…}). Same MoA
    // rules as gossip_json_shape_ok but for a single JSON object.
    static bool host_stats_json_shape_ok(const std::string& v) {
        if (v.size() < 2 || v.front() != '{' || v.back() != '}') return false;
        if (v.size() > 2048) return false;
        int depth_square = 0, depth_curly = 0;
        bool in_str = false, esc = false;
        for (size_t i = 0; i < v.size(); ++i) {
            char ch = v[i];
            if (static_cast<unsigned char>(ch) < 0x20 && ch != '\t') return false;
            if (in_str) {
                if (esc) { esc = false; continue; }
                if (ch == '\\') { esc = true; continue; }
                if (ch == '"') in_str = false;
                continue;
            }
            switch (ch) {
                case '"': in_str = true; break;
                case '{': ++depth_curly; break;
                case '}':
                    if (--depth_curly < 0) return false;
                    if (depth_curly == 0) {
                        for (size_t j = i + 1; j < v.size(); ++j)
                            if (!std::isspace(static_cast<unsigned char>(v[j]))) return false;
                        return depth_square == 0;
                    }
                    break;
                case '[': ++depth_square; break;
                case ']': if (--depth_square < 0) return false; break;
                default: break;
            }
        }
        return false;
    }

    // Remove this transport's attachment while leaving the server-owned PTY alive.
    // A replacement connection with the same identity may already be attached
    // during a reconnect race; in that case keep the shared peer-id reference.
    void detach_connection_session(Conn& conn, bool replacement_attached) {
        auto* session = conn.attached_session;
        conn.attached_session = nullptr;
        uint32_t aid = conn.attach_id;
        conn.attach_id = 0;
        conn.spectator = false;
        if (!session || replacement_attached) return;
        // Prefer detach-by-attach_id (precise, multi-attach safe). Fall back to
        // detach-all when the attach_id is unknown (legacy/tests that set
        // attached_session directly without the AttachMsg path).
        if (aid != 0 && sessions_.session_by_attach_id(aid) != nullptr)
            sessions_.detach(aid);
        else
            sessions_.detach_all(session->name);
        log_event("session_transport_detached", session->name + " attach_id=" + std::to_string(aid) + " from " + conn.peer_name);
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

    // Extract join request processing into a reusable method.
    // Used by both handle_inbound_session (promoted conn) and ReadJoinRequest (inline handshake).
    JoinReplyMsg process_join_request(const JoinRequestMsg& jr, const std::string& peer_pubkey) {
        JoinReplyMsg reply;
        {
            std::lock_guard lock(invite_mutex_);
            auto now = std::chrono::steady_clock::now();
            pending_invites_.erase(
                std::remove_if(pending_invites_.begin(), pending_invites_.end(),
                    [now](auto& p) { return (now - p.created_at) > std::chrono::hours(2); }),
                pending_invites_.end());
            auto it = std::find_if(pending_invites_.begin(), pending_invites_.end(),
                [&](auto& p) { return p.token == jr.token && p.claimed_by.empty(); });
            if (it == pending_invites_.end()) {
                reply.ok = false;
                reply.error = "invalid or expired token";
            } else {
                it->claimed_by = peer_pubkey;
                reply.ok = true;
                reply.node_name = "node-" + jr.token.substr(0, 8);
                std::ostringstream seeds;
                for (size_t si = 0; si < config_.seeds.size(); ++si) {
                    if (si) seeds << '|';
                    seeds << config_.seeds[si].name << ':' << config_.seeds[si].addr;
                }
                reply.seeds_csv = seeds.str();
                reply.host_pubkey = our_pubkey_;
                reply.host_addr = config_.listen_addr + ":" + std::to_string(config_.listen_port);
                std::ostringstream pkjson;
                pkjson << "[";
                for (size_t si = 0; si < config_.seeds.size(); ++si) {
                    if (si) pkjson << ",";
                    pkjson << "{\"name\":\"" << gossip_json_escape(config_.seeds[si].name)
                            << "\",\"addr\":\"" << gossip_json_escape(config_.seeds[si].addr)
                            << "\",\"pubkey_hex\":\"" << gossip_json_escape(config_.seeds[si].pubkey_hex)
                            << "\"}";
                }
                pkjson << "]";
                reply.peer_pubkeys_json = pkjson.str();
            }
        }
        if (reply.ok && !peer_pubkey.empty()) {
            std::string auth_path = config_.authorized_keys_path;
            std::string dir = auth_path;
            auto slash = dir.rfind('/');
            if (slash == std::string::npos) slash = dir.rfind('\\');
            if (slash != std::string::npos) dir = dir.substr(0, slash);
            if (!bs::mesh::ensure_private_directory(dir)) {
                reply.ok = false;
                reply.error = "host could not prepare authorized_keys dir";
            } else {
                bool already_authorized = false;
                {
                    std::ifstream existing(auth_path);
                    std::string line;
                    while (std::getline(existing, line)) {
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        if (line == "pubkey " + peer_pubkey || line == peer_pubkey) {
                            already_authorized = true;
                            break;
                        }
                    }
                }
                if (!already_authorized) {
                    std::ofstream af(auth_path, std::ios::app);
                    if (af.is_open()) {
                        af << "pubkey " << peer_pubkey << "\n";
                    } else {
                        reply.ok = false;
                        reply.error = "host could not persist authorization";
                    }
                }
            }
        }
        return reply;
    }

    // Close join window when all invites are claimed or expired.
    // Called from the event loop tick to auto-close after invite expiry.
    void maybe_close_join_window() {
        if (!g_allow_join_connections.load(std::memory_order_relaxed)) return;
        std::lock_guard lock(invite_mutex_);
        auto now = std::chrono::steady_clock::now();
        // Expire old invites (2h)
        pending_invites_.erase(
            std::remove_if(pending_invites_.begin(), pending_invites_.end(),
                [now](auto& p) { return (now - p.created_at) > std::chrono::hours(2); }),
            pending_invites_.end());
        // Close window if no unclaimed invites remain
        bool any_unclaimed = false;
        for (const auto& p : pending_invites_) {
            if (p.claimed_by.empty()) { any_unclaimed = true; break; }
        }
        if (!any_unclaimed) {
            g_allow_join_connections.store(false, std::memory_order_relaxed);
        }
    }
    void handle_inbound_session(Conn& conn, Message& msg) {
        // JoinRequest — new node onboarding
        if (std::holds_alternative<JoinRequestMsg>(msg)) {
            auto& jr = std::get<JoinRequestMsg>(msg);
            JoinReplyMsg reply = process_join_request(jr, conn.peer_pubkey);
            try { write_frame(conn.ssl.get(), reply, CONTROL_STREAM_ID); } catch (...) {}
            return;
        }
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
            uint16_t eff_c = 0, eff_r = 0;
            // 2.0.8 MoA fix: a second AttachMsg on an already-attached conn
            // would overwrite conn.attach_id and ORPHAN the previous
            // Session::Attachment (leaks geometry into min-wins, blocks the
            // last-detach signal). Detach the old attachment first.
            if (conn.attach_id != 0 || conn.attached_session != nullptr) {
                log_event("session_reattach_detaching_previous",
                          a.session_name + " old_attach_id=" + std::to_string(conn.attach_id));
                detach_connection_session(conn, false);
            }
            uint32_t aid = sessions_.attach_connection(a.session_name,
                                       shell_cmd,
                                       a.cols, a.rows, a.term,
                                       conn.peer_pubkey,
                                       a.client_instance_id, a.spectator,
                                       eff_c, eff_r);
            auto* s = (aid != 0) ? sessions_.get(a.session_name) : nullptr;
            if (s) {
                // Record the detach-signal request (v2.1) so the server can
                // signal the child when the last peer detaches.
                if (!a.signal_on_detach.empty()) s->detach_signal = a.signal_on_detach;
                conn.attached_session = s;
                conn.attach_id = aid;
                conn.spectator = a.spectator;

                // 2.0.8: AttachAck reports the effective (min-wins) geometry so
                // the client can align its PTY view to the narrowest pane.
                AttachAckMsg ack;
                ack.attach_id = aid;
                ack.session_name = a.session_name;
                ack.cols = eff_c; ack.rows = eff_r;
                try { write_frame(conn.ssl.get(), ack, CONTROL_STREAM_ID); }
                catch (...) {}

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
                          a.session_name + " from " + conn.peer_name
                          + " attach_id=" + std::to_string(aid));
            } else {
                log_event("session_attach_failed",
                          a.session_name + " from " + conn.peer_name);
            }
            return;
        }

        // KeystrokeMsg — peer typed something; forward to PTY
        if (std::holds_alternative<KeystrokeMsg>(msg)) {
            auto& ks = std::get<KeystrokeMsg>(msg);
            // 2.0.8: spectators are read-only — reject input injection.
            if (conn.spectator) {
                log_event("pty_input_rejected_spectator", conn.attached_session
                          ? conn.attached_session->name : "?");
                return;
            }
            if (conn.attached_session && conn.attached_session->is_valid()) {
                if (!write_pty_input(*conn.attached_session,
                                     ks.data.data(), ks.data.size())) {
                    log_event("pty_input_rejected", conn.attached_session->name);
                    conn.close_requested = true;
                }
            }
            return;
        }

        // CuaRequestMsg — computer-use action (full dispatch lands in P5).
        // P1 invariant: spectators may never drive CUA.
        if (std::holds_alternative<CuaRequestMsg>(msg)) {
            auto& req = std::get<CuaRequestMsg>(msg);
            if (conn.spectator) {
                log_event("cua_rejected_spectator", conn.attached_session
                          ? conn.attached_session->name : "?");
                CuaResponseMsg resp;
                resp.request_id = req.request_id;
                resp.status = 1;
                resp.error = "spectator cannot drive computer use";
                try { write_frame(conn.ssl.get(), resp, CONTROL_STREAM_ID); } catch (...) {}
                return;
            }
            // Non-spectator: dispatch to platform CUA backend (2.0.8 P5).
            CuaResponseMsg resp = cua_execute(req, home_dir_);
            resp.request_id = req.request_id;
            // Screenshots often exceed 64KB; use u32 frames when peer
            // advertised +frm2 (default allow_large=false silently drops).
            const bool allow_large = version_has_cap(conn.remote_version, kCapFrm2);
            try {
                write_frame(conn.ssl.get(), resp, CONTROL_STREAM_ID, allow_large);
            } catch (const std::exception& e) {
                log_event("cua_response_write_failed", e.what());
                CuaResponseMsg fail;
                fail.request_id = req.request_id;
                fail.status = 1;
                fail.error = std::string("cua response send failed: ") + e.what();
                try { write_frame(conn.ssl.get(), fail, CONTROL_STREAM_ID); } catch (...) {}
            } catch (...) {
                log_event("cua_response_write_failed", "unknown");
            }
            return;
        }

        // CuaVideoCaptureMsg — remote video capture (2.0.12)
        if (std::holds_alternative<CuaVideoCaptureMsg>(msg)) {
            auto& req = std::get<CuaVideoCaptureMsg>(msg);
            CuaVideoCaptureResultMsg resp = video_capture_execute(req);
            resp.request_id = req.request_id;
            const bool allow_large = version_has_cap(conn.remote_version, kCapFrm2);
            try { write_frame(conn.ssl.get(), resp, CONTROL_STREAM_ID, allow_large); } catch (...) {}
            return;
        }

        // ResizeMsg — peer resized their terminal
        if (std::holds_alternative<ResizeMsg>(msg)) {
            auto& r = std::get<ResizeMsg>(msg);
            if (conn.attached_session && conn.attached_session->is_valid()) {
                // 2.0.8: update this attachment's stored geometry and re-apply
                // MIN-wins so the PTY tracks the narrowest pane (not the last
                // resizer). Spectators may resize too — it only shrinks/grows
                // the shared effective geometry, never injects input.
                // Legacy fallback: when attach_id is 0 (old test path, conn
                // wired directly without the AttachMsg handler), resize the
                // PTY directly.
                if (conn.attach_id != 0)
                    sessions_.set_attachment_geometry(conn.attach_id, r.cols, r.rows);
                else {
                    // Legacy fallback: resize PTY directly (old test path without attach_id).
#ifndef _WIN32
                    if (conn.attached_session->master_fd >= 0)
                        (void)resize_pty(static_cast<intptr_t>(conn.attached_session->master_fd), r.cols, r.rows);
#else
                    if (conn.attached_session->hpcon)
                        (void)resize_pty(reinterpret_cast<intptr_t>(conn.attached_session->hpcon), r.cols, r.rows);
#endif
                }
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
            // 2.0.8: spectators are read-only — process control (SIGINT/kill/
            // Restart with client-supplied command) is a write capability.
            if (conn.spectator) {
                log_event("signal_rejected_spectator", conn.attached_session
                    ? conn.attached_session->name : std::string{});
                return;
            }
            if (conn.attached_session && conn.attached_session->is_valid()) {
#ifdef _WIN32
                if (conn.attached_session->child_pid) {
                    DWORD ctrl_event = 0;
                    switch (sig.signal) {
                        case SignalMsg::SignalType::CtrlC:
                            ctrl_event = CTRL_C_EVENT; break;
                        case SignalMsg::SignalType::CtrlZ:
                            ctrl_event = CTRL_BREAK_EVENT; break;
                        case SignalMsg::SignalType::Kill:
                            TerminateProcess(conn.attached_session->child_pid, 1); break;
                        default: break;
                    }
                    if (ctrl_event)
                        GenerateConsoleCtrlEvent(ctrl_event,
                            GetProcessId(conn.attached_session->child_pid));
                }
#else
                if (conn.attached_session->child_pid > 0) {
                    auto& sm = std::get<SignalMsg>(msg);
                    if (sm.signal == SignalMsg::SignalType::Kill) {
                        // Terminate the shell AND every process in its group
                        // (the forkpty session-leader is the process-group id),
                        // so background jobs cannot outlive the session.
                        kill(-conn.attached_session->child_pid, SIGHUP);
                        kill(-conn.attached_session->child_pid, SIGTERM);
                    } else {
                        int s = (sm.signal == SignalMsg::SignalType::CtrlC) ? SIGINT :
                                (sm.signal == SignalMsg::SignalType::CtrlZ) ? SIGTSTP :
                                SIGQUIT;
                        kill(conn.attached_session->child_pid, s);
                    }
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

        // ── 2.0.8 P4: Conversation messages ─────────────────────
        if (std::holds_alternative<ConversationAppendMsg>(msg)) {
            auto& ca = std::get<ConversationAppendMsg>(msg);
            // 2.0.8 MoA fix: the store is the ONLY seq authority. Honoring a
            // peer-supplied seq lets a peer hide messages from since_seq
            // queries (low value) or poison ordering (huge value).
            {
                std::lock_guard lock(conversations_mutex_);
                ca.seq = next_conv_seq_++;
            }
            if (ca.ts == 0) {
                using namespace std::chrono;
                ca.ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            }
            {
                std::lock_guard lock(conversations_mutex_);
                conversations_[ca.conv_id].push_back(ca);
                // 2.0.8 MoA fix: bound the store — cap messages per
                // conversation (drop oldest) and distinct conv_ids.
                auto& vec = conversations_[ca.conv_id];
                static constexpr size_t kMaxMsgsPerConv = 10000;
                if (vec.size() > kMaxMsgsPerConv)
                    vec.erase(vec.begin(), vec.begin() + (vec.size() - kMaxMsgsPerConv));
                static constexpr size_t kMaxConvs = 1024;
                if (conversations_.size() > kMaxConvs) {
                    // evict oldest-keyed conversation (map order is arbitrary;
                    // eviction is a backstop, not an LRU)
                    conversations_.erase(conversations_.begin());
                }
            }
            log_event("conversation_append", ca.conv_id + " seq=" + std::to_string(ca.seq));
            return;
        }
        if (std::holds_alternative<ConversationQueryMsg>(msg)) {
            auto& cq = std::get<ConversationQueryMsg>(msg);
            ConversationBatchMsg batch;
            batch.conv_id = cq.conv_id;
            {
                std::lock_guard lock(conversations_mutex_);
                auto it = conversations_.find(cq.conv_id);
                if (it != conversations_.end()) {
                    for (auto& m : it->second)
                        if (m.seq > cq.since_seq) batch.messages.push_back(m);
                }
            }
            try { write_frame(conn.ssl.get(), batch, CONTROL_STREAM_ID); } catch (...) {}
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
                // Serialize once, fan out to all attached connections.
                // On write failure, enqueue to the per-connection output
                // queue instead of silently dropping (2.0.8 P3).
                for (auto& target : conns_) {
                    if (target.attached_session != s || target.sock_fd == INVALID_SOCKET ||
                        !target.ssl) {
                        continue;
                    }
                    if (target.exec_busy && target.exec_busy->load()) continue;
                    try {
                        write_frame(target.ssl.get(), message, 0);
                    } catch (...) {
                        // Enqueue for later retry; drop oldest if over high-water.
                        if constexpr (std::is_same_v<std::decay_t<decltype(message)>, OutputMsg>) {
                            Conn::QueuedOutput qo;
                            qo.data = message.data;
                            qo.render_markdown = message.render_markdown;
                            if (target.output_queue.size() >= Conn::kOutputQueueHighWater) {
                                auto& oldest = target.output_queue.front();
                                target.output_dropped_bytes += oldest.data.size();
                                target.output_gap_pending = true;
                                target.output_queue.pop_front();
                            }
                            target.output_queue.push_back(std::move(qo));
                        }
                        // Non-OutputMsg types (ClipboardMsg, SessionDiedMsg etc.)
                        // are control messages and not queued for retry.
                    }
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
                s->release_exited_runtime();
                s->state = SessionState::Died;

                SessionDiedMsg sdm;
                sdm.exit_code = static_cast<int32_t>(exit_code);
                sdm.signal_num = 0;
                for (auto& target : conns_) {
                    if (target.sock_fd == INVALID_SOCKET || !target.ssl) continue;
                    if (target.exec_busy && target.exec_busy->load()) continue;
                    // 2.0.9 fix: deliver SessionDied to both attached sessions
                    // AND DirectSession connections (non-interactive -x mode).
                    // Previously only matched attached_session, so -x callers
                    // spun forever waiting for a death notice that never arrived
                    // (RCA 2026-07-23: Start-Process descendants → -x hangs).
                    const bool match =
                        target.attached_session == s ||
                        target.purpose == ConnectionPurpose::DirectSession;
                    if (!match) continue;
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
                    // The shell exited (`exit` / Ctrl-D). Kill the process group
                    // so background jobs it left behind do not outlive the
                    // session as orphans holding the PTY open.
                    if (s->child_pid > 0) {
                        ::kill(-s->child_pid, SIGHUP);
                        ::kill(-s->child_pid, SIGTERM);
                    }
                    SessionDiedMsg sdm;
                    if (WIFEXITED(status)) {
                        sdm.exit_code = WEXITSTATUS(status);
                    } else if (WIFSIGNALED(status)) {
                        sdm.signal_num = WTERMSIG(status);
                    }
                    sessions_.record_finished(*s, sdm.exit_code, "died");
                    s->child_pid = -1;
                    s->release_exited_runtime();
                    s->state = SessionState::Died;
                    fanout(sdm);
                }
            }
#endif
        }
    }

    // Non-blocking control/output TX: encode + queue + best-effort flush.
    // Returns false if the frame was dropped (overflow / no transport).
    bool enqueue_frame(Conn& c, const Message& msg, uint16_t stream_id = CONTROL_STREAM_ID) {
        if (c.sock_fd == INVALID_SOCKET || !c.ssl) return false;
        if (c.exec_busy && c.exec_busy->load()) return false;
        std::vector<uint8_t> frame;
        try {
            const bool allow_large = version_has_cap(c.remote_version, kCapFrm2);
            frame = encode(msg, stream_id, allow_large);
        } catch (...) {
            return false;
        }
        if (c.tx_queue_bytes + frame.size() > Conn::kTxQueueHighWaterBytes) {
            log_event("tx_queue_overflow", c.peer_name + " bytes=" +
                      std::to_string(c.tx_queue_bytes));
            return false;
        }
        c.tx_queue_bytes += frame.size();
        c.tx_queue.push_back(Conn::QueuedTxFrame{std::move(frame), 0});
        c.want_write = true;
        flush_tx_queue(c);
        return true;
    }

    // Drain encoded frames with non-blocking SSL_write. Never sleeps.
    void flush_tx_queue(Conn& c) {
        if (c.sock_fd == INVALID_SOCKET || !c.ssl) {
            c.tx_queue.clear();
            c.tx_queue_bytes = 0;
            c.want_write = false;
            return;
        }
        if (c.exec_busy && c.exec_busy->load()) return;
        while (!c.tx_queue.empty()) {
            auto& fr = c.tx_queue.front();
            if (fr.offset >= fr.data.size()) {
                c.tx_queue.pop_front();
                continue;
            }
            size_t remain = fr.data.size() - fr.offset;
            size_t n = 0;
            clear_stale_ssl_errors_before_io();
            int ret = SSL_write_ex(c.ssl.get(), fr.data.data() + fr.offset, remain, &n);
            if (ret > 0 && n > 0) {
                fr.offset += n;
                if (c.tx_queue_bytes >= n) c.tx_queue_bytes -= n;
                else c.tx_queue_bytes = 0;
                c.bytes_out += n;
                if (fr.offset >= fr.data.size()) c.tx_queue.pop_front();
                continue;
            }
            int err = SSL_get_error(c.ssl.get(), ret);
            if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) {
                c.want_write = true;
                return;
            }
            log_event("tx_queue_write_fail", c.peer_name + " ssl_err=" + std::to_string(err));
            c.tx_queue.clear();
            c.tx_queue_bytes = 0;
            c.want_write = false;
            close_conn(c);
            return;
        }
        c.want_write = false;
    }

    // 4b. drain_output_queues — promote logical OutputMsg queue into the
    // encoded tx_queue, then flush. Also emits pending OutputGap notices.
    void drain_output_queues() {
        for (auto& target : conns_) {
            if (target.output_queue.empty() && !target.output_gap_pending
                && target.tx_queue.empty()) continue;
            if (target.sock_fd == INVALID_SOCKET || !target.ssl) {
                target.output_queue.clear();
                target.output_gap_pending = false;
                target.tx_queue.clear();
                target.tx_queue_bytes = 0;
                target.want_write = false;
                continue;
            }
            if (target.exec_busy && target.exec_busy->load()) continue;
            if (target.output_gap_pending) {
                OutputGapMsg gap;
                gap.dropped_bytes = target.output_dropped_bytes;
                if (enqueue_frame(target, gap, 0)) {
                    target.output_gap_pending = false;
                    target.output_dropped_bytes = 0;
                }
            }
            size_t promoted = 0;
            while (!target.output_queue.empty() && promoted < 8) {
                auto& qo = target.output_queue.front();
                OutputMsg om;
                om.data = qo.data;
                om.render_markdown = qo.render_markdown;
                if (!enqueue_frame(target, om, 0)) break;
                target.output_queue.pop_front();
                ++promoted;
            }
            while (target.output_queue.size() > Conn::kOutputQueueHighWater) {
                target.output_dropped_bytes += target.output_queue.front().data.size();
                target.output_gap_pending = true;
                target.output_queue.pop_front();
            }
            if (!target.tx_queue.empty()) flush_tx_queue(target);
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
            // P2 audit fix: initialize max_ms from config so the operator's
            // reconnect_backoff_max_secs actually takes effect (was hardcoded 30s).
            bo.max_ms = std::max(1000, config_.reconnect_backoff_max_secs * 1000);
            if (bo.attempt > 0 && now < bo.next_attempt) continue;

            // Attempt connect (non-blocking; handshake completes in event loop).
            bool started = start_outbound_handshake(s);
            if (started) {
                backoffs_.erase(s.addr);
            } else {
                bo.attempt++;
                // P2: ±25% jitter to prevent thundering herd on reconnect.
                // Use a seeded RNG (not rand()) so timing is not predictable
                // across daemon restarts.
                int jitter = bo.delay_ms * (rng_() % 50 - 25) / 100;
                bo.next_attempt = now + std::chrono::milliseconds(bo.delay_ms + jitter);
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
                    // P2 audit fix: apply the same ±25% seeded jitter as seeds
                    // (was missing — discovered peers reconnected in lockstep).
                    int jitter = bo.delay_ms * (rng_() % 50 - 25) / 100;
                    bo.next_attempt = now + std::chrono::milliseconds(bo.delay_ms + jitter);
                    bo.delay_ms = std::min(std::max(bo.delay_ms * 2, 1000), bo.max_ms);
                    break; // one failed bounded dial per loop; keep accept/read responsive
                }
            }
        }
    }

    // ── Build local sessions summary JSON (BridgePanel v3 gossip) ──

    static std::string gossip_json_escape(const std::string& v) {
        std::string r;
        for (char ch : v) {
            switch (ch) {
                case '"': r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n"; break;
                case '\r': r += "\\r"; break;
                case '\t': r += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                        r += buf;
                    } else r += ch;
            }
        }
        return r;
    }

    std::string build_sessions_summary_json() const {
        static constexpr size_t kCapBytes = 4096;
        std::vector<std::string> entries;
        for (const auto& info : sessions_.list()) {
            auto* s = sessions_.get(info.name);
            if (!s) continue;
            entries.push_back(
                "{\"name\":\"" + gossip_json_escape(s->name) + "\","
                "\"state\":\"" + gossip_json_escape(session_state_str(s->state)) + "\","
                "\"kind\":\"" + gossip_json_escape(session_kind_str(s->kind)) + "\","
                "\"command\":\"" + gossip_json_escape(s->command) + "\","
                "\"bytes\":" + std::to_string(s->scrollback.total_written()) + "}");
        }
        // Cap total size: drop oldest entries first, keep the newest that fit.
        size_t budget = kCapBytes > 2 ? kCapBytes - 2 : 0; // reserve for "[" "]"
        size_t start = entries.size();
        size_t running = 0;
        while (start > 0) {
            size_t next_len = entries[start - 1].size() + (start < entries.size() ? 1 : 0);
            if (running + next_len > budget) break;
            running += next_len;
            --start;
        }
        std::string out = "[";
        for (size_t i = start; i < entries.size(); ++i) {
            if (i > start) out += ",";
            out += entries[i];
        }
        out += "]";
        return out;
    }

    // ── Send Gossip to all connections ─────────────────────────
    // Delta gossip: each connection receives only peers that changed since
    // its last_gossip_generation. First contact (gen 0) gets a full snapshot.
    void broadcast_gossip() {
        uint32_t cur_gen = gossip_generation_.load();
        HostStats hs = collect_host_stats(home_dir_);
        ServerInfoMsg info;
        info.hostname = config_.node_name;
        info.version = std::string(kBridgeSessionsVersion);
        info.load = hs.load1 >= 0 ? hs.load1 : 0.0;
        info.sessions_summary_json = build_sessions_summary_json();
        info.host_stats_json = host_stats_to_json(hs);

        for (auto& c : conns_) {
            if (c.sock_fd == INVALID_SOCKET) continue;
            if (c.exec_busy && c.exec_busy->load()) continue;
            // gen 0 → full snapshot; stale gen → delta; current gen → skip
            if (c.last_gossip_generation == 0 ||
                c.last_gossip_generation < cur_gen) {
                auto g = build_gossip(c.last_gossip_generation);
                if (!g.peers.empty())
                    (void)enqueue_frame(c, g, CONTROL_STREAM_ID);
                c.last_gossip_generation = cur_gen;
            }
            (void)enqueue_frame(c, info, CONTROL_STREAM_ID);
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
            // Non-blocking enqueue — never stall the event loop on a slow peer.
            if (!enqueue_frame(c, PingMsg{}, CONTROL_STREAM_ID)) {
                // Overflow only: leave connection up; next tick may drain.
                log_event("mesh_ping_enqueue_drop", c.peer_name);
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

        // Config override: if config specifies a receive_dir, use that instead.
        // Needed when daemon runs as SYSTEM but needs files in user's home.
        if (!config_.receive_dir_override.empty()) {
            receive_dir_ = expand_home(config_.receive_dir_override);
        }
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
    static constexpr size_t kMaxPendingHandshakes_for_test() {
        return kMaxPendingHandshakes;
    }
    bool start_outbound_handshake_for_test(const PeerEntry& peer) {
        return start_outbound_handshake(peer);
    }
    [[nodiscard]] long next_backoff_ms_for_test(int attempt) const {
        return next_backoff_ms(attempt);
    }
    void advance_handshakes_for_test() { advance_handshakes(); }
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
        // 2.0.8: only remove when the file still contains OUR token — CLI mesh
        // clients (e.g. `bs shell`) share this path with the long-running daemon;
        // without the check, a client exit would strip the daemon's live token.
        if (!ipc_token_path_.empty() && !ipc_token_.empty()) {
            std::error_code ec;
            std::string on_disk;
            {
                std::ifstream f(ipc_token_path_);
                if (f) on_disk.assign(std::istreambuf_iterator<char>(f),
                                      std::istreambuf_iterator<char>());
            }
            // Exact-match (mod trailing whitespace): a substring check would
            // delete a file that merely CONTAINS our token among other data.
            while (!on_disk.empty() &&
                   (on_disk.back() == '\n' || on_disk.back() == '\r' ||
                    on_disk.back() == ' ' || on_disk.back() == '\t'))
                on_disk.pop_back();
            if (!on_disk.empty() && on_disk == ipc_token_) {
                std::filesystem::remove(ipc_token_path_, ec);
            }
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
        // hold the loop beyond the deadline regardless of request size.
        set_socket_timeouts(cfd, 250);
        // 128 KiB request buffer: CONV_APPEND carries b64 bodies up to the
        // 65535-byte wire limit (≈87.4 KiB encoded) plus token + verb
        // overhead. The absolute 2s deadline is the slow-trickle DoS control,
        // NOT the buffer size — enlarging the buffer does not widen that
        // window, it only raises the max single-request length.
        std::vector<char> req_buf(128 * 1024);
        char* buf = req_buf.data();
        int n = 0;
        bool newline_seen = false;
        const auto request_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::seconds(2);
        while (n < static_cast<int>(req_buf.size() - 1) &&
               std::chrono::steady_clock::now() < request_deadline) {
            int got = recv(cfd, buf + n,
                           static_cast<int>(req_buf.size() - 1) - n, 0);
            if (got <= 0) break;
            n += got;
            if (std::memchr(buf, '\n', static_cast<size_t>(n)) != nullptr) {
                newline_seen = true;
                break;
            }
        }
        std::string response = "ERROR bad request\n";
        bool response_sent = false;
        if (n > 0) {
            buf[n] = '\0';
            std::string line(buf);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            const bool authorized = const_time_token_match(line, ipc_token_);
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
            else if (line == "INVITE") {
                // Generate a 2-hour invite token
                std::lock_guard lock(invite_mutex_);
                unsigned char raw_bytes[16];
                RAND_bytes(raw_bytes, sizeof(raw_bytes));
                std::ostringstream tok;
                for (size_t i = 0; i < sizeof(raw_bytes); ++i)
                    tok << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(raw_bytes[i]);
                auto now = std::chrono::steady_clock::now();
                pending_invites_.erase(
                    std::remove_if(pending_invites_.begin(), pending_invites_.end(),
                        [now](auto& p) { return (now - p.created_at) > std::chrono::hours(2); }),
                    pending_invites_.end());
                PendingInvite pi;
                pi.token = tok.str();
                pi.created_at = now;
                response = pi.token + "\n";
                pending_invites_.push_back(std::move(pi));
                // Open the join window so unknown peers can TLS-connect to
                // present their invite token. Closed after successful join
                // or naturally expires when invites time out.
                g_allow_join_connections.store(true, std::memory_order_relaxed);
            }
            else if (line == "SESSIONS") {
                response = sessions_.summary() + "\n";
            }
            else if (line == "PEERS") {
                // One line per live mesh conn: name addr state=... last_pong_s=N
                std::ostringstream out;
                auto now = std::chrono::steady_clock::now();
                auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                                  ? config_.pong_timeout_secs : 30);
                for (auto& c : conns_) {
                    if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
                    bool ok = (now - c.last_pong) <= fresh;
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - c.last_pong).count();
                    out << c.peer_name << " " << c.peer_addr
                        << " state=" << (ok ? "healthy" : "no-pong")
                        << " last_pong_s=" << age << "\n";
                }
                out << "END\n";
                response = out.str();
            }
            else if (line == "FLEET") {
                // JSON fleet directory: self + live mesh peers + configured seeds
                // that are not connected (status=offline). Host metrics only when
                // known — never invent load1=0.0 from legacy ServerInfo defaults.
                auto emit_host_fields = [](std::ostringstream& out, const HostStats& hs) {
                    auto num_or_null = [](double v) -> std::string {
                        if (v < 0) return "null";
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "%.1f", v);
                        return buf;
                    };
                    out << ",\"cpu_pct\":" << num_or_null(hs.cpu_pct)
                        << ",\"mem_pct\":" << num_or_null(hs.mem_pct)
                        << ",\"disk_pct\":" << num_or_null(hs.disk_pct)
                        << ",\"load1\":" << num_or_null(hs.load1)
                        << ",\"os\":\"" << gossip_json_escape(hs.os) << "\""
                        << ",\"arch\":\"" << gossip_json_escape(hs.arch) << "\""
                        << ",\"ncpu\":" << hs.ncpu
                        << ",\"mem_used_mb\":" << hs.mem_used_mb
                        << ",\"mem_total_mb\":" << hs.mem_total_mb
                        << ",\"disk_used_gb\":" << hs.disk_used_gb
                        << ",\"disk_total_gb\":" << hs.disk_total_gb
                        << ",\"cua\":" << (hs.cua_helper ? "true" : "false")
                        << ",\"metrics\":true";
                };
                // Only emit metrics when peer advertised host_stats_json.
                // Do NOT fall back to ServerInfo.load — legacy peers leave it 0.
                auto emit_host_from_json = [&](std::ostringstream& out,
                                               const std::string& js) {
                    if (js.empty()) {
                        out << ",\"metrics\":false";
                        return;
                    }
                    try {
                        auto j = nlohmann::json::parse(js);
                        auto getd = [&](const char* k) -> std::string {
                            if (!j.contains(k) || j[k].is_null()) return "null";
                            if (j[k].is_number()) {
                                char buf[32];
                                std::snprintf(buf, sizeof(buf), "%.1f", j[k].get<double>());
                                return buf;
                            }
                            return "null";
                        };
                        out << ",\"cpu_pct\":" << getd("cpu")
                            << ",\"mem_pct\":" << getd("mem")
                            << ",\"disk_pct\":" << getd("disk")
                            << ",\"load1\":" << getd("load")
                            << ",\"os\":\"" << gossip_json_escape(j.value("os", "")) << "\""
                            << ",\"arch\":\"" << gossip_json_escape(j.value("arch", "")) << "\""
                            << ",\"ncpu\":" << j.value("ncpu", 0)
                            << ",\"mem_total_mb\":" << j.value("mem_mb", 0)
                            << ",\"disk_total_gb\":" << j.value("disk_gb", 0)
                            << ",\"cua\":" << (j.value("cua", false) ? "true" : "false")
                            << ",\"metrics\":true";
                    } catch (...) {
                        out << ",\"metrics\":false";
                    }
                };
                auto seed_addr_for = [&](const std::string& name) -> std::string {
                    for (const auto& s : config_.seeds)
                        if (peer_name_eq(s.name, name) && !s.addr.empty()) return s.addr;
                    return {};
                };

                std::ostringstream out;
                out << "{";
                auto now = std::chrono::steady_clock::now();
                auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                                  ? config_.pong_timeout_secs : 30);
                HostStats self_hs = collect_host_stats(home_dir_);
                if (self_hs.cpu_pct < 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                    self_hs = collect_host_stats(home_dir_);
                }
                std::unordered_set<std::string> listed; // lowercased names
                auto mark = [&](const std::string& n) {
                    std::string k = n;
                    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    listed.insert(k);
                };
                auto listed_has = [&](const std::string& n) {
                    std::string k = n;
                    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return listed.count(k) > 0;
                };

                // Self: prefer real advertise IP over 0.0.0.0 listen bind.
                std::string self_host = self_hs.primary_addr;
                if (self_host.empty() || self_host == "0.0.0.0" || self_host == "::")
                    self_host = config_.listen_addr;
                if (self_host.empty() || self_host == "0.0.0.0" || self_host == "::")
                    self_host = "127.0.0.1";
                std::string self_addr = self_host + ":" + std::to_string(config_.listen_port);
                out << "\"" << gossip_json_escape(config_.node_name) << "\":{\"name\":\""
                    << gossip_json_escape(config_.node_name)
                    << "\",\"addr\":\""
                    << gossip_json_escape(self_addr)
                    << "\",\"version\":\"" << gossip_json_escape(std::string(kBridgeSessionsVersion))
                    << "\",\"status\":\"self\"";
                emit_host_fields(out, self_hs);
                out << "}";
                mark(config_.node_name);

                // Live mesh peers
                for (auto& c : conns_) {
                    if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
                    if (c.peer_name.empty()) continue;
                    bool ok = (now - c.last_pong) <= fresh;
                    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                        now - c.connected_at).count();
                    // Prefer configured seed listen addr (stable) over conn
                    // peer_addr (often an ephemeral inbound source port).
                    std::string addr = peer_listen_addr_for(c.peer_name, c.peer_pubkey);
                    if (addr.empty()) addr = seed_addr_for(c.peer_name);
                    if (addr.empty()) addr = c.peer_addr;
                    out << ",\"" << gossip_json_escape(c.peer_name) << "\":{";
                    out << "\"addr\":\"" << gossip_json_escape(addr) << "\",";
                    out << "\"version\":\"" << gossip_json_escape(c.remote_version) << "\",";
                    out << "\"status\":\"" << (ok ? "healthy" : "no-pong") << "\",";
                    out << "\"uptime_s\":" << uptime;
                    emit_host_from_json(out, c.remote_host_stats_json);
                    out << "}";
                    mark(c.peer_name);
                }

                // Configured seeds with no live conn → offline (full fleet directory)
                for (const auto& s : config_.seeds) {
                    if (s.name.empty()) continue;
                    if (peer_name_eq(s.name, config_.node_name)) continue;
                    if (listed_has(s.name)) continue;
                    out << ",\"" << gossip_json_escape(s.name) << "\":{";
                    out << "\"name\":\"" << gossip_json_escape(s.name) << "\",";
                    out << "\"addr\":\"" << gossip_json_escape(s.addr) << "\",";
                    out << "\"version\":\"\",";
                    out << "\"status\":\"offline\",";
                    out << "\"source\":\"seed\",";
                    out << "\"metrics\":false";
                    out << "}";
                    mark(s.name);
                }

                // Runtime-learned (discovered) peers with no live conn → stale
                // (ephemeral rentals that have not been seen for a while but are
                // still within mesh.discovered_ttl_secs before auto-prune).
                for (const auto& d : config_.discovered) {
                    if (d.name.empty()) continue;
                    if (peer_name_eq(d.name, config_.node_name)) continue;
                    if (listed_has(d.name)) continue;
                    out << ",\"" << gossip_json_escape(d.name) << "\":{";
                    out << "\"name\":\"" << gossip_json_escape(d.name) << "\",";
                    out << "\"addr\":\"" << gossip_json_escape(d.addr) << "\",";
                    out << "\"version\":\"\",";
                    out << "\"status\":\"stale\",";
                    out << "\"source\":\"discovered\",";
                    out << "\"last_seen\":" << d.last_seen << ",";
                    out << "\"metrics\":false";
                    out << "}";
                    mark(d.name);
                }
                out << "}\n";
                response = out.str();
            }
            else if (line.rfind("SCROLLBACK ", 0) == 0) {
                // SCROLLBACK <session> <since_byte> → OK <new_offset> <b64>[ RESET]
                auto rest = line.substr(11);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: SCROLLBACK <session> <since_byte>\n";
                } else {
                    std::string sname = rest.substr(0, sp);
                    size_t since = 0;
                    bool bad_offset = false;
                    try { since = static_cast<size_t>(std::stoull(rest.substr(sp + 1))); }
                    catch (...) { bad_offset = true; }
                    if (bad_offset) {
                        response = "ERROR bad offset\n";
                    } else {
                        auto* s = sessions_.get(sname);
                        if (!s) {
                            response = "ERROR no such session\n";
                        } else {
                            auto [chunk, reset] = s->scrollback.read_since(since);
                            // On RESET the client fast-forwards to the live edge.
                            size_t new_off = reset ? s->scrollback.total_written()
                                                   : since + chunk.size();
                            response = "OK " + std::to_string(new_off) + " "
                                     + b64enc(chunk) + (reset ? " RESET" : "") + "\n";
                        }
                    }
                }
            }
            else if (line == "MESH_TREE") {
                // Single-line JSON: node, uptime, peers[], local sessions[]
                std::ostringstream out;
                auto now = std::chrono::steady_clock::now();
                auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - started_at_).count();
                auto fresh = std::chrono::seconds(config_.pong_timeout_secs > 0
                                                  ? config_.pong_timeout_secs : 30);
                auto jesc = [](const std::string& v) {
                    std::string r;
                    for (char ch : v) {
                        switch (ch) {
                            case '"': r += "\\\""; break;
                            case '\\': r += "\\\\"; break;
                            case '\n': r += "\\n"; break;
                            case '\r': r += "\\r"; break;
                            case '\t': r += "\\t"; break;
                            default:
                                if (static_cast<unsigned char>(ch) < 0x20) {
                                    char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", ch);
                                    r += buf;
                                } else r += ch;
                        }
                    }
                    return r;
                };
                out << "{\"node\":\"" << jesc(config_.node_name) << "\","
                    << "\"uptime_s\":" << uptime << ",\"peers\":[";
                bool first = true;
                for (auto& c : conns_) {
                    if (c.purpose != ConnectionPurpose::Mesh || c.sock_fd == INVALID_SOCKET) continue;
                    if (!first) out << ",";
                    first = false;
                    bool ok = (now - c.last_pong) <= fresh;
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(now - c.last_pong).count();
                    std::string peer_sessions = "[]";
                    {
                        std::shared_lock glock(gossip_sessions_mutex_);
                        auto git = gossip_sessions_json_.find(c.peer_name);
                        if (git != gossip_sessions_json_.end()) peer_sessions = git->second;
                    }
                    out << "{\"name\":\"" << jesc(c.peer_name) << "\","
                        << "\"addr\":\"" << jesc(c.peer_addr) << "\","
                        << "\"healthy\":" << (ok ? "true" : "false") << ","
                        << "\"last_pong_s\":" << age << ","
                        << "\"sessions\":" << peer_sessions << "}";
                }
                out << "],\"sessions\":[";
                first = true;
                for (const auto& info : sessions_.list()) {
                    auto* s = sessions_.get(info.name);
                    if (!s) continue;
                    if (!first) out << ",";
                    first = false;
                    out << "{\"name\":\"" << jesc(s->name) << "\","
                        << "\"state\":\"" << session_state_str(s->state) << "\","
                        << "\"kind\":\"" << session_kind_str(s->kind) << "\","
                        << "\"command\":\"" << jesc(s->command) << "\","
                        << "\"bytes\":" << s->scrollback.total_written() << "}";
                }
                out << "]}\n";
                response = out.str();
            }
            else if (line == "TELEMETRY") {
                response = transfer_telemetry_.to_json() + "\n";
            }
            else if (line.rfind("CONV_APPEND ", 0) == 0) {
                // CONV_APPEND <conv_id> <role> <b64_body> → OK <seq>
                auto rest = line.substr(12);
                auto sp1 = rest.find(' ');
                auto sp2 = (sp1 == std::string::npos) ? sp1 : rest.find(' ', sp1 + 1);
                if (sp1 == std::string::npos || sp2 == std::string::npos) {
                    response = "ERROR usage: CONV_APPEND <conv> <role> <b64>\n";
                } else {
                    std::string conv = rest.substr(0, sp1);
                    std::string role = rest.substr(sp1 + 1, sp2 - sp1 - 1);
                    std::string body_b64 = rest.substr(sp2 + 1);
                    uint8_t role_u8;
                    if (role == "system") role_u8 = 0;
                    else if (role == "user") role_u8 = 1;
                    else if (role == "agent") role_u8 = 2;
                    else if (role == "tool") role_u8 = 3;
                    else {
                        response = "ERROR bad role\n";
                        role_u8 = 255;
                    }
                    if (role_u8 != 255) {
                        ConversationAppendMsg ca;
                        ca.conv_id = conv;
                        ca.role = role_u8;
                        ca.agent_id = "ipc";
                        ca.body = b64dec(body_b64);
                        if (ca.body.size() > 65535) {
                            // Wire limit (u16-prefixed) — reject before store
                            // so a later ConversationQuery serialize can never throw.
                            response = "ERROR body too large (max 65535 bytes)\n";
                        } else {
                        using namespace std::chrono;
                        ca.ts = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
                        {
                            std::lock_guard lock(conversations_mutex_);
                            ca.seq = next_conv_seq_++;
                            conversations_[ca.conv_id].push_back(ca);
                            // Same bounds as the mesh path (2.0.8 MoA).
                            auto& vec = conversations_[ca.conv_id];
                            static constexpr size_t kMaxMsgsPerConv = 10000;
                            if (vec.size() > kMaxMsgsPerConv)
                                vec.erase(vec.begin(), vec.begin() + (vec.size() - kMaxMsgsPerConv));
                            static constexpr size_t kMaxConvs = 1024;
                            if (conversations_.size() > kMaxConvs)
                                conversations_.erase(conversations_.begin());
                        }
                        log_event("conversation_append", ca.conv_id + " seq=" + std::to_string(ca.seq) + " via=ipc");
                        response = "OK " + std::to_string(ca.seq) + "\n";
                        }
                    }
                }
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
                // FILE_SEND_WAIT_B64 <peer> <b64-local-path> [b64-dest-path]
                auto rest = line.substr(19);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: FILE_SEND_WAIT_B64 <peer> <b64-local-path> [b64-dest]\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string rest2 = rest.substr(sp + 1);
                    auto sp2 = rest2.find(' ');
                    std::string path, dest;
                    if (sp2 == std::string::npos) {
                        path = b64dec(rest2);
                    } else {
                        path = b64dec(rest2.substr(0, sp2));
                        dest = b64dec(rest2.substr(sp2 + 1));
                    }
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
                        task.path2 = dest;  // scp-style remote dest (optional)
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
            else if (line.rfind("CUA_VIDEO_CAPTURE_B64 ", 0) == 0) {
                auto rest = line.substr(22);
                auto sp = rest.find(' ');
                if (sp == std::string::npos) {
                    response = "ERROR usage: CUA_VIDEO_CAPTURE_B64 <peer> <b64-fps-dur-qual-maxw>\n";
                } else {
                    std::string peer_name = rest.substr(0, sp);
                    std::string params_b64 = rest.substr(sp + 1);
                    std::string params = b64dec(params_b64);
                    // params format: "fps:duration:quality:maxw"
                    std::stringstream ss(params);
                    std::string token;
                    std::vector<int> vals;
                    while (std::getline(ss, token, ':')) {
                        // P3 audit fix: std::stoi throws on non-numeric input —
                        // malformed params crashed the daemon. Validate instead.
                        if (token.empty() ||
                            token.find_first_not_of("0123456789") != std::string::npos) {
                            vals.clear();
                            break;
                        }
                        try {
                            vals.push_back(std::stoi(token));
                        } catch (...) { vals.clear(); break; }
                    }
                    if (vals.size() < 4) {
                        response = "ERROR video capture: expected fps:duration:quality:maxw\n";
                    } else {
                        CuaVideoCaptureMsg req;
                        req.fps = static_cast<uint8_t>(vals[0]);
                        req.duration_sec = static_cast<uint16_t>(vals[1]);
                        req.quality = static_cast<uint8_t>(vals[2]);
                        req.max_width = static_cast<uint16_t>(vals[3]);
                        Conn* target = nullptr;
                        for (auto& c : conns_) {
                            if (is_live_mesh_transport_for(c, peer_name, false)) { target = &c; break; }
                        }
                        if (!target) {
                            response = "ERROR no conn to " + peer_name + "\n";
                        } else {
                            (void)req;
                            response =
                                "ERROR CUA_VIDEO_CAPTURE requires direct TLS; "
                                "use: bs capture-video " + peer_name + "\n";
                        }
                    }
                }
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
            // 2.0.8 MoA fix: send() may short-write (large SCROLLBACK/MESH_TREE
            // replies). Loop until the full reply is out or the socket fails —
            // a truncated reply silently corrupts the client's incremental sync.
            size_t sent_total = 0;
            while (sent_total < response.size()) {
                int snt = send(cfd, response.data() + sent_total,
                               static_cast<int>(response.size() - sent_total), 0);
                if (snt <= 0) break; // timeout/closed — client retries from last offset
                sent_total += static_cast<size_t>(snt);
            }
            if (!newline_seen) {
                // Overlong/truncated request: the client may still be sending.
                // Closing a socket with unread receive data triggers RST,
                // which can destroy our own in-flight response (observed as
                // ECONNRESET client-side). Half-close the write side, then
                // drain briefly so the peer's remaining bytes land harmlessly.
#ifdef _WIN32
                ::shutdown(cfd, SD_SEND);
#else
                ::shutdown(cfd, SHUT_WR);
#endif
                char sink[4096];
                const auto drain_deadline = std::chrono::steady_clock::now() +
                                            std::chrono::milliseconds(300);
                while (std::chrono::steady_clock::now() < drain_deadline) {
                    int got = recv(cfd, sink, static_cast<int>(sizeof(sink)), 0);
                    if (got <= 0) break; // EOF (client done) or 250ms timeout
                }
            }
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
                                    int wait_ms, bool wait_for_completion = false,
                                    const std::string& dest_path = {}) {
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
        std::string cmd = token + " FILE_SEND_WAIT_B64 " + peer_name + " " + b64enc(path);
        if (!dest_path.empty()) cmd += " " + b64enc(dest_path);
        cmd += "\n";
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
        std::string cmd = token + " FILE_RECV_WAIT_B64 " + peer_name + " " + b64enc(path) + " " + b64enc(local_dest) + "\n";
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

    // ── Startup network readiness gate ────────────────────────
    // Probes each seed's TCP port with a 2s connect timeout. Returns when any
    // seed is reachable or after max_secs. Services inbound connections while
    // waiting so the daemon isn't deaf during boot.
    void startup_wait_for_network(int max_secs) {
        if (max_secs <= 0 || config_.seeds.empty()) return;
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(max_secs);
        log_event("startup_network_wait_begin",
                  "max_secs=" + std::to_string(max_secs) +
                  " seeds=" + std::to_string(config_.seeds.size()));

        while (running_ && std::chrono::steady_clock::now() < deadline) {
            for (const auto& seed : config_.seeds) {
                if (seed.addr.empty()) continue;
                sockaddr_in sa = resolve_addr(seed.addr);
                SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
                if (sfd == INVALID_SOCKET) continue;
                set_socket_timeouts(sfd, 2000);
                auto cr = connect_socket_with_timeout(
                    sfd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa), 2000);
                if (cr.connected) {
                    CLOSESOCK(sfd);
                    log_event("startup_network_wait_ok", seed.addr);
                    return;
                }
                CLOSESOCK(sfd);
            }
            // Service inbound connections while waiting (non-blocking 1s tick).
            if (listen_fd_ != INVALID_SOCKET)
                service_reconnect_wait_once(1000);
            else
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        log_event("startup_network_wait_timeout",
                  "no seed reachable in " + std::to_string(max_secs) + "s");
    }

    // ── Main event loop ───────────────────────────────────────

    void run() {
        running_ = true;

        // P0 UAF fix: wire session-erased callback to null dangling attached_session pointers
        sessions_.set_on_session_erased([this](const std::string& name) {
            for (auto& c : conns_)
                if (c.attached_session && c.attached_session->name == name)
                    c.attached_session = nullptr;
        });

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
        // poll() has no FD_SETSIZE limit — high FDs are fine.
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

        // Boot-time network readiness gate: wait for at least one seed before
        // entering the event loop (config: mesh.startup_wait_secs, default 30).
        startup_wait_for_network(config_.startup_wait_secs);

        while (running_) {
            // 1. Build pollfd set (poll/WSAPoll — no FD_SETSIZE ceiling).
            std::vector<bs_pollfd> pfds;
            pfds.reserve(8 + conns_.size() + pending_handshakes_.size());

            auto add_poll = [&](SOCKET fd, short events) {
                if (fd == INVALID_SOCKET) return;
                bs_pollfd p{};
                p.fd = static_cast<decltype(p.fd)>(fd);
                p.events = events;
                p.revents = 0;
                pfds.push_back(p);
            };

            add_poll(listen_fd_, POLLIN);
            // CLI IPC listen — event-driven so health is not starved mid-handshake.
            if (cli_listen_fd_ != INVALID_SOCKET)
                add_poll(cli_listen_fd_, POLLIN);

            for (auto& c : conns_) {
                // Skip conns whose socket/SSL is owned by a worker (exec_busy).
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
                    short ev = POLLIN;
                    if (c.want_write || !c.tx_queue.empty() || !c.output_queue.empty()
                        || c.output_gap_pending)
                        ev = static_cast<short>(ev | POLLOUT);
                    add_poll(c.sock_fd, ev);
                }
            }
#ifndef _WIN32
            // PTY masters wake the loop immediately (avoids torn TUI frames).
            for (const auto& info : sessions_.list()) {
                Session* session = sessions_.get(info.name);
                if (!session || !session->is_pollable() || session->master_fd < 0)
                    continue;
                short ev = POLLIN;
                if (!session->pending_input.empty())
                    ev = static_cast<short>(ev | POLLOUT);
                add_poll(session->master_fd, ev);
            }
#endif
            if (mdns_fd_ != INVALID_SOCKET)
                add_poll(mdns_fd_, POLLIN);

            for (auto& ph : pending_handshakes_) {
                if (ph.sock_fd == INVALID_SOCKET) continue;
                short ev = 0;
                if (ph.want_read) ev = static_cast<short>(ev | POLLIN);
                if (ph.want_write) ev = static_cast<short>(ev | POLLOUT);
                if (ev == 0) ev = POLLIN; // always watch something
                add_poll(ph.sock_fd, ev);
            }

            // Adaptive timeout: Windows needs faster ConPTY polling when busy.
#ifdef _WIN32
            int poll_timeout_ms = sessions_.empty() ? 500 : 50;
#else
            int poll_timeout_ms = 100;
#endif
            int nfds = pfds.empty()
                ? 0
                : bs_poll(pfds.data(),
#ifdef _WIN32
                          static_cast<unsigned long>(pfds.size()),
#else
                          static_cast<nfds_t>(pfds.size()),
#endif
                          poll_timeout_ms);

            if (nfds < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEINTR) continue;
#else
                if (errno == EINTR) continue;
#endif
                log_event("mesh_poll_error", "errno=" + std::to_string(
#ifdef _WIN32
                    WSAGetLastError()
#else
                    errno
#endif
                ));
                continue;
            }
            // Snapshot readiness for the rest of the tick (pfds holds revents).
            const std::vector<bs_pollfd>& ready = pfds;

            auto now = std::chrono::steady_clock::now();
            maybe_reload_config_seeds();
            maybe_prune_revoked_connections();
            maybe_close_join_window();
            if (now - last_session_prune_time_ >= std::chrono::minutes(1)) {
                if (config_.idle_timeout_hours > 0)
                    sessions_.prune_idle(std::chrono::hours(config_.idle_timeout_hours));
                // Always prune agent/health one-shots — they accumulate PTYs otherwise.
                sessions_.prune_ephemeral_sessions(std::chrono::seconds(90));
                last_session_prune_time_ = now;
            }
            if (now - last_discovered_prune_time_ >= std::chrono::minutes(1)) {
                prune_stale_discovered_peers();
                last_discovered_prune_time_ = now;
            }
#ifndef _WIN32
            if (g_config_reload_requested.exchange(false))
                reload_seeds_from_disk();
#endif

            // Service CLI IPC the moment a request arrives (event-driven).
            if (cli_listen_fd_ != INVALID_SOCKET && poll_fd_readable(ready, cli_listen_fd_)) {
                cli_ipc_accept_one();
            }

            // 3. Accept new connections
            if (poll_fd_readable(ready, listen_fd_)) {
                accept_inbound();
            }
            // 3.5. mDNS read
            if (mdns_fd_ != INVALID_SOCKET && poll_fd_readable(ready, mdns_fd_)) {
                mdns_check();
            }

            // 4. Read from established connections
            for (int i = 0; i < static_cast<int>(conns_.size()); ++i) {
                auto& conn = conns_[static_cast<size_t>(i)];
                if (conn.exec_busy && conn.exec_busy->load()) continue;
                if (conn.sock_fd != INVALID_SOCKET &&
                    poll_fd_readable(ready, conn.sock_fd)) {
                    check_conn_read(i);
                }
            }

            // 4.2. Writable sockets: flush non-blocking TX queues.
            for (auto& conn : conns_) {
                if (conn.exec_busy && conn.exec_busy->load()) continue;
                if (conn.sock_fd == INVALID_SOCKET) continue;
                if (conn.want_write || !conn.tx_queue.empty()) {
                    if (poll_fd_writable(ready, conn.sock_fd) || SSL_pending(conn.ssl.get()) > 0
                        || !conn.tx_queue.empty()) {
                        flush_tx_queue(conn);
                    }
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

            // 9.6. Drain per-connection output queues (2.0.8 P3 streaming)
            drain_output_queues();

#ifndef _WIN32
            for (const auto& info : sessions_.list()) {
                Session* session = sessions_.get(info.name);
                if (!session || session->pending_input.empty() || session->master_fd < 0)
                    continue;
                if (poll_fd_writable(ready, session->master_fd))
                    (void)drain_pending_pty_input(*session);
            }
#endif

            // 10. Reap dead sessions
            // Attached-session death is owned by pty_output_poller(), which
            // drains final output and emits SessionDied. Reaping it here first
            // steals waitpid() and leaves the client waiting forever.
            sessions_.reap_dead(false);
            (void)nfds; // readiness is driven by poll revents, not the count
        }

        // P1 fix: persist sessions on graceful shutdown
        try { sessions_.save_persisted_sessions(); } catch (...) {}
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

    // ── Peer name resolution: 4-tier fuzzy matching ────────────
    // Tier 1: exact case-insensitive (backward-compatible)
    // Tier 2: (reserved for config aliases — not yet in config format)
    // Tier 3: hyphen-segment suffix/prefix (shadow → lab-shadow)
    // Tier 4: Levenshtein ≤ 2 (shadwo → shadow)
    struct PeerResolveResult {
        std::string name;       // resolved canonical name, empty if not found
        std::string addr;       // "host:port"
        std::string pubkey_hex;
        enum Tier { None_, Exact, Suffix, Levenshtein } tier = None_;
        std::vector<std::string> suggestions; // when ambiguous, the candidates
    };

    // Collect all configured peer names (seeds + discovered).
    std::vector<std::string> all_peer_names() const {
        std::vector<std::string> names;
        names.reserve(config_.seeds.size() + config_.discovered.size());
        for (const auto& s : config_.seeds) names.push_back(s.name);
        for (const auto& d : config_.discovered) names.push_back(d.name);
        return names;
    }

    // Look up addr+pubkey for a canonical name.
    bool peer_lookup(const std::string& canonical,
                     std::string& addr, std::string& pubkey) const {
        for (const auto& s : config_.seeds)
            if (peer_name_eq(s.name, canonical)) { addr = s.addr; pubkey = s.pubkey_hex; return true; }
        for (const auto& d : config_.discovered)
            if (peer_name_eq(d.name, canonical)) { addr = d.addr; pubkey = d.pubkey_hex; return true; }
        return false;
    }

    [[nodiscard]] PeerResolveResult resolve_peer(const std::string& query) const {
        PeerResolveResult r;
        // Tier 1: exact match (case-insensitive)
        std::string addr, pubkey;
        if (peer_lookup(query, addr, pubkey)) {
            r.name = query; r.addr = addr; r.pubkey_hex = pubkey;
            r.tier = PeerResolveResult::Exact;
            return r;
        }
        // Tier 3: hyphen-segment suffix/prefix
        std::vector<std::string> segment_matches;
        auto check_segments = [&](const std::vector<PeerEntry>& peers) {
            for (const auto& p : peers) {
                if (name_has_segment(p.name, query))
                    segment_matches.push_back(p.name);
            }
        };
        check_segments(config_.seeds);
        check_segments(config_.discovered);
        // P2 fix: actually deduplicate (same peer in seeds + discovered)
        std::sort(segment_matches.begin(), segment_matches.end());
        segment_matches.erase(std::unique(segment_matches.begin(), segment_matches.end()), segment_matches.end());
        if (segment_matches.size() == 1) {
            if (peer_lookup(segment_matches[0], addr, pubkey)) {
                r.name = segment_matches[0]; r.addr = addr; r.pubkey_hex = pubkey;
                r.tier = PeerResolveResult::Suffix;
                return r;
            }
        }
        if (segment_matches.size() > 1) {
            r.suggestions = segment_matches;
            return r; // ambiguous
        }
        // Tier 4: Levenshtein ≤ 3 (consistent with print_peer_not_found close-check)
        std::vector<std::string> fuzzy_matches;
        auto check_fuzzy = [&](const std::vector<PeerEntry>& peers) {
            for (const auto& p : peers) {
                if (!query.empty() && query.size() >= 3 &&
                    p.name.size() >= 3 &&
                    levenshtein(query, p.name) <= 3)
                    fuzzy_matches.push_back(p.name);
            }
        };
        check_fuzzy(config_.seeds);
        check_fuzzy(config_.discovered);
        if (fuzzy_matches.size() == 1) {
            if (peer_lookup(fuzzy_matches[0], addr, pubkey)) {
                r.name = fuzzy_matches[0]; r.addr = addr; r.pubkey_hex = pubkey;
                r.tier = PeerResolveResult::Levenshtein;
                return r;
            }
        }
        if (fuzzy_matches.size() > 1) {
            r.suggestions = fuzzy_matches;
            return r; // ambiguous
        }
        // No match at any tier
        r.suggestions = segment_matches.empty() ? fuzzy_matches : segment_matches;
        return r;
    }

    // Print "Peer not found" with suggestions and available peers list.
    void print_peer_not_found(const std::string& query) const {
        auto result = resolve_peer(query);
        bool showed_suggestions = false;
        if (!result.suggestions.empty()) {
            showed_suggestions = true;
            std::cerr << "Ambiguous peer name: \"" << query << "\"\n";
            std::cerr << "Did you mean one of these?\n";
            for (const auto& s : result.suggestions)
                std::cerr << "  " << s << "\n";
        } else {
            std::cerr << "Peer not found: \"" << query << "\"\n";
            // Offer fuzzy suggestions if any are close
            std::vector<std::string> close;
            auto check_close = [&](const std::vector<PeerEntry>& peers) {
                for (const auto& p : peers) {
                    if (!query.empty() && query.size() >= 3 &&
                        p.name.size() >= 3 &&
                        levenshtein(query, p.name) <= 3)
                        close.push_back(p.name);
                }
            };
            check_close(config_.seeds);
            check_close(config_.discovered);
            if (!close.empty()) {
                showed_suggestions = true;
                std::cerr << "Did you mean one of these?\n";
                for (const auto& s : close)
                    std::cerr << "  " << s << "\n";
            }
        }
        // P3: only dump the full peer list when no suggestions were shown
        if (!showed_suggestions) {
            auto names = all_peer_names();
            if (!names.empty()) {
                std::cerr << "\nAvailable peers:\n";
                for (const auto& n : names)
                    std::cerr << "  " << n << "\n";
            } else {
                std::cerr << "\nNo peers configured. Use 'bs seed <host:port>' to add one.\n";
            }
        }
    }

    // print_connect_failure + connect_with_retry defined after SslConn/connect_and_hello below

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
                              const std::string& expected_pubkey = {},
                              bool trust_on_first_use = false) {
        SslConn out;
        if (expected_pubkey.empty() && !trust_on_first_use) {
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
        set_tcp_nodelay(sfd);  // interactive shell performance
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

    // ── Rich connect failure diagnostics ───────────────────────
    // Per-reason actionable error messages.
    void print_connect_failure(const std::string& peer_name, const SslConn& sc) const {
        std::cerr << "\nFailed to connect to " << peer_name << ": "
                  << connect_fail_string(sc.fail);
        if (!sc.fail_detail.empty())
            std::cerr << " (" << sc.fail_detail << ")";
        std::cerr << "\n\n";
        switch (sc.fail) {
        case ConnectFailReason::Refused:
            std::cerr << "The peer refused the TCP connection.\n"
                      << "  → Is the bridgesessions daemon running on " << peer_name << "?\n"
                      << "    Try: bs activate  (on the remote host)\n"
                      << "  → Check firewall rules allow port 19949.\n";
            break;
        case ConnectFailReason::Timeout:
            std::cerr << "Connection timed out — no response from " << peer_name << ".\n"
                      << "  → Check network/VPN/Tailscale connectivity.\n"
                      << "    Try: ping <peer-ip>  or  tailscale status\n"
                      << "  → The peer may be offline or overloaded.\n";
            break;
        case ConnectFailReason::TlsRejected:
            std::cerr << "TLS handshake failed — key mismatch or rejection.\n"
                      << "  → The peer's public key doesn't match the pinned key.\n"
                      << "    Re-authorize: bs seed <host:port> pubkey=<new-key>\n"
                      << "  → First connect uses trust-on-first-use (TOFU).\n"
                      << "    Verify the key out-of-band before trusting.\n";
            break;
        case ConnectFailReason::HelloRejected:
            std::cerr << "Hello/auth handshake failed.\n"
                      << "  → Possible version incompatibility or authorization issue.\n"
                      << "    Check: bs ctl logs  (on the remote peer)\n"
                      << "  → Ensure both peers run compatible protocol versions.\n";
            break;
        default:
            std::cerr << "Unknown connection failure.\n"
                      << "  → Check: bs ctl logs\n";
            break;
        }
        std::cerr << "\n";
    }

    // ── Connect with retry for transient failures ──────────────
    // Retries only timeout/refused (transient). Permanent failures
    // (tls_rejected/hello_rejected) fail immediately. NEVER falls back
    // to ssh/winrm/telnet — see NO-FALLBACK CONTRACT at top of file.
    SslConn connect_with_retry(const std::string& addr,
                               const std::string& expected_pubkey,
                               int max_attempts = 3,
                               bool trust_on_first_use = false) {
        SslConn result;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            result = connect_and_hello(addr, expected_pubkey, trust_on_first_use);
            if (result.ssl && result.sfd != INVALID_SOCKET) break;
            const bool retryable = result.fail == ConnectFailReason::Timeout ||
                                   result.fail == ConnectFailReason::Refused;
            if (!retryable || attempt == max_attempts - 1) break;
            // Linear backoff with jitter: 250ms, 500ms, ...
            int base_ms = 250 * (attempt + 1);
            int jitter = std::rand() % (base_ms / 4 + 1);
            std::this_thread::sleep_for(
                std::chrono::milliseconds(base_ms + jitter));
        }
        return result;
    }

    // ── Shutdown ───────────────────────────────────────────────

    void shutdown() { mdns_shutdown(); running_ = false; }

    // ── CLI: shell_peer_detach (--detach) ──────────────────────
    // Returns: 0 on success, 255 on failure (fire-and-forget).
    int shell_peer_detach(const std::string& peer_name, const std::string& session_name,
                          const std::string& cmd, uint16_t cols, uint16_t rows,
                          const std::string& term) {
        std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) { std::cerr << "Peer not found: " << peer_name << "\n"; return 255; }
        const std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        auto sc = connect_and_hello(addr, expected_pubkey);
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            print_connect_failure(peer_name, sc);
            return 255;
        }
        try {
            AttachMsg am;
            am.session_name = session_name;
            am.cols = cols; am.rows = rows; am.term = term;
            am.command = cmd;
            write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
            // Read AttachAck or error response to confirm session was created
            Message resp = read_frame(sc.ssl.get());
            if (std::holds_alternative<AttachAckMsg>(resp)) {
                auto& ack = std::get<AttachAckMsg>(resp);
                std::cout << "Session " << ack.session_name
                          << " started on " << peer_name
                          << " (attach_id=" << ack.attach_id << ")\n";
                std::cout << "Reattach: bs shell " << peer_name
                          << " -n " << ack.session_name << "\n";
                return 0;
            }
            if (std::holds_alternative<SessionDiedMsg>(resp)) {
                auto& sd = std::get<SessionDiedMsg>(resp);
                std::cerr << "Session died (exit=" << sd.exit_code
                          << " signal=" << sd.signal_num << ")\n";
                return sd.exit_code != 0 ? sd.exit_code : 1;
            }
            if (std::holds_alternative<OutputMsg>(resp)) {
                std::cerr << "Session output (detached): "
                          << std::get<OutputMsg>(resp).data << "\n";
                return 255;
            }
        } catch (...) {}
        // If we didn't get AttachAck, the session might still have been created
        std::cout << "Session sent to " << peer_name
                  << ". Use 'bs sessions' to verify.\n";
        return 0;
    }

    // ── CLI: shell_peer ────────────────────────────────────────
    // Returns: 0 on success (interactive), session exit_code on non-interactive,
    //          255 on connection/peer failure.
    int shell_peer(const std::string& peer_name, const std::string& session_name,
                   const std::string& cmd, uint16_t cols, uint16_t rows, const std::string& term,
                   bool signal_forward = true, const std::string& signal_on_detach = "") {
        // 4-tier peer name resolution (exact → suffix → levenshtein)
        auto resolved = resolve_peer(peer_name);
        if (resolved.name.empty()) {
            print_peer_not_found(peer_name);
            return 255;
        }
        // Warn on non-exact match
        if (resolved.tier == PeerResolveResult::Suffix)
            std::cerr << "Resolved \"" << peer_name << "\" → \"" << resolved.name << "\" (suffix match)\n";
        else if (resolved.tier == PeerResolveResult::Levenshtein)
            std::cerr << "Resolved \"" << peer_name << "\" → \"" << resolved.name << "\" (fuzzy match)\n";
        std::string addr = resolved.addr;  // non-const: interactive loop re-resolves on reconnect
        std::string expected_pubkey = resolved.pubkey_hex.empty()
            ? trusted_peer_pubkey(config_, resolved.name) : resolved.pubkey_hex;
        const bool non_interactive = !shell_command_uses_interactive_mode(cmd, stdin_is_terminal());

        // Ephemeral session name for one-shots on the CLI default "default" so
        // concurrent agents do not fight one shared PTY.
        std::string effective_session = session_name;
        if (non_interactive && !cmd.empty()
            && (effective_session.empty() || effective_session == "default")) {
            effective_session = make_ephemeral_cmd_session_name();
        }

        if (non_interactive) {
            auto sc = connect_with_retry(addr, expected_pubkey);
            if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
                print_connect_failure(resolved.name, sc);
                return 255;
            }
            int32_t exit_code = 0;
            bool running = true;
            bool transport_error = false;
            bool saw_session_end = false;
            const int shell_timeout_sec = noninteractive_shell_timeout_sec();
            const auto shell_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(shell_timeout_sec);
            try {
                AttachMsg am;
                am.session_name = effective_session;
                am.cols = cols;
                am.rows = rows;
                am.term = term;
                am.command = cmd;
                am.signal_on_detach = signal_on_detach;
                write_frame(sc.ssl.get(), am, CONTROL_STREAM_ID);
                while (running) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= shell_deadline) {
                        std::cerr << "Shell timed out after " << shell_timeout_sec
                                  << "s waiting for session end"
                                  << " (session=" << effective_session
                                  << ", peer=" << resolved.name << ")\n"
                                  << "  → One-shots use an ephemeral session; if this persists,"
                                  << " check peer version and PTY reaper health.\n";
                        CLOSESOCK(sc.sfd);
                        sc.sfd = INVALID_SOCKET;
                        return 124;
                    }
                    int remain_ms = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            shell_deadline - now).count());
                    if (remain_ms < 1) remain_ms = 1;
                    if (remain_ms > 5000) remain_ms = 5000;
                    fd_set read_fds;
                    FD_ZERO(&read_fds);
                    FD_SET((int)sc.sfd, &read_fds);
                    timeval tv{remain_ms / 1000, (remain_ms % 1000) * 1000};
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

                auto last_ctrl_c = std::chrono::steady_clock::time_point{};
                auto forward_local_input = [&](std::string_view input) {
                    // Double Ctrl-C (two 0x03 within 600ms) hard-terminates the
                    // session: send a Kill signal for the whole process group and
                    // leave the attach loop, instead of forwarding the second 0x03
                    // as a keystroke. A single Ctrl-C still forwards normally so
                    // nested TUIs can cancel/quit themselves.
                    if (!input.empty() && input.find('\x03') != std::string_view::npos) {
                        size_t ctrlc = 0;
                        for (char ch : input) if (ch == '\x03') ++ctrlc;
                        const auto now = std::chrono::steady_clock::now();
                        const bool double_press =
                            ctrlc >= 2 ||
                            (last_ctrl_c != std::chrono::steady_clock::time_point{} &&
                             (now - last_ctrl_c) <= std::chrono::milliseconds(600));
                        if (double_press) {
                            last_ctrl_c = std::chrono::steady_clock::time_point{};
                            try {
                                SignalMsg kill;
                                kill.signal = SignalMsg::SignalType::Kill;
                                write_frame(sc.ssl.get(), kill, CONTROL_STREAM_ID);
                            } catch (...) {}
                            local_stop = true;
                            transport_alive = false;
                            return;
                        }
                        last_ctrl_c = now;
                    }
                    // Every keystroke, including a single Ctrl-C (0x03), goes
                    // to the remote PTY. Nested TUIs (Claude Code) use double
                    // Ctrl-C to quit themselves — the client must not steal it.
                    // Leave the attach loop only on remote SessionDied / Detach,
                    // stdin EOF, or Ctrl-C while waiting to reconnect.
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
                    timeval sock_tv{0, 1000};  // 1ms poll — was 50ms (caused typing lag)
                    if (!local_stop && transport_alive &&
                        (select(0, &sock_fds, nullptr, nullptr, &sock_tv) > 0 ||
                         SSL_pending(sc.ssl.get()) > 0)) {
                        bool session_ended = false;
                        transport_alive = process_shell_response(sc.ssl.get(), &session_ended);
                        if (session_ended) local_stop = true;  // remote exit / Detach
                    }
#else
                    fd_set read_fds;
                    FD_ZERO(&read_fds);
                    FD_SET(STDIN_FILENO, &read_fds);
                    FD_SET((int)sc.sfd, &read_fds);
                    int maxfd = std::max(STDIN_FILENO, (int)sc.sfd);
                    timeval tv{0, 1000};  // 1ms poll — was 50ms (caused typing lag)
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
                        bool session_ended = false;
                        transport_alive = process_shell_response(sc.ssl.get(), &session_ended);
                        if (session_ended) local_stop = true;  // remote exit / Detach
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
            } else if (std::holds_alternative<ScrollbackMsg>(resp)) {
                // Reattach may push prior scrollback; strip and surface it so
                // agents see context, but do not treat as session end.
                std::cout << strip_ansi_escapes(std::get<ScrollbackMsg>(resp).data) << std::flush;
            } else if (std::holds_alternative<AttachAckMsg>(resp)) {
                // Handshake ack — keep waiting for Output/SessionDied.
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

    // Interactive shell response pump.
    // Returns true while the transport should keep reading.
    // Returns false on transport loss or session end.
    // If session_ended is non-null and the remote PTY exited cleanly (SessionDied /
    // ExitCode / server Detach after shell exit), *session_ended is set true so the
    // client leaves the attach loop instead of auto-reconnecting. Typing `exit`
    // in the remote shell produces SessionDied.
    bool process_shell_response(SSL* ssl, bool* session_ended = nullptr) {
        if (session_ended) *session_ended = false;
        try {
            Message resp = read_frame(ssl);
            if (std::holds_alternative<OutputMsg>(resp)) {
                std::cout << std::get<OutputMsg>(resp).data << std::flush;
            } else if (std::holds_alternative<ScrollbackMsg>(resp)) {
                std::cout << std::get<ScrollbackMsg>(resp).data << std::flush;
            } else if (std::holds_alternative<SessionDiedMsg>(resp)) {
                // Remote shell/PTY exited (e.g. user typed `exit`). Do not
                // resurrect — that would ignore the user's intent to leave.
                auto& sd = std::get<SessionDiedMsg>(resp);
                if (session_ended) *session_ended = true;
                std::cerr << "\r\n[session ended"
                          << " exit=" << sd.exit_code;
                if (sd.signal_num != 0)
                    std::cerr << " signal=" << sd.signal_num;
                std::cerr << "] — disconnecting...\r\n";
                return false;
            } else if (std::holds_alternative<ExitCodeMsg>(resp)) {
                if (session_ended) *session_ended = true;
                std::cerr << "\r\n[session ended exit="
                          << std::get<ExitCodeMsg>(resp).code
                          << "] — disconnecting...\r\n";
                return false;
            } else if (std::holds_alternative<DetachMsg>(resp)) {
                // Server-initiated detach without death — treat as leave (not
                // silent reconnect) so the CLI returns to the prompt.
                if (session_ended) *session_ended = true;
                std::cerr << "\r\n[detached] — disconnecting...\r\n";
                return false;
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
            auto drain_late_output = [&](int32_t code) {
                bool transport_eof = false;
                const auto drain_deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                while (!health_probe_drain_complete(
                           code, stdout_buf, nonce, transport_eof) &&
                       std::chrono::steady_clock::now() < drain_deadline) {
                    if (SSL_pending(sc.ssl.get()) <= 0) {
                        fd_set drain_fds;
                        FD_ZERO(&drain_fds);
                        FD_SET(sc.sfd, &drain_fds);
                        timeval drain_tv{0, 50'000};
#ifdef _WIN32
                        if (select(0, &drain_fds, nullptr, nullptr, &drain_tv) <= 0) continue;
#else
                        if (select(static_cast<int>(sc.sfd) + 1,
                                   &drain_fds, nullptr, nullptr, &drain_tv) <= 0) continue;
#endif
                    }
                    try {
                        Message late = read_frame(sc.ssl.get());
                        if (std::holds_alternative<OutputMsg>(late)) {
                            stdout_buf += strip_ansi_escapes(
                                std::get<OutputMsg>(late).data);
                        } else if (std::holds_alternative<PingMsg>(late)) {
                            write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                        }
                    } catch (...) {
                        transport_eof = true;
                    }
                }
            };
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
                    // Exit can race ahead of the final OutputMsg on Windows.
                    drain_late_output(exit_code);
                    break;
                } else if (std::holds_alternative<SessionDiedMsg>(resp)) {
                    exit_code = std::get<SessionDiedMsg>(resp).exit_code;
                    drain_late_output(exit_code);
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
    // dest_path: optional scp-style remote destination (absolute, ~/…, or
    // relative under peer receive_dir). Empty → classic receive_dir/basename.
    std::string file_send(const std::string& peer_name, const std::string& local_path,
                          bool wait_for_completion,
                          const std::string& dest_path = {}) {
        namespace fs = std::filesystem;
        if (!fs::exists(local_path) || fs::is_directory(local_path)) {
            return "ERROR file not found or is a directory: " + local_path;
        }
        // Try daemon IPC first (reuses existing mesh conns)
        std::string ipc = daemon_send_via_ipc(peer_name, local_path, 120000,
                                              wait_for_completion, dest_path);
        if (!ipc.empty()) {
            // If daemon IPC failed with "no conn", fall back to direct TLS
            if (ipc.rfind("ERROR no conn", 0) == 0 || ipc.rfind("ERROR no daemon", 0) == 0) {
                std::string direct = direct_connect_file_send(
                    peer_name, local_path, wait_for_completion, dest_path);
                if (!direct.empty()) return direct;
            }
            return ipc;
        }
        // No daemon at all — try direct TLS
        std::string direct = direct_connect_file_send(
            peer_name, local_path, wait_for_completion, dest_path);
        if (!direct.empty()) return direct;
        return "ERROR no daemon running and direct TLS file send failed — cannot send";
    }

    // ── CLI: file_recv ──────────────────────────────────────────
    // ── CLI: file_recv ──────────────────────────────────────────
    std::string file_recv(const std::string& peer_name, const std::string& remote_path,
                          const std::string& local_dest, bool wait_for_completion) {
        std::string dest = local_dest.empty() ? "." : local_dest;
        std::string ipc = daemon_recv_via_ipc(peer_name, remote_path, dest, 120000, wait_for_completion);
        if (!ipc.empty()) {
            // If daemon IPC failed with "no conn", fall back to direct TLS
            if (ipc.rfind("ERROR no conn", 0) == 0 || ipc.rfind("ERROR no daemon", 0) == 0) {
                std::string direct = direct_connect_file_recv(peer_name, remote_path, dest);
                if (!direct.empty()) return direct;
            }
            return ipc;
        }
        // No daemon at all — try direct TLS
        std::string direct = direct_connect_file_recv(peer_name, remote_path, dest);
        if (!direct.empty()) return direct;
        return "ERROR no daemon running and direct TLS file recv failed";
    }

    // ── CLI: capture_video ────────────────────────────────────────
    // Use a dedicated direct TLS connection. The old daemon-IPC path only
    // checked for a peer connection and then captured on the local machine.
    std::string capture_video(const std::string& peer_name, const CuaVideoCaptureMsg& request) {
        const std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) return "ERROR peer not found: " + peer_name;
        const std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        auto sc = connect_and_hello(addr, expected_pubkey);
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            const std::string detail = sc.fail_detail.empty()
                ? connect_fail_string(sc.fail)
                : sc.fail_detail;
            return "ERROR failed to connect to " + peer_name + ": " + detail;
        }

        CuaVideoCaptureMsg req = request;
        req.request_id = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds((std::max)(30, static_cast<int>(req.duration_sec) + 30));
        try {
            write_frame(sc.ssl.get(), req, CONTROL_STREAM_ID);
            while (std::chrono::steady_clock::now() < deadline) {
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET((int)sc.sfd, &read_fds);
                timeval tv{1, 0};
#ifdef _WIN32
                const int ready = select(0, &read_fds, nullptr, nullptr, &tv);
#else
                const int ready = select((int)sc.sfd + 1, &read_fds, nullptr, nullptr, &tv);
#endif
                if (ready < 0) {
#ifndef _WIN32
                    if (errno == EINTR) continue;
#endif
                    throw std::runtime_error("select failed while waiting for video capture");
                }
                if (ready == 0 && SSL_pending(sc.ssl.get()) <= 0) continue;

                Message msg = read_frame(sc.ssl.get());
                if (std::holds_alternative<CuaVideoCaptureResultMsg>(msg)) {
                    const auto& result = std::get<CuaVideoCaptureResultMsg>(msg);
                    if (result.request_id != req.request_id) continue;
                    ssl_close(sc.ssl.get(), sc.sfd);
                    sc.sfd = INVALID_SOCKET;
                    if (result.status != 0) return "ERROR " + result.error;
                    return "video captured at " + result.file_path +
                           " — use 'file recv " + peer_name + " " +
                           result.file_path + " .' to retrieve";
                }
                if (std::holds_alternative<PingMsg>(msg)) {
                    write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                }
            }
            ssl_close(sc.ssl.get(), sc.sfd);
            sc.sfd = INVALID_SOCKET;
            return "ERROR timed out waiting for remote video capture";
        } catch (const std::exception& e) {
            if (sc.sfd != INVALID_SOCKET) ssl_close(sc.ssl.get(), sc.sfd);
            return "ERROR remote video capture transport failed: " + std::string(e.what());
        }
    }

    // ── CLI: send_cua_request ──────────────────────────────────
    // Sends CuaRequestMsg to a peer via direct TLS, returns CuaResponseMsg.
    CuaResponseMsg send_cua_request(const std::string& peer_name, uint8_t action,
                                    int16_t x, int16_t y, uint8_t button,
                                    uint32_t hid_key, uint8_t modifiers,
                                    const std::string& text) {
        CuaResponseMsg err;
        err.status = 1;
        const std::string addr = find_peer_addr(peer_name);
        if (addr.empty()) { err.error = "peer not found: " + peer_name; return err; }
        const std::string expected_pubkey = trusted_peer_pubkey(config_, peer_name);
        auto sc = connect_and_hello(addr, expected_pubkey);
        if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
            err.error = "failed to connect to " + peer_name + ": " +
                (sc.fail_detail.empty() ? connect_fail_string(sc.fail) : sc.fail_detail);
            return err;
        }
        CuaRequestMsg req;
        req.request_id = static_cast<uint32_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        req.action = action;
        req.x = x; req.y = y;
        req.button = button;
        req.hid_key = hid_key;
        req.modifiers = modifiers;
        req.text = text;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        try {
            write_frame(sc.ssl.get(), req, CONTROL_STREAM_ID);
            while (std::chrono::steady_clock::now() < deadline) {
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET((int)sc.sfd, &read_fds);
                timeval tv{1, 0};
#ifdef _WIN32
                const int ready = select(0, &read_fds, nullptr, nullptr, &tv);
#else
                const int ready = select((int)sc.sfd + 1, &read_fds, nullptr, nullptr, &tv);
#endif
                if (ready < 0) {
#ifndef _WIN32
                    if (errno == EINTR) continue;
#endif
                    throw std::runtime_error("select failed while waiting for cua response");
                }
                if (ready == 0 && SSL_pending(sc.ssl.get()) <= 0) continue;
                Message msg = read_frame(sc.ssl.get());
                if (std::holds_alternative<CuaResponseMsg>(msg)) {
                    auto resp = std::get<CuaResponseMsg>(msg);
                    if (resp.request_id != req.request_id) continue;
                    ssl_close(sc.ssl.get(), sc.sfd);
                    sc.sfd = INVALID_SOCKET;
                    return resp;
                }
                if (std::holds_alternative<PingMsg>(msg)) {
                    write_frame(sc.ssl.get(), PongMsg{}, CONTROL_STREAM_ID);
                }
            }
            ssl_close(sc.ssl.get(), sc.sfd);
            sc.sfd = INVALID_SOCKET;
            err.error = "timed out waiting for remote cua response";
            return err;
        } catch (const std::exception& e) {
            if (sc.sfd != INVALID_SOCKET) ssl_close(sc.ssl.get(), sc.sfd);
            err.error = std::string("cua transport failed: ") + e.what();
            return err;
        }
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

    // ── CLI: run_script ─────────────────────────────────────────
    // Sends a script file to a peer, then executes it via shell_peer.
    // Composes file_send + shell execution — zero new wire protocol.
    int run_script(const std::string& peer_name, const std::string& local_path,
                   const std::string& interpreter = "auto",
                   int timeout_ms = 120000) {
        namespace fs = std::filesystem;

        // 1. Resolve local script file (slurp stdin if "-")
        std::string script_path = local_path;
        std::string temp_local; // cleaned up at end
        if (local_path == "-") {
            // Read all of stdin into a temp file
            std::string content((std::istreambuf_iterator<char>(std::cin)),
                                std::istreambuf_iterator<char>());
            if (content.empty()) {
                std::cerr << "ERROR stdin is empty — nothing to run\n";
                return 1;
            }
            // Detect extension from content shebang or default to .sh
            std::string ext = ".sh";
            if (content.size() >= 2 && content[0] == '#' && content[1] == '!') {
                auto nl = content.find('\n');
                std::string shebang = (nl != std::string::npos)
                    ? content.substr(0, nl) : content;
                if (shebang.find("python") != std::string::npos) ext = ".py";
                else if (shebang.find("powershell") != std::string::npos
                         || shebang.find("pwsh") != std::string::npos) ext = ".ps1";
            }
            temp_local = create_private_temp_file("stdin", ext);
            if (temp_local.empty()) {
                std::cerr << "ERROR cannot create temp file\n"; return 1;
            }
            std::ofstream tf(temp_local, std::ios::binary | std::ios::trunc);
            if (!tf) { std::cerr << "ERROR cannot write temp file\n"; return 1; }
            tf.write(content.data(), static_cast<std::streamsize>(content.size()));
            tf.close();
            script_path = temp_local;
        }

        // 2. Read script content locally — we'll pipe it to the remote via shell
        std::ifstream sf(script_path, std::ios::binary);
        if (!sf) {
            std::cerr << "ERROR cannot read script: " << script_path << "\n";
            if (!temp_local.empty()) fs::remove(temp_local);
            return 1;
        }
        std::string script_content((std::istreambuf_iterator<char>(sf)),
                                   std::istreambuf_iterator<char>());
        sf.close();

        // 3. Resolve interpreter
        std::string interp = interpreter;
        if (interp == "auto" || interp.empty()) {
            interp = detect_interpreter(script_path);
        }

        // 4. Remote temp is a random stem — never the operator filename.
        std::string b64 = base64_encode(script_content);
        for (unsigned char c : b64) {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
            if (!ok) {
                std::cerr << "ERROR run-script: invalid base64 encoding\n";
                if (!temp_local.empty()) fs::remove(temp_local);
                return 1;
            }
        }
        std::string exec_cmd;
        std::string session_id = "run-script";
        if (auto tok = generate_ipc_token(); tok.size() >= 16)
            session_id += "-" + tok.substr(0, 16);

        if (interp == "powershell" || interp == "pwsh") {
            std::string exe = (interp == "pwsh") ? "pwsh" : "powershell";
            exec_cmd = exe + " -NoProfile -Command \""
                "$d = Join-Path $env:USERPROFILE '.bridgesessions\\tmp'; "
                "New-Item -ItemType Directory -Force -Path $d | Out-Null; "
                "$p = Join-Path $d ([guid]::NewGuid().ToString('N') + '.ps1'); "
                "[IO.File]::WriteAllBytes($p, [Convert]::FromBase64String('" + b64 + "')); "
                + exe + " -ExecutionPolicy Bypass -NoProfile -File $p; "
                "$ec = $LASTEXITCODE; Remove-Item $p -ErrorAction SilentlyContinue; "
                "exit $ec\"";
        } else if (interp == "cmd" || interp == "bat") {
            exec_cmd = "powershell -NoProfile -Command \""
                "$d = Join-Path $env:USERPROFILE '.bridgesessions\\tmp'; "
                "New-Item -ItemType Directory -Force -Path $d | Out-Null; "
                "$p = Join-Path $d ([guid]::NewGuid().ToString('N') + '.cmd'); "
                "[IO.File]::WriteAllBytes($p, [Convert]::FromBase64String('" + b64 + "')); "
                "cmd /c $p; $ec = $LASTEXITCODE; "
                "Remove-Item $p -ErrorAction SilentlyContinue; exit $ec\"";
        } else {
            std::string runner = (interp == "python" || interp == "python3") ? "python3" : "sh";
            exec_cmd =
                "d=\"${HOME}/.bridgesessions/tmp\"; mkdir -p \"$d\" && chmod 700 \"$d\" && "
                "f=$(mktemp \"$d/bs-rs.XXXXXX\") && "
                "printf '%s\\n' '" + b64 + "' | base64 -d > \"$f\" && "
                + runner + " \"$f\"; rc=$?; rm -f \"$f\"; exit $rc";
        }

        std::cerr << "executing run-script on " << peer_name
                  << ": interpreter=" << interp
                  << " bytes=" << script_content.size() << "\n";
        if (!temp_local.empty()) fs::remove(temp_local);
        auto [cols, rows] = get_winsize();
        return shell_peer(peer_name, session_id, exec_cmd,
                          cols, rows, "xterm-256color");
    }

    // Base64 encode for run-script (no external deps)
    static std::string base64_encode(const std::string& in) {
        static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        size_t i = 0;
        for (; i + 2 < in.size(); i += 3) {
            uint32_t n = (uint8_t)in[i] << 16 | (uint8_t)in[i+1] << 8 | (uint8_t)in[i+2];
            out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
            out += tbl[(n >> 6) & 63];  out += tbl[n & 63];
        }
        if (i < in.size()) {
            uint32_t n = (uint8_t)in[i] << 16;
            if (i + 1 < in.size()) n |= (uint8_t)in[i+1] << 8;
            out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
            out += (i + 1 < in.size()) ? tbl[(n >> 6) & 63] : '=';
            out += '=';
        }
        return out;
    }

    // P3: detect_interpreter returns "sh" for .sh to match the runner which uses "sh"
    static std::string detect_interpreter(const std::string& path) {
        namespace fs = std::filesystem;
        std::string ext = fs::path(path).extension().string();
        // lowercase
        for (auto& c : ext) { if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); }
        if (ext == ".sh") return "sh";
        if (ext == ".ps1") return "powershell";
        if (ext == ".py") return "python";
        if (ext == ".bat" || ext == ".cmd") return "cmd";
        // No extension or unknown: assume sh
        return "sh";
    }

    // ── Content-addressed script cache (bs script) ───────────────────
    // Scripts stored in ~/.bridgesessions/scripts/<sha256>.sh, named via symlink.

    [[nodiscard]] std::string script_cache_dir() const {
        return home_dir_ + "/scripts";
    }

    [[nodiscard]] std::string script_cache_path(const std::string& sha256) const {
        return script_cache_dir() + "/" + sha256 + ".sh";
    }

    // Resolve a script name (or hash) to its cache path. Returns empty on not found.
    [[nodiscard]] std::string script_resolve(const std::string& name_or_hash) const {
        namespace fs = std::filesystem;
        std::string dir = script_cache_dir();
        // Direct hash file
        std::string hash_path = script_cache_path(name_or_hash);
        if (fs::exists(hash_path)) return hash_path;
        // Symlink by name
        std::string link_path = dir + "/" + name_or_hash;
        if (fs::exists(link_path) && fs::is_symlink(link_path)) {
            auto target = fs::read_symlink(link_path);
            if (target.is_absolute()) return target.string();
            return (fs::path(dir) / target).string();
        }
        return {};
    }

    // bs script add <file> [--name alias]
    // Hashes content, stores as <sha256>.sh, optionally creates named symlink.
    // Returns "OK <sha256> <path>" or "ERROR ...".
    std::string script_add(const std::string& file_path, const std::string& alias) {
        namespace fs = std::filesystem;
        std::ifstream f(file_path, std::ios::binary);
        if (!f) return "ERROR cannot read file: " + file_path;
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        f.close();
        std::string hash = sha256_hex(content);
        std::string dir = script_cache_dir();
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string dest = script_cache_path(hash);
        if (!fs::exists(dest)) {
            std::ofstream out(dest, std::ios::binary | std::ios::trunc);
            if (!out) return "ERROR cannot write to cache: " + dest;
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            out.close();
        }
        std::string result = "OK " + hash + " " + dest;
        if (!alias.empty()) {
            std::string link = dir + "/" + alias;
            std::error_code lec;
            fs::remove(link, lec);
            std::string target = hash + ".sh";
            fs::create_symlink(target, link, lec);
            if (lec) {
                // Symlink may fail on Windows without admin — copy as fallback
                std::ofstream cp(link, std::ios::binary | std::ios::trunc);
                if (cp) cp.write(content.data(), static_cast<std::streamsize>(content.size()));
            }
            if (!lec) result += " alias=" + alias;
        }
        return result;
    }

    // bs script list — prints all cached scripts
    void script_list() const {
        namespace fs = std::filesystem;
        std::string dir = script_cache_dir();
        if (!fs::exists(dir)) { std::cout << "No scripts cached.\n"; return; }
        std::cout << "=== Cached scripts (" << dir << ") ===\n";
        // Collect symlinks (names) and hash files
        std::vector<std::string> hashes, names;
        for (auto& e : fs::directory_iterator(dir)) {
            auto name = e.path().filename().string();
            if (name.size() == 67 && name.substr(64) == ".sh") {
                hashes.push_back(name.substr(0, 64));
            } else if (e.is_symlink() || e.is_regular_file()) {
                // Skip .sh hash files (already handled)
                if (!(name.size() == 67 && name.substr(64) == ".sh"))
                    names.push_back(name);
            }
        }
        std::sort(hashes.begin(), hashes.end());
        for (auto& h : hashes) {
            std::cout << "  " << h.substr(0, 16) << "...";
            // Find aliases pointing to this hash
            std::string target_hash = h + ".sh";
            std::vector<std::string> aliases;
            for (auto& e : fs::directory_iterator(dir)) {
                if (e.is_symlink()) {
                    auto tgt = fs::read_symlink(e.path());
                    if (tgt.filename().string() == target_hash)
                        aliases.push_back(e.path().filename().string());
                }
            }
            if (!aliases.empty()) {
                std::cout << "  [";
                for (size_t i = 0; i < aliases.size(); ++i) {
                    if (i > 0) std::cout << ", ";
                    std::cout << aliases[i];
                }
                std::cout << "]";
            }
            std::cout << "\n";
        }
        if (hashes.empty()) std::cout << "  (none)\n";
    }

    // bs script remove <name> — remove symlink or hash file
    std::string script_remove(const std::string& name) {
        namespace fs = std::filesystem;
        std::string dir = script_cache_dir();
        std::string link = dir + "/" + name;
        std::error_code ec;
        if (fs::exists(link) || fs::is_symlink(link)) {
            fs::remove(link, ec);
            if (ec) return "ERROR failed to remove: " + link;
            return "OK removed " + name;
        }
        // Try as hash
        std::string hash_path = script_cache_path(name);
        if (fs::exists(hash_path)) {
            fs::remove(hash_path, ec);
            if (ec) return "ERROR failed to remove: " + hash_path;
            // Also remove any symlinks pointing to it
            for (auto& e : fs::directory_iterator(dir)) {
                if (e.is_symlink()) {
                    auto tgt = fs::read_symlink(e.path());
                    if (tgt.filename().string() == name + ".sh")
                        fs::remove(e.path(), ec);
                }
            }
            return "OK removed " + name;
        }
        return "ERROR script not found: " + name;
    }

    // bs script push <name> --peer <peer>
    // Sends script to peer's ~/.bridgesessions/scripts/ cache if not already present.
    // Composes file_send + shell_peer — no new wire protocol.
    std::string script_push(const std::string& name, const std::string& peer) {
        std::string path = script_resolve(name);
        if (path.empty()) return "ERROR script not found: " + name;
        namespace fs = std::filesystem;
        // Extract hash from filename (<hash>.sh)
        std::string fname = fs::path(path).filename().string();
        std::string hash;
        if (fname.size() >= 4 && fname.substr(fname.size() - 3) == ".sh")
            hash = fname.substr(0, fname.size() - 3);
        else
            return "ERROR unexpected cache filename: " + fname;

        // Check if peer already has it
        std::string check_cmd = "test -f ~/.bridgesessions/scripts/" + hash + ".sh && echo PRESENT || echo ABSENT";
        auto [cols, rows] = get_winsize();
        std::string check_output;
        // Use daemon_shell_via_ipc for quick check (non-interactive)
        int ec = daemon_shell_via_ipc(peer, "script-check-" + hash.substr(0, 8),
                                      check_cmd, &check_output);
        if (ec >= 0 && check_output.find("PRESENT") != std::string::npos) {
            return "OK " + hash + " already on " + peer;
        }
        // Also try direct shell for peer check if IPC failed
        if (ec < 0) {
            // Use shell_peer in non-interactive mode to check
            // (if daemon not available, shell_peer does direct TLS)
            std::string cmd_out;
            // Can't easily capture from shell_peer here — just push if check failed
        }

        // Ensure peer has the scripts directory
        std::string mkdir_cmd = "mkdir -p ~/.bridgesessions/scripts";
        daemon_shell_via_ipc(peer, "script-mkdir", mkdir_cmd, nullptr);

        // Send the script file via existing file_send
        std::string result = file_send(peer, path, true);
        if (result.rfind("ERROR", 0) == 0) return result;

        // Move received file to scripts cache on peer
        // file_send delivers to peer's received/ dir — move it
        std::string received_name = fname;
        std::string move_cmd = "mv ~/.bridgesessions/received/" + received_name
                             + " ~/.bridgesessions/scripts/" + hash + ".sh 2>/dev/null"
                             + " || true";
        daemon_shell_via_ipc(peer, "script-move-" + hash.substr(0, 8), move_cmd, nullptr);

        return "OK pushed " + hash + " to " + peer;
    }

    // bs script run <name> --peer <peer> [-- args...]
    // Ensures script is on peer (push if needed), then executes it.
    int script_run(const std::string& name, const std::string& peer,
                   const std::string& args) {
        std::string path = script_resolve(name);
        if (path.empty()) {
            std::cerr << "ERROR script not found: " + name + "\n";
            return 1;
        }
        namespace fs = std::filesystem;
        std::string fname = fs::path(path).filename().string();
        std::string hash;
        if (fname.size() >= 4 && fname.substr(fname.size() - 3) == ".sh")
            hash = fname.substr(0, fname.size() - 3);
        else {
            std::cerr << "ERROR unexpected cache filename: " + fname + "\n";
            return 1;
        }

        // Check if peer has it; push if not
        std::string check_cmd = "test -f ~/.bridgesessions/scripts/" + hash + ".sh && echo PRESENT || echo ABSENT";
        std::string check_output;
        int ec = daemon_shell_via_ipc(peer, "script-runcheck-" + hash.substr(0, 8),
                                      check_cmd, &check_output);
        bool need_push = (ec < 0) || (check_output.find("PRESENT") == std::string::npos);
        if (need_push) {
            std::cerr << "Script not on peer, pushing...\n";
            std::string push_result = script_push(name, peer);
            std::cerr << push_result << "\n";
            if (push_result.rfind("ERROR", 0) == 0) return 1;
        }

        // Execute on peer
        std::string exec_cmd = "bash ~/.bridgesessions/scripts/" + hash + ".sh";
        if (!args.empty()) exec_cmd += " " + args;
        auto [cols, rows] = get_winsize();
        return shell_peer(peer, "script-" + hash.substr(0, 8), exec_cmd,
                          cols, rows, "xterm-256color");
    }

    // ── CLI: show_stats ───────────────────────────────────────
    void show_stats() const {
        // Same table layout as daemon STATS IPC (local / no-daemon fallback).
        std::cout << daemon_stats_summary();
        if (!our_pubkey_.empty())
            std::cout << "pubkey   " << our_pubkey_.substr(0, 16) << "...\n";
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
        return const_time_token_match(line, ipc_token_);
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
    std::string tmpl = create_private_temp_file("img", "");
    if (tmpl.empty()) return false;
    int tmp_fd = ::open(tmpl.c_str(), O_WRONLY | O_TRUNC);
    if (tmp_fd < 0) { ::unlink(tmpl.c_str()); return false; }
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
    std::string tmpl = create_private_temp_file("frm", "");
    if (tmpl.empty()) return false;
    int tmp_fd = ::open(tmpl.c_str(), O_WRONLY | O_TRUNC);
    if (tmp_fd < 0) { ::unlink(tmpl.c_str()); return false; }
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
#include "bs-session-worker.h"

} // namespace bs::mesh
// ────────────────────────────────────────────────────────────────────
// Convert daemon pipe-separated SESSIONS output to JSON (global helper)

static std::string sess_text_to_json(const std::string& text) {
    if (text.empty() || text.find("No sessions.") == 0) return "[]";
    // Convert daemon pipe-separated SESSIONS output to JSON array
    std::ostringstream out;
    out << "[";
    bool first = true;
    std::istringstream ss(text);
    std::string record;
    while (std::getline(ss, record, '|')) {
        while (!record.empty() && (record.back() == '\n' || record.back() == '\r')) record.pop_back();
        if (record.empty()) continue;
        auto sp = record.find(' ');
        if (sp == std::string::npos) continue;
        std::string kind = record.substr(0, sp);
        std::string rest = record.substr(sp + 1);
        auto sp2 = rest.find(' ');
        std::string name = (sp2 == std::string::npos) ? rest : rest.substr(0, sp2);
        std::string kv = (sp2 == std::string::npos) ? "" : rest.substr(sp2 + 1);
        std::string state, command, pid, uptime, bytes;
        bool has_state = false, has_command = false, has_pid = false,
             has_uptime = false, has_bytes = false;
        std::istringstream kvs(kv);
        std::string token;
        while (kvs >> token) {
            auto eq = token.find('=');
            if (eq == std::string::npos) continue;
            std::string k = token.substr(0, eq);
            std::string v = token.substr(eq + 1);
            if (k == "state")      { state = v;   has_state = true; }
            else if (k == "command") { command = v; has_command = true; }
            else if (k == "pid")     { pid = v;     has_pid = true; }
            else if (k == "uptime")  { uptime = v;  has_uptime = true; }
            else if (k == "bytes")   { bytes = v;   has_bytes = true; }
        }
        // Omit fields that were not present in the IPC record instead of
        // emitting misleading "unknown"/"0" defaults (P2-3 audit finding).
        if (!first) out << ",";
        first = false;
        out << "{\"name\":\"" << name << "\",\"kind\":\"" << kind << "\"";
        if (has_state)   out << ",\"state\":\"" << state << "\"";
        if (has_command) out << ",\"command\":\"" << command << "\"";
        if (has_pid)     out << ",\"pid\":\"" << pid << "\"";
        if (has_uptime)  out << ",\"uptime_s\":" << uptime;
        if (has_bytes)   out << ",\"bytes\":" << bytes;
        out << "}";
    }
    out << "]";
    return out.str();
}
