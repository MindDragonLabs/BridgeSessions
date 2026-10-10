#!/usr/bin/env python3
"""Authenticated local IPC helpers for fleet session behavioral checks."""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import socket
import sys
import time
from pathlib import Path

IPC_PORT = int(os.environ.get("BRIDGESESSIONS_IPC_PORT", "19980"))


def ipc(verb: str, timeout: float = 3.0) -> str:
    token_path = Path.home() / ".bridgesessions" / "ipc-token"
    try:
        token = token_path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""
    if not token:
        return ""
    try:
        with socket.create_connection(("127.0.0.1", IPC_PORT), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(f"{token} {verb}\n".encode())
            chunks = []
            while True:
                part = sock.recv(65536)
                if not part:
                    break
                chunks.append(part)
            return b"".join(chunks).decode("utf-8", "replace").strip()
    except OSError:
        return ""


def input_session(machine: str, session: str, data: bytes) -> str:
    if not re.fullmatch(r"(?:\.|[A-Za-z0-9][A-Za-z0-9._-]{0,63})", machine) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", session):
        return "ERROR invalid machine or session"
    return ipc(f"SESSION_INPUT {machine} {session} {base64.b64encode(data).decode('ascii')}")


def scrollback(machine: str, session: str, offset: int = 0, limit: int = 65536) -> tuple[int, str]:
    raw = ipc(f"SESSION_SCROLLBACK {machine} {session} {offset} {limit}")
    parts = raw.split()
    if len(parts) < 3 or parts[0] != "OK":
        raise RuntimeError(raw or "session control IPC unavailable")
    try:
        next_offset = int(parts[1])
        payload = "" if parts[2] == "-" else base64.b64decode(parts[2], validate=True).decode("utf-8", "replace")
    except (ValueError, base64.binascii.Error) as exc:
        raise RuntimeError("malformed session scrollback reply") from exc
    return next_offset, payload


def isolation_verdict(output_a: str, output_b: str, marker_a: str, marker_b: str) -> tuple[bool, str]:
    a, b = marker_a in output_a, marker_b in output_b
    crossed = marker_b in output_a or marker_a in output_b
    if not output_a.strip() or not output_b.strip():
        return False, "empty session output"
    if crossed:
        return False, "cross-talk between named sessions"
    if not (a and b):
        return False, "executed marker missing from its session"
    return True, "each executed marker appeared only in its own session"


def wait_for_marker(machine: str, session: str, marker: str, timeout: float = 12.0, interval: float = 0.25) -> bool:
    deadline = time.monotonic() + timeout
    offset = 0
    seen = ""
    while time.monotonic() < deadline:
        try:
            offset, chunk = scrollback(machine, session, offset)
            seen += chunk
        except RuntimeError:
            pass
        if marker in seen:
            return True
        time.sleep(interval)
    return marker in seen


def main() -> int:
    parser = argparse.ArgumentParser()
    subs = parser.add_subparsers(dest="op", required=True)
    p = subs.add_parser("input"); p.add_argument("machine"); p.add_argument("session"); p.add_argument("data")
    p = subs.add_parser("read"); p.add_argument("machine"); p.add_argument("session"); p.add_argument("offset", type=int, nargs="?", default=0)
    p = subs.add_parser("wait-marker"); p.add_argument("machine"); p.add_argument("session"); p.add_argument("marker"); p.add_argument("--timeout", type=float, default=12)
    p = subs.add_parser("verdict"); p.add_argument("output_a"); p.add_argument("output_b"); p.add_argument("marker_a"); p.add_argument("marker_b")
    args = parser.parse_args()
    try:
        if args.op == "verdict":
            ok, detail = isolation_verdict(args.output_a, args.output_b, args.marker_a, args.marker_b)
            print(detail)
            return 0 if ok else 1
        if args.op == "input":
            result = input_session(args.machine, args.session, args.data.encode())
            if result != "OK":
                raise RuntimeError(result or "session input IPC unavailable")
        elif args.op == "read":
            next_offset, output = scrollback(args.machine, args.session, args.offset)
            print(json.dumps({"offset": next_offset, "output": output}))
        else:
            if not wait_for_marker(args.machine, args.session, args.marker, args.timeout):
                raise RuntimeError("executed marker not observed before timeout")
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
