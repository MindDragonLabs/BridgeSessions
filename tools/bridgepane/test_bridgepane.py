#!/usr/bin/env python3
from __future__ import annotations

import http.client
import importlib.util
import json
import os
import tempfile
import threading
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("bridgepane.py")
spec = importlib.util.spec_from_file_location("bridgepane", MODULE_PATH)
assert spec and spec.loader
bp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bp)


class BridgePaneUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.old_home = os.environ.get("BRIDGEPANE_HOME")
        self.old_config = os.environ.get("BRIDGEPANE_CONFIG")
        os.environ["BRIDGEPANE_HOME"] = str(Path(self.tmp.name) / "data")
        os.environ["BRIDGEPANE_CONFIG"] = str(Path(self.tmp.name) / "config")
        self.token = bp.ensure_dirs()

    def tearDown(self) -> None:
        if self.old_home is None:
            os.environ.pop("BRIDGEPANE_HOME", None)
        else:
            os.environ["BRIDGEPANE_HOME"] = self.old_home
        if self.old_config is None:
            os.environ.pop("BRIDGEPANE_CONFIG", None)
        else:
            os.environ["BRIDGEPANE_CONFIG"] = self.old_config
        self.tmp.cleanup()

    def test_safe_name_flattens_traversal(self) -> None:
        self.assertEqual(bp.safe_name("../../etc/passwd"), "passwd")
        self.assertEqual(bp.safe_name(r"..\..\evil<script>.md"), "evil_script_.md")

    def test_markdown_escapes_html_and_renders_table(self) -> None:
        source = "# Report\n\n<script>alert(1)</script>\n\n| A | B |\n|---|---|\n| 1 | 2 |"
        rendered = bp.markdown_to_html(source)
        self.assertIn("<h1>Report</h1>", rendered)
        self.assertIn("&lt;script&gt;alert(1)&lt;/script&gt;", rendered)
        self.assertNotIn("<script>", rendered)
        self.assertIn("<table>", rendered)

    def test_publish_updates_current(self) -> None:
        source = Path(self.tmp.name) / "report.md"
        source.write_text("# Done", encoding="utf-8")
        target = bp.publish(source)
        self.assertTrue(target.exists())
        self.assertEqual(bp.read_state()["current"], target.name)

    def test_resolve_rejects_symlink_escape(self) -> None:
        secret = Path(self.tmp.name) / "secret.txt"
        secret.write_text("secret", encoding="utf-8")
        link = bp.artifacts_dir() / "link.txt"
        link.symlink_to(secret)
        self.assertIsNone(bp.resolve_item("artifact", "link.txt"))


class BridgePaneHTTPTests(BridgePaneUnitTests):
    def setUp(self) -> None:
        super().setUp()
        self.server = bp.ThreadingHTTPServer(("127.0.0.1", 0), bp.BridgePaneHandler)
        self.server.bridgepane_token = self.token
        setattr(self.server, "trusted_ips", set())
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = self.server.server_address[1]

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        super().tearDown()

    def request(self, method: str, path: str, body: bytes | None = None, headers: dict | None = None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=3)
        conn.request(method, path, body=body, headers=headers or {})
        response = conn.getresponse()
        payload = response.read()
        result = (response.status, dict(response.getheaders()), payload)
        conn.close()
        return result

    def test_auth_gate_and_health(self) -> None:
        status, _, _ = self.request("GET", "/")
        self.assertEqual(status, 404)
        status, _, payload = self.request("GET", "/healthz")
        self.assertEqual(status, 200)
        self.assertTrue(json.loads(payload)["ok"])
        status, headers, payload = self.request("GET", f"/{self.token}/")
        self.assertEqual(status, 200)
        self.assertIn(b"BridgePane", payload)
        self.assertEqual(headers["Cache-Control"], "no-store")

    def test_upload_list_download_round_trip(self) -> None:
        boundary = "----bridgepane-test-boundary"
        content = b"hello from workstation\n"
        body = (
            f"--{boundary}\r\n"
            'Content-Disposition: form-data; name="file"; filename="../../hello.txt"\r\n'
            "Content-Type: text/plain\r\n\r\n"
        ).encode() + content + f"\r\n--{boundary}--\r\n".encode()
        status, _, payload = self.request(
            "POST",
            f"/{self.token}/upload",
            body,
            {"Content-Type": f"multipart/form-data; boundary={boundary}", "Content-Length": str(len(body))},
        )
        self.assertEqual(status, 201, payload)
        uploaded = json.loads(payload)
        self.assertEqual(uploaded["name"], "hello.txt")
        self.assertEqual((bp.inbox_dir() / "hello.txt").read_bytes(), content)

        status, _, payload = self.request("GET", f"/{self.token}/api/list")
        self.assertEqual(status, 200)
        self.assertIn("hello.txt", [item["name"] for item in json.loads(payload)["inbox"]])

        status, headers, payload = self.request("GET", f"/{self.token}/download?kind=inbox&name=hello.txt")
        self.assertEqual(status, 200)
        self.assertEqual(payload, content)
        self.assertTrue(headers["Content-Disposition"].startswith("attachment"))

    def test_trusted_tailnet_client_can_use_short_root_url(self) -> None:
        setattr(self.server, "trusted_ips", {"127.0.0.1"})
        status, _, payload = self.request("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(b"BridgePane", payload)
        status, _, payload = self.request("GET", "/api/list")
        self.assertEqual(status, 200)
        self.assertIn("artifacts", json.loads(payload))

    def test_traversal_cannot_read_outside_storage(self) -> None:
        outside = Path(self.tmp.name) / "outside.txt"
        outside.write_text("do not expose", encoding="utf-8")
        status, _, payload = self.request(
            "GET", f"/{self.token}/download?kind=artifact&name=..%2F..%2Foutside.txt"
        )
        self.assertEqual(status, 404)
        self.assertNotIn(b"do not expose", payload)

    def test_active_html_is_served_as_plain_text(self) -> None:
        page = bp.artifacts_dir() / "hostile.html"
        page.write_text("<script>alert(1)</script>", encoding="utf-8")
        status, headers, payload = self.request("GET", f"/{self.token}/raw?kind=artifact&name=hostile.html")
        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("text/plain"))
        self.assertIn(b"<script>", payload)


if __name__ == "__main__":
    unittest.main(verbosity=2)
