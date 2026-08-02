# BridgeSessions Identity/Binary Audit Report
## Incident: test-pc5 identity regeneration + stale binary downgrade

---

## FINDING 1 (CRITICAL): `cmd_keygen` overwrites identity unconditionally

**File:** `main.cpp:23-53`
**Symbol:** `cmd_keygen()`

`cmd_keygen` ALWAYS generates a new ed25519 keypair and ALWAYS writes to
`id_ed25519.pem` / `id_ed25519-cert.pem` / `id_ed25519.pub` — no existence check.

```
// main.cpp:32-41  (NO guard before writing)
auto [cert, key] = bs::mesh::generate_cert_key_pair("bridgesessions");
auto pubkey = bs::mesh::pubkey_hex_from_pem(key);
// ... directly writes to id_ed25519.pem, etc.
```

Contrast with `bootstrap_identity()` at **bs-protocol.h:1873-1890** which DOES check:

```
// bs-protocol.h:1884-1889
if (fs::exists(id_key)) {
    // restrict permissions, return — does NOT regenerate
    return;
}
```

**Trigger:** Running `bridgesessions keygen` on a machine with existing identity.
Every other CLI path calls `bootstrap_identity()` (idempotent), but `keygen`
subcommand goes through `cmd_keygen()` which unconditionally overwrites.

**Severity:** P0 — irreversibly breaks all pinned trust relationships.

**Prevention gate:**
```cpp
// Add to cmd_keygen(), before generate_cert_key_pair:
if (std::filesystem::exists(key_path)) {
    std::cerr << "Identity already exists: " << key_path << "\n"
              << "Use --force to regenerate (WARNING: breaks all peer pins)\n";
    return 1;
}
```

---

## FINDING 2 (CRITICAL): `update_peer` sends binary without platform validation

**File:** `bs-protocol.h:12301-12477`
**Symbol:** `MeshController::update_peer()`

The `bs update` command sends whatever binary is at `--bin` (or self-resolved path)
to the remote peer with NO validation that the binary matches the remote platform.

Remote platform is probed (`uname -s`, line 12346) and used only to:
- Reject non-POSIX targets (line 12352-12356)
- Choose `pkill -f ...` vs `systemctl --user stop` in the updater script

The updater script copies the binary to ALL discovered install targets
(bs-protocol.h:12417-12420) without checking ELF/Mach-O magic.

**Exploit scenario:** Running `bs update test-pc5` from an x86_64 Linux host sends
an ELF binary to an arm64 macOS target → lands as invalid Mach-O → daemon cannot
restart → identity survives but binary is corrupted.

**Prevention gate:**
```cpp
// In update_peer(), after probing OS (line 12348) and before staging:
// Validate binary format against target platform
bool validate_binary_platform(const std::string& bin_path, const std::string& target_os) {
    std::ifstream f(bin_path, std::ios::binary);
    unsigned char magic[4];
    f.read(reinterpret_cast<char*>(magic), 4);
    if (target_os == "Linux")   return magic[0]==0x7f && magic[1]=='E' && magic[2]=='L' && magic[3]=='F';
    if (target_os == "Darwin")  return (magic[0]==0xcf && magic[1]==0xfa)  // Mach-O 64
                                    || (magic[0]==0xfe && magic[1]==0xed)   // Mach-O (fat)
                                    || (magic[0]==0xca && magic[1]==0xfe);  // Mach-O 64 universal
    return false;
}
```

Also validate `dist/` binaries at release time (see Finding 5).

---

## FINDING 3 (MEDIUM): `install.sh` no binary format validation after download

**File:** `scripts/install.sh:45-49`

Downloads binary, `chmod +x`, runs `--version`. No check that the downloaded file
is actually an executable of the correct format. A network error delivering an
HTML error page, a truncated download, or a wrong-platform binary from a
mislabelled release all pass through silently until `--version` fails.

**Prevention gate:**
```bash
# After curl download (line 47):
if ! file "${INSTALL_DIR}/${BIN_NAME}" | grep -qE '(ELF|Mach-O)'; then
    echo "ERROR: Downloaded binary is not a valid executable" >&2
    file "${INSTALL_DIR}/${BIN_NAME}" >&2
    exit 1
fi
```

---

## FINDING 4 (MEDIUM): `install.sh` maps all Linux→x86_64, all Darwin→arm64

**File:** `scripts/install.sh:21-36`

No `arch` handling: `Linux` always downloads `bridgesessions-linux-x86_64`,
`Darwin` always downloads `bridgesessions-macos-arm64`.

**Prevention gate:**
```bash
case "${os}-${arch}" in
  Linux-x86_64)     BIN="bridgesessions-linux-x86_64" ;;
  Darwin-arm64)     BIN="bridgesessions-macos-arm64" ;;
  Darwin-x86_64)    echo "macOS x86_64 not yet supported" >&2; exit 1 ;;
  Linux-aarch64)    echo "Linux aarch64 not yet supported" >&2; exit 1 ;;
  *)                echo "Unsupported: ${os}-${arch}" >&2; exit 1 ;;
esac
```

---

## FINDING 5 (LOW): No release-pipeline binary format verification

**File:** `tests/test_release.py` — tests packaging/checksums/source archives
but never validates that `dist/bridgesessions-linux-x86_64` is ELF,
`dist/bridgesessions-macos-arm64` is Mach-O, `dist/bridgesessions-windows-x86_64.exe` is PE.

The v2.0.14 static-linux regression (CHANGELOG.md:161-163: "Linux dist binary is
properly static again — the 2.0.10-alpha5 artifact was a dynamically-linked host
build") would have been caught by a `file` + `ldd` check in CI.

**Current actual binaries in dist/:**
- `bridgesessions-linux-x86_64`: ELF 64-bit, dynamically linked (interpreter `/lib64/ld-linux-x86-64.so.2`) — still not fully static
- `bridgesessions-macos-arm64`: Mach-O 64-bit arm64 — correct
- `bridgesessions-windows-x86_64.exe`: PE32+ — correct

**Prevention gate:** Add to `tests/test_release.py`:
```python
def test_binary_formats_match_expected_platforms():
    import struct
    checks = {
        "bridgesessions-linux-x86_64": b'\x7fELF',
        "bridgesessions-macos-arm64": b'\xcf\xfa\xed\xfe',  # Mach-O 64
        "bridgesessions-windows-x86_64.exe": b'MZ',
    }
    for name, magic in checks.items():
        path = REPO_ROOT / "dist" / name
        if path.exists():
            assert path.read_bytes()[:len(magic)] == magic, f"{name}: wrong format"
```

---

## FINDING 6 (TEST GAP): No `cmd_keygen` idempotency test

**File:** `tests/test_identity.cpp` tests `bootstrap_identity` idempotency
(lines 63-102) and legacy migration (lines 125-165), but has ZERO coverage for
`cmd_keygen()`. The function lives in `main.cpp` behind `#ifndef BS_TESTING`,
making it untestable in the current harness.

**Prevention:** Extract `cmd_keygen` into a testable free function in
`bs-protocol.h` or add integration test that invokes `bridgesessions keygen`
twice and verifies pubkey does not change.

---

## FINDING 7 (TEST GAP): No binary platform validation in update path

No test verifies that `update_peer` refuses to send an ELF binary to a Darwin
target. The updater script's binary-swap path (running `mv -f`) has no test
validating it survives interruption without leaving a corrupted target binary.

---

## Summary of Prevention Gates (priority order)

| # | Gate | Location | Effort |
|---|------|----------|--------|
| 1 | `cmd_keygen`: refuse overwrite w/o `--force` | `main.cpp:32` | 3 lines |
| 2 | `update_peer`: validate binary format vs target OS | `bs-protocol.h:12347` | 20 lines |
| 3 | `install.sh`: validate downloaded binary with `file` | `scripts/install.sh:47` | 5 lines |
| 4 | `install.sh`: reject unsupported arch combos | `scripts/install.sh:21` | 10 lines |
| 5 | Release pipeline: verify `dist/` binary platform formats | `tests/test_release.py` | 15 lines |
| 6 | Test: `cmd_keygen` idempotency | `tests/test_identity.cpp` | 20 lines |
| 7 | Test: `update_peer` cross-platform refusal | new test file | 40 lines |

## Recommendation

Apply gates 1-5 immediately. The `cmd_keygen` fix (gate 1) is the direct
root-cause fix for identity regeneration. The `update_peer` fix (gate 2) is the
direct root-cause fix for cross-platform binary corruption/downgrade.
