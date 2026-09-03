# Protocol (`bs://`)

BridgeSessions uses binary frames over mutually authenticated TLS on TCP. The wire is documented here at the level a new operator or a contributor needs. The authoritative enum, variant, serializer, and decoder live in `bs-codec.h` (included from `bs-protocol.h`). The on-the-wire shape and the source must agree; a new message is incomplete until round-trip tests cover it.

## Transport

- TCP port `19949` is the default mesh port.
- TLS uses a self-signed certificate that carries the node's Ed25519 public key. Mutual TLS is required.
- The current compatibility profile negotiates TLS 1.2.
- `TCP_NODELAY` is set on interactive connections so keystrokes do not wait for a Nagle delay.
- zstd compression is used for payloads above 256 bytes when both peers advertise it.

## Identity

Every node owns one Ed25519 key. The public key is the mesh identity. It appears in three places that the daemon checks against the configured pin:

1. The peer certificate's SAN.
2. The `Hello` frame's identity block.
3. The configured `seed ... pubkey=` line or the local `authorized_keys` entry.

The mesh refuses the connection if any of the three disagree. There is no certificate hierarchy, no public CA, and no revocation list. Adding a peer is `bs join` with a single-use token; removing a peer is editing `authorized_keys`.

## Frames

A frame is a header followed by an optional payload.

Small / legacy frame:

```text
stream_id:u16be | type:u8 | flags:u8 | length:u16be | payload
```

A peer that advertises `+frm2` in `Hello.version` may use the larger frame:

```text
stream_id:u16be | type:u8 | flags:u8 | length:u32be | payload
```

The version negotiation is forward-only. A peer that does not advertise `+frm2` never receives an 8-byte header.

### Size limits

- u16 payload ceiling: 65,535 bytes.
- `frm2` payload ceiling: 4 MiB.
- Image input ceiling: 50 MiB before conversion or encoding.
- The decompressed payload is checked against the same logical cap before allocation or use.

### Flags

| Bit | Name | Meaning |
|---|---|---|
| `0x01` | `compressed` | zstd payload |
| `0x02` | `control` | control stream frame |
| `0x04` | `render markdown` | output rendering hint |
| `0x08` | `u32 length` | 8-byte header / `frm2` |

## Streams

- Stream `0` is the control stream. Hello, attach and detach, keepalive, gossip, and file-transfer control travel on stream `0`.
- Non-zero stream IDs identify session traffic on one connection.
- Stream IDs are connection-scoped. A reattach creates a new attachment; the server-side session persists.

A separate stream per attachment keeps unrelated output from interleaving and lets the daemon bound the per-stream queue.

## Message families

| Family | Examples | Purpose |
|---|---|---|
| Identity and mesh | `Hello`, `Gossip`, `DirectoryEnroll`, `Ping`, `Pong` | bootstrap, directory, liveness |
| Sessions | `Attach`, `AttachAck`, `Detach`, `SessionList`, `SessionDied`, `Signal` | attach lifecycle, kill, signal |
| Terminal | `Keystroke`, `Output`, `Resize`, `Scrollback`, `ScrollbackAck` | PTY/ConPTY traffic |
| Files | `FileMeta`, `FileChunk`, `FileAck`, `FileRequest` | resumable transfer |
| Clipboard | `Clipboard`, `ClipboardEcho` | OS clipboard echo |
| Desktop | `CuaRequest`, `CuaResponse`, `CuaVideoCapture`, `CuaVideoResult` | capture and input |
| Optional transports | DHT and WebRTC negotiation | mDNS and optional relay setup |

The authoritative enum lives in `bs-codec.h`. Adding a message means updating the enum, the serializer, the decoder, the wire format sketch in this file, and the round-trip tests.

## Handshake

A new connection follows this order:

1. TCP connect to `<peer>:19949`.
2. Mutual TLS handshake. The peer certificate's SAN carries the Ed25519 public key.
3. Extract the raw Ed25519 key from the peer certificate.
4. Exchange `Hello`. Each side announces its identity, version, and capability suffix list.
5. Verify the pin, certificate key, Hello key, and peer name all agree.
6. Promote the transport to a live mesh connection. Earlier frames are dropped.

During a bounded invite window, an unknown certificate may submit a `JoinRequest`. The single-use token is checked before authorization. Successful enrollment is signed; only configured seed keys are accepted as issuers.

## Liveness

`Ping` and `Pong` run on the control stream. `bs health <peer>` combines data-plane success with a recent pong. A local IPC reply alone is not enough; the operator wants to know the peer daemon can actually move a frame.

## File transfer

The protocol keeps the receiver in charge of flow control.

1. The sender publishes `FileMeta`: filename, byte size, SHA-256, chunk count, chunk size, optional destination.
2. The receiver validates metadata and the destination path. It opens `<target>.part` and replies with the first chunk it needs. The chunk request is resume-aware: if a valid partial already exists, the receiver asks for the next missing chunk instead of restarting from zero.
3. The sender streams a bounded window of chunks. The receiver validates order and size and acknowledges progress.
4. When the file is complete, the receiver verifies SHA-256, atomically renames the partial, and returns a final `FileAck`. The success line for an operator or a script is the final `OK`. Progress lines are not success.

A transport failure that leaves a valid partial preserves the partial and the sidecar for resume. A validation or write failure removes the partial.

## Sessions

1. The client sends `Attach` with the session name and the stream ID it wants to use.
2. The server replies with `AttachAck` and starts piping the PTY/ConPTY to the requested stream.
3. Output, keystrokes, resize, and scrollback travel on the non-zero stream.
4. `Detach` ends the attachment. The session stays alive on the server. `exit` in the remote shell ends the session.
5. `SessionDied` notifies other observers when the server-side session ends.

## CUA

A CUA request is a typed `CuaRequest` with a payload (capture, click, type, scroll, key, video) plus parameters. The peer daemon validates the caller, checks the call is not from a spectator, and forwards to the helper. The helper replies with a typed `CuaResponse`. Video capture returns a `CuaVideoResult` containing the encoded MP4.

Spectator attachments are rejected before any CUA action. The helper IPC is loopback-only and token-authenticated.

## Backward compatibility

- Capabilities are additive suffixes in `Hello.version` (for example `+frm2`).
- A peer never sends a new frame shape to a peer that did not advertise the capability.
- Protocol changes require mixed-version tests, not only same-version loopback tests. The release pipeline runs mixed-version regression coverage as part of the release gate.

## What this protocol does not

- There is no multi-tenant authorization. The mesh is for operator-controlled peer sets.
- There is no central server. Discovery and directory enrollment run on the mesh.
- There is no general RPC. Adding a remote procedure means adding a typed message and round-trip tests.
- There is no compression for sub-256-byte payloads. The compression header overhead outweighs the gain.