from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bridgepanel import auth


class TestAuthIsolated(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.old = os.environ.get("BRIDGEPANEL_CONFIG")
        os.environ["BRIDGEPANEL_CONFIG"] = self.tmp.name
        auth.SESSIONS.clear()
        auth._attempts.clear()

    def tearDown(self):
        auth.SESSIONS.clear()
        if self.old is None:
            os.environ.pop("BRIDGEPANEL_CONFIG", None)
        else:
            os.environ["BRIDGEPANEL_CONFIG"] = self.old
        self.tmp.cleanup()

    def test_password_cookie_and_cross_process_generation_reset(self):
        auth.set_password("admin", "correct horse battery")
        self.assertTrue(auth.verify_password("admin", "correct horse battery", "fixture"))
        cookie = auth.new_session()
        self.assertTrue(auth.session_cookie_valid(f"bp_session={cookie}"))
        auth.set_password("admin", "new correct horse")
        self.assertFalse(auth.session_cookie_valid(f"bp_session={cookie}"))

    def test_persisted_password_reset_revokes_cookie_across_process(self):
        auth.set_password("admin", "correct horse battery")
        cookie = auth.new_session()
        self.assertTrue(auth.session_cookie_valid(f"bp_session={cookie}"))
        env = os.environ.copy()
        env["BRIDGEPANEL_CONFIG"] = self.tmp.name
        env["PYTHONPATH"] = str(Path(__file__).resolve().parents[1]) + os.pathsep + env.get("PYTHONPATH", "")
        child = subprocess.run(
            [sys.executable, "-c", "from bridgepanel.auth import set_password; set_password('admin', 'reset from child process')"],
            env=env,
            timeout=20,
            check=False,
        )
        self.assertEqual(child.returncode, 0)
        self.assertFalse(auth.session_cookie_valid(f"bp_session={cookie}"))
        self.assertEqual(auth._load_auth()["generation"], 2)

    def test_malformed_persisted_config_fails_closed(self):
        path = Path(self.tmp.name) / "auth.json"
        path.write_text(json.dumps({"user": "admin", "salt": "00", "hash": "x"}))
        self.assertFalse(auth.login_enabled())
        self.assertFalse(auth.verify_password("admin", "anything", "fixture"))

    def test_forwarded_https_requires_a_trusted_proxy(self):
        # X-Forwarded-Proto is set by the client, so it must not be able to
        # mark the session cookie Secure (or not) on its own. Only a peer
        # listed in BRIDGESPANEL_TRUSTED_PROXY_IPS may assert an https hop.
        from bridgepanel.server import BridgePanelHandler

        class FakeServer:
            is_https = False

        def handler_for(client_ip, forwarded):
            h = object.__new__(BridgePanelHandler)
            h.server = FakeServer()
            h.client_address = (client_ip, 5555)
            h.headers = {"X-Forwarded-Proto": forwarded}
            return h

        env_name = "BRIDGESPANEL_TRUSTED_PROXY_IPS"
        previous = os.environ.get(env_name)
        try:
            os.environ.pop(env_name, None)
            self.assertFalse(handler_for("203.0.113.9", "https")._forwarded_https())
            os.environ[env_name] = "203.0.113.9"
            self.assertTrue(handler_for("203.0.113.9", "https")._forwarded_https())
            self.assertFalse(handler_for("203.0.113.9", "http")._forwarded_https())
            os.environ[env_name] = "198.51.100.1"
            self.assertFalse(handler_for("203.0.113.9", "https")._forwarded_https())
        finally:
            if previous is None:
                os.environ.pop(env_name, None)
            else:
                os.environ[env_name] = previous

    def test_audit_trail_records_credential_lifecycle(self):
        # S2 requires audit events for the credential lifecycle. The log must
        # identify the credential without ever containing the secret, because
        # it is written to disk in the config home.
        row = auth.issue_api_token("audit-phone", ["read"], ttl=60)
        self.assertTrue(auth.revoke_api_token(row["id"]))
        events = auth.read_audit()
        self.assertTrue(events, "no audit events were recorded")
        kinds = [e.get("event") for e in events]
        self.assertIn("token_issued", kinds)
        self.assertIn("token_revoked", kinds)
        blob = json.dumps(events)
        self.assertNotIn(row["token"], blob)
        issued = next(e for e in events if e.get("event") == "token_issued")
        self.assertEqual(issued["id"], row["id"])
        self.assertEqual(issued["label"], "audit-phone")
        self.assertEqual(issued["scopes"], ["read"])
        self.assertIn("ts", issued)
        # Written 0600: the log sits in the config home next to the token store.
        self.assertEqual(auth.audit_log_path().stat().st_mode & 0o777, 0o600)

    def test_audit_survives_a_malformed_line(self):
        auth.issue_api_token("before", ["read"], ttl=60)
        with auth.audit_log_path().open("a", encoding="utf-8") as handle:
            handle.write("this is not json\n")
        auth.issue_api_token("after", ["read"], ttl=60)
        labels = [e.get("label") for e in auth.read_audit()]
        self.assertIn("before", labels)
        self.assertIn("after", labels)

    def test_scoped_token_expiry_and_revoke(self):
        row = auth.issue_api_token("phone", ["read"], ttl=60)
        self.assertEqual(auth.credential_scope(row["token"]), {"read"})
        self.assertNotIn("write", auth.credential_scope(row["token"]))
        self.assertTrue(auth.revoke_api_token(row["id"]))
        self.assertEqual(auth.credential_scope(row["token"]), set())

    def test_concurrent_issue_and_revoke_preserve_every_transaction(self):
        rows = []
        rows_lock = threading.Lock()
        errors = []

        def issue(index):
            try:
                row = auth.issue_api_token(f"device-{index}", ["read"])
                with rows_lock:
                    rows.append(row)
            except Exception as exc:  # surface worker failures in the test thread
                errors.append(exc)

        workers = [threading.Thread(target=issue, args=(i,)) for i in range(24)]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join(20)
        self.assertFalse(any(worker.is_alive() for worker in workers))
        self.assertEqual(errors, [])
        self.assertEqual(len(rows), 24)
        self.assertEqual(len(auth.list_api_tokens()), 24)

        target = rows[0]
        revoker = threading.Thread(target=auth.revoke_api_token, args=(target["id"],))
        issuer = threading.Thread(target=issue, args=(99,))
        revoker.start()
        issuer.start()
        revoker.join(20)
        issuer.join(20)
        persisted = auth._load_tokens()
        self.assertEqual(len(persisted), 25)
        self.assertTrue(next(row for row in persisted if row["id"] == target["id"])["revoked"])
        self.assertEqual(len(auth.list_api_tokens()), 24)

    def test_rate_and_session_maps_are_bounded(self):
        with mock.patch.object(auth, "_MAX_ATTEMPT_CLIENTS", 2), mock.patch.object(auth, "_load_auth", return_value=None):
            for client in ("one", "two", "three"):
                self.assertFalse(auth.verify_password("admin", "wrong", client))
            self.assertEqual(len(auth._attempts), 2)
        auth.set_password("admin", "fixture password long")
        with mock.patch.object(auth, "_MAX_SESSIONS", 2):
            first = auth.new_session()
            auth.new_session()
            last = auth.new_session()
            self.assertEqual(len(auth.SESSIONS), 2)
            self.assertFalse(auth.session_cookie_valid("bp_session=" + first))
            self.assertTrue(auth.session_cookie_valid("bp_session=" + last))

    def test_malformed_token_scopes_and_expiry_fail_closed(self):
        row = auth.issue_api_token("phone", ["read"])
        rows = auth._load_tokens()
        rows[0]["scopes"] = ["admin"]
        auth._write_tokens(rows)
        self.assertEqual(auth.credential_scope(row["token"]), set())
        rows[0]["scopes"] = ["read"]
        rows[0]["expires_at"] = float("nan")
        auth._write_tokens(rows)
        self.assertEqual(auth.credential_scope(row["token"]), set())

    def test_concurrent_process_token_issuance_preserves_all_rows(self):
        env = os.environ.copy()
        env["BRIDGEPANEL_CONFIG"] = self.tmp.name
        env["PYTHONPATH"] = str(Path(__file__).resolve().parents[1]) + os.pathsep + env.get("PYTHONPATH", "")
        code = "from bridgepanel.auth import issue_api_token; issue_api_token('process', ['read'])"
        children = [
            subprocess.Popen([sys.executable, "-c", code], env=env)
            for _ in range(8)
        ]
        statuses = [child.wait(timeout=20) for child in children]
        self.assertEqual(statuses, [0] * len(children))
        self.assertEqual(len(auth._load_tokens()), len(children))


if __name__ == "__main__":
    unittest.main()
