#!/usr/bin/env python3
"""Tests for BridgePanel v2 (tools/bridgepanel/bridgepanel.py).

Stdlib-only (unittest) so it runs anywhere Python 3.10+ is present.
Covers: filename safety, path-traversal protection, markdown rendering,
BS IPC multi-chunk parse (regression for P1-1), and the live HTTP
surface (auth, tree, content, save, health check).

Run:  python3 -m unittest test_bridgepanel -v
"""

import importlib.util
import json
import os
import socket
import tempfile
import threading
import unittest
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
SPEC = importlib.util.spec_from_file_location("bridgepanel_under_test",
                                            os.path.join(HERE, "bridgepanel.py"))
assert SPEC is not None and SPEC.loader is not None, "failed to load bridgepanel.py spec"
bp = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bp)


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
                self._chunks = [
                    b"sess_a: state=up command=/bin/zsh\nsess_b: st",
                    b"ate=up command=/bin/bash\nsess_c: state=down command=\n",
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
        orig = bp.socket.socket
        bp.socket.socket = lambda *a, **k: fake
        try:
            sessions = bp.query_bs_sessions()
        finally:
            bp.socket.socket = orig
        names = {s["name"] for s in sessions}
        self.assertEqual(names, {"sess_a", "sess_b", "sess_c"})


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
        # Breadcrumb belongs inside the sidebar, not the toolbar.
        self.assertIn('class="sidebar-header">Sessions<', raw)
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
