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
            self.assertIn("bridgesessions join", c["cmd"])
        self.assertIn(GOOD_TOKEN, cmds[1]["cmd"])


if __name__ == "__main__":
    unittest.main()
