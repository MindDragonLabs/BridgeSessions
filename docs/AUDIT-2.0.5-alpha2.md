# BridgeSessions 2.0.5-alpha2 full-codebase audit

**Audit date:** 2026-07-17  
**Base commit:** `b2742f5bfdd6`  
**Audit branch:** `audit/public-2.0.5-alpha2`  
**Canonical release implementation:** `bridgesessions.cpp` + root `CMakeLists.txt`

## Verdict

- **GO:** Linux x86_64, Windows x86_64, and macOS arm64 public **alpha2**
  artifacts built from this source (after commit/tag).
- **Not claimed:** production-secure SSH replacement; signed supply chain;
  macOS fully static binary without Homebrew runtime deps.
- **Not performed:** push/signing/public forge publication (operator step).

This is an alpha security/reliability candidate, not a production-readiness
claim.

## Scope and coverage

The working-tree inventory contained **156 files**, including **139 text files**,
**10 binary files**, and **33,222 text lines**. Coverage included:

- canonical C++ implementation and every discovered Catch2 test;
- tracked modular/legacy C++ trees;
- Python, shell, CMake, services, packaging, AI-harness integration, docs, and
  release artifacts;
- protocol framing, TLS identity, peer naming, transfer shape/integrity,
  config-path isolation, IPC, file permissions, and public-source hygiene.

Five bounded, read-only line-review lanes covered the monolith, modular code,
tests, and non-C++/release files. Codex CLI could not run because its native
ChatGPT token refresh returned HTTP 401; authenticated Kimi CLI was used as the
fallback. Every high-severity model finding was independently checked against
source or a runnable repro. Model-only assertions were not accepted as facts.

The tracked modular implementation (`bs-*`, `protocol`, and `transport`) is not
built by the root CMake project and contains stale security assumptions. It is
preserved in git but marked non-shipping in `LEGACY_CODE.md` and excluded from
alpha2 source archives via `.gitattributes`.

## Confirmed defects fixed

| Severity | Defect | Evidence / fix |
|---|---|---|
| P0 | Decoder length overflow | `Decoder::ok(SIZE_MAX)` returned true because `p + need` wrapped, creating an out-of-bounds-read primitive for hostile length fields. Reproduced under test; fixed with remaining-length subtraction in `bridgesessions.cpp`. |
| P0 | Direct CLI accepted unpinned peers | Daemon mesh dialing required pins, but direct `shell`/`health`/session paths allowed an empty expected key. Direct connects now reject before DNS/TCP. |
| P0 | Inbound peer-name impersonation | Inbound Hello accepted an empty key and did not reject configured name/key collisions. Added symmetric certificate ↔ Hello key ↔ configured name validation. |
| P0 | Transfer size-limit bypass | Receivers trusted declared `filesize` while accepting arbitrary chunk counts and decompressed byte totals. Added overflow-safe canonical metadata and exact chunk/byte validation. |
| P1 | Cross-peer receive-state corruption | One global `FileReceiveState` was shared by all connections. State now belongs to each `Conn`, including per-connection partial cleanup. |
| P1 | Corrupt file published before hash check | Receiver renamed `.part` to the final filename before verifying SHA-256. It now hashes the partial first and removes corrupt/incomplete files. |
| P1 | Broken transfer collision/resume behavior | Concurrent same-name transfers could target the same partial file, and partial-resume negotiation was not implemented end to end. Alpha2 reserves a unique final/partial path and does not claim resume support. |
| P1 | Explicit config reload watched the wrong file | `--config /custom/path` loaded once but reload watched `<app_home>/config`. `MeshConfig` now carries source provenance and the constructor accepts an explicit override. |
| P1 | TLS verification callback leaks | Null-storage `create_node_tls` allocations leaked in sanitizer runs. OpenSSL `SSL_CTX` ex-data now owns and destroys fallback callback state. |
| P1 | Malformed authorized keys accepted as zero bytes | Invalid hex nibbles decoded as zero. `hex_decode` now rejects odd-length and non-hex input; `authorize` validates and normalizes exactly 32-byte Ed25519 keys. |
| P1 | Private-key permission race | Key/cert/authorization files were created through `ofstream` and only sometimes chmod'd afterward; legacy migration copied source modes. Secure open/write/append helpers enforce `0600` from first byte and app homes use `0700`. |
| P1 | Remote filename reached `system()` | `edit` concatenated the downloaded path into a shell command. Editor launch now passes the path as a dedicated argv element on POSIX and Windows. |
| P1 | TCP timeout was not portable | Direct dialing relied on `SO_SNDTIMEO`, which does not bound `connect()` on macOS. Both mesh and direct paths now share nonblocking connect + `select` + `SO_ERROR`. |
| P1 | Config-dir logging leak | Logger initialization used `$HOME/.bridgesessions` or `/tmp`, independent of `--config-dir`. Logger state is now app-home-bound and reconfigurable. |
| P1 | Quadratic progress replay | Long `file send --wait` calls reprocessed the entire IPC accumulator after every recv, replaying every previous `PROGRESS` line. Incremental parsing now emits each line once and retains only a trailing partial line. |
| P1 | Release artifacts were mislabeled/stale | macOS/Windows binaries were older builds, Linux metadata reported 2.0.6, Hello advertised 1.0.0, and BridgePanel reported 2.0.0. A canonical `VERSION` file now drives CMake, CLI, Hello, BridgePanel, scripts, and artifact gates. |
| P1 | Checksum/SBOM generator duplicated and emitted invalid JSON | Overlapping Windows globs duplicated entries and hand-written comma handling could produce malformed CycloneDX JSON. Replaced with deduplicated artifact discovery and Python JSON generation. |
| P2 | BridgePanel IPC socket leak | Connection failures skipped `close()`. Added unconditional cleanup and ResourceWarnings-as-errors verification. |
| P2 | Host-specific clang-tidy config | The checked-in config referenced one macOS home/sysroot and failed on Linux. It is now repository/host independent. |
| P2 | Public configuration docs used ignored keys | Docs used `mesh.node_name` / `mesh.listen`; the parser expects `node.name` / `node.listen`. Examples and seed-pin syntax were corrected. |
| P2 | Public source archive leaked non-shipping/operator material | Added deterministic export policy, generic config, secret-like path guard, legacy quarantine, pycache exclusion, and archive content scans. |

## Findings rejected after verification

- **“Remote command execution is shell injection.”** Rejected as stated: execution
  of commands from an already-authorized peer is the core product capability.
  The separate local-editor filename injection was real and fixed.
- **“GIF magic predicate is always true.”** Rejected; operator precedence yields
  two valid GIF signature alternatives.
- **“Busy flag leaks on early send failure.”** Rejected; `BusyGuard` already
  clears `exec_busy` on every return.
- **“BridgePanel has unlimited POST bodies.”** Rejected; the active handler has a
  10 MiB cap and rejects oversized bodies before reading.
- **“TLS is 1.3-only in the canonical binary.”** Rejected for the shipping tree;
  canonical policy is TLS 1.2 minimum with TLS 1.3 preferred. The stale modular
  tree did contain 1.3-only code and is excluded.

## Verification evidence

- CMake RelWithDebInfo build: warning-free.
- CTest: **237/237 passed** (parallel release build).
- ASan + UBSan + leak detection: **237/237 passed** serially.
- Parallel sanitizer anomaly: one TLS mesh test failed once; it passed **5/5** in
  isolation and the full serial sanitizer suite passed. Classified as test
  interference, not a release defect.
- Targeted TSan: ring buffer and two-node Hello exchange passed.
- clang-tidy (`clang-analyzer-*`): clean.
- Clang scan-build: clean.
- BridgePanel: **13/13** HTTP tests passed with `ResourceWarning` promoted to
  errors; Ruff, Bandit, compileall passed.
- Shell: ShellCheck and `bash -n` passed for public scripts.
- systemd unit: `systemd-analyze verify` passed.
- Direct `build.sh`: produced `bridgesessions 2.0.5-alpha2`.
- Permission integration: app home `0700`; key, cert, public key, and
  `authorized_keys` `0600`; default home untouched under `--config-dir`.
- Transfer soak: **524,288,000 bytes** of incompressible data sent through two
  real pinned localhost daemons and verified byte-identical by SHA-256.
- Post-progress-fix transfer: **31,457,280 bytes**, 3 progress records / 3
  unique records, final SHA-256 and byte comparison matched, no `.part` residue.
- Artifacts: Linux binary and deterministic source archive pass `SHA256SUMS`.
- CycloneDX 1.5 binary artifact manifest validates against the official schema.
- Extracted source archive scan: no private keys, secret-like assignments,
  private user/home paths, host-specific fleet configs, pycache, or legacy
  modular directories.

## Release contents

- `dist/bridgesessions-linux-x86_64`
- `dist/bridgesessions-windows-x86_64.exe`
- `dist/bridgesessions-macos-arm64`
- `dist/bridgesessions-2.0.5-alpha2-source.tar.gz`
- `dist/SHA256SUMS`
- `dist/SBOM-binaries.json`

## Residual risks / explicit blockers

1. **macOS binary links Homebrew dylibs** (OpenSSL/zstd/fmt/spdlog). Recipients need matching Homebrew deps or should rebuild locally.
2. **Windows PE is MinGW-static cross-compile** — smoke-tested for version string and PE validity; full Windows integration suite remains operator soak.
3. **No full source dependency SBOM.** `syft` optional. Included CycloneDX covers release artifacts only.
4. **No release signing.** Checksums exist; GPG/minisign/cosign remains an operator step.
5. **Legacy modular code remains in git.** Non-shipping and export-ignored; not security-supported.
6. **TSan scope is targeted.** Full TSan not claimed for fork/PTY-heavy tests.
7. **Public alpha, not production SSH replacement.** See SECURITY.md.
