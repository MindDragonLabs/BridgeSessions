#!/usr/bin/env bash
set -euo pipefail

repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="${PREFIX:-$HOME/.local}"
source_binary="${1:-}"

if [[ -z "$source_binary" ]]; then
  for candidate in \
    "$repo/build/bridgesessions" \
    "$repo/build/release/bridgesessions"; do
    if [[ -x "$candidate" ]]; then
      source_binary="$candidate"
      break
    fi
  done
fi

if [[ -z "$source_binary" || ! -x "$source_binary" ]]; then
  printf 'BridgeSessions binary not found. Build it first with: cmake --build build --target bridgesessions\n' >&2
  exit 1
fi

mkdir -p "$prefix/bin"
install -m 0755 "$source_binary" "$prefix/bin/bridgesessions"
ln -sfn bridgesessions "$prefix/bin/bs"
printf 'Installed %s and %s\n' "$prefix/bin/bridgesessions" "$prefix/bin/bs"
printf 'Fast path: bs <ssh-alias> [session]\n'
