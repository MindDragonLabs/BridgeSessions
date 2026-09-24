"""Behavioral tests for source archive VERSION validation."""
from __future__ import annotations

import os
import subprocess
import tarfile
import warnings
import zipfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / "VERSION").read_text(encoding="utf-8").strip()


def make_archives(directory: Path, embedded_version: str) -> None:
    member = f"bridgesessions-{VERSION}/VERSION"
    (directory / "payload").write_text(embedded_version, encoding="ascii")
    with zipfile.ZipFile(directory / f"bridgesessions-{VERSION}-source.zip", "w") as archive:
        archive.write(directory / "payload", member)
    with tarfile.open(directory / f"bridgesessions-{VERSION}-source.tar.gz", "w:gz") as archive:
        archive.add(directory / "payload", arcname=member)
    (directory / "payload").unlink()


def run_validator(directory: Path) -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env["BS_RELEASE_DIR"] = str(directory)
    return subprocess.run(
        ["bash", str(ROOT / "scripts/release-checksums.sh")],
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        timeout=20,
    )


def test_release_checksum_validator_accepts_exact_source_versions(tmp_path: Path):
    make_archives(tmp_path, VERSION + "\n")
    result = run_validator(tmp_path)
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "SHA256SUMS").is_file()
    assert (tmp_path / "SBOM-binaries.json").is_file()
    first_checksums = (tmp_path / "SHA256SUMS").read_bytes()
    first_sbom = (tmp_path / "SBOM-binaries.json").read_bytes()
    rerun = run_validator(tmp_path)
    assert rerun.returncode == 0, rerun.stderr
    assert (tmp_path / "SHA256SUMS").read_bytes() == first_checksums
    assert (tmp_path / "SBOM-binaries.json").read_bytes() == first_sbom


def test_release_checksum_validator_rejects_duplicate_source_version_member(tmp_path: Path):
    member = f"bridgesessions-{VERSION}/VERSION"
    archive_path = tmp_path / f"bridgesessions-{VERSION}-source.zip"
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", UserWarning)
        with zipfile.ZipFile(archive_path, "w") as archive:
            archive.writestr(member, VERSION + "\n")
            archive.writestr(member, VERSION + "\n")
    with tarfile.open(tmp_path / f"bridgesessions-{VERSION}-source.tar.gz", "w:gz") as archive:
        payload = tmp_path / "payload"
        payload.write_text(VERSION + "\n", encoding="ascii")
        archive.add(payload, arcname=member)
        payload.unlink()
    result = run_validator(tmp_path)
    assert result.returncode != 0
    assert "exactly one VERSION member" in result.stderr


@pytest.mark.parametrize("bad_content", ["wrong-version\n", VERSION + "\nextra\n", VERSION])
def test_release_checksum_validator_rejects_nonexact_source_versions(tmp_path: Path, bad_content: str):
    make_archives(tmp_path, bad_content)
    result = run_validator(tmp_path)
    assert result.returncode != 0
    assert "invalid source archive" in result.stderr or "version mismatch" in result.stderr
