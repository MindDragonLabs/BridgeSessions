# bridgesessions — Design Sketch (GUIDELINE.md)

**Replace:** `mosh → ssh → zellij → hermes --tui`
**With:** bridgemind.ai GUI ↔ mesh peers → any shell, any AI agent, anywhere

One binary. One keypair per device. Every node is a peer. The mesh is the infrastructure.

**Language:** C++23 | **Transport:** TLS 1.3 over TCP (v1), QUIC via msquic (v2)
**Compression:** zstd | **Auth:** ed25519 mutual TLS + TOFU | **Build:** single-file, MSVC/g++/clang

---

## What This Is

bridgesessions replaces a pile of disparate tools — SSH, mosh, zellij, scp, tmux, clipboard managers — with a single peer-to-peer mesh built around the **bridgemind.ai GUI** as the operator console.

The GUI is a mesh peer that runs 8+ terminal panes. Each pane can attach to **any node on the mesh** (Shadow/linux-a/linux-b/macos-peer, or a fleet of Linux VPS, macOS laptops, Windows PCs). Not individual SSH connections hopping out to each machine — a direct mesh link, peer to peer.

The real payload: **run AI agents remotely.** Open a pane → `hermes`, `codex`, `claude`, `bash` — any shell, on any node, with the same connection model. Copy output from one node and paste it into another. Transfer files between any two nodes without a middleman. This is a human-driven swarm in one window.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  bridgemind.ai GUI (operator console, mesh peer)                  │
│                                                                   │
│  ┌─ Pane 1 ──────────────────┐  ┌─ Pane 2 ───────────────────┐  │
│  │ hermes on linux-b           │  │ bash on macos-peer            │  │
│  │ ┌──────────────────────┐  │  │ ┌──────────────────────┐   │  │
│  │ │ agent output + MD    │  │  │ │ ls -la /projects/   │   │  │
│  │ │ rendered in-browser  │  │  │ │ vim config.yaml     │   │  │
│  │ └──────────────────────┘  │  │ └──────────────────────┘   │  │
│  └──────────┬───────────────┘  └──────────┬──────────────────┘  │
│             │ mesh peer link               │ mesh peer link       │
│             ▼                              ▼                      │
│  ┌──────────────────────────────────────────────────────────┐    │
│  │  Pane 3             Pane 4           Pane 5              │    │
│  │  codex on linux-b     bash on Shadow   hermes on macos-peer   │    │
│  └──────────────────────────────────────────────────────────┘    │
│                                                                   │
│  Copy/paste, file transfer, virtual folders — peer to peer        │
└──────────────────────────────────────────────────────────────────┘
         │                  │                  │
         ▼                  ▼                  ▼
   ┌─────────┐       ┌──────────┐       ┌──────────┐
   │ Shadow  │◄─────►│  linux-b   │◄─────►│  macos-peer │
   │ Windows │       │  Linux   │       │  macOS   │
   │ NSSM    │       │ systemd  │       │LaunchAgent│
   └────┬────┘       └────┬─────┘       └──────────┘
        │                 │
        ▼                 ▼
   ┌─────────┐       ┌──────────┐
   │ VPS-1   │       │  VPS-2   │
   │ Linux   │       │  Linux   │
   └─────────┘       └──────────┘
```

Every node has one binary, one keypair, one config. All connect to each other. No central server. The mesh auto-discovers peers via seeds + mDNS + gossip.

## Mesh Protocol: bs://

Binary length-prefixed frames over TLS 1.3 TCP. App-level stream IDs multiplex multiple sessions per connection.

```
Frame: [stream_id: u16] [type: u8] [flags: u8] [length: u16] [data]
```

**22 message types** covering session lifecycle, clipboard, keystroke/output, ping/pong, image transfer, hello/gossip discovery, session search across mesh.

**Stream ID 0** = control (Attach, Detach, Ping, Pong, Hello, Gossip, SessionSearch).
**Stream IDs 1+** = session data (Keystroke, Output, Clipboard, Resize, Signal, Image).

**Compression:** zstd on frames >256 bytes. Never compress Keystroke or Ping/Pong.

## Clipboard — First-Class Mesh Primitive

Clipboard is a protocol message, not an escape-sequence passthrough. Two-way across any mesh links:

```
Copy on node A  → OSC 52 captured → ClipboardGet → paste anywhere
Copy on GUI     → system clipboard → ClipboardPut → inject into any session on any node
```

Hash-echo race prevention: `ClipboardEcho` confirms receipt. Won't re-send content with the same hash.

**Extends to images, files:** Any binary payload up to 50 MB per frame (ImageData/ImageFrame types already exist). Copy an image from one node, paste into a terminal on another.

## Session Lifecycle

```
CREATED → RUNNING → DETACHED (client gone, PTY alive)
      ↘ DIED → (auto_restart?) → RUNNING / EXITED
      ↘ KILLED (explicit or idle timeout: 7 days)
```

Sessions survive client disconnect. Circular output buffer (zstd-compressed, 16K lines). Reattach replays last 2K lines in 500-line chunks with ACK pacing.

Multi-attach: multiple peers can watch/interact with one session simultaneously.

## File Transfer — Mesh-Native

**Not just scp.** File transfer between any two nodes directly over the mesh link, not via the GUI as intermediary:

- `bsmesh file send <peer> <local-path> [remote-path]` — direct peer-to-peer transfer
- `bsmesh file recv <peer> <remote-path> [local-dir]`
- Transfer directory trees with zstd streaming
- Resume partial transfers (chunk-level checksums)

## Virtual Folder Mapping

Local folder on the GUI (or any node) maps to a remote path on another node, with intelligent sync:

```
local:   ~/projects/ → remote:linux-b:/home/agent/projects/  (sync every 30s or on file change)
```

Sync semantics (not rsync — mesh-aware):
- **Change detection:** inotify/FSEvents/ReadDirectoryChanges trigger push
- **Smart caching:** recent files kept warm locally; cold files lazy-fetched on open
- **Bidirectional:** edits on either side propagate
- **Compressed zstd channel:** matching the wire protocol
- **Conflict markers:** mesh consensus on last-writer-wins with conflict notification

v1: on-demand `cp`-like file transfer. v2: live sync folders.

## File Editing Over Mesh

Edit any text file on any node as if local:

- `bsmesh edit linux-b:/etc/nginx/nginx.conf` → opens locally (vim/nano/helix/notepad++)
- Save → delta patch transmitted over mesh → applied on remote
- No full-file re-scp on every save
- Syntax highlighting, yaml/conf/txt/md awareness — all work against the remote file

## Markdown — Browser Rendering as First-Class Output

Terminal output that is markdown gets rendered in the bridgemind.ai GUI's browser engine — not as raw ANSI in a terminal pane, but as rich rendered HTML (headings, code blocks, tables, images). This is what that GUI already does with its 8-pane SSH view.

**Impact:** `hermes agent` output (which is heavily markdown) renders as beautiful documents inline. Config files, READMEs, `--help` text that ships markdown all render properly. The mesh protocol carries a `render_hint` flag so the GUI knows "this OutputMsg is markdown, render it" vs "this OutputMsg is terminal, display it raw."

## Quick Process Restart

Every node needs the ability to restart key processes from any other node over the mesh:

- `bsmesh restart <peer> bash` — restart shell on that node
- `bsmesh restart <peer> hermes` — `hermes update && hermes --tui --yolo` on that node
- `bsmesh restart <peer> codex`
- `bsmesh restart <peer> claude`

Wire format: SignalMsg with a new `Restart` signal (0x0D extended). The daemon receives it, kills the target process, spawns a fresh one in the same session. No SSH, no `systemctl`, no hoop-jumping.

## Mesh Wire — Key Differences from SSH

| Capability | SSH | bridgesessions mesh |
|-----------|-----|-------------------|
| Auth model | Key file per host | Single ed25519 keypair per device, authorized_keys per node |
| Connection model | Client→Server | Peer↔Peer (any talk to any) |
| Session survival | Dies on disconnect | Survives indefinitely, reattach later |
| Clipboard | None / escape-sequence hack | First-class protocol message, hash-confirmed |
| File transfer | scp/sftp (separate auth) | Mesh-native, same identity, same wire |
| Multi-machine | tmux/byobu on one host | Direct peer links to multiple hosts simultaneously |
| Image/video in terminal | None | Protocol message (ImageData/ImageFrame) |
| Markdown rendering | Raw text | render_hint flag → browser rendering in GUI |
| AI agent integration | Manual SSH loop | Direct pane → attach → run hermes/codex/claude |
| File editing | scp out, edit, scp back | Delta-patch over mesh wire |
| Auto-discovery | None / manual config | mDNS + Gossip + seed files |
| Process restart | SSH in, kill/restart | Mesh signal message |
| NAT/firewall | SSH on 22 (often blocked) | TLS 1.3 over TCP, any port, SO_REUSEADDR |

## Key Design Decisions

| Decision | Choice | Why |
|----------|--------|-----|
| Language | C++23 | Zero-GC latency, std::expected, OpenSSL native |
| Transport v1 | TLS 1.3 over TCP | Firewall-friendly, cross-platform, proven |
| Transport v2 | QUIC via msquic | Library swap, same protocol layer |
| Compression | zstd | 2-8x on terminal output, faster than gzip |
| Auth | ed25519 mTLS + TOFU | No CA, no PKI, one keypair per device |
| Binary | Single-file .cpp | Deploy: scp one file, compile, done |
| Operator console | bridgemind.ai GUI | 8-pane browser-rendered terminal, mesh peer |
| Swarm model | Human-driven | Open panes to any node, run AI agents there |
| File transfer | Mesh-native streaming | Same transport as session data, no second protocol |

## What bridgesessions Replaces (and what it doesn't)

| Tool | Replaced? | Notes |
|------|-----------|-------|
| SSH | ✅ | Mesh peer link replaces ssh for session access |
| mosh | ✅ | Mesh link survives network blips, reconnect built in |
| zellij/tmux | ✅ | Session multiplexer is built into the daemon |
| scp/sftp | ✅ | Mesh-native file transfer supersedes for mesh peers |
| rsync | 🔄 | Virtual folder sync replaces for mesh-managed folders |
| SSHFS | 🔄 | Virtual folder mapping replaces for mesh peers |
| vim/nano (remote editing) | ⚡ Enhanced | Editing over mesh with delta patches, not full-file scp |
| clipboard manager | ✅ | First-class protocol, cross-machine clipboard |
| terminal emulator | 🚫 | GUI *is* the emulator; bsmesh is the transport |
| tmux/copy-mode scrollback | ✅ | Native ring buffer + scrollback replay protocol |

## v1.4 Feature Cut (shipped and validated)

- ✅ Single-binary peer-to-peer mesh (4 nodes cross-platform)
- ✅ TLS 1.3 mutual auth with ed25519 + TOFU
- ✅ Session lifecycle with multi-attach, detach, kill, resurrection
- ✅ mDNS LAN discovery + gossip peer propagation
- ✅ zstd-compressed frames
- ✅ 22 message types (Hello, Gossip, Keystroke → SessionSearch 0x17)
- ✅ Clipboard capture (OSC 52) and paste (ClipboardPut)
- ✅ mTLS authorized_keys access control
- ✅ Config file with seeds, discovered peers, settings
- ✅ Daemon health IPC port (:19980) on all platforms
- ✅ Single-instance guard (prevent split-brain on double-start)
- ✅ CRLF-safe config parsing, seed pubkey token parsing
- ✅ Session persistence (v1:plain JSON, atomic write)
- ✅ Reconnect backoff with per-addr scheduling (no event-loop starvation)
- ✅ Duplicate connection resolution (deterministic tie-break)
- ✅ Peer pubkey dedup in dial loop (don't re-dial connected peers)
- ✅ 1009/1009 test suite on Windows, 16 suites

## v1.5 — Next priorities (what the GUIDELINE defines as proximate)

- `file send` / `file recv` mesh-native transfer
- Quick `restart` signal for bash/hermes/codex/claude over mesh
- `render_hint` flag on OutputMsg for markdown vs terminal
- Virtual folder mapping (local ↔ remote sync)
- Delta-patch remote file editing
- Stateless `edit` subcommand (open remote file locally, save delta)

## v2 — Future

- QUIC via msquic transport backend
- Nonblocking TLS handshakes in main event loop
- Session recording / replay
- Read-only session spectators (fan-out mode)
- SRV record peer discovery
- Dictionary-trained zstd for terminal output

---

*Full architectural decisions with ADR numbers: see [ARCHITECTURE.md](./ARCHITECTURE.md).*
*Implementation plan: see [PLANS.md](./PLANS.md).*
*Active task checklist: see [TODO.md](./TODO.md).*
*Agent dispatch rules: see [AUTONOMOUS.md](./AUTONOMOUS.md).*
