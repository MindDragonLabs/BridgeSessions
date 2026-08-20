# Protocol (`bs://`)

BridgeSessions uses binary frames over mutually authenticated TLS/TCP.

## Transport

- TCP port: 19949 by default.
- Current compatibility profile: TLS 1.2.
- Identity: pinned Ed25519 public key carried in a self-signed certificate and repeated in Hello.
- Latency: `TCP_NODELAY` on interactive connections.
- Compression: zstd for payloads above 256 bytes.

## Frames

Small/legacy frame:

```text
stream_id:u16be | type:u8 | flags:u8 | length:u16be | payload
```

A peer advertising `+frm2` in `Hello.version` may use:

```text
stream_id:u16be | type:u8 | flags:u8 | length:u32be | payload
```

Limits:

- u16 payload: 65,535 bytes,
- `frm2` payload: 4 MiB,
- image input: 50 MiB before conversion/encoding,
- decompressed payload is checked against the same logical cap before allocation/use.

Flags:

| Bit | Name | Meaning |
|---|---|---|
| `0x01` | compressed | zstd payload |
| `0x02` | control | control stream frame |
| `0x04` | render markdown | output rendering hint |
| `0x08` | u32 length | 8-byte header / `frm2` |

## Streams

- Stream 0 is control: Hello, attach/detach, keepalive, gossip, transfer control.
- Non-zero streams identify session traffic on one connection.
- Stream IDs are connection-scoped. Reattach creates a new attachment; the server-side session persists.

## Message families

| Family | Examples |
|---|---|
| identity/mesh | Hello, Gossip, DirectoryEnroll, Ping/Pong |
| sessions | Attach/Ack, Detach, SessionList, SessionDied, Signal |
| terminal | Keystroke, Output, Resize, Scrollback/Ack |
| files | FileMeta, FileChunk, FileAck, FileRequest |
| clipboard | Clipboard, ClipboardEcho |
| desktop | CuaRequest/Response, CuaVideoCapture/Result |
| optional transports | DHT and WebRTC negotiation messages |

The authoritative enum, variant, serializers, and decoders are in `bs-protocol.h`. A new message is incomplete unless all four agree and round-trip tests cover it.

## Handshake and identity

1. TCP connect.
2. Mutual TLS handshake.
3. Extract raw Ed25519 key from the peer certificate.
4. Exchange Hello.
5. Require the expected pin, certificate key, Hello key, and peer name to agree.
6. Only then promote the transport to a live mesh connection.

During a bounded invite window, an unknown certificate may send a JoinRequest. The single-use token is checked before authorization. Successful enrollment is signed; only configured seed keys are accepted as issuers.

## File transfer

1. Sender sends metadata: filename, byte size, SHA-256, chunk count, chunk size, optional destination.
2. Receiver validates metadata and path, opens `<target>.part`, and replies with the first needed chunk (resume-aware).
3. Sender streams a bounded window of chunks; receiver validates ordering/size and acknowledges progress.
4. Receiver verifies SHA-256, atomically renames the partial, and returns a final ACK.

Transport failure preserves a valid partial and sidecar for resume. Validation or write failure removes it.

## Backward compatibility

Capabilities are additive suffixes in `Hello.version` (for example `+frm2`). Never send a new frame shape to a peer that did not advertise it. Protocol changes require mixed-version tests, not only same-version loopback tests.
