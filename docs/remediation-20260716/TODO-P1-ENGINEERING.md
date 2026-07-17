# TODO — P1 Engineering Risks (Review 2026-07-16)

**Status:** OPEN  
**Depends on:** Prefer completing P0 security before large architecture churn; config-dir and CMake can proceed in parallel carefully.

---

## P1-1 — Two implementations (monolith vs modular)

**Finding:** Root CMake builds monolith `bridgesessions` only. Modular `bs-client`/`bs-server`/`bs-protocol`/`bs-transport` diverge (version 0.5.0, port 9943, different TOFU/TLS). Dual stacks multiply audit surface.

### Tasks
- [ ] **P1-1a:** Record DECISION: **Option A** monolith is production (recommended short-term) **or** **Option B** modular is future sole build
- [ ] **P1-1b:** If A: move modular tree to `_archive/modular-0.5/` or `experimental/`; README “do not ship”; stop advertising modular TLS as product TLS
- [ ] **P1-1c:** If B: root CMake `add_subdirectory` modular only; delete or freeze monolith with sunset date
- [ ] **P1-1d:** Single version string generator shared by CLI `--version`, doctor, CMake `project(VERSION …)`
- [ ] **P1-1e:** CI builds only the canonical binary; fail if second binary appears in `dist/` without provenance note

**Done when:** One buildable security-sensitive protocol stack; other is clearly non-shipping.

---

## P1-2 — Build documentation ≠ CMake reality

**Finding:** Docs claim `bs-server`/`bs-client` libs; root produces monolith + tests. Catch2 required even for release binary; missing deps in docs; no in-repo CI despite sanitizer claims.

### Tasks
- [ ] **P1-2a:** Rewrite `docs/building.md` / README build section to match root `CMakeLists.txt` outputs
- [ ] **P1-2b:** Add `BUILD_TESTS` (or `BS_BUILD_TESTS`) option; default ON in CI, OFF for minimal release
- [ ] **P1-2c:** Document required packages: OpenSSL, zstd, CLI11, nlohmann-json, Catch2 (when tests on), platform notes
- [ ] **P1-2d:** Windows: configure **and** build presets; one documented path
- [ ] **P1-2e:** Add GitHub/Codeberg CI: Linux + macOS + Windows matrix — configure → build → ctest → package
- [ ] **P1-2f:** Wire sanitizers (ASan/UBSan) and protocol fuzz jobs **or** remove “every PR” claims from docs
- [ ] **P1-2g:** Script `scripts/release-build.sh` that prints exact commands used for `dist/` artifacts

**Done when:** Fresh clone + docs-only instructions produce same binary class as CI; CI green on main.

---

## P1-3 — `--config-dir` not authoritative

**Finding:** Option sets some home override but many paths still hardcode `~/.bridgesessions` (doctor, identity, keys, mesh, shell).

**Evidence:** `--config-dir` option ~L8996–9123; residual `resolve_home("~/.bridgesessions")` ~L8928 and similar.

### Tasks
- [ ] **P1-3a:** After CLI parse, compute single `AppPaths { home, config, keys, logs, received, state }`
- [ ] **P1-3b:** Thread `AppPaths` (or absolute paths) into MeshController, doctor, keygen, authorized_keys, receive_dir, persistence
- [ ] **P1-3c:** Ban new `resolve_home("~/.bridgesessions")` call sites; grep gate in CI
- [ ] **P1-3d:** Isolation test: `TMPDIR` + `--config-dir $tmp` → `doctor` + `keygen` + daemon start; assert **zero** open/read/write under real `$HOME/.bridgesessions` (strace/proc or file mtime probe)
- [ ] **P1-3e:** Windows doctor regression: `--config-dir C:\tmp\bs-audit-home` must not touch `%USERPROFILE%\.bridgesessions`

**Done when:** Isolation test green on Linux + Windows.

---

## P1-4 — BridgePanel web hardening

**Finding:** Strong base (sanitization, CSP, path checks) but: body read before size reject; trusted-IP may bypass token on writes; IPC socket ResourceWarnings in tests.

**Evidence:** `tools/bridgepanel/bridgepanel.py` ~L1331–1332 `raw_body = self.rfile.read(length)` after Content-Length parse; trusted_ips ~L1267+.

### Tasks
- [ ] **P1-4a:** Reject `Content-Length > MAX_UPLOAD` **before** `rfile.read`; reject missing/negative length
- [ ] **P1-4b:** Socket read timeout on request body
- [ ] **P1-4c:** Writes (PUT/POST/PATCH/DELETE): always require token (or separate write token) even if IP trusted; validate Origin/Referer or CSRF token; require `Content-Type` allowlist for JSON/multipart
- [ ] **P1-4d:** IPC socket: `with` / `finally: close()`; fix ResourceWarnings in pytest
- [ ] **P1-4e:** Add tests: oversized CL rejected; trusted IP without token cannot write; socket closed after query

**Done when:** BridgePanel pytest green + new security tests; no ResourceWarnings.

---

## P1 exit criteria
- [ ] Architecture decision recorded and tree layout matches it
- [ ] CI builds canonical product on 3 OS
- [ ] `--config-dir` isolation test green
- [ ] BridgePanel write-auth + body-limit tests green
