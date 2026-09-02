// SPDX-License-Identifier: BUSL-1.1
// Copyright (c) Mind-Dragon. Licensed under the Business Source License 1.1.
// bridgesessions.cpp — Mesh peer-to-peer terminal sharing
// Facade: protocol, TLS, session, and mesh logic live in sibling headers included below.
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
#include <mach-o/dyld.h>  // global scope: worker header is included inside
                          // namespace bs::mesh and would otherwise pull this in there
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
#include <cwchar>
#include <algorithm>
#include <cctype>
#include <span>
#include <optional>
#include <expected>
#include <chrono>
#include <random>
#include <cstdio>
#include <cstdlib>
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
// Include facade (R6). Sibling headers are included in declaration
// order inside namespace bs::mesh — the same order as the former monolith.
//   bs-codec.h             messages, serializer, decoder, transfer checks
//   bs-tls.h               TLS transport + framed read/write
//   bs-osc52.h             OSC 52 clipboard scanner
//   bs-pty.h               PTY + hosted session-worker
//   bs-cua-dispatch.h      computer-use + video capture
//   bs-config.h            config, paths, persistence, protocol logs
//   bs-session-registry.h  session lifecycle
//   bs-mesh-controller.h   event loop, gossip, CLI helpers
// ────────────────────────────────────────────────────────────────────

namespace bs::mesh {

#include "bs-session.h"
#ifndef BS_VERSION
#define BS_VERSION "0.0.0-dev"
#endif
inline constexpr std::string_view kBridgeSessionsVersion = BS_VERSION;

#include "bs-codec.h"
#include "bs-tls.h"
#include "bs-osc52.h"
#include "bs-pty.h"
#include "bs-cua-dispatch.h"
#include "bs-config.h"
#include "bs-session-registry.h"
#include "bs-mesh-controller.h"

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
