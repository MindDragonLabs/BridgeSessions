"""Source-tree release policy tests.

Generated binaries are staged locally and published as GitHub Release assets;
they are deliberately absent from git.
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def tracked_files() -> set[str]:
    out = subprocess.check_output(
        ["git", "ls-files"], cwd=ROOT, text=True
    )
    return set(out.splitlines())


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_generated_release_artifacts_are_not_tracked():
    tracked = tracked_files()
    forbidden_prefixes = ("dist/", "release/")
    assert not [path for path in tracked if path.startswith(forbidden_prefixes)]
    assert "BSMenubar/BSMenubar-bin" not in tracked


def test_release_staging_is_ignored():
    text = read(".gitignore")
    assert "/dist/" in text
    assert "/release/" in text
    assert "!dist/" not in text


def test_release_scripts_exist_and_parse():
    scripts = [
        "scripts/package-release.sh",
        "scripts/release-checksums.sh",
        "scripts/github-release.sh",
        "scripts/install.sh",
    ]
    for script in scripts:
        path = ROOT / script
        assert path.is_file(), f"missing {script}"
        subprocess.run(["bash", "-n", str(path)], check=True)


def test_github_release_script_is_fail_closed():
    text = read("scripts/github-release.sh")
    for required in (
        "git status --porcelain",
        "git rev-parse HEAD",
        "git ls-remote origin",
        "gh release create",
        "--verify-tag",
        "SHA256SUMS",
        "SBOM-binaries.json",
    ):
        assert required in text
    assert "codeberg" not in text.lower()


def test_installers_use_release_assets_and_mandatory_hashes():
    sh = read("scripts/install.sh")
    ps = read("scripts/install.ps1")
    assert "github.com/MindDragonLabs/BridgeSessions/releases/download/v${TAG}" in sh
    assert "refusing unverified binary" in sh
    assert sh.index("SHA-256 verified") < sh.index('REPORTED_VERSION=$("${TMP_BIN}" --version)')
    assert "github.com/MindDragonLabs/BridgeSessions/releases/download/v$TAG" in ps
    assert "Get-FileHash" in ps
    assert "Move-Item $TMP_PATH $BIN_PATH -Force" in ps


def test_static_builder_uses_supported_dependency_lines():
    text = read("scripts/Dockerfile.static-linux")
    assert "openssl-3.5.7" in text
    assert "spdlog-1.17.0" in text
    assert "fmt-12.2.0" in text
    assert "Catch2-3.8.0" not in text
    assert "openssl-3.3.2" not in text
    assert "spdlog-1.15.0" not in text


def test_version_is_single_line_and_cmake_reads_it():
    version = read("VERSION").strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+(?:-[A-Za-z0-9.-]+)?", version)
    cmake = read("CMakeLists.txt")
    assert 'file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"' in cmake
    assert 'add_compile_definitions(BS_VERSION="${BRIDGESESSIONS_VERSION}")' in cmake



def test_release_automation_is_executable():
    for rel in ("scripts/github-release.sh", "scripts/package-release.sh"):
        assert os.access(ROOT / rel, os.X_OK), f"{rel} must be executable"
