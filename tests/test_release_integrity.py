"""Behavioral tests for source archive VERSION validation."""
from __future__ import annotations

import os
import subprocess
import tarfile
import warnings
import zipfile
import hashlib
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


def publisher_fixture(tmp_path: Path):
    """Isolate the real publisher/validator from GitHub and platform tooling."""
    root = tmp_path / "repo"
    scripts = root / "scripts"
    scripts.mkdir(parents=True)
    (root / "VERSION").write_text(VERSION + "\n")
    for name in ("github-release.sh", "release-checksums.sh"):
        (scripts / name).write_bytes((ROOT / "scripts" / name).read_bytes())
    assets = root / "dist"
    assets.mkdir()
    make_archives(assets, VERSION + "\n")
    binary = assets / "bridgesessions-linux-x86_64"
    binary.write_text(f"#!/bin/sh\necho {VERSION}\n")
    binary.chmod(0o755)
    for name in ("bridgesessions-macos-arm64", "bridgesessions-windows-x86_64.exe", "bs_tray.ps1"):
        (assets / name).write_text(VERSION + "\n")
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    tools = {
        "git": 'case "$1" in status) exit 0;; *) echo abc123;; esac\n',
        "gh": '''if [ "$1 $2" = "release create" ]; then
    touch "$PUBLISH_LOG"
elif [ "$1 $2" = "release view" ]; then
    test -f "$PUBLISH_LOG"
fi
''',
        "file": '''case "$2" in
    *linux*) echo 'ELF 64-bit LSB executable, x86-64';;
    *macos*) echo 'Mach-O 64-bit executable arm64';;
    *windows*) echo 'PE32+ executable x86-64';;
esac
''',
    }
    for name, body in tools.items():
        path = fake_bin / name
        path.write_text("#!/bin/sh\n" + body)
        path.chmod(0o755)
    log = tmp_path / "published"
    env = os.environ.copy()
    env.update(PATH=str(fake_bin) + os.pathsep + env["PATH"],
               BS_RELEASE_DIR=str(assets), PUBLISH_LOG=str(log))
    subprocess.run(["bash", str(scripts / "release-checksums.sh")],
                   env=env, capture_output=True, text=True, check=True, timeout=20)
    return scripts, assets, env, log


def test_local_publisher_rejects_changed_staging_before_upload(tmp_path: Path):
    scripts, assets, env, log = publisher_fixture(tmp_path)
    (assets / "bs_tray.ps1").write_text("changed after checksums\n")
    result = subprocess.run(["bash", str(scripts / "github-release.sh")],
                            env=env, capture_output=True, text=True, timeout=20)
    assert result.returncode != 0
    assert "checksum did NOT match" in result.stderr
    assert not log.exists()


def test_local_publisher_rejects_wrong_version_with_matching_hashes(tmp_path: Path):
    scripts, assets, env, log = publisher_fixture(tmp_path)
    binary = assets / "bridgesessions-linux-x86_64"
    binary.write_text("#!/bin/sh\necho wrong-version\n")
    sums = assets / "SHA256SUMS"
    sums.write_text("".join(
        f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
        for path in assets.iterdir() if path != sums
    ))
    result = subprocess.run(["bash", str(scripts / "github-release.sh")],
                            env=env, capture_output=True, text=True, timeout=20)
    assert result.returncode != 0
    assert "version mismatch" in result.stderr
    assert not log.exists()


def test_local_publisher_validates_current_staging_before_upload(tmp_path: Path):
    scripts, _, env, log = publisher_fixture(tmp_path)
    result = subprocess.run(["bash", str(scripts / "github-release.sh")],
                            env=env, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, result.stderr
    assert "Validated" in result.stdout
    assert log.exists()
