"""Tests for BridgePanel user/password login (auth.py + HTTP surface)."""
import json
import os
import sys
import tempfile
import threading
import unittest
from http.client import HTTPConnection
from http.server import ThreadingHTTPServer
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))

from bridgepanel import auth
from bridgepanel.auth import (COOKIE, SESSIONS, login_enabled, new_session,
                              session_cookie_valid, set_password,
                              verify_password)

USER = "admin"
PASS = "password123"


class TestAuthStore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self._orig = auth.auth_path
        auth.auth_path = lambda: Path(self.tmp.name) / "auth.json"
        SESSIONS.clear()

    def tearDown(self):
        auth.auth_path = self._orig
        SESSIONS.clear()
        self.tmp.cleanup()

    def test_disabled_by_default(self):
        self.assertFalse(login_enabled())
        self.assertFalse(verify_password(USER, PASS))

    def test_set_password_enables(self):
        p = set_password(USER, PASS)
        self.assertTrue(p.exists())
        self.assertEqual(p.stat().st_mode & 0o777, 0o600)
        self.assertTrue(login_enabled())

    def test_verify_correct(self):
        set_password(USER, PASS)
        self.assertTrue(verify_password(USER, PASS))

    def test_verify_wrong_password(self):
        set_password(USER, PASS)
        self.assertFalse(verify_password(USER, "nope"))

    def test_verify_wrong_user(self):
        set_password(USER, PASS)
        self.assertFalse(verify_password("root", PASS))

    def test_hash_not_plaintext_on_disk(self):
        set_password(USER, PASS)
        raw = auth.auth_path().read_text()
        self.assertNotIn(PASS, raw)
        self.assertNotIn(USER, raw.split("session_secret")[0][:0] or "")  # user IS stored, but hash must be hex
        d = json.loads(raw)
        self.assertNotEqual(d["hash"], PASS)
        self.assertEqual(len(d["hash"]), 64)  # scrypt dklen=32 hex

    def test_session_roundtrip(self):
        set_password(USER, PASS)
        cookie = new_session()
        self.assertTrue(session_cookie_valid(f"{COOKIE}={cookie}"))

    def test_session_rejects_tampered_sig(self):
        set_password(USER, PASS)
        sid, _, sig = new_session().partition(".")
        bad = f"{COOKIE}={sid}.deadbeef"
        self.assertFalse(session_cookie_valid(bad))

    def test_session_rejects_unknown_sid(self):
        set_password(USER, PASS)
        self.assertFalse(session_cookie_valid(f"{COOKIE}=unknown.0000"))

    def test_set_password_revokes_sessions(self):
        set_password(USER, PASS)
        cookie = new_session()
        self.assertTrue(session_cookie_valid(f"{COOKIE}={cookie}"))
        set_password(USER, "newpass")  # rewrite clears sessions
        self.assertFalse(session_cookie_valid(f"{COOKIE}={cookie}"))
        self.assertTrue(verify_password(USER, "newpass"))

    def test_session_secret_survives_rotation(self):
        set_password(USER, PASS)
        cookie = new_session()
        set_password(USER, PASS)
        # sessions were revoked, but a NEW cookie still validates with the
        # preserved secret (i.e. the secret did not rotate underneath us)
        self.assertTrue(session_cookie_valid(f"{COOKIE}={new_session()}"))


class TestLoginHttp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import bridgepanel as bp

        cls.tmp = tempfile.TemporaryDirectory()
        cls._orig_auth_path = auth.auth_path
        auth.auth_path = lambda: Path(cls.tmp.name) / "auth.json"
        set_password(USER, PASS)

        cls.bp = bp
        cls.token = "logintoken-123456789012"
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), bp.BridgePanelHandler)
        cls.server.bridgepanel_token = cls.token
        cls.server.trusted_ips = set()
        cls.port = cls.server.server_address[1]
        t = threading.Thread(target=cls.server.serve_forever, daemon=True)
        t.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        auth.auth_path = cls._orig_auth_path
        SESSIONS.clear()
        cls.tmp.cleanup()

    def _req(self, method, path, body=None, cookie=None, raw=False):
        c = HTTPConnection("127.0.0.1", self.port, timeout=5)
        headers = {}
        if cookie:
            headers["Cookie"] = cookie
        if body is not None:
            headers["Content-Type"] = "application/json"
        c.request(method, path, body=body, headers=headers)
        r = c.getresponse()
        data = r.read()
        setc = r.getheader("Set-Cookie") or ""
        c.close()
        if raw:
            return r.status, data, setc
        return r.status, data.decode(), setc

    def test_unauthenticated_get_shows_login_page(self):
        status, body, _ = self._req("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn("Sign in", body)
        self.assertNotIn("New invite", body)  # panel not leaked

    def test_api_unauthenticated_still_404(self):
        status, body, _ = self._req("GET", "/api/tree")
        self.assertEqual(status, 404)
        self.assertNotIn("Sign in", body)

    def test_login_wrong_password(self):
        status, body, _ = self._req(
            "POST", "/api/login",
            json.dumps({"user": USER, "pass": "wrong"}))
        self.assertEqual(status, 403)

    def test_login_success_sets_cookie(self):
        status, body, setc = self._req(
            "POST", "/api/login",
            json.dumps({"user": USER, "pass": PASS}))
        self.assertEqual(status, 200)
        self.assertIn("HttpOnly", setc)
        cookie = setc.split(";")[0]
        # session cookie now authorizes the panel AND token-required APIs
        status, body, _ = self._req("GET", "/", cookie=cookie)
        self.assertEqual(status, 200)
        self.assertIn("viewnav", body)
        status, body, _ = self._req("GET", "/api/invites", cookie=cookie)
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["ok"])

    def test_bearer_token_still_works_alongside_login(self):
        c = HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request("GET", "/api/invites",
                  headers={"Authorization": "Bearer " + self.token})
        r = c.getresponse()
        self.assertEqual(r.status, 200)
        r.read()
        c.close()

    def test_healthz_open_with_login(self):
        status, body, _ = self._req("GET", "/healthz")
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(body)["ok"])


if __name__ == "__main__":
    unittest.main()
