# Usage

This page is the command reference. Install and join first. See [Quickstart](QUICKSTART.md).

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
bs file send <peer> <local> --dest <remote> --wait
bs file recv <peer> <remote> --to <local> --wait
bs telemetry
```

Transfers are resumable. The receiver checks SHA-256. Success is the final `OK` line. A `PROGRESS` line is not success.

Peers serve only from `receive_dir` by default. Do not enable sensitive or arbitrary paths unless you accept host-level file access.

Large binaries can stall a busy mesh hop. Prefer a direct path to the target peer. Keep transfers well under a few megabytes when you use a relay that is not the target.

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
bs upgrade --tag 26.09.01-release
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
