# Quickstart

This page takes you from a download to a working remote shell. Read [Security](https://github.com/MindDragonLabs/BridgeSessions/blob/main/SECURITY.md) first.

Current release: **`26.08.25-beta7`**.

## 1. Install

### Linux / macOS

```bash
curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash
```

### Windows PowerShell

```powershell
irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
```

### Check

```bash
bs --version
bs doctor
```

`bs --version` must print `26.08.25-beta7`.

If the shell cannot find `bs`, add `~/.local/bin` (Linux/macOS) or `%LOCALAPPDATA%\bridgesessions` (Windows) to `PATH`.

## 2. Join a mesh

You need a seed that already runs BridgeSessions.

**On the seed:**

```bash
bs invite
```

**On the new node**, use the printed address and token at once:

```bash
bs join <seed-address>:19949 <token> --start
bs health <seed-name>
```

Only a pinned seed can issue an enrollment that the mesh accepts.

If you are building the first node, follow [Always-online seed](always-online-seed.md) before you invite anyone.

## 3. Open a shell

```bash
bs peers list
bs shell <peer>
bs shell <peer> --name agent
bs shell <peer> --cmd 'uname -a'
```

`Ctrl-D` detaches. The remote PTY stays. Reuse `--name` to reattach.

One `bs shell --cmd` is one new process. It does not keep the working directory from the last call. For several steps, use one stacked command or `bs run-script`.

## 4. Copy a file

```bash
bs file send <peer> ./artifact.bin --wait
bs file send <peer> ./config.toml --dest configs/config.toml --wait
bs file recv <peer> received/report.md --to ./report.md --wait
```

Wait for the final `OK`. The transfer uses SHA-256. A dropped link can resume.

Peers serve files from `receive_dir` only, unless you change that on purpose.

## 5. Run a script

```bash
bs run-script <peer> ./deploy.sh
bs run-script <peer> ./task.ps1 --interpreter powershell
```

The tool picks bash, PowerShell, or Python from the extension or the shebang.

## 6. Computer use (optional)

Windows and macOS need one `bridgesessions --cua-helper` in the interactive user session. macOS also needs Screen Recording and Accessibility permission for the signed app.

```bash
bs cua screen <peer>
bs cua capture <peer> -o screen.png
bs cua click <peer> --x 500 --y 300
```

Capture the screen before you click.

## 7. Review files in a browser (optional)

```bash
python3 -m tools.bridgepanel
```

Open the printed URL. See [Bridge Panel](bridge-panel.md).

## Next

- [Usage](usage.md) — full command list
- [Configuration](configuration.md) — config file
- [Always-online seed](always-online-seed.md) — central node
- [Why BridgeSessions](why-bridge-sessions.md) — when to use this tool
