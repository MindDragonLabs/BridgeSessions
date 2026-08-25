#!/usr/bin/env python3
"""Volume navigation — Phase 0/1 (inbox crumbs, named roots, path safety)."""
from __future__ import annotations

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from bridgepanel.panel_html import INDEX_HTML  # noqa: E402
from bridgepanel import volumes as vol  # noqa: E402


class TestInboxVolume(unittest.TestCase):
    def test_inbox_is_always_first_and_writable(self):
        v = vol.inbox_volume("avirserver2016")
        self.assertEqual(v["token"], "inbox")
        self.assertEqual(v["kind"], "inbox")
        self.assertTrue(v["writable"])
        self.assertEqual(v["group"], "primary")
        self.assertEqual(v["label"], "Inbox")

    def test_assemble_puts_inbox_first(self):
        disks = [
            vol.classify_windows_volume(
                "avirserver2016", "D:", 3, "ReFS",
                int(8378.9 * 1e9), int(4311.1 * 1e9), "New Volume",
                pins=("C", "D", "E"),
            ),
        ]
        out = vol.assemble_volumes("avirserver2016", disks)
        self.assertEqual(out[0]["token"], "inbox")
        self.assertEqual(out[1]["token"], "D")


class TestWindowsClassify(unittest.TestCase):
    def test_hides_system_reserved(self):
        self.assertIsNone(vol.classify_windows_volume(
            "avirserver2016", "F:", 3, "NTFS",
            int(0.5 * 1e9), int(0.1 * 1e9), "System Reserved",
        ))

    def test_hides_recovery_and_tiny(self):
        self.assertIsNone(vol.classify_windows_volume(
            "x", "R:", 3, "NTFS", 400_000_000, 100_000_000, "Recovery",
        ))

    def test_c_is_primary(self):
        v = vol.classify_windows_volume(
            "avirserver2016", "C:", 3, "NTFS",
            int(232.3 * 1e9), int(92.3 * 1e9), "",
            pins=("C", "D", "E"),
        )
        self.assertIsNotNone(v)
        self.assertEqual(v["token"], "C")
        self.assertEqual(v["kind"], "fixed")
        self.assertFalse(v["writable"])
        self.assertEqual(v["group"], "primary")
        self.assertEqual(v["os_path"], "C:\\")

    def test_d_pinned_stays_primary_despite_size(self):
        v = vol.classify_windows_volume(
            "avirserver2016", "D:", 3, "ReFS",
            int(8378.9 * 1e9), int(4311.1 * 1e9), "New Volume",
            pins=("C", "D", "E"),
        )
        self.assertEqual(v["group"], "primary")
        self.assertIn("New Volume", v["label"])

    def test_g_veeam_is_other_disks(self):
        v = vol.classify_windows_volume(
            "avirserver2016", "G:", 3, "ReFS",
            int(14899.9 * 1e9), int(14805.4 * 1e9), "VeeamRepo",
            pins=("C", "D", "E"),
        )
        self.assertEqual(v["group"], "other")
        self.assertIn("VeeamRepo", v["label"])


class TestLinuxClassify(unittest.TestCase):
    def test_hides_boot_and_tmpfs(self):
        self.assertIsNone(vol.classify_linux_mount(
            "fecv3", "/boot", "ext3", 988_700_000, 49_800_000,
        ))
        self.assertIsNone(vol.classify_linux_mount(
            "fecv3", "/run", "tmpfs", 16_000_000_000, 15_000_000_000,
        ))

    def test_root_and_srvnvme(self):
        root = vol.classify_linux_mount(
            "fecv3", "/", "ext4", int(869.1 * 1e9), int(369.4 * 1e9),
        )
        nvme = vol.classify_linux_mount(
            "fecv3", "/srvnvme", "ext4", int(1.3 * 1e12), int(1.0 * 1e12),
        )
        self.assertEqual(root["token"], "/")
        self.assertEqual(root["label"], "/")
        self.assertEqual(nvme["token"], "_srvnvme")
        self.assertEqual(nvme["os_path"], "/srvnvme")
        self.assertEqual(nvme["group"], "primary")


class TestPathSafety(unittest.TestCase):
    def test_inbox_still_uses_safe_relpath(self):
        self.assertEqual(vol.normalize_rel("inbox", "docs/a.md"), "docs/a.md")
        self.assertIsNone(vol.normalize_rel("inbox", "../etc/passwd"))
        self.assertIsNone(vol.normalize_rel("inbox", "C:\\Windows"))

    def test_rejects_dotdot_and_unc_and_ads(self):
        self.assertIsNone(vol.normalize_rel("C", r"..\Windows"))
        self.assertIsNone(vol.normalize_rel("C", "foo/../../Windows"))
        self.assertIsNone(vol.normalize_rel("C", r"\\host\share"))
        self.assertIsNone(vol.normalize_rel("C", r"\\?\C:\Windows"))
        self.assertIsNone(vol.normalize_rel("C", "file.txt:stream"))
        self.assertIsNone(vol.normalize_rel("C", "C:foo"))

    def test_accepts_normal_windows_rel(self):
        self.assertEqual(vol.normalize_rel("C", "Users/testadmin"), "Users/testadmin")
        self.assertEqual(vol.normalize_rel("D", r"Backups\daily"), "Backups/daily")

    def test_linux_rel(self):
        self.assertEqual(vol.normalize_rel("/", "etc"), "etc")
        self.assertEqual(vol.normalize_rel("_srvnvme", "iso/a.iso"), "iso/a.iso")
        self.assertIsNone(vol.normalize_rel("/", "../etc"))


class TestHiddenNames(unittest.TestCase):
    def test_identity_and_keys_are_hidden(self):
        for name in (
            "authorized_keys", "id_ed25519", "secret.pem", "tls.key",
            "ipc-token", "config.yaml",
        ):
            self.assertTrue(vol.is_hidden_name(name), name)

    def test_normal_files_visible(self):
        self.assertFalse(vol.is_hidden_name("notes.md"))
        self.assertFalse(vol.is_hidden_name("photo.png"))

    def test_filter_counts_hidden(self):
        items = [
            {"name": "notes.md", "dir": False},
            {"name": "authorized_keys", "dir": False},
            {"name": "id_rsa", "dir": False},
        ]
        kept, n = vol.filter_hidden(items)
        self.assertEqual([i["name"] for i in kept], ["notes.md"])
        self.assertEqual(n, 2)


class TestJoinOsPath(unittest.TestCase):
    def test_windows_join_stays_under_root(self):
        ok, err = vol.resolve_os_path("C:\\", "Users/testadmin", windows=True)
        self.assertEqual(err, "")
        self.assertTrue(ok.replace("/", "\\").upper().startswith("C:\\USERS\\TESTADMIN"))

    def test_windows_escape_rejected(self):
        ok, err = vol.resolve_os_path("D:\\", "../C:/Windows", windows=True)
        self.assertEqual(ok, "")
        self.assertEqual(err, "path_rejected")

    def test_linux_join(self):
        ok, err = vol.resolve_os_path("/srvnvme", "iso", windows=False)
        self.assertEqual(err, "")
        self.assertEqual(ok, "/srvnvme/iso")

    def test_linux_escape_rejected(self):
        ok, err = vol.resolve_os_path("/srvnvme", "../etc", windows=False)
        self.assertEqual(ok, "")
        self.assertEqual(err, "path_rejected")


class TestUploadRootGuard(unittest.TestCase):
    def test_non_inbox_root_is_write_forbidden(self):
        self.assertFalse(vol.root_writable("C"))
        self.assertFalse(vol.root_writable("/"))
        self.assertTrue(vol.root_writable("inbox"))
        self.assertTrue(vol.root_writable(""))

    def test_acl_marks_named_root_writable(self):
        import tempfile
        tmp = tempfile.TemporaryDirectory()
        old = os.environ.get("BRIDGEPANEL_CONFIG")
        os.environ["BRIDGEPANEL_CONFIG"] = tmp.name
        try:
            path = os.path.join(tmp.name, "browse_roots.json")
            with open(path, "w", encoding="utf-8") as fh:
                json.dump({
                    "fecv3": [{"path": "/tmp/outbox-test", "token": "outbox", "label": "Outbox", "writable": True}],
                }, fh)
            self.assertTrue(vol.root_writable("outbox", "fecv3"))
            self.assertFalse(vol.root_writable("C", "fecv3"))
            vols = vol.apply_acl("fecv3", [vol.inbox_volume("fecv3")])
            tokens = [v["token"] for v in vols]
            self.assertIn("outbox", tokens)
            extra = next(v for v in vols if v["token"] == "outbox")
            self.assertTrue(extra["writable"])
            self.assertEqual(extra["os_path"], "/tmp/outbox-test")
        finally:
            if old is None:
                os.environ.pop("BRIDGEPANEL_CONFIG", None)
            else:
                os.environ["BRIDGEPANEL_CONFIG"] = old
            tmp.cleanup()


class TestMediaHint(unittest.TestCase):
    def test_media_when_majority_images(self):
        items = [
            {"name": "a.png", "dir": False, "kind": "image"},
            {"name": "b.jpg", "dir": False, "kind": "image"},
            {"name": "c.md", "dir": False, "kind": "md"},
        ]
        self.assertEqual(vol.media_hint_from_items(items), "media")

    def test_mixed_when_docs(self):
        items = [
            {"name": "a.md", "dir": False, "kind": "md"},
            {"name": "b.md", "dir": False, "kind": "md"},
            {"name": "c.png", "dir": False, "kind": "image"},
        ]
        self.assertEqual(vol.media_hint_from_items(items), "mixed")


class TestHtmlPhase0(unittest.TestCase):
    def test_panel_has_volume_row_and_inbox_not_received(self):
        html = INDEX_HTML
        self.assertIn('id="volrow"', html)
        self.assertIn('selRoot', html)
        self.assertNotIn('label: "received"', html)
        self.assertNotIn('["received"]', html)
        self.assertNotIn(">received<", html)
        self.assertIn("/api/volumes", html)

    def test_phase2_tree_grid_and_keys(self):
        html = INDEX_HTML
        self.assertIn('id="treeToggle"', html)
        self.assertIn('id="gridToggle"', html)
        self.assertIn('id="tree"', html)
        self.assertIn("bp-tree-open", html)
        self.assertIn("Backspace", html)
        self.assertIn("toggleTree", html)
        self.assertIn("viewMode", html)

    def test_phase3_dest_hint(self):
        html = INDEX_HTML
        self.assertIn('id="destHint"', html)
        self.assertIn("rootIsWritable", html)
        self.assertIn("Uploads go to:", html)

    def test_listing_cache_ui(self):
        html = INDEX_HTML
        self.assertIn('id="listBanner"', html)
        self.assertIn("bp-list-cache", html)
        self.assertIn("refresh=1", html)
        self.assertIn('id="cmHost"', html)
        self.assertIn("detectLanguage", html)
        self.assertIn("/api/mkdir", html)


if __name__ == "__main__":
    unittest.main()
