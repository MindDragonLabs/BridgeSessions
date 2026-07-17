#!/usr/bin/env bash
# R6.1 — one-command POSIX build
set -euo pipefail
cd "$(dirname "$0")"
VERSION="$(tr -d '\r\n' < VERSION)"
VERSION_DEFINE="-DBS_VERSION=\"${VERSION}\""
g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT \
  "$VERSION_DEFINE" -o bridgesessions bridgesessions.cpp \
  -lssl -lcrypto -lzstd -pthread -lutil -lfmt -lspdlog
detected="$(./bridgesessions --version 2>/dev/null)"
[[ "$detected" == "$VERSION" ]] || {
  printf 'version mismatch: expected=%s detected=%s\n' "$VERSION" "$detected" >&2
  exit 1
}
printf 'BUILD OK: bridgesessions %s\n' "$detected"
