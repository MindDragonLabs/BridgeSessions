# TODO — AI shell stacking + Windows peer awareness

## Command stacking
Agents must **not** open one mesh shell per command when a batch will do.

**Preferred patterns**

| Peer OS | Stack |
|---------|--------|
| Windows (cmd) | `cmd /c "cmd1 && cmd2 && cmd3"` |
| Windows (PowerShell) | `powershell -NoProfile -Command "cmd1; cmd2; if (-not $?) { exit 1 }; cmd3"` |
| Linux/macOS | `bash -lc 'cmd1 && cmd2 && cmd3'` |

- [ ] Document in skill + `bs shell --help` examples
- [ ] Reject anti-pattern in skill: three separate `bs shell` for dependent steps
- [ ] Optional: `bs shell <peer> --stack a.cmd b.cmd` helper (later)

## Windows peer — frequent reminders
When peer is Windows (`test-pc7`, hostname `SHADOW-*`, `os=windows`):

| Don't | Do |
|-------|-----|
| `tar xvf`, `apt`, `yum`, `./configure` | `Expand-Archive`, `tar` only if available, `msiexec`, `winget` |
| `chmod +x`, shebang scripts | `.ps1` / `.cmd` / `.bat` |
| Assume `/tmp` | `%TEMP%` / `$env:TEMP` |
| Ship Linux ELF as `.exe` | Cross-compile MinGW **only for BS binary**, not as general Linux substitute |
| `bs shell` for gameplay GUI | WinRM Session-1 / documented gameplay path |

**MinGW note:** MinGW builds Windows PE binaries; it is **not** a Linux userspace. Do not tell operators to “just use mingw instead of Linux tools” for arbitrary workflows.

- [ ] Skill section + doctor note when peer Windows
- [ ] shell help banner when peer name matches known Windows seeds
