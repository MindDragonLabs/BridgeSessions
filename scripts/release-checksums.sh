#!/usr/bin/env bash
# Validate release artifacts, write SHA256SUMS, and generate a CycloneDX 1.5
# SBOM.  Does not publish or sign anything.
#
# SHA256SUMS covers every dist artifact except itself and SBOM-binaries.json.
# The SBOM is generated afterwards and its hash is appended to SHA256SUMS.
# The SBOM never lists SHA256SUMS or SBOM-binaries.json.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="${BS_RELEASE_DIR:-$ROOT/dist}"
VERSION="$(tr -d '\r\n' < "$ROOT/VERSION")"
cd "$DIST"
shopt -s nullglob

candidates=(bridgesessions bridgesessions-* bs_tray.ps1)
files=()
for file in "${candidates[@]}"; do
  [[ -f "$file" ]] || continue
  case "$file" in
    SHA256SUMS|*.json) continue ;;
  esac
  files+=("$file")
done

if [[ ${#files[@]} -eq 0 ]]; then
  printf 'no release binaries in %s\n' "$DIST" >&2
  exit 1
fi

for file in "${files[@]}"; do
  # The PowerShell tray companion is data/code shipped with the binary release,
  # but its source is intentionally not version-stamped. It is still covered
  # by SHA256SUMS and the SBOM below.
  [[ "$file" == "bs_tray.ps1" ]] && continue
  detected=""
  if [[ "$file" == *-source.tar.gz || "$file" == *-source.zip ]]; then
    # Read exactly the expected archive member, reject duplicates/missing
    # entries, and compare its complete contents to VERSION (not strings in
    # the archive which may come from unrelated files).
    detected=$(python3 - "$file" "$VERSION" <<'PY'
import sys, tarfile, zipfile
path, version = sys.argv[1:]
member = f"bridgesessions-{version}/VERSION"
try:
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
            if names.count(member) != 1:
                raise ValueError("expected exactly one VERSION member")
            value = archive.read(member).decode("ascii")
    else:
        with tarfile.open(path, "r:gz") as archive:
            members = [m for m in archive.getmembers() if m.name == member]
            if len(members) != 1 or not members[0].isfile():
                raise ValueError("expected exactly one regular VERSION member")
            value = archive.extractfile(members[0]).read().decode("ascii")
    if value != version + "\n":
        raise ValueError("VERSION contents do not exactly match expected release version")
    print(version)
except Exception as exc:
    print(f"invalid source archive: {exc}", file=sys.stderr)
    sys.exit(1)
PY
    )
  else
    description=$(file -b "$file")
    if [[ "$file" == bridgesessions-linux-x86_64 ]]; then
      [[ "$description" == ELF\ 64-bit\ LSB*x86-64* || "$description" == ELF\ 64-bit\ LSB*X86-64* ]] || {
        printf 'unexpected artifact format: %s: %s\n' "$file" "$description" >&2; exit 1;
      }
    elif [[ "$file" == bridgesessions-macos-arm64 ]]; then
      [[ "$description" == Mach-O\ 64-bit*arm64* ]] || {
        printf 'unexpected artifact format: %s: %s\n' "$file" "$description" >&2; exit 1;
      }
    elif [[ "$file" == bridgesessions-windows-x86_64.exe ]]; then
      [[ "$description" == PE32+*x86-64* ]] || {
        printf 'unexpected artifact format: %s: %s\n' "$file" "$description" >&2; exit 1;
      }
    else
      printf 'unrecognized release binary name: %s\n' "$file" >&2; exit 1
    fi
    # Execute only the native Linux artifact. Cross-platform formats must at
    # least have the expected file type and exact embedded version token.
    if [[ "$file" == bridgesessions-linux-x86_64 ]]; then
      if [[ -x "$file" ]]; then
        detected=$("./$file" --version 2>/dev/null | tr -d '\r' | head -n 1) || {
          echo "could not execute Linux release binary: $file" >&2; exit 1;
        }
      else
        echo "Linux release binary is not executable: $file" >&2; exit 1
      fi
    fi
    if [[ -z "$detected" ]]; then
      # Avoid grep -q under pipefail: early close SIGPIPEs strings and the
      # pipeline fails even when the version string is present.
      if strings -a "$file" | grep -Fx -- "$VERSION" >/dev/null; then
        detected="$VERSION"
      fi
    fi
  fi
  if [[ "$detected" != "$VERSION" ]]; then
    printf 'version mismatch: %s expected=%s detected=%s\n' \
      "$file" "$VERSION" "${detected:-unknown}" >&2
    exit 1
  fi
done

# Write checksums for all artifacts.
sha256sum "${files[@]}" > SHA256SUMS

# Generate a stable SBOM serial so rebuilding the same release payload is
# byte-for-byte reproducible and can be safely compared against a published
# release during a workflow rerun.
python3 - "$VERSION" "${files[@]}" <<'PY'
import hashlib
import json
import pathlib
import sys
import uuid

version, *names = sys.argv[1:]
names.sort()
components = []
for name in names:
    path = pathlib.Path(name)
    components.append({
        "type": "file",
        "name": name,
        "version": version,
        "hashes": [{
            "alg": "SHA-256",
            "content": hashlib.sha256(path.read_bytes()).hexdigest(),
        }],
        "properties": [{
            "name": "bridgesessions:artifact:size-bytes",
            "value": str(path.stat().st_size),
        }],
    })

identity = version + "\n" + "\n".join(
    f"{item['name']}:{item['hashes'][0]['content']}" for item in components
)
bom = {
    "bomFormat": "CycloneDX",
    "specVersion": "1.5",
    "serialNumber": f"urn:uuid:{uuid.uuid5(uuid.NAMESPACE_URL, identity)}",
    "version": 1,
    "metadata": {
        "component": {
            "type": "application",
            "name": "bridgesessions",
            "version": version,
        }
    },
    "components": components,
}
pathlib.Path("SBOM-binaries.json").write_text(
    json.dumps(bom, indent=2) + "\n", encoding="utf-8")
PY

# Append the SBOM hash; neither the SBOM nor SHA256SUMS list themselves.
sha256sum SBOM-binaries.json >> SHA256SUMS

sha256sum --check SHA256SUMS >/dev/null

python3 -m json.tool SBOM-binaries.json >/dev/null
if command -v cyclonedx >/dev/null 2>&1; then
  cyclonedx validate --input-file SBOM-binaries.json >/dev/null
fi

printf 'Validated %s artifact(s) for %s\n' "${#files[@]}" "$VERSION"
printf 'Wrote %s/SHA256SUMS and %s/SBOM-binaries.json\n' "$DIST" "$DIST"
