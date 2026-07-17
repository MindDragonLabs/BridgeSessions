#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN="${BRIDGESESSIONS_BIN:-$ROOT/build/bridgesessions}"
APP_HOME="${BRIDGESESSIONS_HOME:-$HOME/.bridgesessions}"
exec "$BIN" --config-dir "$APP_HOME" --daemon
