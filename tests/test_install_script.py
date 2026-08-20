import os
import subprocess
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
INSTALL_SCRIPT = REPO_ROOT / "scripts" / "install.sh"


def make_fake_tools(tmp_path: Path, os_name: str, arch: str, payload: bytes = b"not an executable\n"):
    fake_bin = tmp_path / "fake-bin"
    fake_bin.mkdir()

    uname = fake_bin / "uname"
    uname.write_text(
        "#!/bin/sh\n"
        f"case \"$1\" in -s) printf '%s\\n' '{os_name}' ;; -m) printf '%s\\n' '{arch}' ;; *) exit 2 ;; esac\n"
    )
    uname.chmod(0o755)

    curl = fake_bin / "curl"
    curl.write_text(
        "#!/usr/bin/env python3\n"
        "import os, pathlib, sys\n"
        "args = sys.argv[1:]\n"
        "pathlib.Path(os.environ['CURL_LOG']).write_text(' '.join(args))\n"
        "dest = args[args.index('-o') + 1]\n"
        "pathlib.Path(dest).write_bytes(bytes.fromhex(os.environ['CURL_PAYLOAD_HEX']))\n"
    )
    curl.chmod(0o755)

    env = os.environ.copy()
    env.update(
        HOME=str(tmp_path / "home"),
        PATH=f"{fake_bin}:{env['PATH']}",
        CURL_LOG=str(tmp_path / "curl.log"),
        CURL_PAYLOAD_HEX=payload.hex(),
        BRIDGESESSIONS_TAG="v-test",
    )
    return env


def run_installer(tmp_path: Path, os_name: str, arch: str, payload: bytes = b"not an executable\n"):
    env = make_fake_tools(tmp_path, os_name, arch, payload)
    return subprocess.run(
        ["bash", str(INSTALL_SCRIPT)],
        text=True,
        capture_output=True,
        env=env,
        timeout=15,
    ), env


@pytest.mark.parametrize(
    ("os_name", "arch"),
    [("Linux", "aarch64"), ("Darwin", "x86_64")],
)
def test_installer_refuses_unsupported_arch_before_download(tmp_path: Path, os_name: str, arch: str):
    result, env = run_installer(tmp_path, os_name, arch)

    assert result.returncode != 0
    assert "Unsupported platform" in result.stderr
    assert not Path(env["CURL_LOG"]).exists()


def test_installer_does_not_record_unverified_payload(tmp_path: Path):
    result, _ = run_installer(tmp_path, "Linux", "x86_64")

    assert result.returncode != 0
    assert "SHA256SUMS has no valid entry" in result.stderr
    assert not (tmp_path / "home/.local/bin/.bridgesessions-version").exists()
