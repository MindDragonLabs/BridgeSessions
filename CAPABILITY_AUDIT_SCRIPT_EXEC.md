# BridgeSessions Capability Audit — Command Stacking & Script Reuse

**Scope:** how `--cmd` / `run_command` / `run_job` execute today, where escaping
breaks (especially PowerShell), and concrete proposals for script-file execution
and a reusable script library. All file:line refs are against `~/bridgesessions/`.

---

## 1. How the `--cmd` path works today (and where it breaks)

### The wire path (good — escaping is NOT a wire problem)

The command travels as a **length-prefixed binary field** inside a structured
message, not as a shell-interpolated string:

- `bs_cli.hpp:806` — `-x,--cmd` parsed by CLI11 into `std::string shell_cmd`.
- `bs_cli.hpp:1009` — passed verbatim to `mc.shell_peer(peer, session, shell_cmd, …)`.
- `bs_mesh.hpp:5228` — copied into `AttachMsg::command`.
- `bs_protocol.hpp:636` / `:877` — serialized as `s.str_prefixed(m.command)`:
  a u32 length + raw bytes. **No shell ever touches it on the wire.** Quotes,
  backticks, `$()`, newlines, NULs all survive intact across TLS.
- Same path for `run_command` (`bs_mesh.hpp:4667`) and `run_job` (`:4758`).

**This is the right design.** The wire layer is escaping-clean. The breakage is
entirely at the two spawn endpoints.

### Endpoint A — POSIX spawn (mostly safe)

`bs_session.hpp:510`:
```cpp
execl("/bin/sh", "sh", "-l", "-c", command.c_str(), nullptr);
```
`command` becomes a single `argv[4]` passed to `execl` — **no shell re-parsing
happens at the bs layer.** Whatever `/bin/sh -c` does with it is between the user
and POSIX. Newlines, `&&`, `;`, `$()` all work because `sh -c` takes the whole
string as one script. Multi-line works. This path is **not** the user's pain.

### Endpoint B — Windows / ConPTY spawn (THE breakage site)

`bs_session.hpp:418`:
```cpp
std::string cmdline = "cmd.exe /c \"" + command + "\"";
```
This is the root cause. Three compounding problems:

1. **Double interpretation.** `command` is already a shell snippet, but it gets
   wrapped in `cmd.exe /c "…"` and then passed to `CreateProcessA`, which does
   its *own* quote-parsing of the whole command line. Any `"` inside `command`
   breaks the outer quotes. A script like
   `echo "hello" && echo "world"` becomes
   `cmd.exe /c "echo "hello" && echo "world""` → cmd.exe sees mismatched quotes.

2. **cmd.exe vs PowerShell mismatch.** `default_shell` on Windows is
   `"cmd.exe"` (`bs_session.hpp:608`), but the user writes PowerShell. There is
   no `pwsh`/`powershell` path at all — even `Get-Process` syntax is fed to
   cmd.exe, which rejects `$`, `|`, `()`-cmdlets entirely. The user is forced to
   wrap everything in `powershell -c "…"` **inside** the `--cmd`, producing
   triple-nested quoting: bs's `"` + powershell's `"` + the script's `"`.

3. **Embedded quotes are never escaped.** Line 418 concatenates `command`
   verbatim between two `"`. There is no call to escape `"`→`\"` or `\"`→`\\\"`.
   A single `"` anywhere in the script silently corrupts the command line.

### Why "rewrite the script" happens

The user's loop is: write PowerShell → `--cmd 'powershell -c "…"'` → cmd.exe
mangles the quotes → tweak escaping → still wrong → rewrite. Each iteration is
escaping roulette against *three* nested interpreters (bs string → cmd.exe →
powershell). There is no structural way to say "run this exact blob verbatim."

---

## 2. Survey: how other tools solve this

| Tool | Mechanism | Escaping burden | Relevance to bs |
|------|-----------|-----------------|-----------------|
| **SSH** | `ssh host 'cmd'` → remote `execve(shell, "-c", cmd)`. Single interp. | Low — one shell layer, same as POSIX endpoint | ✅ POSIX path already does this |
| **SSH + file** | `scp script host:; ssh host ./script` | Zero escaping | ✅✅ **Highest relevance** — bs already has file transfer |
| **tmux send-keys** | Sends literal keystrokes to a live PTY; `-l` flag disables key-name table | Medium — `send-keys -l` still needs the target shell to not eat chars | Partial — bs streams keystrokes, but no "paste a file" mode |
| **mosh** | Same as SSH; no command mode, just persistent PTY | N/A | Low |
| **ansible shell module** | Sends script *content* over its own channel, writes to temp file, executes, cleans up | Zero — `{}` free-form writes a file | ✅✅ **Template for `run-script`** |
| **ansible raw/script** | `ansible host -m script -a "./localscript.sh"` copies+execs | Zero | ✅✅ Exact pattern proposed below |
| **kubectl exec** | `exec -- cmd args` → containerd `exec` (argv array, no shell) | Zero — true argv | Partial — bs can't avoid a shell on Windows easily |
| **PowerShell remoting** | `Invoke-Command -ScriptBlock {…}` or `-FilePath` | Zero for `-FilePath`; ScriptBlock is a real AST not a string | ✅ PowerShell `-FilePath` is the model |

**Pattern that wins across all of them:** stop passing scripts as shell strings;
pass them as **files** and execute the file. The escaping problem disappears
because no interpreter re-parses the content.

---

## 3. Concrete proposal: `bs run-script` (send-file-then-exec)

### Why this is the right fix

bs **already has** every primitive needed:
- `file_send` (`bs_mesh.hpp:5582`) + `FileChunkMsg` protocol with checksums,
  resume, and zstd compression.
- `run_command` / `run_job` (`bs_mesh.hpp:4647`/`4740`) for execution.
- `receive_dir_` (`bs_mesh.hpp:1039`) = `~/.bridgesessions/received/`.

The gap is purely **compositional**: nothing chains "send file" → "exec file."
Adding a `run-script` command that does both in one round-trip eliminates the
escaping problem entirely because the script body never touches a shell until the
target's own interpreter reads it from disk.

### Proposed CLI surface

```bash
# Linux/mac target — runs via the POSIX path (sh -l -c), no escaping needed
bs run-script linux-a ./cleanup.sh

# Windows target — explicit interpreter; bs writes file, then invokes it
bs run-script winnode ./diag.ps1 --interpreter powershell
bs run-script winnode ./diag.ps1 --interpreter pwsh      # PS Core
bs run-script winnode ./build.bat                         # default: cmd.exe

# Pass arguments to the script
bs run-script linux-a ./deploy.sh -- --env prod --verbose

# Inline heredoc (no file needed) — bs writes to a temp file on the target
bs run-script linux-a - <<'EOF'
#!/bin/sh
systemctl status nginx
journalctl -u nginx --since "1 hour ago" | tail -50
EOF
```

The `-` / stdin form is the heredoc-style block mode: bs reads stdin locally,
writes it to a temp file on the target via the file-transfer path, executes,
streams output, and deletes the temp file.

### Code structure (sketch — fits existing architecture)

All in `bs_mesh.hpp` as a new `MeshController` method, mirroring `run_command`:

```cpp
// bs_mesh.hpp — new method, ~80 lines, reuses run_command + file primitives
RunCommandResult run_script(
    const std::string& peer,
    const std::string& local_script_path,   // "" → read from stdin
    const std::string& interpreter,          // "" auto-detect from shebang/.ext
    const std::vector<std::string>& script_args,
    int timeout_ms = 30000,
    bool cleanup = true)                     // delete temp file after
{
    // 1. If local_script_path empty, slurp stdin into a buffer (heredoc mode).
    // 2. file_send(peer, local_script_path_or_temp) → lands in
    //    ~/.bridgesessions/received/<name> on target. Reuse existing path.
    //    (daemon_file_send for in-daemon, or the standalone file_send for CLI.)
    // 3. Compute remote path = receive_dir_ + "/" + basename.
    // 4. chmod +x on POSIX targets (one run_command("chmod +x <path>")).
    // 5. Build exec command:
    //      POSIX:  "<remote_path> arg1 arg2"
    //      PS:     "powershell -ExecutionPolicy Bypass -File <remote_path> arg1"
    //      cmd:    "<remote_path> arg1 arg2"
    //    → passed through the SAME AttachMsg.command path (no new wire type).
    // 6. run_command(peer, exec_cmd, timeout_ms) — reuse verbatim.
    // 7. if (cleanup) run_command(peer, "rm -f <remote_path>" or
    //                                 "del <remote_path>", 5000).
    // 8. Return the RunCommandResult (output, exit_code, timed_out, job_id).
}
```

**Key insight:** this needs **zero new wire protocol.** It composes `file_send`
+ `run_command`, both of which exist and are escaping-safe (file is binary;
`run_command`'s exec command is now trivial — just a path + args, no quotes from
the script body).

### Why not a new wire message type?

Adding a `RunScriptMsg` would be cleaner long-term, but it's unnecessary for
v1 and adds protocol-versioning burden. The compose-existing-primitives approach
ships in one method and is testable today. A future `ExecFileMsg` can optimize
the round-trips if profiling shows the extra file-send latency matters.

---

## 4. PowerShell-specific breakages and fixes

### The three-layer quoting hell (current)

```
user writes:  Get-Service | Where-Object {$_.Status -eq 'Running'}
user invokes: bs shell winnode -x "powershell -c \"Get-Service | Where-Object {\$_.Status -eq 'Running'}\""
bs builds:    cmd.exe /c "powershell -c \"Get-Service | Where-Object {\$_.Status -eq 'Running'}\""
```
Three interpreters (bs C++ string → cmd.exe → powershell) each consume quotes.
`$` must be escaped from the *local* shell too. Any `'` or `"` inside the script
breaks a layer. This is fundamentally unfixable while scripts travel as strings.

### Fix 1 (structural — eliminates the problem): `run-script --interpreter powershell`

With `run-script`, the PowerShell code lives in a `.ps1` file. bs sends the file
binary, then runs:
```
powershell -ExecutionPolicy Bypass -File "C:\Users\...\received\diag.ps1" arg1
```
The `-File` flag means PowerShell reads the script from disk — **no string
parsing, no quote escaping, ever.** This is how PowerShell remoting's
`-FilePath` works and why it exists.

### Fix 2 (tactical — helps the existing `--cmd` path): interpreter-aware spawn

Replace the hardcoded `cmd.exe /c "…"` at `bs_session.hpp:418` with
interpreter detection:

```cpp
// bs_session.hpp:417 — replace the single cmdline build
std::string cmdline;
if (command.find_first_of("\n") != std::string::npos ||
    command.size() > 256) {
    // Multi-line or long: write to temp .ps1/.bat, exec the file.
    // Avoids cmd.exe quote-collapse entirely.
    write_temp_and_exec(command, /*ext=*/".ps1", /*interp=*/"powershell -File");
} else if (starts_with_ci(command, "powershell") ||
           starts_with_ci(command, "pwsh")) {
    cmdline = command;  // user already specified interpreter — don't wrap
} else {
    cmdline = "cmd.exe /c \"" + escape_cmd_quotes(command) + "\"";
}
```

`escape_cmd_quotes` would at minimum double internal `"` → `""` per cmd.exe
rules. But **Fix 1 is strictly better** — prefer it.

### Fix 3 (config): let `default_shell` honor `pwsh`

`bs_session.hpp:608` hardcodes `default_shell = "cmd.exe"`. Allow:
```ini
# ~/.bridgesessions/config
sessions.default_shell = "pwsh -NoLogo"
```
The config parser already reads this key (`bs_session.hpp:864`). The spawn path
just needs to not blindly wrap in `cmd.exe /c "…"` when the shell isn't cmd.
Detection: if `default_shell` starts with `pwsh`/`powershell`, use
`powershell -NoProfile -Command <command>` (single layer) instead of the
cmd.exe wrapping.

### PowerShell escaping cheatsheet (for the remaining `--cmd` cases)

| Char in script | Must escape for `cmd.exe /c` | Must escape for `powershell -c` |
|----------------|------------------------------|---------------------------------|
| `"` | `\"` (or `""`) | `\"` or `""` depending on quote style |
| `%` | `%%` (cmd var) | nothing |
| `^` | `^^` (cmd escape) | nothing |
| `$` | nothing (cmd) | `` ` `` (backtick) in double-quoted strings |
| `&` | `^&` (cmd) | nothing |
| `'` | nothing | `''` in single-quoted strings |

**The table itself is the argument for `run-script`:** no one should memorize
this. Files sidestep all of it.

---

## 5. Script reuse: a cached script library on nodes

### Problem

Today every invocation re-sends the script body (either as a fragile `--cmd`
string or, with `run-script`, as a file transfer). For frequently-run
diagnostics/cleanup scripts, this is wasteful and the "which version is on the
node?" question is unanswerable.

### Proposal: `bs script` subcommand — content-addressed script cache

```bash
# Register a script on this node (content-addressed by SHA256)
bs script add ./diag-nginx.sh --name diag-nginx
# → computes sha256, stores in ~/.bridgesessions/scripts/<sha256[:16]>/diag-nginx.sh
# → writes manifest: ~/.bridgesessions/scripts/manifest.json
#   { "diag-nginx": {"sha256": "...", "path": "...", "desc": "..."} }

# Push the library (or one script) to a peer — idempotent, skips if hash matches
bs script push linux-a diag-nginx
bs script push linux-a --all

# Run by name — if the peer already has that hash, NO transfer happens
bs run-script linux-a @diag-nginx -- --env prod
#                ^ @ prefix = resolve from manifest, not local filesystem

# List what's cached on a peer
bs script list linux-a

# Sync the whole library to a peer (one-shot fleet bootstrap)
bs script sync linux-a,linux-b,linux-5
```

### Storage layout on each node

```
~/.bridgesessions/
  scripts/
    manifest.json                 # name → {sha256, desc, mtime}
    <sha256[:16]>/
      diag-nginx.sh               # content-addressed body
    <sha256[:16]>/
      cleanup-disk.ps1
```

Content-addressing means:
- **Idempotent push:** if `diag-nginx` hash matches what the peer has, zero
  bytes transferred (peer reports its manifest hashes in the FileStart ack or a
  new `ScriptManifestMsg`).
- **Version pinning:** `@diag-nginx` always resolves to the manifest entry; pin
  a specific version with `@diag-nginx@<sha256[:16]>`.
- **No clobbering:** two different scripts with the same name but different
  content coexist under different hash dirs; the manifest's `name` field is just
  the friendly alias.

### Wire-level change (minimal)

Add one optional message (or reuse IPC like `file_send`):

```cpp
// bs_protocol.hpp — optional, enables skip-if-cached
struct ScriptManifestMsg {
    std::vector<std::pair<std::string, std::string>> name_to_hash; // name, sha256[:16]
};
```
Peer responds with its manifest; sender diffs and only `file_send`s the missing
hashes. Without this message, `script push` just always sends (still correct,
just not bandwidth-optimal).

### Execution path

`run-script @diag-nginx`:
1. Local: resolve `@diag-nginx` → sha256 from manifest.
2. Ask peer if it has `<sha256>` (via manifest query or cheap stat).
3. If no → `file_send` the body to `~/.bridgesessions/scripts/<hash>/`.
4. `run_command(peer, "<scripts_dir>/<hash>/diag-nginx.sh args…")`.
5. Return output.

No escaping at any step. The script can be arbitrarily complex PowerShell, bash,
Python — it's bytes on disk interpreted by its own shebang/`-File` flag.

---

## 6. Implementation priority

| Priority | Change | LOC | Impact |
|----------|--------|-----|--------|
| **P0** | `bs run-script <peer> <file>` (compose file_send + run_command) | ~80 | Eliminates escaping for all scripts today |
| **P0** | `run-script --interpreter powershell` → `-File` execution | (included in P0) | Fixes the Windows pain directly |
| **P1** | stdin/heredoc mode: `bs run-script peer - <<'EOF'` | ~20 | No temp file needed for one-offs |
| **P1** | Don't wrap in `cmd.exe /c "…"` when command is already multi-line or long (`bs_session.hpp:418`) | ~30 | Backstops the raw `--cmd` path |
| **P2** | `bs script add/push/list` + content-addressed cache | ~200 | Reusable library, idempotent fleet sync |
| **P2** | `ScriptManifestMsg` for skip-if-cached push | ~60 | Bandwidth optimization |
| **P3** | `default_shell = pwsh` honored in spawn (no cmd.exe wrap) | ~25 | Cleaner PS default on Windows nodes |

---

## OVERALL ASSESSMENT

The wire layer is **correct and escaping-safe** — `AttachMsg.command` is a
length-prefixed binary field, so the script body survives TLS intact. The
breakage is entirely at the **Windows spawn endpoint** (`bs_session.hpp:418`),
which wraps every command in `cmd.exe /c "…"` and forces PowerShell users into
triple-nested quoting (bs string → cmd.exe → powershell). The POSIX endpoint
(`execl("/bin/sh","-c",cmd)`) is fine.

**The user's instinct is right:** there is a better way, and bs already has the
primitives to build it. The highest-leverage fix is **`bs run-script`**, which
composes the existing `file_send` + `run_command` into a single command that
sends a script file and executes it by path. This needs **zero new wire
protocol** (~80 LOC) and eliminates escaping for any script of any complexity on
any platform, because the script body is never re-parsed by a shell — it's read
from disk by its own interpreter (`sh`, `powershell -File`, etc.).

For repeated scripts, a **content-addressed script cache** (`bs script add/push`)
makes fleet-wide script reuse idempotent and bandwidth-cheap, with friendly
names mapping to SHA256-pinned bodies. This is the same pattern ansible's
`script` module and PowerShell's `-FilePath` use, and it's proven.

Tactical backstop for the raw `--cmd` path: make the Windows spawn
interpreter-aware (don't wrap in `cmd.exe /c "…"` when the shell is `pwsh` or
the command is multi-line), but treat this as defense-in-depth — `run-script`
is the real fix.
