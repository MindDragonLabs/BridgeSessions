# bridgesessions — Mesh Terminal Relay

One binary, one config, one keypair per device. Every node is a peer.
Peer-to-peer mesh for swarm operations — open terminals to any machine, run AI agents anywhere.

**Design:** [docs/GUIDELINE.md](./docs/GUIDELINE.md) · **Plan:** [docs/PLANS.md](./docs/PLANS.md) · **TODO:** [docs/TODO.md](./docs/TODO.md)

---

## Build

### Windows (MSVC + vcpkg)

```powershell
# One-time: install vcpkg deps
vcpkg install openssl zstd CLI11 spdlog nlohmann-json fmt catch2

# Build
cl /std:c++latest /EHsc /MD /utf-8 /DBS_NO_NAT /DBS_NO_WEBRTC /DBS_NO_DHT ^
    /I C:\vcpkg\installed\x64-windows\include bridgesessions.cpp ^
    /Fe:bridgesessions.exe /link /LIBPATH:C:\vcpkg\installed\x64-windows\lib ^
    libssl.lib libcrypto.lib zstd.lib ws2_32.lib fmt.lib
```

Or run `_run_tests.ps1` which builds all 16 test suites + the main binary.

### Linux (g++)

```bash
g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
    -o bsmesh bridgesessions.cpp \
    -lssl -lcrypto -lzstd -lspdlog -pthread -lfmt
```

### macOS (clang)

```bash
clang++ -std=c++2b -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
    -I$(brew --prefix)/include -L$(brew --prefix)/lib \
    -o bridgesessions bridgesessions.cpp \
    -lssl -lcrypto -lzstd -lspdlog -lfmt -lpthread
```

---

## Quickstart: 4-Node Mesh

### 1. Generate keys

```bash
# First run on each node auto-generates keys, OR:
bridgesessions keygen
```

### 2. Authorize peers

```bash
# On each node, add other nodes' pubkeys:
cat ~/.bridgesessions/id_ed25519.pub   # get your pubkey, share with others
bridgesessions authorize <other-node-hex-pubkey>
```

### 3. Configure

```ini
# ~/.bridgesessions/config
node.name Shadow
node.listen 0.0.0.0:19949

# Seed peers (repeatable)
seed linux-b 203.0.113.12:19949 pubkey=<hex>
seed linux-a 203.0.113.11:19949 pubkey=<hex>
seed macos-peer 203.0.113.16:19949 pubkey=<hex>
```

### 4. Start daemon

```bash
bridgesessions --daemon --config ~/.bridgesessions/config
# Or just: bridgesessions
```

### 5. Verify mesh

```bash
bridgesessions health linux-b          # → linux-b healthy
bridgesessions peers list            # → seed + discovered peers
bridgesessions sessions --all        # → sessions across all peers
bridgesessions shell linux-b           # → open interactive shell
```

---

## Commands

| Command | Description |
|---------|-------------|
| `bridgesessions` | Daemon mode — listen + connect to peers |
| `bridgesessions --daemon` | Daemon mode, detached from terminal |
| `bridgesessions keygen` | Generate ed25519 keypair |
| `bridgesessions authorize <hex>` | Add peer's pubkey to authorized_keys |
| `bridgesessions shell <peer>` | Open interactive shell on remote peer |
| `bridgesessions shell <peer> -n <name> -x <cmd>` | Named session with command override |
| `bridgesessions sessions [peer]` | List sessions |
| `bridgesessions sessions --all` | List sessions across all peers |
| `bridgesessions peers list\|add\|remove` | Manage seed peers |
| `bridgesessions health <peer>` | Ping/pong health check |
| `bridgesessions image <file>` | Preview image in terminal |
| `bridgesessions anim <file>` | Preview animated GIF |
| `bridgesessions stats` | Connection and session stats |
| `bridgesessions --version` | Print version |

---

## Architecture

All code in `bridgesessions.cpp` (~5,800 lines). Single event loop with `select()`.

| Component | Lines | What |
|-----------|-------|------|
| Message types (22) | 100–430 | Keystroke, Output, Resize, Hello, Gossip, SessionSearch, ... |
| Codec | 430–830 | zstd serialize/deserialize, SHA-256 |
| Identity + TLS | 830–1190 | ed25519 certgen, mTLS, authorized_keys |
| Frame I/O | 1190–1280 | length-prefixed framing over SSL |
| Ring buffer | 1280–1350 | Thread-safe circular buffer |
| Session + PTY | 1390–2000 | ConPTY (Win) / forkpty (POSIX) |
| Session Registry | 2100–2500 | Multi-attach session lifecycle |
| Mesh Controller | 2500–4100 | Accept, connect, gossip, CLI, health IPC |
| CLI (CLI11) | 4440–4873 | 12 subcommands + daemon mode |

---

## Tests

```powershell
# Full suite (16 suites, 1009 assertions)
powershell -ExecutionPolicy Bypass -File _run_tests.ps1

# Single suite
powershell -ExecutionPolicy Bypass -File _run_tests.ps1 -Suite test_mesh_reliability
```

---

## Dependencies

| Library | Purpose |
|---------|---------|
| OpenSSL 3.x | TLS 1.3, ed25519 keys, SHA-256 |
| zstd | Wire-format compression |
| CLI11 | CLI argument parsing |
| spdlog | Structured JSON logging |
| nlohmann/json | JSON serialization |
| Catch2 | Unit testing |
| fmt | String formatting (spdlog dep) |

---

## Node Identities (current cluster)

| Node | OS | Tailscale IP | Daemon |
|------|----|-------------|--------|
| Shadow | Windows 11 | 100.124.169.66 | NSSM service |
| linux-b | Linux | 203.0.113.12 | systemd |
| linux-a | Linux | 203.0.113.11 | systemd |
| macos-peer | macOS | 203.0.113.16 | LaunchAgent |

SSH: `agent@linux-b`, `agent@linux-a`, `macos-peer` alias.

---

## License

MIT
