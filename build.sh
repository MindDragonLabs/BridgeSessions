#!/usr/bin/env bash
# R6.1 — one-command POSIX build
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++23 -O2 -DBS_NO_NAT -DBS_NO_WEBRTC -DBS_NO_DHT -o bridgesessions bridgesessions.cpp \
  -lssl -lcrypto -lzstd -pthread -lutil -lfmt -lspdlog
echo "BUILD OK: ./bridgesessions --version"
./bridgesessions --version 2>/dev/null || true