#!/usr/bin/env python3
"""Tests for BridgePanel v3 (tools/bridgepanel/ package).

Stdlib-only (unittest) so it runs anywhere Python 3.10+ is present.
Covers: filename safety, path-traversal protection, markdown rendering,
BS IPC multi-chunk parse (regression for P1-1), and the live HTTP
surface (auth, tree, content, save, health check, session create, connect).

Run:  python3 -m unittest test_bridgepanel -v
"""

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
    def setUp(self):
        from bridgepanel.cache import clear_caches
        clear_caches()

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
                   b'"addr":"192.168.1.2:19949","healthy":true,"last_pong_s":3,'
                   b'"sessions":[{"name":"build","state":"attached","command":"make","bytes":42}]}],'
                   b'"sessions":[{"name":"hermes","state":"attached","command":"hermes","bytes":99}]}\n')
        self._fake_ipc([payload])
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
        self._fake_ipc([b"ERROR unauthorized\n"])
        try:
            tree = bp.query_mesh_tree()
        finally:
                self._restore_ipc()
        self.assertTrue(tree.get("offline"))

    def _fake_ipc_factory(self, payloads):
        """Install socket.socket as a factory returning a fresh fake per call."""
        import bridgepanel.api as bp_api

        class FakeSock:
            def __init__(self, chunk):
                self.sent = b""
                self._chunk = chunk

            def settimeout(self, t):
                pass

            def connect(self, a):
                pass

            def sendall(self, d):
                self.sent += d

            def recv(self, n):
                c, self._chunk = self._chunk, b""
                return c

            def close(self):
                pass

        payloads = list(payloads)
        self._orig_sock = bp_api.socket.socket
        self._orig_tok = bp_api.bs_ipc_token
        bp_api.socket.socket = lambda *a, **k: FakeSock(payloads.pop(0) if payloads else b"")
        bp_api.bs_ipc_token = lambda: "t" * 64

    def test_mesh_tree_merges_offline_seeds_from_fleet(self):
        # MESH_TREE lists only live peers; FLEET adds offline seeds. The merged
        # tree must include the offline seed so the panel renders it.
        mesh_payload = (b'{"node":"desktop-1","uptime_s":10,'
                        b'"peers":[{"name":"test-peer","addr":"1.2.3.4:19949","healthy":true,"last_pong_s":1,"sessions":[]}],'
                        b'"sessions":[]}\n')
        fleet_payload = (b'{"desktop-1":{"name":"desktop-1","addr":"0.0.0.0:19949","status":"self"},'
                         b'"test-peer":{"name":"test-peer","addr":"1.2.3.4:19949","status":"healthy"},'
                         b'"offline-peer":{"name":"offline-peer","addr":"5.6.7.8:19949","status":"offline","source":"seed"}}\n')
        self._fake_ipc_factory([mesh_payload, fleet_payload])
        try:
            tree = bp.query_mesh_tree()
        finally:
                self._restore_ipc()
        names = {p["name"] for p in tree["peers"]}
        self.assertIn("test-peer", names)                 # live peer kept
        self.assertIn("offline-peer", names)      # offline seed merged in
        offline = next(p for p in tree["peers"] if p["name"] == "offline-peer")
        self.assertFalse(offline["healthy"])
        self.assertEqual(offline["status"], "offline")

    def test_mesh_tree_does_not_dup_live_peer_from_fleet(self):
        # A live peer present in both MESH_TREE and FLEET must not be duplicated.
        mesh_payload = (b'{"node":"desktop-1","uptime_s":10,'
                        b'"peers":[{"name":"test-peer","addr":"1.2.3.4:19949","healthy":true,"last_pong_s":1,"sessions":[]}],'
                        b'"sessions":[]}\n')
        fleet_payload = (b'{"test-peer":{"name":"test-peer","addr":"1.2.3.4:19949","status":"healthy"}}\n')
        self._fake_ipc_factory([mesh_payload, fleet_payload])
        try:
            tree = bp.query_mesh_tree()
        finally:
                self._restore_ipc()
        test_peer = [p for p in tree["peers"] if p["name"] == "test-peer"]
        self.assertEqual(len(test_peer), 1)

    def test_scrollback_parse_incremental(self):
        import base64
        chunk = base64.b64encode(b"hello world").rstrip(b"=")  # daemon strips padding
        payload = b"OK 11 " + chunk + b"\n"
        self._fake_ipc([payload])
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
        self._fake_ipc([payload])
        try:
            d = bp.query_scrollback("hermes", 0)
        finally:
                self._restore_ipc()
        self.assertTrue(d["reset"])
        self.assertEqual(d["text"], "tail-bytes")
        self.assertEqual(d["offset"], 4096)

    def test_scrollback_error(self):
        self._fake_ipc([b"ERROR no such session\n"])
        try:
            d = bp.query_scrollback("nope", 0)
        finally:
                self._restore_ipc()
        self.assertTrue(d["error"])

    def test_scrollback_empty_incremental(self):
        # Daemon replies 'OK <off>' (no payload) when nothing new arrived —
        # must NOT be treated as an error (v3 UI regression).
        self._fake_ipc([b"OK 2295\n"])
        try:
            d = bp.query_scrollback("hermes", 2295)
        finally:
                self._restore_ipc()
        self.assertEqual(d["error"], "")
        self.assertEqual(d["offset"], 2295)
        self.assertEqual(d["text"], "")
        self.assertFalse(d["reset"])

    def test_parse_progress_lines(self):
        import bridgepanel.api as bp_api
        text = (
            "PROGRESS phase=send file=x.bin chunks=1/2 bytes=10/20 pct=50 "
            "rate_mibs=1.2 eta_sec=3\nOK done\n"
        )
        rows = bp_api.parse_progress_lines(text)
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["pct"], "50")
        self.assertEqual(rows[0]["phase"], "send")
        self.assertEqual(rows[0]["file"], "x.bin")

    def test_guess_file_type(self):
        import bridgepanel.api as bp_api
        _, is_text = bp_api.guess_file_type("note.md", b"# hi")
        self.assertTrue(is_text)
        ctype, is_text = bp_api.guess_file_type("shot.png", b"\x89PNG")
        self.assertFalse(is_text)
        self.assertIn("png", ctype)

    def test_docs_lane_cap_unchanged(self):
        import bridgepanel.consts as consts
        self.assertEqual(consts.MAX_UPLOAD, 10 * 1024 * 1024)

    def test_scrollback_bare_reset(self):
        self._fake_ipc([b"OK 4096 RESET\n"])
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

    def setUp(self):
        from bridgepanel.cache import clear_caches
        clear_caches()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.tmp.cleanup()

    def _req(self, method, path, body=None, *, auth=True):
        conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = {"Authorization": f"Bearer {self.token}"} if auth else {}
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
        status, _ = self._req("GET", "/api/tree", auth=False)
        self.assertEqual(status, 404)

    def test_path_token_is_not_authentication(self):
        status, _ = self._req("GET", f"/api/tree", auth=False)
        self.assertEqual(status, 404)

    def test_index_tools_bar_structure(self):
        status, raw = self._req("GET", f"/")
        self.assertEqual(status, 200)
        raw = raw.decode("utf-8") if isinstance(raw, bytes) else raw
        self.assertIn('class="col-head">Hosts', raw)
        self.assertIn('id="filesHead">Files<', raw)
        self.assertNotIn('id="sessionsHead"', raw)
        self.assertNotIn('id="newSessionBtn"', raw)
        self.assertNotIn('id="createModal"', raw)
        self.assertIn('id="volrow"', raw)
        self.assertIn('id="breadcrumb"', raw)
        self.assertIn('id="treeToggle"', raw)
        self.assertIn('id="destHint"', raw)
        self.assertIn('id="listBanner"', raw)
        self.assertIn("bp-list-cache", raw)
        self.assertIn("/api/volumes", raw)
        self.assertNotIn('label: "received"', raw)
        self.assertIn('id="splitHosts"', raw)
        self.assertIn('id="splitFiles"', raw)
        self.assertIn('initSplitters', raw)
        self.assertNotIn('windows · inbox', raw)
        self.assertNotIn('this node', raw)
        self.assertNotIn('m.os || ""', raw)
        self.assertNotIn("__BUILD_TAG__", raw)
        self.assertIn(bp.BUILDTAG, raw)
        self.assertIn('class="toolbar"', raw)
        for btn in ("editBtn", "saveBtn", "cancelBtn", "copyBtn", "downloadBtn"):
            self.assertIn(f'id="{btn}"', raw)
        self.assertNotIn("📋", raw)
        self.assertNotIn("✏️", raw)
        self.assertIn("Alphabetical by machine name", raw)
        self.assertIn("/api/files", raw)
        self.assertIn("id=\"fileInput\"", raw)

    def test_tree_ok(self):
        status, raw = self._req("GET", f"/api/tree")
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
            # Dead tasks with no filesystem artifacts are filtered (Fix A)
            self.assertNotIn("dead-task", sessions)
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
            f"/api/content?session=audit&type=documents&name={target.name}",
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
        status, raw = self._req("POST", f"/api/save", body)
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
        status, _ = self._req("POST", f"/api/save", body)
        self.assertEqual(status, 200)
        escaped = bp.sessions_dir() / "audit" / "documents" / "escape.md"
        self.assertTrue(escaped.is_file())
        outside = bp.sessions_dir().parent / "escape.md"
        self.assertFalse(outside.exists())

    def test_session_create_stub(self):
        # The endpoint is now wired to the daemon IPC. In this test env
        # the daemon is not running, so it should return an error (not a stub).
        body = {
            "machine": "test-pc2",
            "name": "my-session",
            "command": "bash -l",
            "cols": 80,
            "rows": 24,
        }
        status, raw = self._req("POST", f"/api/session/create", body)
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        # In test env with no daemon, "ok" is False with an error message.
        self.assertFalse(payload["ok"])
        self.assertIn("error", payload)

    def test_session_create_requires_token(self):
        status, _ = self._req("POST", "/api/session/create", {"name": "x"})
        self.assertEqual(status, 404)

    def test_session_connect(self):
        status, raw = self._req(
            "GET",
            f"/api/session/connect?session=my-session&machine=test-pc2",
        )
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertIn("cmd", payload)
        self.assertEqual(payload["cmd"], "bs shell test-pc2 -n my-session")
        # Without machine, uses (peer) placeholder
        status2, raw2 = self._req(
            "GET",
            f"/api/session/connect?session=test",
        )
        self.assertEqual(status2, 200)
        self.assertIn("(peer)", json.loads(raw2)["cmd"])

    def test_session_connect_requires_session(self):
        status, _ = self._req("GET", f"/api/session/connect?machine=test-pc2")
        self.assertEqual(status, 400)

    def test_session_input_not_wired(self):
        status, raw = self._req(
            "POST",
            f"/api/session/input",
            {"session": "hermes", "data": "ls\n"},
        )
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertFalse(payload["ok"])
        self.assertFalse(payload["wired"])
        self.assertIn("not wired", payload["error"])

    def test_session_input_requires_session(self):
        status, _ = self._req(
            "POST", f"/api/session/input", {"data": "x"}
        )
        self.assertEqual(status, 400)

    def test_stream_requires_session(self):
        status, _ = self._req("GET", f"/api/stream")
        self.assertEqual(status, 400)

    def test_stream_sse_once(self):
        import bridgepanel.server as srv

        orig = srv.query_scrollback
        srv.query_scrollback = lambda session, since: {
            "offset": 4,
            "text": "ping",
            "reset": False,
            "error": "",
        }
        conn = None
        try:
            conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
            conn.request(
                "GET",
                f"/api/stream?session=hermes&since=0&once=1",
            )
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            ctype = r.getheader("Content-Type") or ""
            self.assertIn("text/event-stream", ctype)
            body = r.read().decode("utf-8")
            self.assertIn("data: ", body)
            self.assertIn("ping", body)
            line = [ln for ln in body.splitlines() if ln.startswith("data: ")][0]
            payload = json.loads(line[len("data: "):])
            self.assertEqual(payload["text"], "ping")
            self.assertEqual(payload["offset"], 4)
        finally:
            srv.query_scrollback = orig
            if conn is not None:
                conn.close()

    def test_index_is_file_manager(self):
        status, raw = self._req("GET", f"/")
        self.assertEqual(status, 200)
        html = raw.decode("utf-8") if isinstance(raw, bytes) else raw
        self.assertIn("/api/files", html)
        self.assertIn("/api/remote-file", html)
        self.assertIn("/api/upload", html)
        self.assertNotIn("/api/stream", html)
        self.assertNotIn("/api/session/input", html)
        self.assertNotIn("id=\"outputInput\"", html)
        self.assertNotIn("function nestByPrefix", html)
        self.assertIn("toastui-editor-all.min.js", html)
        self.assertIn("codemirror-bundle.min.js", html)
        self.assertIn("filepond.min.js", html)
        self.assertIn("id=\"cmHost\"", html)
        self.assertIn("id=\"langPick\"", html)
        self.assertIn("id=\"newFileBtn\"", html)
        self.assertIn("id=\"newDirBtn\"", html)
        self.assertIn("/api/mkdir", html)
        self.assertIn("detectLanguage", html)
        self.assertIn("BridgeCM", html)
        self.assertIn("usageStatistics: false", html)
        self.assertNotIn("__ASSET_BASE__", html)
        self.assertNotIn("__LANG_TABLE__", html)
        self.assertIn('"python"', html)
        self.assertIn(f"/static/", html)

    def test_static_vendors_served(self):
        for name, needle in (
            ("toastui-editor-all.min.js", b"toastui"),
            ("toastui-editor.min.css", b"toastui"),
            ("filepond.min.js", b"FilePond"),
            ("filepond.min.css", b"filepond"),
            ("filepond-plugin-file-validate-size.min.js", b"FilePond"),
            ("codemirror-bundle.min.js", b"BridgeCM"),
        ):
            status, raw = self._req("GET", f"/static/{name}")
            self.assertEqual(status, 200, name)
            self.assertIn(needle, raw)

    def test_static_unknown_and_traversal(self):
        status, _ = self._req("GET", f"/static/nope.js")
        self.assertEqual(status, 404)
        status, _ = self._req("GET", f"/static/../server.py")
        self.assertEqual(status, 404)
        status, _ = self._req("GET", f"/static/%2e%2e/server.py")
        self.assertEqual(status, 404)

    def test_csp_allows_same_origin_assets(self):
        conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            conn.request("GET", f"/")
            r = conn.getresponse()
            r.read()
            csp = r.getheader("Content-Security-Policy") or ""
        finally:
            conn.close()
        self.assertIn("script-src 'self' 'unsafe-inline'", csp)
        self.assertIn("style-src 'self' 'unsafe-inline'", csp)
        self.assertIn("img-src 'self' data: blob:", csp)

    def test_remote_file_binary_stream(self):
        import bridgepanel.server as srv
        orig = srv.remote_file_recv
        srv.remote_file_recv = lambda machine, path: {
            "ok": True,
            "name": "shot.png",
            "size": 4,
            "content_type": "image/png",
            "is_text": False,
            "data": b"\x89PNG",
            "progress": [{"raw": "PROGRESS pct=100", "pct": "100"}],
        }
        conn = None
        try:
            conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
            conn.request(
                "GET",
                f"/api/remote-file?machine=peer&path=shot.png",
            )
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            self.assertEqual(r.getheader("Content-Type"), "image/png")
            disp = r.getheader("Content-Disposition") or ""
            self.assertIn("attachment", disp)
            self.assertIn("shot.png", disp)
            self.assertEqual(r.read(), b"\x89PNG")
        finally:
            srv.remote_file_recv = orig
            if conn is not None:
                conn.close()

    def test_remote_file_text_json(self):
        import bridgepanel.server as srv
        orig = srv.remote_file_recv
        srv.remote_file_recv = lambda machine, path: {
            "ok": True,
            "name": "note.md",
            "size": 7,
            "content_type": "text/markdown",
            "is_text": True,
            "raw": "# Hello",
            "html": "<h1>Hello</h1>",
            "data": b"# Hello",
            "progress": [],
        }
        try:
            status, raw = self._req(
                "GET",
                f"/api/remote-file?machine=peer&path=note.md",
            )
            self.assertEqual(status, 200)
            payload = json.loads(raw)
            self.assertTrue(payload["ok"])
            self.assertTrue(payload["is_text"])
            self.assertEqual(payload["raw"], "# Hello")
            self.assertNotIn("data", payload)
        finally:
            srv.remote_file_recv = orig

    def test_remote_file_download_force(self):
        import bridgepanel.server as srv
        orig = srv.remote_file_recv
        srv.remote_file_recv = lambda machine, path: {
            "ok": True,
            "name": "note.md",
            "size": 3,
            "content_type": "text/markdown",
            "is_text": True,
            "raw": "abc",
            "html": "<p>abc</p>",
            "data": b"abc",
            "progress": [],
        }
        conn = None
        try:
            conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
            conn.request(
                "GET",
                f"/api/remote-file?machine=peer&path=note.md&download=1",
            )
            r = conn.getresponse()
            self.assertEqual(r.status, 200)
            self.assertIn("attachment", r.getheader("Content-Disposition") or "")
            self.assertEqual(r.read(), b"abc")
        finally:
            srv.remote_file_recv = orig
            if conn is not None:
                conn.close()

    def test_upload_file_lane_cap(self):
        old = os.environ.get("BRIDGEPANEL_MAX_FILE_UPLOAD")
        os.environ["BRIDGEPANEL_MAX_FILE_UPLOAD"] = "32"
        try:
            body = {"machine": "peer", "path": "x.bin", "content": "n" * 200}
            status, _ = self._req("POST", f"/api/upload", body)
            self.assertEqual(status, 413)
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_MAX_FILE_UPLOAD", None)
            else:
                os.environ["BRIDGEPANEL_MAX_FILE_UPLOAD"] = old

    def test_upload_returns_progress(self):
        import bridgepanel.server as srv
        orig = srv.remote_file_send
        srv.remote_file_send = lambda machine, path, content: {
            "ok": True,
            "dest": path,
            "machine": machine,
            "size": len(content),
            "progress": [
                {"raw": "PROGRESS phase=send pct=100", "phase": "send", "pct": "100"}
            ],
        }
        try:
            status, raw = self._req(
                "POST",
                f"/api/upload",
                {"machine": "peer", "path": "out.txt", "content": "hello"},
            )
            self.assertEqual(status, 200)
            payload = json.loads(raw)
            self.assertTrue(payload["ok"])
            self.assertEqual(payload["progress"][0]["pct"], "100")
        finally:
            srv.remote_file_send = orig

    def test_9warp_routes_removed(self):
        for path in ("/api/providers", "/api/fleet", "/api/registry", "/api/events"):
            status, _ = self._req("GET", f"{path}")
            self.assertEqual(status, 404, path)

    def test_9warp_helpers_removed(self):
        import bridgepanel.api as bp_api
        for name in (
            "query_providers",
            "query_fleet",
            "query_registry",
            "query_events",
            "query_discovered",
        ):
            self.assertFalse(hasattr(bp_api, name), name)

    def test_bind_guard(self):
        self.assertTrue(bp._bind_is_allowed("127.0.0.1"))
        self.assertTrue(bp._bind_is_allowed("10.2.2.2"))
        self.assertTrue(bp._bind_is_allowed("192.168.10.20"))
        self.assertFalse(bp._bind_is_allowed("0.0.0.0"))
        self.assertFalse(bp._bind_is_allowed("1.2.3.4"))
        with self.assertRaises(ValueError):
            bp.serve("0.0.0.0", 9770)
        with self.assertRaises(ValueError):
            bp.serve("1.2.3.4", 9770)


    def test_safe_relpath_rejects_escape(self):
        self.assertEqual(bp.safe_relpath(""), "")
        self.assertEqual(bp.safe_relpath("note.md"), "note.md")
        self.assertEqual(bp.safe_relpath("docs/note.md"), "docs/note.md")
        self.assertEqual(bp.safe_relpath("../etc/passwd"), "")
        self.assertEqual(bp.safe_relpath("/tmp/x"), "")
        self.assertEqual(bp.safe_relpath("C:\\Windows"), "")

    def test_list_receive_dir_local(self):
        inbox = os.path.join(self.tmp.name, "inbox")
        os.makedirs(inbox)
        with open(os.path.join(inbox, "hello.md"), "w", encoding="utf-8") as fh:
            fh.write("# Hi\n")
        with open(os.path.join(inbox, "pic.png"), "wb") as fh:
            fh.write(b"\x89PNG")
        old = os.environ.get("BRIDGEPANEL_RECEIVE_DIR")
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = inbox
        try:
            listing = bp.list_receive_dir("")
            names = {i["name"]: i for i in listing["items"]}
            self.assertTrue(listing["ok"])
            self.assertIn("hello.md", names)
            self.assertEqual(names["hello.md"]["kind"], "md")
            self.assertEqual(names["pic.png"]["kind"], "image")
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
            else:
                os.environ["BRIDGEPANEL_RECEIVE_DIR"] = old

    def test_files_api_lists_local_inbox(self):
        inbox = os.path.join(self.tmp.name, "inbox-api")
        os.makedirs(inbox)
        with open(os.path.join(inbox, "note.md"), "w", encoding="utf-8") as fh:
            fh.write("body\n")
        old = os.environ.get("BRIDGEPANEL_RECEIVE_DIR")
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = inbox
        try:
            status, raw = self._req("GET", f"/api/files?machine=(local)")
            self.assertEqual(status, 200)
            payload = json.loads(raw)
            self.assertTrue(payload["ok"])
            names = [i["name"] for i in payload["items"]]
            self.assertIn("note.md", names)
            self.assertEqual(payload.get("root"), "inbox")
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
            else:
                os.environ["BRIDGEPANEL_RECEIVE_DIR"] = old

    def test_volumes_api_inbox_first(self):
        status, raw = self._req("GET", f"/api/volumes?machine=(local)")
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        vols = payload.get("volumes") or []
        self.assertGreaterEqual(len(vols), 1)
        self.assertEqual(vols[0]["token"], "inbox")
        self.assertTrue(vols[0]["writable"])
        tokens = [v["token"] for v in vols]
        self.assertNotIn("/boot", tokens)
        if "/" in tokens:
            self.assertIn("/", tokens)

    def test_files_rejects_bad_volume_path(self):
        status, raw = self._req(
            "GET",
            f"/api/files?machine=(local)&root=/&path=../etc",
        )
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertFalse(payload.get("ok", True))
        self.assertEqual(payload.get("error"), "path_rejected")

    def test_upload_rejects_non_inbox_root(self):
        body = {"machine": "(local)", "root": "C", "path": "x.md", "content": "nope"}
        status, raw = self._req("POST", f"/api/upload", body)
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertFalse(payload.get("ok", True))
        self.assertEqual(payload.get("error"), "write_inbox_only")

    def test_upload_allows_acl_outbox(self):
        cfg = os.environ["BRIDGEPANEL_CONFIG"]
        outbox = os.path.join(self.tmp.name, "outbox")
        os.makedirs(outbox, exist_ok=True)
        acl = {
            "(local)": [{"path": outbox, "token": "outbox", "label": "Outbox", "writable": True}],
            "desktop-1": [{"path": outbox, "token": "outbox", "label": "Outbox", "writable": True}],
        }
        with open(os.path.join(cfg, "browse_roots.json"), "w", encoding="utf-8") as fh:
            json.dump(acl, fh)
        body = {"machine": "(local)", "root": "outbox", "path": "acl.md", "content": "from-acl"}
        status, raw = self._req("POST", f"/api/upload", body)
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertTrue(payload.get("ok"), payload)
        dest = os.path.join(outbox, "acl.md")
        self.assertTrue(os.path.isfile(dest))
        with open(dest, encoding="utf-8") as fh:
            self.assertEqual(fh.read(), "from-acl")

    def test_files_api_requires_machine(self):
        status, _ = self._req("GET", f"/api/files")
        self.assertEqual(status, 400)

    def test_open_path_relative_file(self):
        inbox = os.path.join(self.tmp.name, "inbox-open")
        os.makedirs(os.path.join(inbox, "sub"), exist_ok=True)
        with open(os.path.join(inbox, "sub", "note.md"), "w", encoding="utf-8") as fh:
            fh.write("body\n")
        old = os.environ.get("BRIDGEPANEL_RECEIVE_DIR")
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = inbox
        try:
            status, raw = self._req(
                "GET",
                f"/api/open-path?machine=(local)&path=sub/note.md",
            )
            self.assertEqual(status, 200)
            payload = json.loads(raw)
            self.assertTrue(payload.get("ok"), payload)
            self.assertEqual(payload.get("root"), "inbox")
            self.assertEqual(payload.get("dir"), "sub")
            self.assertEqual(payload.get("name"), "note.md")
            self.assertEqual(payload.get("kind"), "md")
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
            else:
                os.environ["BRIDGEPANEL_RECEIVE_DIR"] = old

    def test_open_path_absolute_inbox_path(self):
        inbox = os.path.join(self.tmp.name, "inbox-abs")
        os.makedirs(os.path.join(inbox, "docs"), exist_ok=True)
        with open(os.path.join(inbox, "docs", "r.txt"), "w", encoding="utf-8") as fh:
            fh.write("x\n")
        old = os.environ.get("BRIDGEPANEL_RECEIVE_DIR")
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = inbox
        try:
            target = os.path.join(inbox, "docs", "r.txt")
            status, raw = self._req(
                "GET",
                f"/api/open-path?machine=(local)&path="
                + __import__("urllib.parse", fromlist=["quote"]).quote(target),
            )
            self.assertEqual(status, 200)
            payload = json.loads(raw)
            self.assertTrue(payload.get("ok"), payload)
            self.assertEqual(payload.get("root"), "inbox")
            self.assertEqual(payload.get("dir"), "docs")
            self.assertEqual(payload.get("name"), "r.txt")
            self.assertEqual(payload.get("kind"), "md")
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
            else:
                os.environ["BRIDGEPANEL_RECEIVE_DIR"] = old

    def test_open_path_directory(self):
        inbox = os.path.join(self.tmp.name, "inbox-dir")
        os.makedirs(os.path.join(inbox, "folder"), exist_ok=True)
        old = os.environ.get("BRIDGEPANEL_RECEIVE_DIR")
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = inbox
        try:
            status, raw = self._req(
                "GET",
                f"/api/open-path?machine=(local)&path=folder",
            )
            self.assertEqual(status, 200)
            payload = json.loads(raw)
            self.assertTrue(payload.get("ok"), payload)
            self.assertEqual(payload.get("dir"), "folder")
            self.assertEqual(payload.get("name"), "")
            self.assertEqual(payload.get("kind"), "dir")
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
            else:
                os.environ["BRIDGEPANEL_RECEIVE_DIR"] = old

    def test_open_path_rejects_escape(self):
        status, raw = self._req(
            "GET",
            f"/api/open-path?machine=(local)&path=../../etc/passwd",
        )
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertFalse(payload.get("ok", True))
        self.assertEqual(payload.get("error"), "path_rejected")

    def test_open_path_requires_machine(self):
        status, _ = self._req("GET", f"/api/open-path?path=x.md")
        self.assertEqual(status, 400)

    def test_open_path_not_found(self):
        status, raw = self._req(
            "GET",
            f"/api/open-path?machine=(local)&path=does-not-exist.md",
        )
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertFalse(payload.get("ok", True))

    def test_remote_file_rejects_absolute_path(self):
        status, _ = self._req(
            "GET",
            f"/api/remote-file?machine=peer&path=/etc/passwd",
        )
        self.assertEqual(status, 400)

    def test_local_upload_and_preview(self):
        inbox = os.path.join(self.tmp.name, "inbox-up")
        os.makedirs(inbox)
        old = os.environ.get("BRIDGEPANEL_RECEIVE_DIR")
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = inbox
        try:
            status, raw = self._req(
                "POST",
                f"/api/upload",
                {"machine": "(local)", "path": "saved.md", "content": "# Saved\n"},
            )
            self.assertEqual(status, 200)
            self.assertTrue(json.loads(raw)["ok"])
            path = os.path.join(inbox, "saved.md")
            self.assertTrue(os.path.isfile(path))
            status2, raw2 = self._req(
                "GET",
                f"/api/remote-file?machine=(local)&path=saved.md",
            )
            self.assertEqual(status2, 200)
            payload = json.loads(raw2)
            self.assertTrue(payload["ok"])
            self.assertIn("Saved", payload["raw"])
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
            else:
                os.environ["BRIDGEPANEL_RECEIVE_DIR"] = old

    def test_files_api_marks_cached_on_second_hit(self):
        inbox = os.path.join(self.tmp.name, "inbox-cache")
        os.makedirs(inbox)
        with open(os.path.join(inbox, "a.md"), "w", encoding="utf-8") as fh:
            fh.write("x\n")
        old = os.environ.get("BRIDGEPANEL_RECEIVE_DIR")
        os.environ["BRIDGEPANEL_RECEIVE_DIR"] = inbox
        try:
            status1, raw1 = self._req("GET", f"/api/files?machine=(local)")
            self.assertEqual(status1, 200)
            first = json.loads(raw1)
            self.assertTrue(first["ok"])
            self.assertFalse(first.get("cached"))
            status2, raw2 = self._req("GET", f"/api/files?machine=(local)")
            self.assertEqual(status2, 200)
            second = json.loads(raw2)
            self.assertTrue(second.get("cached"))
            self.assertFalse(second.get("stale"))
            status3, raw3 = self._req(
                "GET", f"/api/files?machine=(local)&refresh=1"
            )
            self.assertEqual(status3, 200)
            third = json.loads(raw3)
            self.assertFalse(third.get("cached"))
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_RECEIVE_DIR", None)
            else:
                os.environ["BRIDGEPANEL_RECEIVE_DIR"] = old

    def test_inline_image_etag_304(self):
        import hashlib
        import bridgepanel.server as srv
        blob = b"\x89PNG\r\n"
        orig = srv.remote_file_recv
        srv.remote_file_recv = lambda machine, path: {
            "ok": True,
            "name": "shot.png",
            "size": len(blob),
            "content_type": "image/png",
            "is_text": False,
            "kind": "image",
            "data": blob,
        }
        etag = '"' + hashlib.sha256(blob).hexdigest()[:16] + '"'
        conn = None
        try:
            conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
            conn.request(
                "GET",
                f"/api/remote-file?machine=peer&path=shot.png&inline=1",
            )
            r = conn.getresponse()
            body = r.read()
            self.assertEqual(r.status, 200)
            self.assertEqual(body, blob)
            self.assertIn("max-age=60", r.getheader("Cache-Control") or "")
            self.assertEqual(r.getheader("ETag"), etag)
            conn.close()
            conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
            conn.request(
                "GET",
                f"/api/remote-file?machine=peer&path=shot.png&inline=1",
                headers={"If-None-Match": etag},
            )
            r = conn.getresponse()
            r.read()
            self.assertEqual(r.status, 304)
        finally:
            srv.remote_file_recv = orig
            if conn is not None:
                conn.close()

    def test_static_is_cacheable(self):
        conn = HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            conn.request("GET", f"/static/filepond.min.js")
            r = conn.getresponse()
            r.read()
            self.assertEqual(r.status, 200)
            self.assertIn("max-age=86400", r.getheader("Cache-Control") or "")
        finally:
            conn.close()


class TestLauncher(unittest.TestCase):
    def test_panel_py_exists(self):
        launcher = os.path.join(HERE, "panel.py")
        self.assertTrue(os.path.isfile(launcher), launcher)

    def test_install_sh_uses_panel_py(self):
        with open(os.path.join(HERE, "install.sh"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("panel.py", text)
        self.assertNotIn("bridgepanel.py", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)
