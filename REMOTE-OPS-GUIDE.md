# 🖥️ bridgesessions Remote Ops — linux-b → Windows (Shadow)

## Architecture

```
linux-b (Linux)                         Shadow (Windows 11)
┌─────────────────────┐              ┌─────────────────────────────────┐
│ akta2/videoworks3/  │  TLS 1.3     │ C:\Users\Shadow\                │
│ ┌─────────────┐     │  + ed25519   │ ┌──────────────────────────┐    │
│ │ bs-client    │─────┼──────────────┼─▶ bs-server :19948         │    │
│ │ (controller) │     │  mutual auth │ │ ConPTY → cmd.exe/pwsh   │    │
│ └─────────────┘     │              │ │ Windows clipboard        │    │
│                     │              │ └──────────────────────────┘    │
│ You type here,      │              │ Interactive Windows terminal     │
│ Windows responds    │              │ under your full control          │
└─────────────────────┘              └─────────────────────────────────┘
```

## Prerequisites (already set up — verified ✅)

| Machine | What | Status |
|---|---|---|
| **linux-b** | bs-server binary at `/root/bridgesessions/build/release/bs-server/` | ✅ running (PID 1962296, :19948) |
| **linux-b** | Identity: `/root/.bridgesessions/id_ed25519.pem` | ✅ |
| **linux-b** | Trust store: `known_servers` has Shadow's fingerprint | needs verify |
| **Shadow** | bs-server at `0.0.0.0:19948` | ✅ running (PID 7184) |
| **Shadow** | authorized_keys includes linux-b's pubkey | ✅ `ede970fb...` |
| **Shadow** | ConPTY backing `cmd.exe` | ✅ tested |

## How to use

### 1. Connect from linux-b to Shadow's Windows desktop

```bash
# On linux-b
/root/bridgesessions/build/release/bs-client/bs-client \
  --server=100.124.169.66:19948 \    # Shadow's Tailscale IP + port
  --cert=/root/.bridgesessions/id_ed25519-cert.pem \
  --key=/root/.bridgesessions/id_ed25519.pem \
  --session=akta2-shell               # name the session for later reattach
```

### 2. From any directory on linux-b (add a shell alias)

```bash
# Add to ~/.bashrc or ~/.zshrc
alias shadow='bs-client --server=100.124.169.66:19948 \
  --cert=$HOME/.bridgesessions/id_ed25519-cert.pem \
  --key=$HOME/.bridgesessions/id_ed25519.pem'

# Now just:  shadow
```

### 3. Named sessions (re-attachable)

```bash
bs-client --server=100.124.169.66:19948 \
  --cert=$HOME/.bridgesessions/id_ed25519-cert.pem \
  --key=$HOME/.bridgesessions/id_ed25519.pem \
  --session=videoworks                # same name = reattach to existing PTY
```

### 4. Clipboard integration

Once attached, text copied on Windows is forwarded back to linux-b via OSC 52:

```bash
# In the Windows session:
echo "hello from windows" | clip     # puts text on Windows clipboard
# The client on linux-b receives the clipboard content automatically
```

### 5. File transfer via clipboard (small files)

For configs, snippets, keys (under 1 MB): base64 + clipboard round-trip works.

For larger files: use the existing `scp` or SFTP paths you already have (Bitvise on Windows, OpenSSH on linux-b).

## How we verified this works (cross-relay test results)

| Direction | Test | Result |
|---|---|---|
| Windows → linux-a | `WINDOWS_TO_FECV3_OK_8c4d` | ✅ marker + uname + whoami |
| Windows → linux-b | `WINDOWS_TO_FECV4_OK_2a91` | ✅ marker + uname + whoami |
| linux-a → Windows | `FECV3_TO_HERE_OK_77b3` | ✅ marker + whoami(Shadow) |
| **linux-b → Windows** | `FECV4_TO_HERE_OK_91d6` | ✅ marker + whoami(Shadow) |
| ConPTY E2E | 5 cycles of `echo MARKER` + keystrokes | ✅ |

## Troubleshooting

### "certificate verify failed"

Fecv4's pubkey isn't in Shadow's authorized_keys, OR the known_servers TOFU check mismatched.

```bash
# Check what pubkey linux-b presents
cat /root/.bridgesessions/id_ed25519.pub

# Verify Shadow's authorized_keys has that hex string
ssh root@Shadow ... # or ask Hermes to verify
```

### "connection refused"

Shadow's bs-server is down. Restart it:

```powershell
# On Shadow (via Hermes or direct)
cd C:\SFTP\agent\bridgesessions
build\windows-msvc-debug\bs-server\bs-server.exe \
  --listen 0.0.0.0:19948 \
  --cert C:\Users\Shadow\.bridgesessions\_bs_autocert.pem \
  --key C:\Users\Shadow\.bridgesessions\_bs_autokey.pem \
  --auth C:\Users\Shadow\.bridgesessions\authorized_keys
```

### "Connection timed out"

Network path isn't there. Check:

```bash
# Tailscale IP of Shadow
tailscale status | grep -i shadow

# If Tailscale is down on linux-b
tailscale up
```

## How to work with Hermes via this setup

Once you're connected via bridgesessions, you're in a Windows cmd.exe/PowerShell on Shadow. From there you can:

1. **Use Hermes normally** — Hermes is already running as a persistent service on Shadow, so you don't need to "connect to Hermes" — you ARE on the machine.
2. **Run builds/tests** — `cd C:\SFTP\agent\bridgesessions && configure-msvc.bat && build-msvc.bat`
3. **Check server status** — `netstat -ano | findstr 19948`
4. **View logs** — `type C:\Users\Shadow\.bridgesessions\bs-server.log`
5. **Restart bs-server** — kill + relaunch with certs

**The Hermes agent itself** is running on Shadow as a Windows service — you can interact with it from the Windows terminal you've attached to via bridgesessions.

---

*Last verified: 2026-06-01 · bridgesessions v26.05.31 · cross-relay 4/4 passing*
