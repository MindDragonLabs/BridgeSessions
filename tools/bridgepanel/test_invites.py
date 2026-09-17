#!/usr/bin/env python3
"""Tests for BridgePanel invite management (tools/bridgepanel/invites.py)."""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from bridgepanel import invites  # noqa: E402

GOOD_TOKEN = "0" * 64


def _future_iso(seconds: int) -> str:
    return (datetime.now(timezone.utc) + timedelta(seconds=seconds)).isoformat()


def _past_iso(seconds: int) -> str:
    return (datetime.now(timezone.utc) - timedelta(seconds=seconds)).isoformat()


class TestSeedInfo(unittest.TestCase):
    @mock.patch.object(invites, "_tailscale_ip4", return_value="100.99.0.7")
    @mock.patch.object(invites, "_config_value", return_value="0.0.0.0:19949")
    def test_listen_wildcard_uses_tailscale(self, _cfg, _ts):
        info = invites.seed_info()
        self.assertEqual(info["addr"], "100.99.0.7:19949")
        self.assertEqual(info["host"], "100.99.0.7")
        self.assertEqual(info["port"], 19949)

    @mock.patch.object(invites, "_config_value", return_value="192.0.2.5:12345")
    def test_listen_explicit_host(self, _cfg):
        info = invites.seed_info()
        self.assertEqual(info["addr"], "192.0.2.5:12345")

    @mock.patch.object(invites, "_tailscale_ip4", return_value="")
    @mock.patch.object(invites, "_config_value", return_value="")
    def test_fallback_loopback(self, _cfg, _ts):
        info = invites.seed_info()
        self.assertEqual(info["addr"], "127.0.0.1:19949")


class TestJoinWindow(unittest.TestCase):
    @mock.patch.object(invites, "_config_value", return_value="120")
    def test_window_override(self, _cfg):
        self.assertEqual(invites.join_window_seconds(), 120)

    @mock.patch.object(invites, "_config_value", return_value="")
    def test_window_default(self, _cfg):
        self.assertEqual(invites.join_window_seconds(), 300)

    @mock.patch.object(invites, "_config_value", return_value="bogus")
    def test_window_invalid_falls_back(self, _cfg):
        self.assertEqual(invites.join_window_seconds(), 300)


class TestMint(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.data_home = Path(self._tmp.name)

    def _patch(self):
        patchers = [
            mock.patch.object(invites, "bs_ipc", return_value=GOOD_TOKEN + "\n"),
            mock.patch.object(invites, "data_home", return_value=self.data_home),
            mock.patch.object(invites, "_config_value", return_value="192.0.2.9:19949"),
        ]
        for p in patchers:
            p.start()
            self.addCleanup(p.stop)

    def test_mint_success_records(self):
        self._patch()
        result = invites.mint_invite()
        self.assertTrue(result["ok"], result)
        rec = result["invite"]
        self.assertEqual(rec["token"], GOOD_TOKEN)
        self.assertEqual(rec["seed"], "192.0.2.9:19949")
        self.assertEqual(rec["ttl_seconds"], invites.INVITE_TTL_SEC)
        # Ledger persisted on disk.
        saved = json.loads((self.data_home / "invites.json").read_text())
        self.assertEqual(len(saved), 1)
        self.assertEqual(saved[0]["token"], GOOD_TOKEN)

    def test_mint_daemon_down(self):
        with mock.patch.object(invites, "bs_ipc", return_value=""):
            result = invites.mint_invite()
        self.assertFalse(result["ok"])

    def test_mint_error_response(self):
        with mock.patch.object(invites, "bs_ipc", return_value="ERROR nope"):
            result = invites.mint_invite()
        self.assertFalse(result["ok"])

    def test_mint_malformed_token(self):
        with mock.patch.object(invites, "bs_ipc", return_value="not-hex"):
            result = invites.mint_invite()
        self.assertFalse(result["ok"])


class TestLedger(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.data_home = Path(self._tmp.name)

    def test_list_prunes_expired(self):
        ledger = [
            {"token": "a" * 64, "seed": "x:1", "expires_at": _past_iso(10)},
            {"token": "b" * 64, "seed": "x:1", "expires_at": _future_iso(3600)},
        ]
        (self.data_home / "invites.json").write_text(json.dumps(ledger))
        with mock.patch.object(invites, "data_home", return_value=self.data_home):
            result = invites.list_invites()
        self.assertTrue(result["ok"])
        self.assertEqual(len(result["invites"]), 1)
        self.assertEqual(result["invites"][0]["token"], "b" * 64)


class TestRender(unittest.TestCase):
    def test_render_contains_token_and_seed(self):
        rec = {"token": "f" * 32, "seed": "10.0.0.5:19949",
               "window_seconds": 300, "expires_at": _future_iso(3600)}
        page = invites.render_invite_page(rec)
        self.assertIn("f" * 32, page)
        self.assertIn("10.0.0.5:19949", page)
        self.assertIn("</html>", page)

    def test_render_escapes_malicious_token(self):
        rec = {"token": "<script>alert(1)</script>", "seed": "10.0.0.5:19949",
               "window_seconds": 300, "expires_at": _future_iso(3600)}
        page = invites.render_invite_page(rec)
        self.assertNotIn("<script>alert(1)</script>", page)
        self.assertIn("&lt;script&gt;", page)

    def test_join_commands_shape(self):
        rec = {"token": GOOD_TOKEN, "seed": "10.0.0.5:19949"}
        cmds = invites.join_commands(rec)
        self.assertEqual(len(cmds), 4)
        for c in cmds:
            # every command embeds the token inline (one-click copy/paste)
            self.assertIn(GOOD_TOKEN, c["cmd"])
            # no placeholder left for the operator to fill in
            self.assertNotIn("<path>", c["cmd"])
            self.assertIn("join", c["cmd"])


class TestInvitesHttp(unittest.TestCase):
    """HTTP surface: /api/invites requires a token even from trusted IPs,
    and accepts both the Bearer header and the URL-token path form."""

    @classmethod
    def setUpClass(cls):
        import threading
        from http.client import HTTPConnection
        from http.server import ThreadingHTTPServer

        import bridgepanel as bp
        from bridgepanel import invites as inv

        cls.HTTPConnection = HTTPConnection
        cls.bp = bp
        cls.inv = inv

        cls.tmp = tempfile.TemporaryDirectory()
        cls._orig_data_home = inv.data_home
        cls._orig_bs_ipc = inv.bs_ipc
        cls._orig_ts = inv._tailscale_ip4
        inv.data_home = lambda: Path(cls.tmp.name)
        inv.bs_ipc = lambda verb, timeout=2: (GOOD_TOKEN + "\n")
        inv._tailscale_ip4 = lambda: "100.99.0.7"

        cls.token = "httptoken-123456789012345"
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), bp.BridgePanelHandler)
        cls.server.bridgepanel_token = cls.token
        cls.server.trusted_ips = set()  # 127.0.0.1 is NOT trusted
        cls.port = cls.server.server_address[1]
        t = threading.Thread(target=cls.server.serve_forever, daemon=True)
        t.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.inv.data_home = cls._orig_data_home
        cls.inv.bs_ipc = cls._orig_bs_ipc
        cls.inv._tailscale_ip4 = cls._orig_ts
        cls.tmp.cleanup()

    def _req(self, method, path, body=None, token=None):
        c = self.HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = {}
        if token:
            headers["Authorization"] = "Bearer " + token
        if body is not None:
            headers["Content-Type"] = "application/json"
        c.request(method, path, body=body, headers=headers)
        r = c.getresponse()
        data = r.read().decode()
        c.close()
        return r.status, data

    def test_invites_requires_token_from_untrusted_ip(self):
        status, _ = self._req("GET", "/api/invites")
        self.assertEqual(status, 404)

    def test_invites_header_token(self):
        status, data = self._req("GET", "/api/invites", token=self.token)
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(data)["ok"])

    def test_invites_url_token_path(self):
        status, data = self._req("GET", "/" + self.token + "/api/invites")
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(data)["ok"])

    def test_invites_mint_url_token_path(self):
        status, data = self._req("POST", "/" + self.token + "/api/invites", "{}")
        self.assertEqual(status, 200)
        body = json.loads(data)
        self.assertTrue(body["ok"])
        self.assertEqual(body["invite"]["token"], GOOD_TOKEN)

    def test_invites_wrong_url_token_404(self):
        status, _ = self._req("GET", "/wrongtoken/api/invites")
        self.assertEqual(status, 404)

    def test_invites_page_requires_auth(self):
        status, _ = self._req(
            "POST", "/api/invites/page",
            json.dumps({"token": GOOD_TOKEN, "seed": "10.0.0.5:19949"}),
        )
        self.assertEqual(status, 404)

    def test_invites_page_renders_with_auth(self):
        status, data = self._req(
            "POST", "/api/invites/page",
            json.dumps({"token": GOOD_TOKEN, "seed": "10.0.0.5:19949"}),
            token=self.token,
        )
        self.assertEqual(status, 200)
        body = json.loads(data)
        self.assertTrue(body["ok"])
        # the rendered page embeds the token inline in copy/paste commands
        self.assertIn(GOOD_TOKEN, body["page"])
        # and no placeholder survives anywhere in the page
        self.assertNotIn("<path>", body["page"])

    def test_invites_page_rejects_bad_window(self):
        status, data = self._req(
            "POST", "/api/invites/page",
            json.dumps({"token": GOOD_TOKEN, "window_seconds": "abc"}),
            token=self.token,
        )
        self.assertEqual(status, 400)
        self.assertIn("window_seconds", data)  # reject() replies text/plain

    def test_invites_page_rejects_malformed_token(self):
        # must fail validation BEFORE rendering — never mint a page for garbage
        status, data = self._req(
            "POST", "/api/invites/page",
            json.dumps({"token": "zz-not-hex-!!", "seed": "10.0.0.5:19949"}),
            token=self.token,
        )
        self.assertEqual(status, 400)
        self.assertIn("token format", data)  # reject() replies text/plain

    def test_invites_denied_from_trusted_ip_without_token(self):
        # trusted_ips is normally empty in this class; allow 127.0.0.1 and
        # prove the invite endpoints STILL require the bearer/URL token.
        self.server.trusted_ips = {"127.0.0.1"}
        try:
            status, _ = self._req("GET", "/api/invites")
            self.assertEqual(status, 404)
            status, _ = self._req(
                "POST", "/api/invites/page",
                json.dumps({"token": GOOD_TOKEN}),
            )
            self.assertEqual(status, 404)
        finally:
            self.server.trusted_ips = set()


class TestJsCommandParity(unittest.TestCase):
    """The panel's embedded JS (invCommands) must mirror join_commands().

    Guards against the two generators drifting: extract invCommands from
    INDEX_HTML, evaluate it in isolation, and compare its output shape
    against the Python source of truth.
    """

    def test_js_invcommands_no_placeholder(self):
        from bridgepanel.panel_html import INDEX_HTML
        import re as _re
        m = _re.search(r"function invCommands\(rec\) \{(.*?)\n  \}", INDEX_HTML, _re.S)
        self.assertIsNotNone(m, "invCommands() not found in INDEX_HTML")
        js = m.group(0)
        # four command entries, none keeping a placeholder
        self.assertEqual(js.count("{lbl:"), 4)
        self.assertNotIn("--token-file", js)
        self.assertNotIn("<path>", js)

    def test_js_invcommands_match_python(self):
        """Stronger parity check: run the JS through node and compare with
        join_commands() output directly, when node is available."""
        import shutil
        node = shutil.which("node")
        if not node:
            self.skipTest("node not available")
        from bridgepanel import invites as inv
        from bridgepanel.panel_html import INDEX_HTML
        import re as _re
        import subprocess
        m = _re.search(r"function invCommands\(rec\) \{(.*?)\n  \}", INDEX_HTML, _re.S)
        js = m.group(0)
        script = js + "\nconst rec = {seed: '10.0.0.5:19949', token: 'c'.repeat(64)};\n" \
                      "console.log(JSON.stringify(invCommands(rec)));\n"
        out = subprocess.run([node, "-e", script], capture_output=True, text=True, timeout=10)
        self.assertEqual(out.returncode, 0, out.stderr)
        js_cmds = json.loads(out.stdout)
        self.assertEqual(len(js_cmds), 4)
        py_cmds = inv.join_commands({"token": "c" * 64, "seed": "10.0.0.5:19949"})
        for js_c, py_c in zip(js_cmds, py_cmds):
            self.assertEqual(js_c["cmd"], py_c["cmd"],
                             f"JS/Python drift: {js_c['lbl']!r} != {py_c['label']!r}")
            self.assertEqual(js_c["lbl"], py_c["label"])


if __name__ == "__main__":
    unittest.main()
