# Quickstart

## Install

```bash
curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash
# Windows PowerShell:
# irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex
```

```bash
bs --version
bs doctor
```

## Join

On a pinned seed:

```bash
bs invite
```

On the new node, use the printed address/token immediately:

```bash
bs join <seed-address>:19949 <token> --start
bs health <peer>
```

Only explicitly pinned seeds can issue accepted mesh-wide enrollments.

## Shell

```bash
bs shell <peer>
bs shell <peer> --name agent
bs shell <peer> --cmd 'hostname && uname -a'
```

`Ctrl-D` detaches; the remote PTY remains. Reuse `--name` to reattach.

## Files and scripts

```bash
bs file send <peer> ./artifact.bin --wait
bs file send <peer> ./config.toml --dest configs/config.toml --wait
bs file recv <peer> received/report.md --to ./report.md --wait
bs run-script <peer> ./deploy.sh
```

Success requires the final `OK` after SHA-256 verification.

## Computer use

Windows/macOS need one `bridgesessions --cua-helper` in the interactive user session. macOS also needs Screen Recording and Accessibility permission.

```bash
bs cua screen <peer>
bs cua capture <peer> -o screen.png
bs cua click <peer> --x 500 --y 300
```

Capture before clicking. Continue with [Usage](usage.md), [Configuration](configuration.md), and [Security](https://github.com/MindDragonLabs/BridgeSessions/blob/main/SECURITY.md).
