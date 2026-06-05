# bridgesessions 3-Machine Cross-Relay Test Plan

**Goal:** verify the `bs-server` (Windows native, MSVC) and `bs-client` (Windows native, MSVC) talk to each other AND to the Linux `bs-server`/`bs-client` on **linux-a** and **linux-b**, in all 4 directions.

**Date:** 2026-06-01
**Session:** 20260601_052631_2100cc
**Build state (this session):** ConPTY pipe leak fixed, HPCON plumbing added, TOFU case-insensitive, spdlog flush_on, **authorized_keys inline-comment parser fix** (see Bugs section). Verified locally with 5/5 real E2E cycles on Windows bs-server.

---

## RESULT: 4/4 tests PASSED ✅

| # | Direction | Result | Marker |
|---|---|---|---|
| 1 | here→linux-a (Windows client → Linux server) | ✅ | `WINDOWS_TO_FECV3_OK_8c4d` |
| 2 | here→linux-b (Windows client → Linux server) | ✅ | `WINDOWS_TO_FECV4_OK_2a91` |
| 3 | linux-a→here (Linux client → Windows server) | ✅ | `FECV3_TO_HERE_OK_77b3` |
| 4 | linux-b→here (Linux client → Windows server) | ✅ | `FECV4_TO_HERE_OK_91d6` |

---

## Bugs found and fixed during testing

### Bug A — Stale bs-server process on linux-a (Phase 1)
**Symptom:** Windows bs-client → linux-a bs-server: TLS handshake completes, attach prints, then server disconnects immediately. Strace on the server showed the server `close()`d the socket right after `SSL_accept()` returned. No log events.

**Cause:** The bs-server on linux-a was an old process (PID 580078, started Jun 1 02:57, ~12 hours old) — possibly with stale `SSL_SESS_CACHE_SERVER` state from before my pubkey was added to its authorized_keys. Restarting the server (now PID 635039) fixed it instantly. No code change needed.

**Lesson:** bs-server reads `authorized_keys` at startup. Changes to that file require a server restart to take effect. **There is no SIGHUP-style reload** — restart is the only way.

### Bug B — authorized_keys parser silently drops keys with inline comments (Phase 1.5)
**Symptom:** Inbound tests (linux-a→here, linux-b→here) failed: server logged `ssl_accept failed: err=1 msg=error:0A000086:SSL routines::certificate verify failed`. Loopback tests (here→here) worked fine.

**Root cause:** The `AuthorizedKeys::load_from_file()` in `bs-transport/src/tls.cpp`:
```cpp
while (std::getline(f, line)) {
    line.erase(line.find_last_not_of(" \t\r\n") + 1);
    if (!line.empty() && line[0] != '#') {       // ← ONLY skips LINES that start with '#'
        auto raw = hex_decode(line);
        if (raw.size() == 32) keys.push_back(std::move(raw));
    }
}
```
It skips **lines starting with `#`** but doesn't strip **inline comments** like `key # comment`. So when my file had:
```
4bf5449c5050ae6291a99af2294269bdac09da941a646e62e80188a65e89a40f  # from linux-a
```
…the whole 79-char line was fed to `hex_decode`. It produced 39 bytes (78 hex chars / 2). The `raw.size() == 32` check **silently failed** and the key was dropped. The client was then rejected at TLS verify time with the confusing "certificate verify failed" error.

**Fix:** strip everything from the first `#` to end-of-line before hex_decode:
```cpp
while (std::getline(f, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    line.erase(line.find_last_not_of(" \t\r\n") + 1);
    if (!line.empty()) {
        auto raw = hex_decode(line);
        if (raw.size() == 32) keys.push_back(std::move(raw));
    }
}
```

**Lesson:** Always strip inline comments in INI-like key files. Also, **silently dropping malformed entries is a debugging nightmare** — log a warning when hex_decode returns something that isn't 32 bytes.

---

## 1. Topology & identity

```
┌─────────── Shadow (Windows 11) ───────────┐
│   IP tailscale: 100.124.169.66            │
│   bs-server:   listen=:19948 (TBD)        │
│   bs-client:   ~/.bridgesessions/         │
│   pubkey:      e702d6ad10e1891f4fc2a776…  │
│   authorized_keys: must include linux-a &   │
│                    linux-b pubkeys           │
└────────────────────────────────────────────┘
            ▲           │             ▲
            │           │             │
   linux-a→here│           │here→linux-a   │here→linux-b
            │           ▼             ▼
┌──────── linux-a (Arch, Tailscale 203.0.113.11) ──────┐
│   bs-server running: :9948 (PID 580078)             │
│   bs-client available: /root/bridgesessions/build/  │
│   pubkey:      4bf5449c5050ae6291a99af229…          │
│   authorized_keys: includes my key (just added)    │
└─────────────────────────────────────────────────────┘
            ▲
            │linux-a can SSH to linux-b with same key
            │id_ed25519_shadow_to_linux
            ▼
┌──────── linux-b (Arch, Tailscale 203.0.113.12) ───────┐
│   bridgesessions: NOT INSTALLED                     │
│   pubkey:        (will be generated)                │
│   authorized_keys: (will include my key)            │
└─────────────────────────────────────────────────────┘
```

**Identity rule:** ed25519 pubkey in the server's `authorized_keys` file is the only thing that gates bs-client → bs-server. The bs-server does NOT check the TLS cert against any CA — only pubkey match.

---

## 2. Phases

### Phase 0 — Pre-flight (verify current state)

| Check | Command | Expected |
|---|---|---|
| My pubkey on linux-a | `ssh … linux-a 'grep e702d6ad /root/.bridgesessions/authorized_keys'` | found |
| bs-server on linux-a | `ssh … linux-a 'ss -tlnp \| grep 9948'` | LISTEN pid 580078 |
| No bs-server here | `tasklist \| grep bs-server` | empty (just killed PID 7100) |
| My key in my own authorized_keys (loopback test) | `cat ~/.bridgesessions/authorized_keys` | includes my pubkey |
| Local loopback works | `bs-client --server=127.0.0.1:<port> …` | marker+whoami returned |

### Phase 1 — Investigate the "server disconnected" bug

**Symptom seen this session:** Windows bs-client → linux-a bs-server: TLS completes, attach prints, then server disconnects immediately. No command flow.

**Not yet diagnosed.** Candidates:

- [ ] **(a)** My pubkey addition didn't take effect — server uses stale in-memory copy
- [ ] **(b)** Recent ConPTY/TOFU changes broke the wire format
- [ ] **(c)** OpenSSL TLS 1.3 cipher mismatch between vcpkg and Linux system OpenSSL
- [ ] **(d)** Some message I send post-attach is malformed (Ping from keepalive thread?)

**Debug plan:**
1. Run with `--server=203.0.113.11:9948 --name=diag` and capture both stdout AND stderr
2. Enable bs-server's flush_on by upgrading the server binary (or run a new copy on a different port with the upgraded binary)
3. Check linux-a's `bs-server.log` immediately after a failed connect (it should at least record "attach" or "close")
4. Use `strace -e network -p 580078` on linux-a while connecting (one-shot, then kill strace) to see exactly which packet the server hangs up on
5. Test with the local Linux bs-client from linux-a → its own server (already works) — confirms server is fine; the problem is my Windows client

**Decision point:** if it's a wire format / TLS issue, I need to either rebuild the Windows client from a known-good source, or use the Linux bs-client from Windows (via WSL — REJECTED per user prefs) or rebuild the server to capture more info.

### Phase 2 — Install bridgesessions on linux-b

```bash
# 1. scp source tarball
scp /c/SFTP/agent/bridgesessions/bridgesessions-src.tar.gz root@linux-b:/tmp/
# 2. on linux-b:
cd /tmp && tar xzf bridgesessions-src.tar.gz
cd bridgesessions
./install.sh        # builds + installs
# 3. start bs-server on linux-b
./build/release/bs-server/bs-server --listen=:19948 \
  --cert=/root/.bridgesessions/id_ed25519-cert.pem \
  --key=/root/.bridgesessions/id_ed25519.pem \
  --auth=/root/.bridgesessions/authorized_keys &
# 4. get linux-b's pubkey
cat /root/.bridgesessions/id_ed25519.pub
# 5. add my pubkey to linux-b authorized_keys
echo "e702d6ad10e1891f4fc2a7764df40eef5e3e459b971654996353bc34f8e936bc" \
  >> /root/.bridgesessions/authorized_keys
```

**Required user input:** the user said "get linux-b key from there" — does this mean:
- (a) get linux-b's own bridgesessions pubkey from linux-b after install, OR
- (b) get a key that *linux-b will use* from somewhere else (linux-a)?

I'll go with (a) — install on linux-b, generate its identity there, then add my key to its authorized_keys (and vice versa).

### Phase 3 — Add inbound pubkeys to MY authorized_keys

For tests #5 (linux-a→here) and #6 (linux-b→here) to work, the Windows bs-server must accept their pubkeys:

```bash
# fetches and appends in one shot
ssh … linux-a 'cat /root/.bridgesessions/id_ed25519.pub' \
  >> /c/Users/Shadow/.bridgesessions/authorized_keys
ssh … linux-b 'cat /root/.bridgesessions/id_ed25519.pub' \
  >> /c/Users/Shadow/.bridgesessions/authorized_keys
```

**Important:** user prefs say "reusing existing SSH keys instead of generating new keys during remote access setup" — but bs-server identity (ed25519) is a separate concern from SSH identity. The bridgesessions identity on each machine should remain its own; the SSH key is just how I bootstrap the box.

### Phase 4 — Start bs-server on Windows for inbound

```bat
bs-server.exe --listen 0.0.0.0:19948 ^
  --cert C:/Users/Shadow/.bridgesessions/_bs_autocert.pem ^
  --key  C:/Users/Shadow/.bridgesessions/_bs_autokey.pem ^
  --auth C:/Users/Shadow/.bridgesessions/authorized_keys
```

Bind 0.0.0.0 (not 127.0.0.1) so Tailscale-reachable clients can connect.

**Verify:** `netstat -ano | grep 19948` should show `0.0.0.0:19948 LISTENING`.

### Phase 5 — The 6 test directions

For each test, the script:
1. starts a bs-client
2. waits 3s for TLS + attach
3. sends `echo <UNIQUE_MARKER> && uname -n && whoami`
4. waits 2s for response
5. sends `exit`
6. captures stdout
7. asserts the marker and the host-specific response are present

| # | Direction | What to verify |
|---|---|---|
| 1 | here→linux-a | marker present, "FECv3" present, "root" present |
| 2 | here→linux-b | marker present, "linux-b" present, "root" present |
| 3 | linux-a→here | marker present, "Shadow" present (Windows whoami), bs-server.log shows the event |
| 4 | linux-b→here | marker present, "Shadow" present, bs-server.log shows the event |
| 5 | linux-a→linux-b (BONUS) | verifies cross-Linux routing, not requested but cheap |
| 6 | linux-b→linux-a (BONUS) | same |

Tests 5-6 aren't in the user's request but prove the bs-server is doing the right thing on both Linux boxes.

### Phase 6 — Cleanup

- Kill bs-server on Windows
- Leave linux-a/linux-b servers running (or stop if user prefers)
- No new files to remove on this side (no temp scripts this time)

---

## 3. Risks & decisions

| Risk | Mitigation |
|---|---|
| "server disconnected" bug blocks all tests | Phase 1 must succeed first. If wire format, may need to revert one of my recent client changes. |
| scp blocked to linux-b | Use `ssh … 'cat > /tmp/file'` if scp is disabled (matches what worked above) |
| fevc4 install fails (missing deps) | `install.sh` handles pacman — should be fine on Arch |
| bs-server on Windows firewall | Already verified reachable from linux-a in prior session. Tailscale is in trusted zone. |
| User wants tests 1-4 only, not 5-6 | Skip them, they were never requested. |
| User "STOP" mid-test | User rule: "STOP immediately when told to stop" — pause all tool calls, do not chain |

---

## 4. Success criteria

- [ ] Phase 0 baseline confirmed
- [ ] Phase 1 root cause identified and fixed (or worked around)
- [ ] linux-b has bs-server running with my key in authorized_keys
- [ ] My authorized_keys has linux-a + linux-b pubkeys
- [ ] Windows bs-server bound on 0.0.0.0:19948
- [ ] Test 1 (here→linux-a): marker + uname + whoami all present in output
- [ ] Test 2 (here→linux-b): marker + uname + whoami all present
- [ ] Test 3 (linux-a→here): marker + Windows whoami present
- [ ] Test 4 (linux-b→here): marker + Windows whoami present
- [ ] bs-server.log on Windows shows 2 attach events from the inbound tests
- [ ] bs-server.log on linux-a/linux-b shows 1 attach event each from outbound

---

## 5. Execution order

1. **Phase 1 first** — without solving the "server disconnected" bug, tests 1 and 2 are dead in the water. Tests 3 and 4 (inbound) are independent of the bug because the server side is bs-server (which I just fixed locally) and the client is the existing Linux bs-client.
2. **Phase 2 (install linux-b) can run in parallel with Phase 1** — they're independent.
3. **Phase 3 (add pubkeys) depends on Phase 2** (need linux-b's pubkey first).
4. **Phase 4 (start my server) depends on Phase 3**.
5. **Phase 5 (6 tests) depends on Phases 1, 2, 3, 4**.
