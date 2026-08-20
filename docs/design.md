# Design

BridgeSessions is a **single C++23 binary** — a mesh terminal + file relay that
replaces SSH, MOSH, SCP, tmux/Zellij, and WinRM with one encrypted mesh over TLS.

**Replace:** `ssh → scp → tmux → hermes --tui`
**With:** `bs shell <peer> --cmd "…"` / `bs file send <peer> …` / `bs cua …`

One protocol. One binary. One mesh. No SSH. No mosh. No zellij.

| Property | Choice |
|---|---|
| Language | C++23 |
| Transport | TLS 1.2+ (1.3 re-enabled once Windows PE links OpenSSL ≥ 3.6) over TCP, port **19949** |
| Compression | zstd per-frame (over `COMPRESSION_THRESHOLD`) |
| Auth | ed25519 mutual TLS + pinned pubkey (`authorized_keys` + `seed … pubkey=`) |
| Build | CMake 3.25+ (single `main.cpp` + header monolith) |

## Architecture

One `bridgesessions` process is simultaneously the mesh daemon, the CLI client,
and the session multiplexer. A node holds a persistent ed25519 identity
(`~/.bridgesessions/id_ed25519.pem` + self-signed cert), joins a mesh of peers via
pinned seeds or a `join <token>`, and both *serves* shell/file/CUA requests from
peers and *issues* them to peers over the same encrypted transport.

```
        ┌──────────────────────────────────────────────┐
        │  bridgesessions (one binary, all roles)      │
        │  ├─ mesh daemon    (TLS 1.2+, port 19949)    │
        │  ├─ CLI client     (bs shell / file / cua)   │
        │  └─ session multiplexer (PTY / ConPTY)       │
        └──────────────────────┬───────────────────────┘
                               │  mesh over TLS 19949
        ┌──────────────────────┼───────────────────────┐
        ▼                      ▼                       ▼
   Linux peer             macOS peer             Windows peer
   (PTY sessions)         (PTY + ScreenCaptureKit)  (ConPTY + cua-helper)
```

- **Shell** — `bs shell <peer>` opens a PTY (forkpty / ConPTY) session on the peer;
  one-shot `--cmd` pipes a single command through the daemon IPC.
- **File transfer** — `bs file send/recv` streams chunks over direct TLS, zstd
  pipelined (depth 16), SHA-256 verified, with `PROGRESS` lines.
- **CUA** — `bs cua screen/capture/click/type/key/scroll` drives a remote desktop
  via a user-session helper (`--cua-helper` on Windows/macOS, xdotool on Linux).
- **Bridge Panel** — a local web surface (`tools/bridgepanel/`) for document
  review between agent and human.

## Identity & trust

- **Keypair:** ed25519, at `~/.bridgesessions/id_ed25519.pem`; the peer's public
  key is pinned per-seed (`seed <name> <addr> pubkey=<hex>`).
- **Inbound auth:** the TLS server verifies the client cert against
  `authorized_keys` (one `pubkey <hex>` per line) — `SSL_VERIFY_PEER |
  FAIL_IF_NO_PEER_CERT`.
- **Outbound auth:** the client compares the server cert + Hello pubkey against the
  pinned `expected_pubkey` immediately post-handshake and rejects on mismatch
  (`verify_outbound_peer_identity`). TOFU is transport-level only; the pin is
  enforced at the app layer.
- **Mesh bootstrap (Tailscale model):** `bs invite` on a trusted seed, then
  `curl …/install.sh | bash -s -- join <host> <token>`. The host auto-vouches for
  the new node (signed `DirectoryEnrollMsg`), gossips it, and peers auto-trust it —
  no manual key copying. Only explicitly-pinned **seed** peers may vouch.

## CLI surface

`shell`, `sessions`, `keygen`, `authorize`, `doctor`, `peers`, `health`,
`reconnect`, `invite`, `enroll`, `join`, `image`, `anim`, `stats`, `fleet`,
`upgrade`, `telemetry`, `file send|recv`, `capture-video`, `cua …`.

See [Usage](usage.md) and [Configuration](configuration.md) for the commands.
