#!/usr/bin/env bash
# Validate release artifacts, write SHA256SUMS, and generate a CycloneDX 1.5
# SBOM.  Does not publish or sign anything.
#
# SHA256SUMS covers every dist artifact except itself and SBOM-binaries.json.
# The SBOM is generated afterwards and its hash is appended to SHA256SUMS.
# The SBOM never lists SHA256SUMS or SBOM-binaries.json.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
VERSION="$(tr -d '\r\n' < "$ROOT/VERSION")"
cd "$DIST"
shopt -s nullglob

candidates=(bridgesessions bridgesessions-*)
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
  detected=""
  if [[ "$file" == *-source.tar.gz ]]; then
    detected=$(tar -xOzf "$file" "bridgesessions-${VERSION}/VERSION" 2>/dev/null \
      | tr -d '\r\n' || true)
  else
    # Only execute native Linux ELF binaries. PE/Mach-O are validated via
    # embedded strings so checksums work on a single release host.
    if file -b "$file" | grep 'ELF .*executable' >/dev/null; then
      if [[ -x "$file" ]]; then
        detected=$("./$file" --version 2>/dev/null | tr -d '\r' | head -n 1 || true)
      fi
    fi
    if [[ -z "$detected" ]]; then
      # Avoid grep -q under pipefail: early close SIGPIPEs strings and the
      # pipeline fails even when the version string is present.
      if strings -a "$file" | grep -F -- "$VERSION" >/dev/null; then
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

# Generate the SBOM with a fresh random UUID4.
python3 - "$VERSION" "${files[@]}" <<'PY'
import hashlib
import json
import pathlib
import sys
import uuid

version, *names = sys.argv[1:]
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

bom = {
    "bomFormat": "CycloneDX",
    "specVersion": "1.5",
    "serialNumber": f"urn:uuid:{uuid.uuid4()}",
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
