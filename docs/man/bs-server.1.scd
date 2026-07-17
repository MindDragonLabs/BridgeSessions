bs-server(1)

# NAME

bs-server - bridgesessions session daemon

# SYNOPSIS

*bs-server* [OPTIONS]
*bs-server* *authorize* _pubkey_
*bs-server* *keygen*

# DESCRIPTION

*bs-server* is the remote session daemon for the bridgesessions system. It runs
as a single-threaded event loop on a Linux host, listening for TLS 1.2+ (prefer 1.3)
connections on a configurable TCP port (default 9948).

The daemon manages named sessions, each backed by a pseudo-terminal (PTY) that
runs a user-specified command (default: _hermes --tui_). Clients attach via
protocol messages and relay keystrokes, output, resize events, signals, and
clipboard data.

Session names are scoped by the authenticated client public key. Two different
authorized clients can both attach to a session named _agent_ without colliding;
the same key reattaches to its own _agent_ session.

Key features:

- *Up to 64 concurrent connections* in a single-threaded *poll()*-based event
  loop.
- *TLS 1.2+ (prefer 1.3)* with ed25519 mutual authentication. Client certificates are
  validated against an _authorized_keys_ file.
- *Per-authorized-client namespaces:* logical session names are user-friendly
  but isolated by client key identity.
- *Session persistence:* session metadata is saved to a JSON file and reloaded
  on startup. Sessions marked _recoverable_ can be resurrected when a client
  reattaches.
- *Scrollback ring buffer:* 1 MiB (default) circular buffer per session, storing
  the most recent terminal output. On attach, the last 2000 lines are replayed
  in 500-line chunks with client acknowledgement.
- *Keepalive:* *Ping* messages are sent to all connected clients every 5 seconds.
- *OSC 52 clipboard capture:* clipboard escape sequences in PTY output are
  intercepted, stripped from the rendered stream, and forwarded as
  _ClipboardMsg_ protocol messages.
- *Signal forwarding:* client-sent ^C, ^Z, ^\\ are delivered as *SIGINT*,
  *SIGTSTP*, *SIGQUIT* to the foreground process group.
- *Structured JSON logging* to _~/.bridgesessions/bs-server.log_ (rotating,
  1 MiB, 3 files).

# OPTIONS

*-l, --listen* _port_
	Listen port. Defaults to _9948_. The daemon binds on *INADDR_ANY*
	(0.0.0.0) with *SO_REUSEADDR*.

*-c, --cert* _path_
	Path to a TLS certificate file (PEM). If not provided, an in-memory
	self-signed ed25519 certificate is generated automatically.

*-k, --key* _path_
	Path to a TLS private key file (PEM). If not provided, an in-memory
	ed25519 key is generated automatically.

*--auth* _path_
	Path to an _authorized_keys_ file. One hex-encoded ed25519 public key
	per line. If not provided, all client certificates are accepted (no
	authorization check).

*-p, --persist* _path_
	Path to a session persistence JSON file. On startup, sessions are loaded
	from this file and marked _recoverable_. On shutdown, all active
	sessions are saved. Default path is
	_~/.bridgesessions/sessions.json_.

*-V, --version*
	Print version string and exit.

# SUBCOMMANDS

*authorize* _pubkey_
	Register a hex-encoded ed25519 public key in
	_~/.bridgesessions/authorized_keys_. The command checks for duplicates
	before appending.

*keygen*
	Generate a new ed25519 keypair (same implementation as
	*bs-client keygen*). Writes to _~/.bridgesessions/_.

# SESSION LIFECYCLE

Sessions progress through these states:

[ *State*
: Description
| created
:  Initial state, PTY allocation pending.
| running
:  PTY and child process are active.
| attached
:  A client is connected to this session.
| detached
:  No client connected; PTY and child remain alive.
| died
:  PTY or child process exited unexpectedly.
| exited
:  Child process exited normally.
| killed
:  Explicitly killed, or pruned for idle timeout.
| recoverable
:  Loaded from persistence; awaiting reattach to spawn.

Sessions survive client disconnects indefinitely. Idle sessions (no PTY output
for 7 days) are eligible for pruning.

# FILES

_~/.bridgesessions/authorized_keys_
	Hex-encoded ed25519 public keys, one per line. Used by the *--auth*
	option to validate client certificates during the TLS handshake.

_~/.bridgesessions/sessions.json_
	Default session persistence file. An atomic write (temp file + rename)
	of a JSON array of session metadata objects: name, owner_id, command,
	state, and ISO 8601 creation timestamp.

_~/.bridgesessions/bs-server.log_
	Structured JSON event log (rotating, 1 MiB max, 3 retained files).

# SIGNALS

*SIGINT*, *SIGTERM*
	Graceful shutdown: stops accepting new connections, closes all client
	connections, saves persisted sessions, and exits.

*SIGPIPE*
	Ignored.

# EXAMPLES

Start the daemon on the default port with auto-generated keys:
```
bs-server
```

Start with explicit keys and authorization:
```
bs-server -l 9948 -c /path/to/cert.pem -k /path/to/key.pem --auth ~/.bridgesessions/authorized_keys
```

Enable session persistence:
```
bs-server -p ~/.bridgesessions/sessions.json
```

Authorize a client public key:
```
bs-server authorize a1b2c3d4e5f6...
```

# SEE ALSO

*bs-client*(1), *bs-protocol*(7)
