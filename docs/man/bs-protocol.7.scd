bs-protocol(7)

# NAME

bs-protocol — bridgesessions wire protocol

# DESCRIPTION

The *bs-protocol* defines the framing, message types, and serialisation rules
for all communication between *bs-client*(1) and *bs-server*(1). Every message
is carried inside a binary frame over a TLS 1.3 connection.

# WIRE FORMAT

## Frame Header

Each frame begins with a 6-byte header:

```
[stream_id: u16][type: u8][flags: u8][length: u16][data: length bytes]
```

All multi-byte integers are big-endian.

[ Field
: Size     : Description
| stream_id
:  2 bytes :  Application-level stream identifier. Stream 0 is the control channel; 1–65535 are session data channels.
| type
:  1 byte  :  Message type (see *MESSAGE TYPES* below).
| flags
:  1 byte  :  Bit 0: compressed (zstd). Bit 1: control frame (set when stream_id == 0).
| length
:  2 bytes :  Payload length in bytes (0–65535).
| data
:  _length_:  Serialised message payload, optionally zstd-compressed.

## Compression

Frames whose serialised payload exceeds 256 bytes are compressed with *zstd*
at level 3 before the frame is assembled. The *FLAG_COMPRESSED* bit (0x01) is
set in the flags byte. The length field always holds the compressed payload
size.

Compression is applied to _Output_, _Scrollback_, and _ClipboardMsg_ payloads.
_Keystroke_, _Ping_, _Pong_, and other small messages are never compressed.

## Control Stream (ID 0)

Stream ID 0 is reserved for control messages: _Attach_, _Detach_, _SessionList_,
_ServerInfo_, _Ping_, _Pong_, _ScrollbackAck_. Control frames set the
*FLAG_CONTROL* bit (0x02) in the flags byte.

## Max Frame Size

The maximum frame size is 65535 bytes (constrained by the u16 length field).
Payloads that would exceed this limit (e.g., very large clipboard content) are
not supported in v1.

# MESSAGE TYPES

17 message types are defined. Each maps to a C++ *std::variant* member.

## Client → Server

[ Type byte : Name : Payload
| 0x01 : Keystroke : Raw key bytes.
| 0x03 : Resize : cols:u16, rows:u16.
| 0x05 : ClipboardPut : hash (newline-terminated), text.
| 0x06 : Attach : cols:u16, rows:u16, term_len:u8, term, name_len:u8, session_name.
| 0x07 : Detach : (empty).
| 0x0B : Pong : (empty).
| 0x0D : Signal : signal_type:u8 (0=CtrlC, 1=CtrlZ, 2=CtrlBackslash).
| 0x0F : ScrollbackAck : (empty).

## Server → Client

[ Type byte : Name : Payload
| 0x02 : Output : Rendered PTY stdout.
| 0x04 : ClipboardGet : hash (newline-terminated), text.
| 0x08 : SessionList : Repeated: name_len:u8, name, state_len:u8, state, uptime_seconds:u32be.
| 0x09 : ServerInfo : hostname (newline-terminated), version (newline-terminated), load:f64.
| 0x0A : Ping : (empty).

## Bidirectional

*Ping* (0x0A) and *Pong* (0x0B) may be sent by either side. The server sends
*Ping* every 5 seconds; clients reply with *Pong*.

## Server → Client (continued)

[ Type byte : Name : Payload
| 0x0C : Scrollback : total_lines:u32be, chunk_index:u32be, data.
| 0x0E : ExitCode : code:u32be.
| 0x10 : SessionDied : exit_code:u32be, signal_num:u32be.
| 0x11 : ClipboardEcho : hash.

## Message Semantics

*Keystroke* (0x01)
	Raw bytes read from the client's stdin. On the server these are written
	directly to the session's PTY master fd.

*Output* (0x02)
	Rendered terminal output from the session PTY. Written directly to the
	client's stdout.

*Resize* (0x03)
	Sent when the client's terminal window changes size (SIGWINCH). The
	server applies *TIOCSWINSZ* to the PTY.

*ClipboardGet* (0x04) / *ClipboardPut* (0x05)
	The *ClipboardMsg* struct is used for both directions. The hash field
	prevents clipboard echo loops.

*Attach* (0x06)
	Requests attachment to a named session. The server creates or reattaches
	to the session, sending scrollback on reattach.

*Detach* (0x07)
	Disconnects the client from the session without destroying it. The
	server replies with _ExitCode_ and closes the connection.

*SessionList* (0x08)
	Returns all sessions known to the server, each with name, state, and
	uptime in seconds.

*ServerInfo* (0x09)
	Server metadata: hostname, version string, load average.

*Ping* (0x0A) / *Pong* (0x0B)
	Keepalive exchange. The server sends *Ping* every 5 seconds to detect
	dead connections.

*Scrollback* (0x0C)
	Replay chunk of session history. Chunks are 500 lines each. The client
	must send _ScrollbackAck_ after each chunk before the next is delivered.

*Signal* (0x0D)
	Forwarded control characters. Maps to POSIX signals on the server.

*ExitCode* (0x0E)
	The foreground process in the session exited. Carries the exit code.

*ScrollbackAck* (0x0F)
	Client acknowledgement for a scrollback chunk. Controls replay pacing.

*SessionDied* (0x10)
	The PTY or child process terminated abnormally. Carries exit code and
	signal number.

*ClipboardEcho* (0x11)
	Server confirms receipt of a clipboard hash to prevent re-sending.

# CLIPBOARD PROTOCOL

Clipboard synchronisation uses hash-based race prevention:

. The client polls the macOS pasteboard every 500ms and hashes the content.
. If the hash differs from the last server-acknowledged hash, the client
  sends _ClipboardPut_.
. The server echoes the hash back via _ClipboardEcho_.
. The server captures OSC 52 sequences from PTY output and sends them as
  _ClipboardGet_.

The clipboard hash is a *std::hash<std::string>* value (platform
*native* hash, not SHA-256). It is sufficient for content-change detection
and is not used for security.

# SCROLLBACK REPLAY

On reattach, the server replays the last 2000 lines of terminal output from
the session's ring buffer:

. Chunks are 500 lines each.
. Each chunk is a _Scrollback_ message with _total_lines_, _chunk_index_,
  and _data_.
. The client writes the chunk to stdout and sends _ScrollbackAck_.
. The server waits for the ack before sending the next chunk.

# AUTHENTICATION

Authentication occurs at the TLS layer:

. Both client and server hold ed25519 keypairs.
. During the TLS 1.3 handshake, the server requests a client certificate.
. The client certificate's public key is checked against the server's
  _authorized_keys_ file (one hex-encoded key per line).
. The server's certificate fingerprint is verified via TOFU (Trust On First
  Use): on first connect the fingerprint is cached; subsequent connections
  must match.

# SEE ALSO

*bs-client*(1), *bs-server*(1)
