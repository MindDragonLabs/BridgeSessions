#!/usr/bin/env python3
"""Isolated v1 control-IPC socket fixture harness.

This starts a disposable loopback IPC fixture, exercises the exact socket
framing used by ``bridgepanel.api``, and exits non-zero on any contract
failure. It does not run the real daemon or prove its backend behavior.
It never reads a real BridgeSessions token or credential file.
"""
from __future__ import annotations

import base64
import json
from pathlib import Path
import socketserver
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from bridgepanel import api, v1

PAYLOAD = b"fixture" + bytes([0]) + b"output"


class Handler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        line = self.rfile.readline().decode("utf-8").rstrip("\n")
        _, _, verb = line.partition(" ")
        if verb == "SESSIONS":
            out = "live codex state=up command=fixture\n"
        elif verb.startswith("SESSION_INPUT "):
            out = "OK\n"
        elif verb.startswith("SESSION_SCROLLBACK "):
            out = "OK 7 " + base64.b64encode(PAYLOAD).decode("ascii") + " RESET\n"
        elif verb.startswith("SESSION_KILL "):
            out = "OK\n"
        else:
            out = "ERROR unknown command\n"
        self.wfile.write(out.encode("utf-8"))


def main() -> int:
    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    old_port, old_token = api.BS_IPC_PORT, api.bs_ipc_token
    api.BS_IPC_PORT = server.server_address[1]
    api.bs_ipc_token = lambda: "fixture-token"
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        status, caps = v1.capabilities()
        assert status == 200 and caps["backend"]["connected"]
        assert api.daemon_session_input_v1(".", "codex", b"x")["ok"]
        scroll = api.daemon_session_scrollback_v1(".", "codex", 0, 64)
        assert scroll["ok"] and scroll["reset"]
        assert base64.b64decode(scroll["text_b64"]) == PAYLOAD
        assert api.daemon_session_kill_v1(".", "codex")["ok"]
        print(json.dumps({"ok": True, "harness": "ipc_fixture_only", "capabilities": caps["routes"]}, sort_keys=True))
        return 0
    finally:
        api.BS_IPC_PORT, api.bs_ipc_token = old_port, old_token
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
