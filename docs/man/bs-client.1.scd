bs-client(1)

# NAME

bs-client - bridgesessions terminal relay agent

# SYNOPSIS

*bs-client* [_target_] [OPTIONS]
*bs-client* *stats* [--json]
*bs-client* *keygen*
*bs-client* *host* *add* _name_ _server[:port]_ [OPTIONS]
*bs-client* *host* *list*
*bs-client* *host* *remove* _name_

# LEGACY NOTICE

BridgeSessions 2.0.6 ships the single *bridgesessions*(1) monolith. This modular
*bs-client* interface is not built or installed by the shipping CMake target;
use *bridgesessions --help*.

# DESCRIPTION

*bs-client* is the macOS terminal relay agent for the bridgesessions system. It
sits between a terminal emulator (such as BridgeSpace.app) and a remote
*bs-server*(1) daemon, translating stdin/stdout into protocol messages carried
over a TLS 1.2+ (prefer 1.3) connection.

On startup, *bs-client* resolves the server address, establishes a TCP
connection, performs a TLS 1.2+ handshake (prefer 1.3) with ed25519 mutual authentication,
and sends an _Attach_ message to join a named session. Once attached, it enters
a relay loop:

Server-side session names are scoped by this client's ed25519 public key. If
two authorized clients both use *--name=agent*, they get separate sessions;
reusing the same key reattaches to that key's own _agent_ session.

- Keystrokes from stdin are forwarded to the server as _Keystroke_ messages.
- PTY output from the server is written to stdout as _Output_ messages.
- Terminal resize events (*SIGWINCH*) are forwarded as _Resize_ messages.
- The macOS clipboard is bridged bidirectionally via _ClipboardMsg_ exchanges.
- In terminal mode, signal characters (^C, ^Z, ^\\) are forwarded as _Signal_ messages.
- In pipe/bridge mode, control/meta bytes are passed through verbatim as _Keystroke_ payloads.
- *Ctrl+D* triggers a graceful detach in terminal mode only.

On attach, the client clears the terminal and emits:

```
BridgeSpace Sessions Client v26.05.31
```

Each running client writes a live stats JSON file under
_~/.bridgesessions/clients/_; view it with *bs-client stats*.

If the connection drops, *bs-client* retries with exponential backoff (100ms
→ 5s max, with jitter) for up to 30 seconds. A session that outlives the
reconnect window prints instructions for manual reattachment.

# OPTIONS

_target_
	Optional ssh-style host alias or raw _host[:port]_. If _target_ matches an
	entry in _~/.bridgesessions/hosts_, that entry supplies the server address
	and optional default key, certificate, and session name. Example:
	*bs-client test-pc1 --name=hms*.

*-s, --server* _host:port_
	Server address. Defaults to _127.0.0.1:9948_. The port component is
	parsed from the value; IPv6 addresses must be bracketed.

*--hosts* _path_
	Host alias file. Defaults to _~/.bridgesessions/hosts_.

*-n, --name* _name_
	Session name to attach to. Defaults to _default_. Use _name:command_ to
	also override the command (shorthand for *--cmd*).

*-x, --cmd* _command_
	Override the command to execute in the session. When set, the server
	uses this command instead of its configured default. Takes precedence
	over server-side configuration.

*-c, --cert* _path_
	Path to a TLS certificate file (PEM). If not provided, an in-memory
	self-signed ed25519 certificate is generated automatically.

*-k, --key* _path_
	Path to a TLS private key file (PEM). If not provided, an in-memory
	ed25519 key is generated automatically.

*-t, --term* _type_
	TERM environment override. Passed to the server in the _Attach_ message.
	If not set, the current _TERM_ environment variable is used, falling
	back to _xterm-256color_.

*-q, --quiet*
	Suppress status messages on stderr (connection state, detach
	notifications, reconnection progress). Does not suppress the terminal banner;
	use *--no-banner* for that.

*--pipe*
	Force stdio pipe mode. Raw TTY mode is skipped; control, meta/option,
	command-mapped, and other byte sequences received on stdin are forwarded
	verbatim as _Keystroke_ messages.

*--no-clean-start*
	Do not emit the ANSI reset/clear-screen/clear-scrollback sequence on attach.

*--no-banner*
	Do not emit the _BridgeSpace Sessions Client v26.05.31_ startup banner.

*--stats-dir* _path_
	Directory for live client stats JSON files. Defaults to
	_~/.bridgesessions/clients_.

*--stats-file* _path_
	Exact live stats JSON path for this client. Overrides *--stats-dir* for the
	current process.

*--key-log* _path_
	Diagnostics only: append received key bytes as hex/class labels. Useful for
	checking whether BridgeSpace pipe mode is sending control/meta/command key
	sequences. The log does not print plaintext, but the hex bytes are still
	input data; use only in trusted local diagnostics.

*-V, --version*
	Print version string and exit.

# SUBCOMMANDS

*stats* [--json]
	Show live client-side stats from _~/.bridgesessions/clients_. The human view
	includes PID, running state, mode, session, server, uptime, byte counters,
	key frame count, meta-sequence count, and last event. Use *--json* for the
	full JSON records.

*keygen*
	Generate a new ed25519 keypair and write it to
	_~/.bridgesessions/_. Creates the following files:

	- _id_ed25519.pem_ (0600) — private key
	- _id_ed25519-cert.pem_ — self-signed certificate
	- _id_ed25519.pub_ — hex-encoded public key

	The hex public key must be registered on the server via
	*bs-server authorize* before the client can connect.

*host add* _name_ _server[:port]_
	Add or update a host alias. Options: *--key* _path_, *--cert* _path_,
	*--session* _name_. After adding an alias, connect with *bs-client name*.

*host list*
	List configured host aliases.

*host remove* _name_
	Remove a host alias.

# FILES

_~/.bridgesessions/id_ed25519.pem_
	Default ed25519 private key (0600).

_~/.bridgesessions/id_ed25519-cert.pem_
	Default ed25519 certificate.

_~/.bridgesessions/id_ed25519.pub_
	Hex-encoded public key fingerprint.

_~/.bridgesessions/hosts_
	Named host aliases, one per line:
	_name server[:port] [key=path] [cert=path] [session=name]_.

_~/.bridgesessions/known_servers_
	TOFU (Trust On First Use) server fingerprint cache. On first connection
	the server's certificate fingerprint is stored; subsequent connections
	verify against this cache. Fingerprint mismatches fail closed.

_~/.bridgesessions/clients/bs-client-<pid>.json_
	Live client stats written atomically by each running client.

# SIGNALS

In terminal mode, _bs-client_ sets stdin to raw mode (*cfmakeraw*) at startup.
Raw mode is skipped in bridge mode and stdio pipe mode. Terminal mode handles:

*SIGWINCH*
	Queries the terminal window size and sends a _Resize_ message to the
	server.

*SIGINT*, *SIGTERM*
	Sets a flag causing the relay loop to exit cleanly, restoring the
	terminal and printing reattach instructions if the session survives.

*SIGPIPE*
	Ignored (prevents crash on write to a closed TLS connection).

# EXAMPLES

Connect to the default session on a local server:
```
bs-client --server=dev.example.com
```

Attach to a named session:
```
bs-client --server=dev.example.com --name=hms
```

Add a host alias and connect like ssh:
```
bs-client host add test-pc1 203.0.113.10:9948 \
  --key ~/.bridgesessions/id_ed25519.pem \
  --cert ~/.bridgesessions/id_ed25519-cert.pem
bs-client test-pc1 --name=hms
```

Attach with a custom command:
```
bs-client --server=dev.example.com --name=logs --cmd="journalctl -f"
```

Show live clients:
```
bs-client stats
bs-client stats --json
```

Diagnose key bytes in BridgeSpace pipe mode:
```
bs-client test-pc1 --pipe --key-log ~/.bridgesessions/key-debug.log
```

Generate a keypair:
```
bs-client keygen
```

Reattach after a disconnect (printed by the client on timeout):
```
bs-client --server=dev.example.com --name=hms
```

# SEE ALSO

*bs-server*(1), *bs-protocol*(7)
