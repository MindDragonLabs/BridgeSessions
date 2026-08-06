# BridgeSessions Error Handling & Diagnostics Design

## 1. Current Error Catalog (What Users See Today)

### 1a. `ConnectFailReason` → `connect_fail_string()` (bs_mesh.hpp:4450)

| Enum Value | String Output | Trigger |
|---|---|---|
| `Refused` | `"refused"` | TCP RST, ECONNREFUSED, socket() failure |
| `Timeout` | `"timeout"` | ETIMEDOUT, SSL_ERROR_WANT_READ/WRITE, ECONNRESET, ECONNABORTED |
| `TlsRejected` | `"tls_rejected"` | TLS handshake failure (cert mismatch, wrong SSL err, clean EOF during handshake) |
| `HelloRejected` | `"hello_rejected"` | Expected HelloMsg but got other, read exception during hello, catch-all |
| `None` (fallthrough) | `"unknown"` | Unreachable in practice |

### 1b. `print_connect_failure()` (bs_mesh.hpp:4549)

```
Failed to connect to <peer>: <reason> (<detail>)
```
- Reason is the raw word (`refused`, `timeout`, etc.)
- Detail is internal-facing (`ssl_err=5`, `connect errno=111`, `syscall_errno=104`)
- **No remediation guidance. No actionable next step.**

### 1c. `PeerNotFound` (bs_mesh.hpp:5209)

```
Peer not found: <peer>
```
- **No list of available peers. No "did you mean?" suggestions. No hint about `peers list`.**

### 1d. `ShellExitReason` → Exit Codes (bs_cli.hpp:318-330)

| Reason | Exit Code | User Message |
|---|---|---|
| `Detached` | 0 | `[Detached — session "X" still running on Y; reattach: ...]` |
| `RemoteDied` | 1 | `[Session died: exit=N]` |
| `ConnectionLost` | 2 | `[Connection lost]` |
| `PeerNotFound` | 3 | `Peer not found: X` |
| `ConnectFailed` | 4 | `Failed to connect to X: reason (detail)` |
| `AuthRejected` | 5 | Same as ConnectFailed (same `print_connect_failure`) |
| `Denied` | 6 | `[Denied: <reason>]` |

### 1e. Daemon IPC Errors (bs_cli.hpp:296-313)

| Scenario | Message |
|---|---|
| No daemon reachable | `no daemon running (or IPC socket unreachable)` |
| Daemon returned error | `error: <message>` |
| Malformed JSON | `malformed response from daemon` |
| stats command | `no daemon running` |

### 1f. health_check (bs_mesh.hpp:5533)

| Scenario | Status String |
|---|---|
| IPC success | `healthy` / `unhealthy` |
| Peer unknown | `unknown peer` |
| Connection failure | Raw `connect_fail_string` (`refused`, `timeout`, etc.) |
| No pong | `no pong` |
| Exception | `error: <what>` |

### 1g. run_command Error (bs_mesh.hpp:4647)

```
could not connect to <peer>
could not connect to <peer> (auth rejected)
```

### 1h. Session list/attach failures

```
Peer not found: <peer>
Timeout
```

---

## 2. Improved Error Messages — What Users SHOULD See

### Design principles:
1. **Plain language headline** — not `tls_rejected`, but `TLS handshake failed`
2. **Likely cause** — 1-2 sentences explaining why this typically happens
3. **Next step** — a concrete command or action
4. **Internal detail** — available via `--verbose` or `-v`, hidden by default

---

### 2a. `ConnectFailReason::Refused`

**Today:** `Failed to connect to macos-peer: refused (connect errno=111)`

**Proposed:**
```
✗ Cannot reach peer "macos-peer" — connection refused

  The remote host rejected the TCP connection. This usually means:
  • The BridgeSessions daemon is not running on macos-peer
  • The daemon is listening on a different port
  • A firewall is blocking the connection

  Try:
    bridgesessions diagnose macos-peer       # full connectivity check
    bridgesessions health macos-peer          # quick liveness probe
```

**Verbose (`-v`):** append `(connect errno=111, addr=192.168.1.50:19980)`

---

### 2b. `ConnectFailReason::Timeout`

**Today:** `Failed to connect to shadow-pc: timeout (ssl_err=5)`

**Proposed:**
```
✗ Cannot reach peer "shadow-pc" — connection timed out

  No response within 10s. Common causes:
  • The peer is offline or its network is unreachable
  • High latency on a relay (DERP/Tailscale) connection
  • MTU/MSS mismatch causing packet black-holing

  Try:
    bridgesessions diagnose shadow-pc       # check each layer
    bridgesessions peers list               # verify the address is current

  Tip: If this is a Tailscale link, check for MTU issues:
    tailscale status
```

**Verbose:** append `(ssl_err=5, syscall_errno=110, timeout_ms=10000)`

---

### 2c. `ConnectFailReason::TlsRejected`

**Today:** `Failed to connect to macos-peer: tls_rejected (ssl_err=5 ...)`

**Proposed:**
```
✗ Cannot establish secure connection to "macos-peer" — TLS handshake failed

  The TCP connection succeeded but the TLS handshake was rejected. This means:
  • The peer's identity key changed (reinstalled? re-paired?)
  • Your pinned key for this peer is stale
  • The peer is running an incompatible TLS version

  Try:
    bridgesessions diagnose macos-peer         # will report key mismatch
    bridgesessions peers list               # check pin status

  If the peer was legitimately re-paired:
    bridgesessions peers remove macos-peer
    bridgesessions peers add macos-peer <addr>  # re-add with new key
```

**Verbose:** append `(ssl_err=1, openssl: certificate verify failed, syscall_errno=0)`

---

### 2d. `ConnectFailReason::HelloRejected`

**Today:** `Failed to connect to macos-peer: hello_rejected (expected HelloMsg)`

**Proposed:**
```
✗ Cannot connect to "macos-peer" — protocol handshake failed

  The TLS connection succeeded but the BridgeSessions protocol exchange failed.
  This usually means:
  • The peer is running an incompatible version
  • Something else is listening on port 19980 (not BridgeSessions)

  Try:
    bridgesessions diagnose macos-peer         # distinguish protocol vs network issue
    bridgesessions --version                # check your version
```

**Verbose:** append `(detail: expected HelloMsg)` or `(detail: <exception>)`

---

### 2e. `PeerNotFound`

**Today:** `Peer not found: mmini`

**Proposed:**
```
✗ Unknown peer "mmini"

  Available peers:
    macos-peer       192.168.1.50:19980    [seed, pinned]
    shadow-pc     10.0.0.5:19980        [seed, unpinned]
    linux-a         linux-a.lan:19980        [discovered]

  Did you mean: macos-peer?

  List peers:  bridgesessions peers list
  Add a peer:  bridgesessions peers add <name> <addr>
```

---

### 2f. `ConnectionLost` (mid-session)

**Today:** `[Connection lost]`

**Proposed:**
```
[Connection to "macos-peer" lost]

  The session was interrupted. The remote process may still be running.
  Try:
    bridgesessions ctl attach macos-peer <session>   # reconnect
    bridgesessions diagnose macos-peer               # check if peer recovered
```

---

### 2g. `Denied` (task dispatch)

**Today:** `[Denied: task dispatch not enabled on this node (tasks.accept_dispatch)]`

**Proposed:**
```
✗ "macos-peer" refused the command — remote task dispatch is disabled

  The peer allows terminal sessions but not automated command dispatch.
  To enable it on "macos-peer":
    bridgesessions ctl config set tasks.accept_dispatch true

  Or open an interactive shell instead:
    bridgesessions shell macos-peer
```

---

### 2h. No Daemon Running

**Today:** `no daemon running (or IPC socket unreachable)`

**Proposed:**
```
✗ BridgeSessions daemon is not running

  Start it with:
    bridgesessions start              # or: bridgesessions daemon

  Check its status:
    bridgesessions status
    bridgesessions ctl logs --follow  # tail daemon logs
```

---

## 3. `bs diagnose <peer>` Command Design

### Command

```bash
bridgesessions diagnose <peer> [--json]
```

### Output (human-readable)

```
Diagnosing peer "macos-peer" (192.168.1.50:19980)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

  [1/4] Local daemon ......... ✓ running (pid 45231, uptime 2h15m)
  [2/4] Network reachability . ✓ TCP connect OK (23ms RTT)
  [3/4] TLS handshake ........ ✗ failed — key mismatch
  [4/4] Protocol hello ....... ⊘ skipped (TLS failed)

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
Diagnosis: TLS key mismatch

The remote peer's identity key does not match the pinned key.
This happens after reinstalling or re-pairing the remote node.

Fix:
  bridgesessions peers remove macos-peer
  bridgesessions peers add macos-peer 192.168.1.50:19980

Then verify:
  bridgesessions health macos-peer
```

### Implementation: 4-stage check pipeline

```cpp
// bs_mesh.hpp — new method on MeshController

struct DiagnoseResult {
    // Stage results: true = passed, false = failed, "skipped" = precondition not met
    bool daemon_running = false;
    std::string daemon_detail;       // pid, uptime, or error

    bool network_reachable = false;
    std::string network_detail;      // RTT, errno, or "unreachable"
    int rtt_ms = -1;

    bool tls_ok = false;
    std::string tls_detail;          // "OK" / "key mismatch" / OpenSSL error

    bool protocol_ok = false;
    std::string protocol_detail;     // "HelloMsg received, peer=macos-peer v1.0.0"

    // Summary
    std::string diagnosis;           // human-readable root cause
    std::string fix_hint;            // actionable fix string
};

DiagnoseResult diagnose_peer(const std::string& peer_name) {
    DiagnoseResult dr;

    // Stage 1: Local daemon check (IPC socket)
    auto ipc = ipc2_client_call(expand_home("~/.bridgesessions"),
                                "status", {}, 3000);
    dr.daemon_running = ipc.reached && ipc.ok;
    if (dr.daemon_running) {
        dr.daemon_detail = "pid=" + ipc.result.value("pid", "?").dump() +
                          ", uptime=" + ipc.result.value("uptime", "?").dump();
    } else {
        dr.daemon_detail = "not running (start: bridgesessions start)";
    }

    // Resolve peer address
    std::string addr = find_peer_addr(peer_name);
    if (addr.empty()) {
        dr.diagnosis = "unknown peer: " + peer_name;
        dr.fix_hint = "bridgesessions peers list";
        return dr;
    }

    // Stage 2: TCP reachability (raw socket, no TLS)
    sockaddr_in sa = resolve_addr(addr);
    SOCKET sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd != INVALID_SOCKET) {
        auto t0 = std::chrono::steady_clock::now();
        int rc = connect_with_timeout(sfd, (sockaddr*)&sa, sizeof(sa), 5000);
        auto t1 = std::chrono::steady_clock::now();
        if (rc != SOCKET_ERROR) {
            dr.network_reachable = true;
            dr.rtt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            dr.network_detail = "TCP connect OK (" + std::to_string(dr.rtt_ms) + "ms)";
        } else {
            int err = tls_last_syscall_errno();
            dr.network_detail = "TCP connect failed (errno=" + std::to_string(err) + ")";
            dr.diagnosis = "network unreachable";
            dr.fix_hint = "Check if the peer is online and port 19980 is open";
            CLOSESOCK(sfd);
            return dr;
        }
        CLOSESOCK(sfd);
    }

    // Stage 3: TLS handshake (reuse connect_and_hello's logic, extract detail)
    auto sc = connect_and_hello(addr);
    if (!sc.ssl || sc.sfd == INVALID_SOCKET) {
        dr.tls_ok = false;
        // Distinguish key mismatch from other TLS errors
        if (sc.fail == ConnectFailReason::TlsRejected) {
            dr.tls_detail = sc.fail_detail;
            // Check if we have a pinned key for this addr
            bool has_pin = pinned_peer_keys_.count(addr) > 0 ||
                std::any_of(config_.seeds.begin(), config_.seeds.end(),
                    [&](auto& s){ return s.addr == addr && !s.pubkey_hex.empty(); });
            if (has_pin) {
                dr.diagnosis = "TLS key mismatch — pinned key does not match remote";
                dr.fix_hint = "bridgesessions peers remove " + peer_name +
                             " && bridgesessions peers add " + peer_name + " " + addr;
            } else {
                dr.diagnosis = "TLS handshake failed (no key pinning configured)";
                dr.fix_hint = sc.fail_detail;
            }
        } else if (sc.fail == ConnectFailReason::Timeout) {
            dr.tls_detail = "TLS handshake timed out (likely MTU/MSS issue on relay)";
            dr.diagnosis = "TLS timeout — packet black-hole suspected";
            dr.fix_hint = "Check MTU on VPN/tunnel interfaces";
        } else {
            dr.tls_detail = connect_fail_string(sc.fail) + " (" + sc.fail_detail + ")";
            dr.diagnosis = "TLS stage failure: " + connect_fail_string(sc.fail);
            dr.fix_hint = "Run with -v for detailed error";
        }
        dr.protocol_detail = "skipped (TLS failed)";
        return dr;
    }

    // Stage 4: Protocol hello already succeeded inside connect_and_hello
    dr.tls_ok = true;
    dr.tls_detail = "handshake OK";
    dr.protocol_ok = true;
    dr.protocol_detail = "HelloMsg received: peer=" + sc.hello.node_name +
                         " version=" + sc.hello.version;
    dr.diagnosis = "all checks passed";
    dr.fix_hint = "";

    if (sc.sfd != INVALID_SOCKET) CLOSESOCK(sc.sfd);
    return dr;
}
```

### CLI integration (bs_cli.hpp)

```cpp
// Register subcommand
auto* diagnose_cmd = app.add_subcommand("diagnose", "Run connectivity diagnostics on a peer");
std::string diagnose_peer;
bool diagnose_json = false;
diagnose_cmd->add_option("peer", diagnose_peer, "Peer name")->required();
diagnose_cmd->add_flag("--json,-j", diagnose_json, "Output JSON");

// Dispatch
if (diagnose_cmd->parsed()) {
    bs::mesh::MeshConfig cfg = bs::mesh::load_config(config_path);
    bs::mesh::MeshController mc(cfg);
    auto dr = mc.diagnose_peer(diagnose_peer);

    if (diagnose_json) {
        nlohmann::json j;
        j["daemon_running"] = dr.daemon_running;
        j["network_reachable"] = dr.network_reachable;
        j["rtt_ms"] = dr.rtt_ms;
        j["tls_ok"] = dr.tls_ok;
        j["protocol_ok"] = dr.protocol_ok;
        j["diagnosis"] = dr.diagnosis;
        j["fix_hint"] = dr.fix_hint;
        std::cout << j.dump(2) << "\n";
    } else {
        std::cout << "\nDiagnosing peer \"" << diagnose_peer << "\"\n";
        std::cout << std::string(50, '=') << "\n\n";

        auto mark = [](bool ok) { return ok ? "✓" : "✗"; };
        std::cout << "  [1/4] Local daemon ......... "
                  << mark(dr.daemon_running) << " " << dr.daemon_detail << "\n";
        std::cout << "  [2/4] Network reachability . "
                  << mark(dr.network_reachable) << " " << dr.network_detail << "\n";
        std::cout << "  [3/4] TLS handshake ........ "
                  << (dr.tls_ok ? "✓" : (dr.network_reachable ? "✗" : "⊘"))
                  << " " << dr.tls_detail << "\n";
        std::cout << "  [4/4] Protocol hello ....... "
                  << (dr.protocol_ok ? "✓" : "⊘")
                  << " " << dr.protocol_detail << "\n\n";

        std::cout << std::string(50, '=') << "\n";
        if (dr.diagnosis == "all checks passed") {
            std::cout << "Result: ✓ All checks passed — peer is healthy\n";
        } else {
            std::cout << "Result: ✗ " << dr.diagnosis << "\n\n";
            if (!dr.fix_hint.empty())
                std::cout << "Fix:\n  " << dr.fix_hint << "\n";
        }
    }

    return (dr.diagnosis == "all checks passed") ? 0 : 1;
}
```

---

## 4. Retry-with-Backoff Logic for Transient Failures

### Current state: No retry in CLI paths

The daemon's mesh event loop has backoff (`Backoff` struct at line 1054: exponential from 100ms to 30s cap). But CLI one-shot commands (`shell`, `ctl attach`, `run_command`, etc.) use `connect_and_hello()` directly — **zero retries, single attempt, fail immediately**.

### Design: Per-Reason Retry Strategy

```cpp
// bs_mesh.hpp — new helper

enum class RetryPolicy {
    NoRetry,         // TlsRejected, HelloRejected — won't change on retry
    RetryLinear,     // Refused — quick retries in case daemon was restarting
    RetryExponential, // Timeout — back off, might recover
};

static RetryPolicy retry_policy_for(ConnectFailReason reason) {
    switch (reason) {
        case ConnectFailReason::Refused:       return RetryPolicy::RetryLinear;
        case ConnectFailReason::Timeout:       return RetryPolicy::RetryExponential;
        case ConnectFailReason::TlsRejected:   return RetryPolicy::NoRetry;
        case ConnectFailReason::HelloRejected: return RetryPolicy::NoRetry;
        default:                               return RetryPolicy::NoRetry;
    }
}

// Retry-aware wrapper around connect_and_hello
SslConn connect_and_hello_with_retry(const std::string& addr,
                                      int max_retries = 2,
                                      bool interactive = false) {
    for (int attempt = 0; attempt <= max_retries; ++attempt) {
        auto sc = connect_and_hello(addr);

        // Success or non-retryable failure
        if (sc.ssl && sc.sfd != INVALID_SOCKET)
            return sc;
        if (sc.fail == ConnectFailReason::None)
            return sc;

        auto policy = retry_policy_for(sc.fail);
        if (policy == RetryPolicy::NoRetry || attempt == max_retries)
            return sc;

        // Compute delay
        int delay_ms;
        if (policy == RetryPolicy::RetryLinear) {
            delay_ms = 500 * (attempt + 1);   // 500ms, 1000ms
        } else {
            delay_ms = 250 * (1 << attempt);   // 250ms, 500ms
        }

        if (interactive) {
            std::cerr << "\r  Retrying (" << (attempt + 1) << "/" << max_retries
                      << ") in " << delay_ms << "ms...    " << std::flush;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    return connect_and_hello(addr);  // final attempt
}
```

### Which commands retry:

| Command | Interactive? | Max Retries | Reason |
|---|---|---|---|
| `shell` | Yes (show retry spinner) | 2 | User is waiting at terminal |
| `ctl attach` | Yes | 2 | Same |
| `run_command` | No (API call) | 1 | Scripted, shouldn't hang |
| `health` | No | 0 | Already has own timeout |
| `ctl sessions list <peer>` | No | 1 | Quick query |
| `file send/recv` | No | 2 | Transfers benefit from retry |
| `diagnose` | No | 0 | Diagnose wants raw failure |

### CLI flag to control retry:

```bash
bridgesessions shell macos-peer --retry 3       # explicit retry count
bridgesessions shell macos-peer --no-retry      # disable retry (scripting)
```

Default retry behavior per command (from table above).

---

## 5. "Did You Mean?" Disambiguation UX

### Implementation: Levenshtein distance on peer names

```cpp
// bs_mesh.hpp — new method

#include <algorithm>
#include <vector>

// Minimal Levenshtein distance (small strings, no external dep)
static int levenshtein(const std::string& a, const std::string& b) {
    int m = a.size(), n = b.size();
    std::vector<int> prev(n + 1), curr(n + 1);
    for (int j = 0; j <= n; ++j) prev[j] = j;
    for (int i = 1; i <= m; ++i) {
        curr[0] = i;
        for (int j = 1; j <= n; ++j) {
            int cost = (tolower(a[i-1]) == tolower(b[j-1])) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1, curr[j-1] + 1, prev[j-1] + cost});
        }
        std::swap(prev, curr);
    }
    return prev[n];
}

// Collect all known peer names
std::vector<std::string> all_peer_names() const {
    std::vector<std::string> names;
    for (auto& s : config_.seeds) names.push_back(s.name);
    for (auto& d : config_.discovered) names.push_back(d.name);
    return names;
}

// Find closest match to a typo'd name
struct PeerSuggestions {
    std::string best_match;           // empty if none within threshold
    int best_distance;
    std::vector<std::pair<std::string, std::string>> all_peers;  // {name, addr}
};

PeerSuggestions suggest_peers(const std::string& input) const {
    PeerSuggestions out;
    out.best_distance = INT_MAX;
    auto names = all_peer_names();

    for (size_t i = 0; i < names.size(); ++i) {
        out.all_peers.push_back({names[i], i < config_.seeds.size()
            ? config_.seeds[i].addr
            : config_.discovered[i - config_.seeds.size()].addr});

        int dist = levenshtein(input, names[i]);
        // Also check prefix match (common case: user typed partial name)
        if (names[i].size() >= input.size() &&
            peer_name_eq(names[i].substr(0, input.size()), input)) {
            dist = std::min(dist, 1);  // treat prefix match as near-perfect
        }
        if (dist < out.best_distance) {
            out.best_distance = dist;
            out.best_match = names[i];
        }
    }

    // Only suggest if within reasonable threshold
    int threshold = std::max(2, (int)(input.size() / 3));  // ~33% edit distance
    if (out.best_distance > threshold)
        out.best_match.clear();

    return out;
}
```

### Integration into `find_peer_addr` callers

Replace every `if (addr.empty()) { cerr << "Peer not found: X\n"; }` pattern with:

```cpp
// Helper: resolve peer or print rich error + suggestions
// Returns empty string if not found (caller should bail)
std::string find_peer_or_suggest(const std::string& peer_name, std::ostream& err = std::cerr) const {
    std::string addr = find_peer_addr(peer_name);
    if (!addr.empty()) return addr;

    auto sug = suggest_peers(peer_name);

    err << "\n✗ Unknown peer \"" << peer_name << "\"\n\n";

    if (sug.all_peers.empty()) {
        err << "  No peers are configured.\n\n"
            << "  Add a peer:\n"
            << "    bridgesessions peers add <name> <host:port>\n"
            << "  Or accept an invite:\n"
            << "    bridgesessions accept <invite-code>\n";
    } else {
        err << "  Available peers:\n";
        for (auto& [name, pa] : sug.all_peers) {
            err << "    " << name;
            err << std::string(std::max(0, (int)20 - (int)name.size()), ' ');
            err << pa << "\n";
        }
        if (!sug.best_match.empty()) {
            err << "\n  Did you mean: " << sug.best_match << "?\n";
        }
        err << "\n  List peers:  bridgesessions peers list\n";
        err << "  Add a peer:  bridgesessions peers add <name> <addr>\n";
    }
    err << "\n";
    return "";
}
```

### Example outputs:

**Typo:**
```
$ bridgesessions shell mcmii
✗ Unknown peer "mcmii"

  Available peers:
    macos-peer             192.168.1.50:19980
    shadow-pc           10.0.0.5:19980

  Did you mean: macos-peer?
```

**No peers configured:**
```
$ bridgesessions shell macos-peer
✗ Unknown peer "macos-peer"

  No peers are configured.

  Add a peer:
    bridgesessions peers add <name> <host:port>
  Or accept an invite:
    bridgesessions accept <invite-code>
```

**Prefix match (user typed "mac"):**
```
$ bridgesessions shell mac
✗ Unknown peer "mac"

  Available peers:
    macos-peer             192.168.1.50:19980
    shadow-pc           10.0.0.5:19980

  Did you mean: macos-peer?
```

---

## 6. Command Error vs Remote Down — Error Classification

### Problem

Today there's no clear boundary between:
- "Your command was wrong" (typo, bad syntax, unknown peer) → **user fixes their command**
- "The remote is down" (refused, timeout, TLS fail) → **infrastructure issue**
- "Auth/policy blocked you" (wrong key, dispatch disabled) → **config issue**

### Design: Three error tiers with distinct visual treatment

```cpp
enum class ErrorTier {
    LocalUserError,    // Your command/input is wrong — fix the command
    RemoteUnreachable, // Can't reach the peer — check infra
    RemoteRejected,    // Peer refused us — check auth/config
};

static ErrorTier classify_error(ShellExitReason r) {
    switch (r) {
        case ShellExitReason::PeerNotFound:  return ErrorTier::LocalUserError;
        case ShellExitReason::Detached:      return ErrorTier::LocalUserError; // not really error
        case ShellExitReason::ConnectFailed: return ErrorTier::RemoteUnreachable;
        case ShellExitReason::ConnectionLost: return ErrorTier::RemoteUnreachable;
        case ShellExitReason::RemoteDied:    return ErrorTier::RemoteUnreachable;
        case ShellExitReason::AuthRejected:  return ErrorTier::RemoteRejected;
        case ShellExitReason::Denied:        return ErrorTier::RemoteRejected;
    }
    return ErrorTier::RemoteUnreachable;
}
```

### Exit code scheme expansion

Current codes 0-6 are fine but lack the tier distinction for scripts. Add `--exit-codes=descriptive`:

```
0  = success
1  = remote process died (expected exit)
2  = connection lost mid-session (retryable)
3  = peer not found (local error — fix command)
4  = connection failed (remote unreachable — check infra)
5  = auth rejected (key/policy issue — reconfigure)
6  = denied (policy issue — change remote config)
```

Already correct! The key improvement is **message clarity** so users understand which tier they're in.

### Visual indicators

```
LocalUserError:     ✗ (red X, "fix your command" framing)
RemoteUnreachable:  ⚠ (orange warning, "check if peer is online" framing)
RemoteRejected:     🔒 (lock, "auth/policy issue" framing)
```

For terminals without color support, the text framing alone is sufficient.

### `--diagnose` flag on any connect command

```bash
bridgesessions shell macos-peer --diagnose
```

On connection failure, automatically runs `diagnose_peer()` and prints the staged output instead of just the one-line error. Equivalent to:

```bash
bridgesessions shell macos-peer || bridgesessions diagnose macos-peer
```

But cleaner — single process, no retry, immediate diagnostic context.

---

## 7. Implementation Plan

### Phase 1: Error Messages (no API changes)
1. Replace `print_connect_failure()` with `print_connect_failure_rich()` that uses tier-based message templates
2. Replace all `"Peer not found: X"` with `find_peer_or_suggest()` calls
3. Add Levenshtein + `suggest_peers()`
4. All messages degrade gracefully (no unicode on Windows CMD)

### Phase 2: Diagnose Command
5. Add `DiagnoseResult` struct + `diagnose_peer()` method
6. Register `diagnose` subcommand in bs_cli.hpp
7. Add JSON output mode for scripting

### Phase 3: Retry Logic
8. Add `RetryPolicy` + `connect_and_hello_with_retry()`
9. Add `--retry N` / `--no-retry` CLI flags
10. Integrate into `shell_peer()`, `run_command()`, `list_sessions_json()`

### Phase 4: Polish
11. Add `--diagnose` flag on shell/attach commands (auto-diagnose on failure)
12. Windows-safe output (no unicode emoji on CMD, use `[FAIL]`/`[OK]`)
13. Update `health_check()` output to use same rich messages

---

## OVERALL ASSESSMENT

**Current state:** The error handling infrastructure is solid architecturally — `ConnectFailReason` correctly classifies failures at 5 distinct levels (TCP refused, timeout, TLS rejection, hello rejection, success), `ShellExitReason` maps to distinguishable exit codes (0-6), and the daemon has proper exponential backoff for mesh reconnections. The problem is entirely in the **presentation layer**.

**Key gaps:**
1. **Messages are developer-facing, not user-facing** — `tls_rejected`, `ssl_err=5`, `errno=111` are OpenSSL/system internals that mean nothing to a user trying to figure out why their connection failed
2. **Zero remediation guidance** — no "try this next" in any error message
3. **PeerNotFound is a dead end** — no available peer list, no suggestions, users have to separately run `peers list`
4. **No retry on CLI paths** — the daemon retries mesh connections, but one-shot CLI commands get a single attempt. A daemon that's mid-restart gives a `refused` that looks permanent
5. **No diagnostic mode** — users have no way to pinpoint WHICH layer failed. `health` gives a single-word answer (`refused`/`timeout`/`healthy`), `diagnose` would give a 4-stage breakdown

**Feasibility:** All proposed changes are additive — no wire protocol changes, no config format changes, no breaking changes to existing exit codes. The `DiagnoseResult` pipeline reuses existing functions (`ipc2_client_call`, `connect_and_hello`, `find_peer_addr`). Levenshtein is 20 lines of code with no new dependency. The entire feature set can be implemented in ~500 lines of additions across `bs_mesh.hpp` and `bs_cli.hpp`.

**Risk:** Low. The only behavioral change is retry on transient failures for interactive commands, which is gated behind `--no-retry` for scripting use cases. Error message changes are cosmetic.
