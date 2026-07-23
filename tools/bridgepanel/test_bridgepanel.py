#!/usr/bin/env python3
"""Tests for BridgePanel v3 (tools/bridgepanel/ package).

Stdlib-only (unittest) so it runs anywhere Python 3.10+ is present.
Covers: filename safety, path-traversal protection, markdown rendering,
BS IPC multi-chunk parse (regression for P1-1), and the live HTTP
surface (auth, tree, content, save, health check, session create, connect).

Run:  python3 -m unittest test_bridgepanel -v
"""

import importlib.util
import json
import os
import sys
import tempfile
import threading
import unittest
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import bridgepanel as bp  # noqa: E402


class TestPureFunctions(unittest.TestCase):
    def test_safe_name_strips_path_separators(self):
        self.assertNotIn("/", bp.safe_name("../../etc/passwd"))
        self.assertNotIn("\\", bp.safe_name("a\\b\\c"))
        self.assertEqual(bp.safe_name(""), "untitled")

    def test_safe_session_name(self):
        self.assertEqual(bp.safe_session_name("my session!"), "my-session")
        self.assertEqual(bp.safe_session_name(""), "default")

    def test_safe_type_defaults(self):
        self.assertEqual(bp.safe_type("comms"), "comms")
        self.assertEqual(bp.safe_type("bogus"), "documents")

    def test_resolve_file_rejects_escape(self):
        # A name that resolves outside sessions/ must return None.
        self.assertIsNone(bp.resolve_file("../escape", "documents", "x.md"))
        self.assertIsNone(bp.resolve_file("ok", "documents", "../../etc/passwd"))

    def test_markdown_render(self):
        html = bp.markdown_to_html("# Hello\n\n- a\n- b\n")
        self.assertIn("<h1>Hello</h1>", html)
        self.assertIn("<li>a</li>", html)
        # Raw HTML must be escaped, not passed through.
        self.assertNotIn("<script>", bp.markdown_to_html("<script>alert(1)</script>"))

    def test_bs_ipc_multi_chunk(self):
        """Regression for P1-1: the SESSIONS reply may arrive in >1 TCP
        segment. The reader must NOT stop at the first newline."""
        class FakeSock:
            def __init__(self):
                self.sent = b""
                self.closed = False
                # Real daemon SESSIONS format: pipe-separated records.
                self._chunks = [
                    b"live sess_a state=up command=/bin/zsh | live sess_b st",
                    b"ate=up command=/bin/bash | live sess_c state=down command=\n",
                ]

            def settimeout(self, t):
                pass

            def connect(self, a):
                pass

            def sendall(self, d):
                self.sent += d

            def recv(self, n):
                return self._chunks.pop(0) if self._chunks else b""

            def close(self):
                self.closed = True

        fake = FakeSock()
        import bridgepanel.api as bp_api
        orig_sock = bp_api.socket.socket
        orig_tok = bp.bs_ipc_token
        bp_api.socket.socket = lambda *a, **k: fake
        bp_api.bs_ipc_token = lambda: "x" * 64
        try:
            sessions = bp.query_bs_sessions()
        finally:
            bp_api.socket.socket = orig_sock
            bp_api.bs_ipc_token = orig_tok
        # Request must be token-prefixed (daemon rejects bare verbs).
        self.assertTrue(fake.sent.startswith(b"x" * 64 + b" "))
        names = {s["name"] for s in sessions}
        self.assertEqual(names, {"sess_a", "sess_b", "sess_c"})

    def _fake_ipc(self, payload_chunks):
        """Install a fake socket returning payload_chunks; returns the fake sock."""
        import bridgepanel.api as bp_api

        class FakeSock:
            def __init__(self):
                self.sent = b""
                self._chunks = list(payload_chunks)

            def settimeout(self, t):
                pass

            def connect(self, a):
                pass

            def sendall(self, d):
                self.sent += d

            def recv(self, n):
                return self._chunks.pop(0) if self._chunks else b""

            def close(self):
                pass

        fake = FakeSock()
        self._orig_sock = bp_api.socket.socket
        self._orig_tok = bp_api.bs_ipc_token
        bp_api.socket.socket = lambda *a, **k: fake
        bp_api.bs_ipc_token = lambda: "t" * 64
        return fake

    def _restore_ipc(self):
        """Restore socket and token after _fake_ipc."""
        import bridgepanel.api as bp_api
        bp_api.socket.socket = getattr(self, '_orig_sock', bp_api.socket.socket)
        bp.bs_ipc_token = getattr(self, '_orig_tok', bp.bs_ipc_token)

    def test_mesh_tree_parse(self):
        payload = (b'{"node":"test-pc1","uptime_s":12,"peers":[{"name":"test-pc2",'
                   b'"addr":"192.168.1.30:19949","healthy":true,"last_pong_s":3,'
                   b'"sessions":[{"name":"build","state":"attached","command":"make","bytes":42}]}],'
                   b'"sessions":[{"name":"hermes","state":"attached","command":"hermes","bytes":99}]}\n')
        fake = self._fake_ipc([payload])
        try:
            tree = bp.query_mesh_tree()
        finally:
                self._restore_ipc()
        self.assertEqual(tree["node"], "test-pc1")
        self.assertEqual(tree["peers"][0]["name"], "test-pc2")
        self.assertEqual(tree["peers"][0]["sessions"][0]["name"], "build")
        self.assertEqual(tree["sessions"][0]["bytes"], 99)
        self.assertNotIn("offline", tree)

    def test_mesh_tree_offline(self):
        fake = self._fake_ipc([b"ERROR unauthorized\n"])
        try:
            tree = bp.query_mesh_tree()
        finally:
                self._restore_ipc()
        self.assertTrue(tree.get("offline"))

    def test_scrollback_parse_incremental(self):
        import base64
        chunk = base64.b64encode(b"hello world").rstrip(b"=")  # daemon strips padding
        payload = b"OK 11 " + chunk + b"\n"
        fake = self._fake_ipc([payload])
        try:
            d = bp.query_scrollback("hermes", 0)
        finally:
                self._restore_ipc()
        self.assertEqual(d["offset"], 11)
        self.assertEqual(d["text"], "hello world")
        self.assertFalse(d["reset"])
        self.assertEqual(d["error"], "")

    def test_scrollback_reset_marker(self):
        import base64
        chunk = base64.b64encode(b"tail-bytes").rstrip(b"=")
        payload = b"OK 4096 " + chunk + b" RESET\n"
        fake = self._fake_ipc([payload])
        try:
            d = bp.query_scrollback("hermes", 0)
        finally:
                self._restore_ipc()
        self.assertTrue(d["reset"])
        self.assertEqual(d["text"], "tail-bytes")
        self.assertEqual(d["offset"], 4096)

    def test_scrollback_error(self):
        fake = self._fake_ipc([b"ERROR no such session\n"])
        try:
            d = bp.query_scrollback("nope", 0)
        finally:
                self._restore_ipc()
        self.assertTrue(d["error"])

    def test_scrollback_empty_incremental(self):
        # Daemon replies 'OK <off>' (no payload) when nothing new arrived —
        # must NOT be treated as an error (v3 UI regression).
        fake = self._fake_ipc([b"OK 2295\n"])
        try:
            d = bp.query_scrollback("hermes", 2295)
        finally:
                self._restore_ipc()
        self.assertEqual(d["error"], "")
        self.assertEqual(d["offset"], 2295)
        self.assertEqual(d["text"], "")
        self.assertFalse(d["reset"])

    def test_scrollback_bare_reset(self):
        fake = self._fake_ipc([b"OK 4096 RESET\n"])
        try:
            d = bp.query_scrollback("hermes", 0)
        finally:
                self._restore_ipc()
        self.assertTrue(d["reset"])
        self.assertEqual(d["text"], "")
        self.assertEqual(d["error"], "")


class TestHttpSurface(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        os.environ["BRIDGEPANEL_HOME"] = os.path.join(cls.tmp.name, "data")
        os.environ["BRIDGEPANEL_CONFIG"] = os.path.join(cls.tmp.name, "cfg")
        cls.token = bp.ensure_dirs()
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), bp.BridgePanelHandler)
        cls.server.bridgepanel_token = cls.token  # type: ignore[attr-defined]
        cls.server.trusted_ips = set()  # type: ignore[attr-defined]
        cls.port = cls.server.server_address[1]
        t = threading.Thread(target=cls.server.serve_forever, daemon=True)
        t.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def _req(self, method, path, body=None):
        conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = {}
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        try:
            conn.request(method, path, body=data, headers=headers)
            r = conn.getresponse()
            raw = r.read()
            return r.status, raw
        finally:
            conn.close()

    def test_healthz_no_auth(self):
        status, _ = self._req("GET", "/healthz")
        self.assertEqual(status, 200)

    def test_auth_required(self):
        status, _ = self._req("GET", "/api/tree")
        self.assertEqual(status, 404)

    def test_index_tools_bar_structure(self):
        status, raw = self._req("GET", f"/{self.token}/")
        self.assertEqual(status, 200)
        raw = raw.decode("utf-8") if isinstance(raw, bytes) else raw
        # Breadcrumb + three-column shell (v3): machines/sessions headers.
        self.assertIn('class="col-header">Machines<', raw)
        self.assertIn('id="sessionsHeader">Sessions<', raw)
        self.assertIn('id="breadcrumb"', raw)
        # Exactly one tools bar with a btn-group of 4 actions.
        self.assertIn('class="toolbar"', raw)
        self.assertIn('class="btn-group"', raw)
        for btn in ("editBtn", "saveBtn", "cancelBtn", "copyBtn"):
            self.assertIn(f'id="{btn}"', raw)
        # Edit/Save/Cancel/Copy ship as outlined buttons with icons (no emoji).
        self.assertIn('id="copyBtn"', raw)
        self.assertNotIn("📋", raw)
        self.assertNotIn("✏️", raw)

    def test_tree_ok(self):
        status, raw = self._req("GET", f"/{self.token}/api/tree")
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertIn("sessions", payload)

    def test_tree_merges_peer_sessions(self):
        """build_tree() merges remote peer sessions from MESH_TREE gossip."""
        import bridgepanel.api as bp_api
        orig_bs = bp_api.query_bs_sessions
        orig_mt = bp_api.query_mesh_tree
        try:
            bp_api.query_bs_sessions = lambda: []
            bp_api.query_mesh_tree = lambda: {
                "node": "testhost",
                "uptime_s": 1,
                "peers": [
                    {
                        "name": "peer-a",
                        "addr": "1.2.3.4:19949",
                        "healthy": True,
                        "last_pong_s": 1,
                        "sessions": [
                            {"name": "remote-shell", "state": "live", "command": "bash", "bytes": 999},
                            {"name": "dead-task", "state": "died", "command": "make", "bytes": 42},
                        ],
                    },
                ],
                "sessions": [],
            }
            tree = bp_api.build_tree()
            sessions = {s["name"]: s for s in tree["sessions"]}
            # Remote live session appears
            self.assertIn("remote-shell", sessions)
            self.assertTrue(sessions["remote-shell"]["live"])
            self.assertEqual(sessions["remote-shell"]["peer"], "peer-a")
            # Remote dead session appears but not live
            self.assertIn("dead-task", sessions)
            self.assertFalse(sessions["dead-task"]["live"])
            self.assertEqual(sessions["dead-task"]["peer"], "peer-a")
        finally:
            bp_api.query_bs_sessions = orig_bs
            bp_api.query_mesh_tree = orig_mt

    def test_publish_then_content(self):
        src = os.path.join(self.tmp.name, "report.md")
        with open(src, "w", encoding="utf-8") as fh:
            fh.write("# Report\n\nBody text.\n")
        target = bp.publish(__import__("pathlib").Path(src), "audit", "documents", None)
        status, raw = self._req(
            "GET",
            f"/{self.token}/api/content?session=audit&type=documents&name={target.name}",
        )
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertIn("Body text.", payload["raw"])
        self.assertIn("<h1>Report</h1>", payload["html"])

    def test_save_writes_file(self):
        body = {
            "session": "audit",
            "type": "documents",
            "name": "note.md",
            "content": "# Saved\n\nEdited inline.\n",
        }
        status, raw = self._req("POST", f"/{self.token}/api/save", body)
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertIn("<h1>Saved</h1>", payload["html"])
        # Confirm it actually hit disk under sessions/.
        path = bp.sessions_dir() / "audit" / "documents" / "note.md"
        self.assertTrue(path.is_file())
        self.assertIn("Edited inline.", path.read_text(encoding="utf-8"))

    def test_traversal_rejected_on_save(self):
        # safe_name neutralises "../" so the file lands inside sessions/.
        body = {
            "session": "audit",
            "type": "documents",
            "name": "../../escape.md",
            "content": "x",
        }
        status, _ = self._req("POST", f"/{self.token}/api/save", body)
        self.assertEqual(status, 200)
        escaped = bp.sessions_dir() / "audit" / "documents" / "escape.md"
        self.assertTrue(escaped.is_file())
        outside = bp.sessions_dir().parent / "escape.md"
        self.assertFalse(outside.exists())

    def test_session_create_stub(self):
        body = {
            "machine": "test-pc2",
            "name": "my-session",
            "command": "bash -l",
            "cols": 80,
            "rows": 24,
            "term": "xterm-256color",
        }
        status, raw = self._req("POST", f"/{self.token}/api/session/create", body)
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertFalse(payload["ok"])
        self.assertIn("not yet implemented", payload["error"])

    def test_session_create_requires_token(self):
        status, _ = self._req("POST", "/api/session/create", {"name": "x"})
        self.assertEqual(status, 404)

    def test_session_connect(self):
        status, raw = self._req(
            "GET",
            f"/{self.token}/api/session/connect?session=my-session&machine=test-pc2",
        )
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertIn("cmd", payload)
        self.assertEqual(payload["cmd"], "bs shell test-pc2 -n my-session")
        # Without machine, uses (peer) placeholder
        status2, raw2 = self._req(
            "GET",
            f"/{self.token}/api/session/connect?session=test",
        )
        self.assertEqual(status2, 200)
        self.assertIn("(peer)", json.loads(raw2)["cmd"])

    def test_session_connect_requires_session(self):
        status, _ = self._req("GET", f"/{self.token}/api/session/connect?machine=test-pc2")
        self.assertEqual(status, 400)


if __name__ == "__main__":
    unittest.main(verbosity=2)
