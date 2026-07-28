"""Release-engineering tests for BridgeSessions.

All tests run in isolated temporary git repositories so they never touch the
real ``dist/`` directory or the public forge.  Network calls are forbidden:
the Codeberg release script is exercised only in ``--dry-run`` mode.
"""

from __future__ import annotations

import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import tarfile
import zipfile
import tempfile
import uuid
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
VERSION = (REPO_ROOT / "VERSION").read_text().strip()
TAG = f"v{VERSION}"

SCRIPTS = {
    "package": REPO_ROOT / "scripts" / "package-release.sh",
    "checksums": REPO_ROOT / "scripts" / "release-checksums.sh",
    "codeberg": REPO_ROOT / "scripts" / "codeberg-release.sh",
}


def run(cmd, cwd=None, check=True, env=None, **kw):
    """Run a command and return its CompletedProcess."""
    if env is None:
        env = os.environ.copy()
    # Ensure reproducible locale for the subprocess.
    env.setdefault("LC_ALL", "C")
    env.setdefault("LANG", "C")
    result = subprocess.run(
        cmd,
        cwd=cwd,
        capture_output=True,
        text=True,
        env=env,
        **kw,
    )
    if check and result.returncode != 0:
        raise subprocess.CalledProcessError(
            result.returncode,
            cmd,
            output=result.stdout,
            stderr=result.stderr,
        )
    return result


@pytest.mark.parametrize(
    ("name", "magic"),
    [
        ("bridgesessions-linux-x86_64", b"\x7fELF"),
        ("bridgesessions-macos-arm64", b"\xcf\xfa\xed\xfe"),
        ("bridgesessions-windows-x86_64.exe", b"MZ"),
    ],
)
def test_committed_dist_binary_matches_platform(name: str, magic: bytes):
    artifact = REPO_ROOT / "dist" / name
    assert artifact.is_file(), f"missing release artifact: {name}"
    assert artifact.read_bytes()[: len(magic)] == magic, (
        f"{name} has the wrong executable format"
    )


def copy_script_sources(repo: Path) -> None:
    """Copy the release scripts and supporting files into an isolated repo."""
    scripts_dir = repo / "scripts"
    scripts_dir.mkdir(exist_ok=True)
    for name, src in SCRIPTS.items():
        shutil.copy(src, scripts_dir / src.name)

    docs_dir = repo / "docs"
    docs_dir.mkdir(exist_ok=True)
    provenance_src = REPO_ROOT / "docs" / "RELEASE-PROVENANCE.md"
    if provenance_src.exists():
        shutil.copy(provenance_src, docs_dir / "RELEASE-PROVENANCE.md")

    # Minimal .gitattributes covering the paths the tests care about.
    (repo / ".gitattributes").write_text(
        "/dist/** export-ignore\n/build/** export-ignore\n"
    )


def make_repo(tmp_path: Path, extra_files: dict[str, str] | None = None) -> Path:
    """Create an isolated git repo with the release scripts and VERSION."""
    repo = tmp_path / "repo"
    repo.mkdir()
    run(["git", "init"], cwd=repo)
    run(["git", "config", "user.email", "release-test@example.com"], cwd=repo)
    run(["git", "config", "user.name", "Release Test"], cwd=repo)

    copy_script_sources(repo)
    (repo / "VERSION").write_text(VERSION + "\n")
    (repo / "bs-protocol.h").write_text(
        f'// BridgeSessions {VERSION}\nint main(){{return 0;}}\n'
    )
    if extra_files:
        for rel, content in extra_files.items():
            path = repo / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)

    run(["git", "add", "-A"], cwd=repo)
    run(["git", "commit", "-m", "initial"], cwd=repo)
    return repo


# ─────────────────────────────────────────────────────────────────────────────
# Static script checks
# ─────────────────────────────────────────────────────────────────────────────


@pytest.mark.parametrize("script", SCRIPTS.values(), ids=SCRIPTS.keys())
def test_bash_syntax(script: Path):
    run(["bash", "-n", str(script)])


@pytest.mark.parametrize("script", SCRIPTS.values(), ids=SCRIPTS.keys())
def test_shellcheck(script: Path):
    if shutil.which("shellcheck") is None:
        pytest.skip("shellcheck not installed")
    run(["shellcheck", "--severity=warning", str(script)])


# ─────────────────────────────────────────────────────────────────────────────
# package-release.sh
# ─────────────────────────────────────────────────────────────────────────────


def archive_path(repo: Path) -> Path:
    return repo / "dist" / f"bridgesessions-{VERSION}-source.tar.gz"


def zip_archive_path(repo: Path) -> Path:
    return repo / "dist" / f"bridgesessions-{VERSION}-source.zip"


def package_cmd(repo: Path, *args: str) -> list[str]:
    return ["bash", str(repo / "scripts" / "package-release.sh"), *args]


def test_package_release_mode_success(tmp_path: Path):
    repo = make_repo(tmp_path)
    run(["git", "tag", TAG], cwd=repo)
    run(package_cmd(repo, "--release"), cwd=repo)
    assert archive_path(repo).is_file()
    assert zip_archive_path(repo).is_file()


def test_package_release_refuses_dirty_tree(tmp_path: Path):
    repo = make_repo(tmp_path)
    run(["git", "tag", TAG], cwd=repo)
    (repo / "bs-protocol.h").write_text("// dirty\n")
    result = run(package_cmd(repo, "--release"), cwd=repo, check=False)
    assert result.returncode != 0
    assert "dirty" in result.stderr.lower()


def test_package_release_refuses_untracked(tmp_path: Path):
    repo = make_repo(tmp_path)
    run(["git", "tag", TAG], cwd=repo)
    (repo / "untracked.log").write_text("noise\n")
    result = run(package_cmd(repo, "--release"), cwd=repo, check=False)
    assert result.returncode != 0
    assert "untracked" in result.stderr.lower()


def test_package_release_refuses_wrong_tag(tmp_path: Path):
    repo = make_repo(tmp_path)
    run(["git", "tag", "v2.0.5"], cwd=repo)
    result = run(package_cmd(repo, "--release"), cwd=repo, check=False)
    assert result.returncode != 0
    assert "tag" in result.stderr.lower()


def test_package_release_commit_override(tmp_path: Path):
    repo = make_repo(tmp_path)
    # Tag an earlier commit, then move HEAD forward and dirty the tree.
    commit_a = run(["git", "rev-parse", "HEAD"], cwd=repo).stdout.strip()
    (repo / "VERSION").write_text(VERSION + "\n")
    (repo / "new.txt").write_text("new\n")
    run(["git", "add", "-A"], cwd=repo)
    run(["git", "commit", "-m", "second"], cwd=repo)
    (repo / "new.txt").write_text("dirty\n")
    (repo / "untracked.log").write_text("noise\n")

    # Dev-mode override should archive commit_a regardless of current state.
    run(package_cmd(repo, "--commit", commit_a), cwd=repo)
    assert archive_path(repo).is_file()
    with tarfile.open(archive_path(repo), "r:gz") as tf:
        names = tf.getnames()
        assert any(f"bridgesessions-{VERSION}/new.txt" in n for n in names) is False
        assert any(f"bridgesessions-{VERSION}/bs-protocol.h" in n for n in names)


def test_package_archive_excludes_export_ignored_paths(tmp_path: Path):
    repo = make_repo(tmp_path)
    # Tracked files under /dist and /build must be excluded from the archive.
    (repo / "dist").mkdir(exist_ok=True)
    (repo / "dist" / "stale.bin").write_text("old\n")
    (repo / "build").mkdir(exist_ok=True)
    (repo / "build" / "artifact.o").write_text("obj\n")
    run(["git", "add", "-A"], cwd=repo)
    run(["git", "commit", "-m", "add ignored"], cwd=repo)
    run(["git", "tag", TAG], cwd=repo)

    run(package_cmd(repo, "--release"), cwd=repo)
    with tarfile.open(archive_path(repo), "r:gz") as tf:
        names = tf.getnames()
    assert not any(n.startswith(f"bridgesessions-{VERSION}/dist/") for n in names)
    assert not any(n.startswith(f"bridgesessions-{VERSION}/build/") for n in names)


def test_package_reproducible_across_umask(tmp_path: Path):
    repo = make_repo(tmp_path)
    run(["git", "tag", TAG], cwd=repo)

    # Use an isolated gitconfig so we never touch the operator's global config.
    gitconfig = tmp_path / "gitconfig"
    gitconfig.write_text("[safe]\n\tdirectory = *\n")

    def build_with_umask(umask: int) -> tuple[str, str]:
        work = tmp_path / f"u{umask}"
        # Clone rather than copytree so git sees a clean, consistent worktree.
        run(["git", "clone", str(repo), str(work)], cwd=tmp_path)
        env = os.environ.copy()
        env["UMASK"] = str(umask)
        env["GIT_CONFIG_GLOBAL"] = str(gitconfig)
        script = (
            "umask $UMASK; "
            f"bash '{work}/scripts/package-release.sh' --release"
        )
        run(["bash", "-c", script], cwd=work, env=env)
        return (
            hashlib.sha256(archive_path(work).read_bytes()).hexdigest(),
            hashlib.sha256(zip_archive_path(work).read_bytes()).hexdigest(),
        )

    hash022 = build_with_umask("022")
    hash077 = build_with_umask("077")

    assert hash022 == hash077, "archive checksum differs across umasks"


def test_zip_archive_contains_tracked_source(tmp_path: Path):
    repo = make_repo(tmp_path)
    run(["git", "tag", TAG], cwd=repo)
    run(package_cmd(repo, "--release"), cwd=repo)
    with zipfile.ZipFile(zip_archive_path(repo)) as archive:
        names = archive.namelist()
    assert f"bridgesessions-{VERSION}/bs-protocol.h" in names


# ─────────────────────────────────────────────────────────────────────────────
# In-archive provenance: source archive must not reference its own hash.
# ─────────────────────────────────────────────────────────────────────────────


def test_provenance_not_self_referential(tmp_path: Path):
    repo = make_repo(tmp_path)
    (repo / "docs" / "RELEASE-PROVENANCE.md").write_text(
        "# Provenance\nSee dist/SHA256SUMS.\n"
    )
    run(["git", "add", "-A"], cwd=repo)
    run(["git", "commit", "-m", "provenance"], cwd=repo)
    run(["git", "tag", TAG], cwd=repo)
    run(package_cmd(repo, "--release"), cwd=repo)

    with tarfile.open(archive_path(repo), "r:gz") as tf:
        provenance = tf.extractfile(
            f"bridgesessions-{VERSION}/docs/RELEASE-PROVENANCE.md"
        )
        assert provenance is not None
        text = provenance.read().decode()

    # The in-archive provenance must not contain a 64-hex hash next to the
    # source archive name.
    self_ref = re.search(
        r"[a-f0-9]{64}\s+bridgesessions-[^\s]*-source\.tar\.gz", text, re.I
    )
    assert self_ref is None, "in-archive provenance contains source-archive hash"


# ─────────────────────────────────────────────────────────────────────────────
# release-checksums.sh
# ─────────────────────────────────────────────────────────────────────────────


def make_dist(repo: Path) -> dict[str, Path]:
    """Create a plausible dist/ with source archive + three platform binaries."""
    dist = repo / "dist"
    dist.mkdir(exist_ok=True)

    # Source archive containing VERSION at the expected prefix path.
    src = dist / f"bridgesessions-{VERSION}-source.tar.gz"
    with tarfile.open(src, "w:gz") as tf:
        data = VERSION.encode()
        info = tarfile.TarInfo(name=f"bridgesessions-{VERSION}/VERSION")
        info.size = len(data)
        tf.addfile(info, io.BytesIO(data))

    # Native-looking binary names; content just needs the version string.
    names = [
        "bridgesessions-linux-x86_64",
        "bridgesessions-macos-arm64",
        "bridgesessions-windows-x86_64.exe",
    ]
    files = {"source": src}
    for name in names:
        path = dist / name
        path.write_text(f"ELF executable {VERSION}\n")
        files[name] = path
    return files


def test_checksums_generates_sha256sums_and_sbom(tmp_path: Path):
    repo = make_repo(tmp_path)
    files = make_dist(repo)
    run(["bash", str(repo / "scripts" / "release-checksums.sh")], cwd=repo)

    sums = repo / "dist" / "SHA256SUMS"
    sbom = repo / "dist" / "SBOM-binaries.json"
    assert sums.is_file()
    assert sbom.is_file()

    lines = sums.read_text().splitlines()
    basenames = [line.split(None, 1)[1] for line in lines if line.strip()]

    # SHA256SUMS must never list itself.
    assert "SHA256SUMS" not in basenames

    # All artifacts must be listed, plus the SBOM (which is hashed after it is
    # generated).
    for key in files:
        assert files[key].name in basenames
    assert "SBOM-binaries.json" in basenames

    # Verify checksum file parses and matches.
    run(["sha256sum", "-c", str(sums)], cwd=repo / "dist")


def test_sbom_has_valid_uuid_and_lists_artifacts(tmp_path: Path):
    repo = make_repo(tmp_path)
    make_dist(repo)
    run(["bash", str(repo / "scripts" / "release-checksums.sh")], cwd=repo)

    sbom = json.loads((repo / "dist" / "SBOM-binaries.json").read_text())
    assert sbom["bomFormat"] == "CycloneDX"
    assert sbom["specVersion"] == "1.5"

    serial = sbom["serialNumber"]
    assert serial.startswith("urn:uuid:")
    uuid_part = serial[len("urn:uuid:") :]
    parsed = uuid.UUID(uuid_part)
    assert parsed != uuid.UUID("00000000-0000-4000-8000-000000000000")
    assert parsed.version == 4

    artifact_names = {c["name"] for c in sbom["components"]}
    assert "SHA256SUMS" not in artifact_names
    assert "SBOM-binaries.json" not in artifact_names
    assert f"bridgesessions-{VERSION}-source.tar.gz" in artifact_names
    assert "bridgesessions-linux-x86_64" in artifact_names


def test_sbom_uuid_changes_each_run(tmp_path: Path):
    repo = make_repo(tmp_path)
    make_dist(repo)

    def serial() -> str:
        run(["bash", str(repo / "scripts" / "release-checksums.sh")], cwd=repo)
        data = json.loads((repo / "dist" / "SBOM-binaries.json").read_text())
        return data["serialNumber"]

    first = serial()
    second = serial()
    assert first != second


# ─────────────────────────────────────────────────────────────────────────────
# codeberg-release.sh
# ─────────────────────────────────────────────────────────────────────────────


def make_codeberg_repo(tmp_path: Path):
    """Return (local_repo, bare_remote) ready for dry-run release tests."""
    repo = make_repo(tmp_path)
    (repo / "docs" / f"RELEASE-NOTES-{VERSION}.md").write_text(
        f"# Release notes {VERSION}\n"
    )
    run(["git", "add", "-A"], cwd=repo)
    run(["git", "commit", "-m", "notes"], cwd=repo)
    run(["git", "tag", TAG], cwd=repo)

    remote = tmp_path / "remote.git"
    remote.mkdir()
    run(["git", "init", "--bare"], cwd=remote)
    run(["git", "remote", "add", "codeberg", str(remote)], cwd=repo)
    run(["git", "push", "codeberg", TAG], cwd=repo)

    make_dist(repo)
    return repo, remote


def codeberg_cmd(repo: Path, *args: str) -> list[str]:
    return ["bash", str(repo / "scripts" / "codeberg-release.sh"), *args]


def test_codeberg_dry_run_no_network(tmp_path: Path):
    repo, _remote = make_codeberg_repo(tmp_path)

    # Put a fake curl first in PATH; if the script calls it, the test fails.
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    (fake_bin / "curl").write_text("#!/bin/sh\necho 'curl was called' >&2; exit 1\n")
    (fake_bin / "curl").chmod(0o755)
    env = os.environ.copy()
    env["PATH"] = f"{fake_bin}:{env['PATH']}"
    env["CODEBERG_REMOTE"] = "codeberg"

    result = run(
        codeberg_cmd(repo, "--dry-run", TAG),
        cwd=repo,
        env=env,
    )
    out = result.stdout + result.stderr
    assert "draft" in out.lower()
    assert "upload" in out.lower()
    assert "publish" in out.lower()
    assert "bridgesessions-linux-x86_64" in out
    assert "bridgesessions-macos-arm64" in out
    assert "bridgesessions-windows-x86_64.exe" in out
    assert "curl was called" not in out


def test_codeberg_draft_only_dry_run_never_publishes(tmp_path: Path):
    repo, _remote = make_codeberg_repo(tmp_path)
    env = os.environ.copy()
    env["CODEBERG_REMOTE"] = "codeberg"

    result = run(
        codeberg_cmd(repo, "--dry-run", "--draft-only", TAG),
        cwd=repo,
        env=env,
    )
    out = result.stdout + result.stderr
    assert "leave release" in out.lower()
    assert "as draft" in out.lower()
    assert "publish release" not in out.lower()


def test_codeberg_refuses_tag_commit_mismatch(tmp_path: Path):
    repo, _remote = make_codeberg_repo(tmp_path)

    # Move local tag to a new commit while remote tag stays at the old commit.
    (repo / "extra.txt").write_text("moved\n")
    run(["git", "add", "-A"], cwd=repo)
    run(["git", "commit", "-m", "moved"], cwd=repo)
    run(["git", "tag", "-f", TAG], cwd=repo)

    env = os.environ.copy()
    env["CODEBERG_REMOTE"] = "codeberg"
    result = run(codeberg_cmd(repo, "--dry-run", TAG), cwd=repo, env=env, check=False)
    assert result.returncode != 0
    assert "tag" in (result.stdout + result.stderr).lower()


def test_codeberg_refuses_tag_different_from_version(tmp_path: Path):
    repo, _remote = make_codeberg_repo(tmp_path)
    env = os.environ.copy()
    env["CODEBERG_REMOTE"] = "codeberg"
    result = run(codeberg_cmd(repo, "--dry-run", "v2.0.5"), cwd=repo, env=env, check=False)
    assert result.returncode != 0
    assert "version" in (result.stdout + result.stderr).lower()


def test_codeberg_dry_run_requires_remote_tag(tmp_path: Path):
    repo, _remote = make_codeberg_repo(tmp_path)
    run(["git", "push", "codeberg", f":refs/tags/{TAG}"], cwd=repo)
    env = os.environ.copy()
    env["CODEBERG_REMOTE"] = "codeberg"
    result = run(codeberg_cmd(repo, "--dry-run", TAG), cwd=repo, env=env, check=False)
    assert result.returncode != 0
    assert "remote tag" in (result.stdout + result.stderr).lower()


def test_codeberg_script_verifies_uploaded_bytes_before_publish():
    text = SCRIPTS["codeberg"].read_text()
    assert "browser_download_url" in text
    assert "cmp -s" in text
    assert '"target_commitish": commit' in text


def test_codeberg_requires_notes_file(tmp_path: Path):
    repo, _remote = make_codeberg_repo(tmp_path)
    (repo / f"docs/RELEASE-NOTES-{VERSION}.md").unlink()
    run(["git", "add", "-A"], cwd=repo)
    run(["git", "commit", "-m", "remove notes"], cwd=repo)
    run(["git", "tag", "-f", TAG], cwd=repo)
    run(["git", "push", "-f", "codeberg", TAG], cwd=repo)

    env = os.environ.copy()
    env["CODEBERG_REMOTE"] = "codeberg"
    result = run(codeberg_cmd(repo, "--dry-run", TAG), cwd=repo, env=env, check=False)
    assert result.returncode != 0
    assert "notes" in (result.stdout + result.stderr).lower()
