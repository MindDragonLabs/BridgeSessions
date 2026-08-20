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
                   b'"addr":"192.168.1.2:19949","healthy":true,"last_pong_s":3,'
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
        mesh_payload = (b'{"node":"macbook","uptime_s":10,'
                        b'"peers":[{"name":"test-peer","addr":"1.2.3.4:19949","healthy":true,"last_pong_s":1,"sessions":[]}],'
                        b'"sessions":[]}\n')
        fleet_payload = (b'{"macbook":{"name":"macbook","addr":"0.0.0.0:19949","status":"self"},'
                         b'"test-peer":{"name":"test-peer","addr":"1.2.3.4:19949","status":"healthy"},'
                         b'"ranas-mac-studio":{"name":"ranas-mac-studio","addr":"5.6.7.8:19949","status":"offline","source":"seed"}}\n')
        self._fake_ipc_factory([mesh_payload, fleet_payload])
        try:
            tree = bp.query_mesh_tree()
        finally:
                self._restore_ipc()
        names = {p["name"] for p in tree["peers"]}
        self.assertIn("test-peer", names)                 # live peer kept
        self.assertIn("ranas-mac-studio", names)      # offline seed merged in
        offline = next(p for p in tree["peers"] if p["name"] == "ranas-mac-studio")
        self.assertFalse(offline["healthy"])
        self.assertEqual(offline["status"], "offline")

    def test_mesh_tree_does_not_dup_live_peer_from_fleet(self):
        # A live peer present in both MESH_TREE and FLEET must not be duplicated.
        mesh_payload = (b'{"node":"macbook","uptime_s":10,'
                        b'"peers":[{"name":"test-peer","addr":"1.2.3.4:19949","healthy":true,"last_pong_s":1,"sessions":[]}],'
                        b'"sessions":[]}\n')
        fleet_payload = (b'{"test-peer":{"name":"test-peer","addr":"1.2.3.4:19949","status":"healthy"}}\n')
        self._fake_ipc_factory([mesh_payload, fleet_payload])
        try:
            tree = bp.query_mesh_tree()
        finally:
                self._restore_ipc()
        test-peer = [p for p in tree["peers"] if p["name"] == "test-peer"]
        self.assertEqual(len(test-peer), 1)

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
        # Header bar + three-column shell (redesign): machines/sessions columns.
        self.assertIn('class="col-head">Machines', raw)
        self.assertIn('id="sessionsHead">Sessions<', raw)
        self.assertIn('id="breadcrumb"', raw)
        # Build tag placeholder is replaced by the panel version, not hardcoded.
        self.assertNotIn("__BUILD_TAG__", raw)
        self.assertIn(bp.BUILDTAG, raw)
        # Exactly one toolbar with the four action buttons.
        self.assertIn('class="toolbar"', raw)
        for btn in ("editBtn", "saveBtn", "cancelBtn", "copyBtn"):
            self.assertIn(f'id="{btn}"', raw)
        # New-session modal keeps the harness picker + auto-approve toggle.
        self.assertIn('id="cmHarness"', raw)
        self.assertIn('id="cmYolo"', raw)
        self.assertIn('value="commandcode"', raw)
        # Buttons ship as text (no emoji).
        self.assertNotIn("📋", raw)
        self.assertNotIn("✏️", raw)
        # Session tree: User/Bots groups, harness detection, CUA helper node.
        self.assertIn("sgroup-head", raw)
        self.assertIn("function harnessOf", raw)
        self.assertIn("computer-use helper", raw)
        # Machines are ordered alphabetically (not live-first).
        self.assertIn("Alphabetical by machine name", raw)

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
        # The endpoint is now wired to the daemon IPC. In this test env
        # the daemon is not running, so it should return an error (not a stub).
        body = {
            "machine": "test-pc2",
            "name": "my-session",
            "command": "bash -l",
            "cols": 80,
            "rows": 24,
        }
        status, raw = self._req("POST", f"/{self.token}/api/session/create", body)
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

    def test_session_input_not_wired(self):
        status, raw = self._req(
            "POST",
            f"/{self.token}/api/session/input",
            {"session": "hermes", "data": "ls\n"},
        )
        self.assertEqual(status, 200)
        payload = json.loads(raw)
        self.assertFalse(payload["ok"])
        self.assertFalse(payload["wired"])
        self.assertIn("not wired", payload["error"])

    def test_session_input_requires_session(self):
        status, _ = self._req(
            "POST", f"/{self.token}/api/session/input", {"data": "x"}
        )
        self.assertEqual(status, 400)

    def test_stream_requires_session(self):
        status, _ = self._req("GET", f"/{self.token}/api/stream")
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
                f"/{self.token}/api/stream?session=hermes&since=0&once=1",
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

    def test_index_has_session_graph_and_stream(self):
        status, raw = self._req("GET", f"/{self.token}/")
        self.assertEqual(status, 200)
        html = raw.decode("utf-8") if isinstance(raw, bytes) else raw
        self.assertIn("parent_id will drive true hierarchy", html)
        self.assertIn("function nestByPrefix", html)
        self.assertIn("/api/stream", html)
        self.assertIn("/api/session/input", html)
        self.assertIn("id=\"outputInput\"", html)

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
                f"/{self.token}/api/remote-file?machine=peer&path=/tmp/shot.png",
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
                f"/{self.token}/api/remote-file?machine=peer&path=note.md",
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
                f"/{self.token}/api/remote-file?machine=peer&path=note.md&download=1",
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
            status, _ = self._req("POST", f"/{self.token}/api/upload", body)
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
                f"/{self.token}/api/upload",
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
            status, _ = self._req("GET", f"/{self.token}{path}")
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

    def test_loopback_bind_guard(self):
        with self.assertRaises(ValueError):
            bp.serve("0.0.0.0", 9770)
        with self.assertRaises(ValueError):
            bp.serve("1.2.3.4", 9770)


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
