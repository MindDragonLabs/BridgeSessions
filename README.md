# bridgesessions — Mesh Terminal Relay

One binary, one config file, one keypair per device. Every node is a peer.

## Build (Windows MSVC + vcpkg)

```powershell
# Prerequisites: vcpkg with openssl, zstd, CLI11, spdlog, nlohmann-json, Catch2, fmt
powershell -File _build_standalone.ps1
```

Output: `bridgesessions.exe`

## Quickstart: 2-Node Mesh

### Node A (listen):

```bash
# First run auto-generates keys
bridgesessions --config ~/.bridgesessions/config
```

Config (`~/.bridgesessions/config`):
```
node.name my-laptop
node.listen :19948
seed my-server 192.168.1.100:19948
```

### Node B (connect):

```bash
bridgesessions --config ~/.bridgesessions/config
```

Both nodes connect, exchange Hello, and are ready.

### Open a shell:

```bash
bridgesessions shell my-server
bridgesessions shell my-server -n build -x "make -j8"
```

## Commands

| Command | Description |
|---|---|
| `bridgesessions` | Daemon mode — listen + connect to peers |
| `bridgesessions shell <peer>` | Open shell on remote peer |
| `bridgesessions shell <peer> -n <name> -x <cmd>` | Named session with command override |
| `bridgesessions sessions [<peer>]` | List sessions |
| `bridgesessions sessions --all` | List sessions across all peers |
| `bridgesessions peers list` | Show known peers |
| `bridgesessions peers add <name> <addr>` | Add seed peer |
| `bridgesessions peers remove <name>` | Remove seed peer |
| `bridgesessions keygen` | Generate ed25519 keypair |
| `bridgesessions authorize <pubkey>` | Authorize a peer to connect |
| `bridgesessions --version` | Print version |

### Shell Options

| Flag | Description | Default |
|---|---|---|
| `-n, --name` | Session name | `default` |
| `-x, --cmd` | Override shell command | Uses config default |
| `--cols` | Terminal columns | `80` |
| `--rows` | Terminal rows | `24` |

## Config Format

Simple `key value` format with `#` comments. Keys use dotted sections.

```ini
# ── Node identity ──
node.name my-device          # Unique name for this node
node.listen 0.0.0.0:19948   # Address to listen on (or just :port)

# ── Mesh settings ──
mesh.max_peers 50                    # Max peer connections
mesh.gossip_interval_secs 30         # Seconds between peer list broadcasts
mesh.ping_interval_secs 5            # Keepalive ping interval
mesh.pong_timeout_secs 30            # Disconnect if no pong within this
mesh.reconnect_backoff_max_secs 30   # Max backoff for reconnect

# ── Bootstrap peers ──
seed linux-a 203.0.113.11:19948      # Seed peer to connect to on startup

# ── Session defaults ──
sessions.default_shell cmd.exe       # Default shell for new sessions
sessions.terminal xterm-256color     # TERM type
sessions.scrollback_lines 2000       # Lines of scrollback buffer
sessions.idle_timeout_hours 168      # Auto-reap idle sessions (1 week)
sessions.persistence_path ~/.bridgesessions/sessions.json
sessions.authorized_keys_path ~/.bridgesessions/authorized_keys
```

### Config Keys Reference

| Key | Type | Default | Description |
|---|---|---|---|
| `node.name` | string | `unnamed` | Node identity name |
| `node.listen` | `addr:port` | `0.0.0.0:19948` | Listen address |
| `mesh.max_peers` | int | `50` | Max concurrent peers |
| `mesh.gossip_interval_secs` | int | `30` | Peer list broadcast interval |
| `mesh.ping_interval_secs` | int | `5` | Keepalive interval |
| `mesh.pong_timeout_secs` | int | `30` | Disconnect timeout |
| `mesh.reconnect_backoff_max_secs` | int | `30` | Max reconnect backoff |
| `seed` | `name addr` | none | Bootstrap peer (repeatable) |
| `sessions.default_shell` | string | `cmd.exe` / `/bin/bash -l` | Default PTY command |
| `sessions.terminal` | string | `xterm-256color` | TERM environment |
| `sessions.scrollback_lines` | int | `2000` | Ring buffer capacity |
| `sessions.idle_timeout_hours` | int | `168` | Idle session reap |
| `sessions.persistence_path` | path | `~/.bridgesessions/sessions.json` | Session metadata store |
| `sessions.authorized_keys_path` | path | `~/.bridgesessions/authorized_keys` | Allowed peer pubkeys |

## How Mesh Works

1. Each node has one ed25519 keypair (`~/.bridgesessions/id_ed25519.pem`, `id_ed25519-cert.pem`, `id_ed25519.pub`)
2. Seed peers are configured in `config` via `seed <name> <addr>` lines
3. On startup, nodes connect to all seeds with mTLS
4. After TLS handshake, nodes exchange `Hello` messages with identity and known peer lists
5. Periodic `Gossip` messages propagate new peers to all connected nodes
6. `authorized_keys` file (one hex pubkey per line) controls who can connect to you
7. Bidirectional sessions — any authorized node can open a shell on any other

### Identity & Security

- **Key generation**: `bridgesessions keygen` creates a new ed25519 keypair in `~/.bridgesessions/`
- **First-run bootstrap**: Daemon mode auto-generates keys if none exist (TOFU — Trust On First Use)
- **Authorization**: Use `bridgesessions authorize <hex-pubkey>` to allow a peer to connect
- **mTLS**: Both client and server authenticate via ed25519 certificates during TLS handshake
- **Custom verify**: Certificate verification checks pubkey against `authorized_keys` list

## Architecture

Single-file C++23 (`bridgesessions.cpp`, ~3,700 lines).

### Component Map

```
bridgesessions.cpp
├── 0. RingBuffer           Thread-safe circular buffer for PTY scrollback
├── 1. Message Types        22 message types (0x01–0x16), zstd codec, SHA-256
├── 3. TLS Transport        Ed25519 mTLS with authorized_keys + TOFU
├── 4. Frame I/O            Length-prefixed message framing over SSL
├── 5. OSC 52 Scanner       Clipboard capture from terminal output
├── 6. Session & PTY        ConPTY (Windows) / PTY (POSIX) process lifecycle
├── 8. Persistence          JSON session metadata (nlohmann/json)
├── 9. Logging              Structured logging via spdlog (rotating file sink)
├── 10. Session Registry    Thread-safe session lifecycle manager
├── 11. Mesh Controller     Connection manager, event loop, gossip protocol
└── 2. Main (CLI)           CLI11 subcommands: shell, sessions, peers, keygen, authorize
```

### Message Types

| Type | Hex | Direction | Description |
|---|---|---|---|
| Keystroke | 0x01 | C→S | Raw key bytes or bracketed paste |
| Output | 0x02 | S→C | PTY stdout |
| Resize | 0x03 | C→S | Terminal dimension change |
| ClipboardGet | 0x04 | S→C | OSC 52 clipboard capture |
| ClipboardPut | 0x05 | C→S | User paste |
| Attach | 0x06 | C→S | Open/navigate to session |
| Detach | 0x07 | C→S | Leave session running |
| SessionList | 0x08 | S→C | Session inventory |
| ServerInfo | 0x09 | S→C | Host metadata |
| Ping | 0x0A | Bidir | Keepalive |
| Pong | 0x0B | Bidir | Keepalive response |
| Scrollback | 0x0C | S→C | History replay chunk |
| Signal | 0x0D | C→S | ^C, ^Z, ^\ |
| ProcExited | 0x0E | S→C | Foreground process exit |
| ScrollbackAck | 0x0F | C→S | Ready for next chunk |
| SessionDied | 0x10 | S→C | PTY crash notification |
| ClipboardEcho | 0x11 | S→C | Hash confirmation |
| ImageData | 0x12 | Bidir | Static image (PNG/JPEG) |
| ImageFrame | 0x13 | Bidir | Animated frame (GIF) |
| ImageAck | 0x14 | Bidir | Frame consumed |
| Hello | 0x15 | Bidir | Mesh node introduction |
| Gossip | 0x16 | Bidir | Peer list exchange |

### Wire Format

```
[4 bytes: zstd-compressed payload length (big-endian)]
[N bytes: zstd-compressed JSON payload]
```

Messages are serialized as JSON, compressed with zstd, then sent as length-prefixed frames over TLS.

## Testing

```powershell
# Build and run individual test suites
powershell -File _build_test.ps1 tests/test_message.cpp test_message
powershell -File _build_test.ps1 tests/test_codec.cpp test_codec
powershell -File _build_test.ps1 tests/test_tls.cpp test_tls
powershell -File _build_test.ps1 tests/test_frame_io.cpp test_frame_io
powershell -File _build_test.ps1 tests/test_osc52.cpp test_osc52
powershell -File _build_test.ps1 tests/test_ring_buffer.cpp test_ring_buffer
powershell -File _build_test.ps1 tests/test_session.cpp test_session
powershell -File _build_test.ps1 tests/test_identity.cpp test_identity
powershell -File _build_test.ps1 tests/test_config.cpp test_config
powershell -File _build_test.ps1 tests/test_session_registry.cpp test_session_registry
powershell -File _build_test.ps1 tests/test_mesh.cpp test_mesh
powershell -File _build_test.ps1 tests/test_relay.cpp test_relay
```

Test files under `tests/`:

| Test file | Covers |
|---|---|
| `test_message.cpp` | Message structs, variant type mapping |
| `test_codec.cpp` | JSON serialization, zstd compression, SHA-256 hashing |
| `test_tls.cpp` | Ed25519 key generation, PEM I/O, authorized_keys |
| `test_frame_io.cpp` | Length-prefixed frame encoding/decoding |
| `test_osc52.cpp` | OSC 52 escape sequence scanner |
| `test_ring_buffer.cpp` | RingBuffer thread-safety, snapshot, read_last_lines |
| `test_session.cpp` | PTY/ConPTY lifecycle, spawn, I/O, resize, signal |
| `test_identity.cpp` | Keypair bootstrap, pubkey extraction |
| `test_config.cpp` | Config parse/save round-trip |
| `test_session_registry.cpp` | Session lifecycle manager |
| `test_mesh.cpp` | Mesh controller, Hello/Gossip protocol |
| `test_relay.cpp` | Bidirectional relay I/O |

## Dependencies

| Library | Purpose |
|---|---|
| OpenSSL 3.x | TLS 1.3, ed25519 keys, SHA-256 |
| zstd | Wire-format compression |
| CLI11 | CLI argument parsing |
| spdlog | Structured JSON logging |
| nlohmann/json | JSON serialization |
| Catch2 | Unit testing |
| fmt | String formatting (spdlog dependency) |

## License

MIT
