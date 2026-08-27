#!/usr/bin/env python3
"""Local mkdir / rename / trash — writable inbox only."""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))


class TestLocalOps(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.inbox = Path(self.tmp.name) / "received"
        self.trash = Path(self.tmp.name) / "trash"
        self.inbox.mkdir()
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = str(self.inbox)
        os.environ["BRIDGEPANEL_TRASH_DIR"] = str(self.trash)
        from bridgepanel.cache import clear_caches
        clear_caches()

    def tearDown(self):
        self.tmp.cleanup()
        os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
        os.environ.pop("BRIDGEPANEL_TRASH_DIR", None)

    def test_mkdir_creates_folder(self):
        from bridgepanel.ops import mkdir_path
        out = mkdir_path("(local)", "inbox", "notes")
        self.assertTrue(out.get("ok"), out)
        self.assertTrue((self.inbox / "notes").is_dir())

    def test_mkdir_refuses_escape(self):
        from bridgepanel.ops import mkdir_path
        out = mkdir_path("(local)", "inbox", "../outside")
        self.assertFalse(out.get("ok"))
        self.assertFalse((Path(self.tmp.name) / "outside").exists())

    def test_rename_file(self):
        from bridgepanel.ops import rename_path
        (self.inbox / "old.md").write_text("hi", encoding="utf-8")
        out = rename_path("(local)", "inbox", "old.md", "new.md")
        self.assertTrue(out.get("ok"), out)
        self.assertFalse((self.inbox / "old.md").exists())
        self.assertEqual((self.inbox / "new.md").read_text(encoding="utf-8"), "hi")

    def test_rename_refuses_slash_in_new_name(self):
        from bridgepanel.ops import rename_path
        (self.inbox / "a.md").write_text("x", encoding="utf-8")
        out = rename_path("(local)", "inbox", "a.md", "b/c.md")
        self.assertFalse(out.get("ok"))
        self.assertTrue((self.inbox / "a.md").exists())

    def test_trash_moves_file(self):
        from bridgepanel.ops import trash_path
        (self.inbox / "gone.md").write_text("bye", encoding="utf-8")
        out = trash_path("(local)", "inbox", "gone.md")
        self.assertTrue(out.get("ok"), out)
        self.assertFalse((self.inbox / "gone.md").exists())
        leftovers = list(self.trash.rglob("gone.md"))
        self.assertEqual(len(leftovers), 1)
        self.assertEqual(leftovers[0].read_text(encoding="utf-8"), "bye")

    def test_write_not_allowed_on_readonly_root(self):
        from bridgepanel.ops import mkdir_path
        out = mkdir_path("(local)", "C", "tmp")
        self.assertFalse(out.get("ok"))
        self.assertEqual(out.get("error"), "write_not_allowed")


class TestHttpMutate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import bridgepanel as bp
        from http.server import ThreadingHTTPServer
        import threading
        from http.client import HTTPConnection
        cls._HTTPConnection = HTTPConnection
        cls.tmp = tempfile.TemporaryDirectory()
        cls.inbox = Path(cls.tmp.name) / "received"
        cls.trash = Path(cls.tmp.name) / "trash"
        cls.inbox.mkdir()
        os.environ["BRIDGEPANEL_HOME"] = str(Path(cls.tmp.name) / "data")
        os.environ["BRIDGEPANEL_CONFIG"] = str(Path(cls.tmp.name) / "cfg")
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = str(cls.inbox)
        os.environ["BRIDGEPANEL_TRASH_DIR"] = str(cls.trash)
        cls.token = bp.ensure_dirs()
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), bp.BridgePanelHandler)
        cls.server.bridgepanel_token = cls.token  # type: ignore[attr-defined]
        cls.server.trusted_ips = {"127.0.0.1"}  # type: ignore[attr-defined]
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()
        os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
        os.environ.pop("BRIDGEPANEL_TRASH_DIR", None)

    def _req(self, method, path, body=None):
        conn = self._HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = {"Authorization": f"Bearer {self.token}"}
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

    def test_mkdir_rename_trash_roundtrip(self):
        status, raw = self._req("POST", "/api/mkdir",
                                {"machine": "(local)", "root": "inbox", "path": "notes"})
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(raw).get("ok"), raw)
        self.assertTrue((self.inbox / "notes").is_dir())
        (self.inbox / "notes" / "a.md").write_text("x", encoding="utf-8")
        status, raw = self._req("POST", "/api/rename",
                                {"machine": "(local)", "root": "inbox", "path": "notes/a.md", "name": "b.md"})
        self.assertEqual(status, 200, raw)
        self.assertTrue(json.loads(raw).get("ok"), raw)
        self.assertTrue((self.inbox / "notes" / "b.md").is_file())
        status, raw = self._req("POST", "/api/trash",
                                {"machine": "(local)", "root": "inbox", "path": "notes/b.md"})
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(raw).get("ok"), raw)
        self.assertFalse((self.inbox / "notes" / "b.md").exists())
        self.assertTrue(any(self.trash.rglob("b.md")))

    def test_readonly_root_rejected(self):
        status, raw = self._req("POST", "/api/mkdir",
                                {"machine": "(local)", "root": "C", "path": "nope"})
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(raw).get("error"), "write_not_allowed")


if __name__ == "__main__":
    unittest.main()
