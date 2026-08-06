# BridgeSessions File Transfer: Root Cause Analysis

## Architecture Overview

```
CLI (file_send)          Daemon (mesh event loop)         Peer
    │                          │                            │
    │── IPC: FILE_SEND_WAIT ──►│                            │
    │   (loopback TCP)         │                            │
    │                          │── find mesh Conn ──────────│
    │                          │── set exec_busy=true ──────│
    │                          │── worker_thread ───────────│
    │                          │      │                     │
    │                          │      ├── write_frame(meta) ├── TLS mesh conn
    │                          │      ├── write_frame(chunk)│
    │                          │      ├── wait_ack() ───────│
    │                          │      └── ...               │
    │◄── PROGRESS / OK ────────│      │                     │
    │   (IPC stream)           │◄─────│                     │
```

**shell_peer (CLI direct)** uses a completely different path:
```
CLI ─── connect_and_hello(peer_addr) ─── direct TLS ───► Peer
```

---

## ROOT CAUSE #1: No Direct TLS File Transfer Path

### The asymmetry

| Operation | Transport | Code |
|-----------|-----------|------|
| `shell_peer` | Direct TLS (`connect_and_hello`) | L12231 |
| `health_check` fallback | Direct TLS | L12637 |
| `file_send` | **Daemon IPC only** | L12735 |
| `file_recv` | **Daemon IPC only** | L12751 |

`file_send` (L12735-12747):
```cpp
std::string file_send(...) {
    std::string ipc = daemon_send_via_ipc(peer_name, local_path, 120000, wait_for_completion);
    if (!ipc.empty()) return ipc;
    return "ERROR no daemon running — cannot send without daemon mesh connection";  // DEAD END
}
```

Compare to `health_check` (L12604) which has a full fallback to `connect_and_hello`.

### Why it was designed this way
- File chunks ride the existing mesh Conn's multiplexed stream (`CONTROL_STREAM_ID = 0`).
- Reusing the mesh conn avoids a second TLS handshake.
- The worker thread model (LongOperationWorkerPool) was built around `exec_busy` to give the worker exclusive SSL access.

### The problem
1. **No daemon = no file transfer at all.** A daemonless CLI cannot send files.
2. **IPC is an extra hop:** CLI → loopback IPC → daemon → mesh TLS → peer. Each hop adds latency.
3. **IPC buffer is 8192 bytes** (L11186, L11329) — PROGRESS lines are byte-scanned line-by-line in `daemon_recv_via_ipc`.
4. **IPC timeout defaults to 120s** even when `--wait` allows up to 7200000ms (L11176). Mismatch.

---

## ROOT CAUSE #2: SSL Object Shared Between Worker Thread and Event Loop

### The race

The worker thread captures a raw `SSL*` pointer from the mesh Conn:
```cpp
// L10888-10889
task.ssl = target->ssl.get();
task.sock_fd = target->sock_fd;
```

The event loop skips busy conns via `exec_busy->load()` checks (L9611, L9692, L9780, L9832, L10085), **but only in specific code paths**. The race window:

1. Worker sets `exec_busy = true` (L10876).
2. **Between the exchange and the event loop's next select() iteration**, the event loop may already be in a `select()` that includes this fd, or may call `read_frame()` on it.
3. If a reconnect happens during a long transfer (heartbeat timeout, network blip), the event loop tears down the Conn and recreates it — **the worker's raw `SSL*` is now a dangling pointer**.

The `BlockingGuard` (L7552) changes socket to blocking mode, but this doesn't protect against the event loop's reconnect logic:
```cpp
struct BlockingGuard {
    SOCKET fd;
    explicit BlockingGuard(SOCKET f) : fd(f), orig(fcntl(f, F_GETFL, 0)) {
        fcntl(f, F_SETFL, orig & ~O_NONBLOCK);  // <-- event loop may race this
    }
};
```

### Evidence in comments (L6102-6107):
```cpp
static std::string shell_ipc_relay_policy_response() {
    // One-shot shell commands use their own direct TLS transport. Sharing a
    // mesh Conn with a detached IPC worker lets the event loop and worker
    // race the same SSL object during reconnect/duplicate cleanup.
    return "ERROR direct TLS required\n";
}
```
**Shell commands were explicitly banned from this pattern. File transfers were not.**

---

## ROOT CAUSE #3: Stop-and-Wait ACK Protocol with Tiny Pipeline

### Transfer loop (L7666-7722):
```cpp
for (uint32_t ci = 0; ci < total_chunks; /* batch */) {
    uint32_t batch_end = std::min(ci + kTransferPipelineSize, total_chunks);  // 8 chunks
    for (; ci < batch_end; ++ci) {
        write_frame(ssl, chunk, CONTROL_STREAM_ID);  // fire-and-forget
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // ← FIXED 2ms STALL
    if (batch_end < total_chunks) {
        ack = wait_ack(batch_end);  // ← BLOCKING wait for ack
    }
}
```

**Constants:**
- `kTransferChunkRawSize = 48 * 1024` (48 KB per chunk)
- `kTransferPipelineSize = 8` (384 KB per batch)
- `kTransferIdleTimeoutSec = 120`

**Effective throughput ceiling:**
- 8 chunks × 48KB = 384KB per RTT
- If RTT = 50ms → max ~7.5 MB/s
- If RTT = 200ms → max ~1.9 MB/s
- The `sleep_for(2ms)` per batch adds dead time: 100 batches = 200ms wasted

**On high-RTT mesh hops** (relay routing), the stop-and-wait at every 8 chunks is devastating. There is **no windowing, no sliding window, no selective ack**.

---

## ROOT CAUSE #4: "No conn" Errors Despite health=healthy

### `daemon_file_send` (L7490-7497):
```cpp
Conn* target = nullptr;
for (auto& c : conns_) {
    if (is_live_mesh_transport_for(c, peer_name)) { target = &c; break; }  // require_idle=true
}
if (!target) {
    log_event("file_send_error", "no conn to " + peer_name);
    return false;
}
```

### `is_live_mesh_transport_for` (L6078-6085):
```cpp
static bool is_live_mesh_transport_for(const Conn& conn, const std::string& peer_name,
                                       bool require_idle = true) {
    return conn.purpose == ConnectionPurpose::Mesh &&
           conn.sock_fd != INVALID_SOCKET &&
           (!require_idle || !conn.exec_busy || !conn.exec_busy->load()) &&  // ← BUSY = NOT LIVE
           peer_name_eq(conn.peer_name, peer_name);
}
```

**The bug:** `daemon_file_send` calls `is_live_mesh_transport_for` with `require_idle=true` (default), so a **healthy connection that is busy with any other transfer** returns false → "no conn".

The IPC handler (L10872) correctly passes `require_idle=false`, but then checks `exec_busy->exchange(true)` manually. So `daemon_file_send` (the fire-and-forget path at L7483) is **more restrictive** than the IPC path.

Meanwhile `health_check` probes via a completely different mechanism (daemon shell IPC or direct TLS attach) and reports healthy. The connection IS live — it's just busy, and `daemon_file_send` conflates "busy" with "no conn".

---

## ROOT CAUSE #5: Chunk Serialization Overhead

### `write_frame` path for FileChunkMsg:
Each chunk goes through:
1. `serialize_msg` — serialize chunk_index, total_chunks, data vector (L731)
2. Frame encoding with compression (v2.0.12c: encode() handles zstd)
3. TLS record write

For 48KB chunks, the per-chunk serialization overhead is small, but the **per-chunk TLS record framing** is significant at high throughput. TLS records have per-record MAC computation and encryption setup. There's no batching of multiple chunks into a single TLS record.

---

## CONCRETE FIXES

### Fix 1: Direct TLS File Transfer Path (eliminates IPC + race)

```cpp
// New: direct TLS file send (mirrors shell_peer's connect_and_hello pattern)
std::string file_send_direct(const std::string& peer_name,
                             const std::string& local_path,
                             bool wait_for_completion) {
    auto resolved = resolve_peer(peer_name);
    if (resolved.name.empty()) return "ERROR peer not found: " + peer_name;
    
    // Direct TLS — no daemon, no shared SSL
    auto sc = connect_with_retry(resolved.addr, resolved.pubkey_hex);
    if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
        return "ERROR cannot connect to " + peer_name;
    }
    
    // Reuse existing transfer logic on this dedicated SSL
    std::string result = file_send_wait_on_transport(
        sc.ssl.get(), sc.sfd, local_path);
    CLOSESOCK(sc.sfd);
    return result;
}

// Updated file_send with fallback
std::string file_send(const std::string& peer_name, const std::string& local_path,
                      bool wait_for_completion) {
    // Try daemon IPC first (for connection reuse)
    std::string ipc = daemon_send_via_ipc(peer_name, local_path, 120000, wait_for_completion);
    if (!ipc.empty() && ipc.rfind("ERROR no daemon", 0) != 0) return ipc;
    
    // Fallback: direct TLS (like shell_peer, health_check)
    return file_send_direct(peer_name, local_path, wait_for_completion);
}
```

The peer side already handles FileMetaMsg/FileChunkMsg on any TLS conn (they're dispatched in the control stream handler regardless of conn purpose). No peer-side changes needed.

### Fix 2: Fix "no conn" vs "busy" conflation

```cpp
bool daemon_file_send(const std::string& peer_name, const std::string& local_path) {
    Conn* target = nullptr;
    bool found_busy = false;
    for (auto& c : conns_) {
        if (c.purpose == ConnectionPurpose::Mesh &&
            c.sock_fd != INVALID_SOCKET &&
            peer_name_eq(c.peer_name, peer_name)) {
            if (!c.exec_busy || !c.exec_busy->load()) {
                target = &c; break;
            }
            found_busy = true;  // conn exists but is busy
        }
    }
    if (!target) {
        log_event("file_send_error",
                  found_busy ? "conn busy: " + peer_name : "no conn to " + peer_name);
        return false;
    }
    // ... rest unchanged
}
```

### Fix 3: Increase pipeline depth + sliding window

```cpp
// Before:
constexpr int kTransferPipelineSize = 8;     // 384 KB per batch

// After: larger window for high-BDP links
constexpr int kTransferPipelineSize = 32;    // 1.5 MB per batch (4x throughput on high-RTT)
constexpr int kTransferPipelineMaxInflight = 64; // sliding window cap

// Replace stop-and-wait with sliding window in file_send_wait_on_transport:
// Track ack.high_water_mark, keep sending new chunks as acks arrive
// instead of blocking at batch boundary
```

### Fix 4: Remove fixed sleep

```cpp
// Before (L7702):
std::this_thread::sleep_for(std::chrono::milliseconds(2));

// After: only sleep if socket write buffer is full (backpressure check)
// Use select() on write-fd or check SO_SNDBUF via SIOCOUTQ
// The 2ms sleep per 384KB batch wastes 0.5s per 100MB
```

### Fix 5: Eliminate the SSL race entirely (structural)

The root issue is that a raw `SSL*` from the mesh Conn is handed to a worker thread while the event loop may reconnect/destroy it. Two options:

**Option A (preferred with Fix 1):** File transfers always use a dedicated direct TLS connection. The mesh Conn is never shared.

**Option B (if mesh conn reuse is required):** Add proper SSL locking:
```cpp
struct Conn {
    // ... existing ...
    std::mutex ssl_mutex;  // serialize all SSL access
    // Event loop and workers both lock before read/write
};
```

---

## SUMMARY TABLE

| Issue | Impact | Fix |
|-------|--------|-----|
| No direct TLS path | Daemon required; extra IPC hop | Fix 1: `file_send_direct` |
| "no conn" on busy conn | Spurious failures | Fix 2: separate busy vs absent |
| Stop-and-wait, pipeline=8 | ~2 MB/s on 200ms RTT | Fix 3: pipeline=32, sliding window |
| 2ms sleep per batch | 0.5s/100MB wasted | Fix 4: backpressure-based |
| SSL pointer race | Corruption/crash on reconnect | Fix 5: dedicated TLS or mutex |
