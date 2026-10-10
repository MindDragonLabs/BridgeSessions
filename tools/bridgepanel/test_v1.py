from __future__ import annotations

import base64
import os
import unittest
from unittest import mock
from pathlib import Path

import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from bridgepanel import api, v1


class TestV1IPC(unittest.TestCase):
    def test_session_input_uses_frozen_command_and_bytes(self):
        seen = []

        def fake(verb, timeout=0):
            seen.append(verb)
            return "OK\n"

        with mock.patch.object(api, "bs_ipc", side_effect=fake):
            result = api.daemon_session_input_v1(".", "codex", b"\x00\xff")
        self.assertTrue(result["ok"])
        self.assertEqual(seen, ["SESSION_INPUT . codex AP8="])

    def test_scrollback_returns_actual_bytes_and_reset(self):
        payload = base64.b64encode(b"\x00\xffout").decode()
        with mock.patch.object(api, "bs_ipc", return_value=f"OK 9 {payload} RESET\n") as ipc:
            result = api.daemon_session_scrollback_v1(".", "codex", 0, 64)
        self.assertTrue(result["ok"])
        self.assertEqual(base64.b64decode(result["text_b64"]), b"\x00\xffout")
        self.assertTrue(result["reset"])
        ipc.assert_called_once_with("SESSION_SCROLLBACK . codex 0 64", timeout=3.0)

    def test_bad_names_are_not_sent_to_ipc(self):
        with mock.patch.object(api, "bs_ipc") as ipc:
            result = api.daemon_session_kill_v1(".", "../escape")
        self.assertFalse(result["ok"])
        ipc.assert_not_called()


class TestV1Contract(unittest.TestCase):
    def test_local_and_remote_session_fixtures(self):
        with mock.patch.object(api, "bs_ipc", return_value="live codex state=up command=sh\n"):
            status, local = v1.sessions(".")
        self.assertEqual(status, 200)
        self.assertEqual(local["sessions"][0]["name"], "codex")
        tree = {"node": "local", "peers": [{"name": "remote", "sessions": [{"name": "hermes", "state": "up"}]}]}
        with mock.patch.object(api, "query_mesh_tree", return_value=tree):
            status, remote = v1.sessions("remote")
        self.assertEqual(status, 200)
        self.assertEqual(remote["sessions"][0]["name"], "hermes")

    def test_capabilities_do_not_claim_disconnected_control(self):
        with mock.patch.object(api, "bs_ipc", return_value=""):
            status, result = v1.capabilities()
        self.assertEqual(status, 200)
        self.assertFalse(result["backend"]["connected"])
        self.assertFalse(result["routes"]["session_input"])

    def test_strict_session_body_and_fixture_chat_lifecycle(self):
        with self.assertRaises(v1.V1Error):
            v1.create_session({"machine": ".", "name": "x", "command": "sh", "extra": 1})
        old = os.environ.get("BRIDGEPANEL_AGENT_FIXTURES")
        os.environ["BRIDGEPANEL_AGENT_FIXTURES"] = "1"
        try:
            status, created = v1.chat_post({
                "machine": ".", "agent": "fixture", "prompt": "hello", "request_id": "req-1",
            }, "test-device")
            self.assertEqual(status, 202)
            self.assertEqual(created["status"], "pending")
            for _ in range(50):
                __import__("time").sleep(0.005)
                status, current = v1.chat_get("req-1", "test-device")
                if current["status"] == "completed":
                    break
            self.assertEqual(current["status"], "completed")
            self.assertEqual(current["result"]["text"], "fixture response: hello")
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_AGENT_FIXTURES", None)
            else:
                os.environ["BRIDGEPANEL_AGENT_FIXTURES"] = old


if __name__ == "__main__":
    unittest.main()
