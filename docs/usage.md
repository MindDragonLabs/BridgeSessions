# Usage

This page is a task-oriented walkthrough. Install and join first. See [Quickstart](QUICKSTART.md).

Every command and flag: [Command reference](cli.md), which is generated from the binary's own `--help`.

The binary name is `bridgesessions`. The usual symlink is `bs`. The commands below use `bs`.

## Mesh and shells

```bash
bs peers list
bs health <peer>
bs fleet
bs stats
bs shell <peer>
bs shell <peer> --name agent
bs shell <peer> --cmd 'uname -a'
bs sessions <peer>
```

Named sessions survive disconnect. The daemon owns the PTY or ConPTY. `Ctrl-D` detaches. Reuse `--name` to reattach.

Use one stacked shell or `run-script` for dependent work. Separate one-shot commands do not share state.

Resolve names with `bs peers list`. Do not guess. Ambiguous names return suggestions.

`bs shell` to the local node name fails at once. The tool does not remap a digit suffix to a sibling name.

## Files

```bash
bs file send <peer> <local> --wait
bs file send <peer> <local> --dest <path-under-receive-dir> --wait
bs file recv <peer> <remote> --to <local> --wait
bs telemetry
```

Transfers are resumable. The receiver checks SHA-256. Success is the final `OK` line. A `PROGRESS` line is not success.

`--dest` is a path **under the peer's receive directory**, not an absolute path. The peer may allow `~` and `/tmp` when `file.dest_allow_home` is set; do not enable that unless you accept host-level file access.

Read the `dest=` field in the `OK` line: it is the path relative to the peer's receive directory, so you can find the file. If the peer is too old to confirm the destination, the sender says so instead of guessing.

The receive directory is a staging area, not storage. Every received file is also written to the caller's destination, so a copy left behind doubles the disk cost of that transfer. Files older than `receive_retention_hours` (default 24) are removed hourly. Set it to `0` to keep them. Partial transfers are never removed.

Peers serve only from `receive_dir` by default. Do not enable sensitive or arbitrary paths unless you accept host-level file access.

Large binaries can stall a busy mesh hop. Prefer a direct path to the target peer. Keep transfers well under a few megabytes when you use a relay that is not the target.

## Sync pairs (folder mirroring)

```bash
bs sync pair init <local-dir> <peer>:<remote-dir>       # dry-run manifest, nothing moves
bs sync pair init <local-dir> <peer>:<remote-dir> --approve
bs sync pair approve <id>                               # approve the pending manifest
bs sync pair run <id>                                   # one-shot apply (resumable, hash-verified)
bs sync pair status [id]
```

A pair mirrors one local directory to a peer directory through explicit,
approved manifests — there is no cwd auto-sync and no daemon loop in this
increment; you run each apply yourself. `init` prints the full manifest
(classified as create / modify / delete) and persists the pair to
`~/.bridgesessions/state/sync-pairs.json`; classification is content-hash
based, and **mtime is never used for ordering** — a Lamport-style logical
clock (one tick per manifest scan, persisted per pair) positions the pair
instead, so cross-host clock skew cannot regress data.

Nothing transfers until the dry-run manifest is approved (`--approve` or
`bs sync pair approve <id>`). Deletion is never inferred from a missing peer
index: deletes are explicit tombstones derived from the previously approved
manifest.

Default exclusions: `.git/`, `.bridgesessions/` (the BridgeSessions config
dir), secrets basenames (`.env*`, `id_rsa`, `id_ed25519`, `*.pem`, `*.key`,
`secrets*`, `credentials.json`, `*secret*`), and bs-sync working files
(`.bs-sync/`, `.tmp-bs*`, `*.conflict-*`). Symlinks are never followed.

Windows peers are allowed only via `bs sync pair init … --via-run-script`
(files pushed through the run-script verb; no PowerShell bodies are pushed).
The default path is a POSIX push to Unix peers.

See `docs/bs-sync-design.md` for the full design and spike findings.

## Scripts

```bash
bs run-script <peer> ./task.sh
bs run-script <peer> ./task.ps1 --interpreter powershell
bs script add ./task.sh --name task
bs script push task --peer <peer>
bs script run task --peer <peer> -- 'arg with spaces'
```

`run-script` detects bash, PowerShell, or Python from the extension or the shebang. Arguments are quoted as separate words.

`bs script run` is a POSIX/bash helper. Do not use it to push Python or PowerShell bodies to Windows peers. Use `run-script` for that.

## Computer use

```bash
bs cua screen <peer>
bs cua capture <peer> -o screen.png
bs cua click <peer> --x 500 --y 300
bs cua type <peer> --text 'hello'
bs capture-video <peer> --duration 10 -o capture.mp4
```

Windows and macOS need one helper in the interactive user session. See [Computer use](cua.md).

## Bootstrap and upgrade

```bash
bs invite
bs join <seed-address>:19949 <token> --start
bs upgrade
bs upgrade --tag <tag>
```

`bs invite` works on a pinned seed. The token is single-use.

`bs upgrade` downloads GitHub Release assets. It checks the checksum and the embedded version before it replaces the binary.

## Daemon

```bash
bridgesessions --daemon --config ~/.bridgesessions/config
```

Prefer the installer service unit. Do not copy obsolete unit files from old tags.

On Linux the installer uses a runtime mask to pause the unit during a swap. It does not persist-disable the unit.

## Bridge Panel

```bash
python3 -m tools.bridgepanel
```

See [Bridge Panel](bridge-panel.md).

## Always-online seed

See [Always-online seed](always-online-seed.md).
