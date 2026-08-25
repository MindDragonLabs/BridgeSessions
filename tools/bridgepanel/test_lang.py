#!/usr/bin/env python3
"""Language detection + file_kind (pdf/code) — TDD for Option C editor."""
from __future__ import annotations

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from bridgepanel.files import file_kind  # noqa: E402
from bridgepanel.lang import detect_language, languages  # noqa: E402


class TestDetectLanguage(unittest.TestCase):
    def test_extension_python(self):
        self.assertEqual(detect_language("worker.py"), "python")

    def test_extension_powershell(self):
        self.assertEqual(detect_language("Get-InboxRoot.ps1"), "powershell")

    def test_extension_json(self):
        self.assertEqual(detect_language("browse_roots.json"), "json")

    def test_special_filename_dockerfile(self):
        self.assertEqual(detect_language("Dockerfile"), "dockerfile")

    def test_special_filename_makefile(self):
        self.assertEqual(detect_language("Makefile"), "makefile")

    def test_shebang_python_beats_no_ext(self):
        self.assertEqual(detect_language("run", "#!/usr/bin/env python3"), "python")

    def test_shebang_bash(self):
        self.assertEqual(detect_language("setup", "#!/bin/bash"), "shell")

    def test_xml_prolog(self):
        self.assertEqual(detect_language("unknown", "<?xml version=\"1.0\"?>"), "xml")

    def test_override_wins(self):
        self.assertEqual(detect_language("foo.py", override="json"), "json")

    def test_unknown_is_plaintext(self):
        self.assertEqual(detect_language("notes.kangaroo"), "plaintext")

    def test_languages_includes_python(self):
        ids = {row["id"] for row in languages()}
        self.assertIn("python", ids)
        self.assertIn("powershell", ids)
        self.assertIn("plaintext", ids)


class TestFileKind(unittest.TestCase):
    def test_md_and_txt_stay_markdown(self):
        self.assertEqual(file_kind("README.md"), "md")
        self.assertEqual(file_kind("notes.txt"), "md")

    def test_pdf(self):
        self.assertEqual(file_kind("spec.pdf"), "pdf")

    def test_code(self):
        self.assertEqual(file_kind("api.py"), "code")
        self.assertEqual(file_kind("script.ps1"), "code")
        self.assertEqual(file_kind("Dockerfile"), "code")

    def test_image_and_video_unchanged(self):
        self.assertEqual(file_kind("shot.png"), "image")
        self.assertEqual(file_kind("clip.mp4"), "video")

    def test_dir(self):
        self.assertEqual(file_kind("src", True), "dir")


if __name__ == "__main__":
    unittest.main()
