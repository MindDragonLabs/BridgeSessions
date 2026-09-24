"""Exercise installer validation-before-stop and binary-swap rollback."""
from __future__ import annotations

import os
import hashlib
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PAYLOAD = b"#!/bin/sh\necho ${REPORTED_VERSION:-test}\n"


def prepare(tmp_path: Path, reported_version: str, fail_swap: bool = False, stale_marker: bool = False):
    fake = tmp_path / "bin"
    fake.mkdir()
    home = tmp_path / "home"
    install = home / ".local/bin"
    install.mkdir(parents=True)
    binary = install / "bridgesessions"
    binary.write_bytes(b"previous-good-binary")
    binary.chmod(0o755)
    (install / ".bridgesessions-version").write_text("vtest\n" if stale_marker else "old-version\n")
    if stale_marker:
        (install / ".bridgesessions-artifact-sha256").write_text(
            hashlib.sha256(binary.read_bytes()).hexdigest() + "\n"
        )

    def tool(name: str, text: str):
        path = fake / name
        path.write_text("#!/bin/sh\n" + text)
        path.chmod(0o755)

    tool("uname", "case $1 in -s) echo Linux;; -m) echo x86_64;; *) exit 2;; esac\n")
    tool("file", "echo 'ELF 64-bit LSB pie executable, x86-64'\n")
    tool("systemctl", "echo \"$*\" >> \"$SYSTEM_LOG\"; [ \"$3\" = is-active ] && exit 1; exit 0\n")
    tool("pgrep", "exit 1\n")
    tool("pkill", "exit 0\n")
    tool("sleep", "exit 0\n")
    tool(
        "curl",
        """python3 - "$@" <<'PY'
import hashlib, os, pathlib, sys
args = sys.argv[1:]
url = next(a for a in args if a.startswith('https://'))
dest = pathlib.Path(args[args.index('-o') + 1])
payload = os.environ['PAYLOAD'].encode()
if url.endswith('/SHA256SUMS'):
    dest.write_text(hashlib.sha256(payload).hexdigest() + '  bridgesessions-linux-x86_64\\n')
else:
    dest.write_bytes(payload)
PY
""",
    )
    tool(
        "mv",
        """src=$1
[ \"$src\" = -f ] && src=$2
if [ \"${FAIL_SWAP:-0}\" = 1 ] && echo \"$src\" | grep -q '\\.download\\.'; then exit 1; fi
exec /bin/mv \"$@\"
""",
    )
    env = os.environ.copy()
    env.update(
        HOME=str(home),
        PATH=f"{fake}:{env['PATH']}",
        SYSTEM_LOG=str(tmp_path / "system.log"),
        PAYLOAD=PAYLOAD.decode(),
        REPORTED_VERSION=reported_version,
        BRIDGESESSIONS_TAG="vtest",
        FAIL_SWAP="1" if fail_swap else "0",
    )
    return subprocess.run(
        ["bash", str(ROOT / "scripts/install.sh")],
        text=True,
        capture_output=True,
        env=env,
        timeout=20,
    ), binary, install, Path(env["SYSTEM_LOG"])


def test_installer_does_not_stop_daemon_until_candidate_is_fully_validated(tmp_path: Path):
    result, binary, install, log = prepare(tmp_path, "wrong-version")
    assert result.returncode != 0
    assert "downloaded binary reports" in result.stderr
    assert "stop" not in log.read_text() if log.exists() else True
    assert binary.read_bytes() == b"previous-good-binary"
    assert (install / ".bridgesessions-version").read_text() == "old-version\n"


def test_same_version_marker_does_not_skip_changed_release_artifact(tmp_path: Path):
    result, binary, install, log = prepare(tmp_path, "wrong-version", stale_marker=True)
    assert result.returncode != 0
    assert "downloaded binary reports" in result.stderr
    assert binary.read_bytes() == b"previous-good-binary"
    assert (install / ".bridgesessions-version").read_text() == "vtest\n"
    assert not log.exists() or "stop" not in log.read_text()


def test_installer_restores_old_binary_when_atomic_swap_fails(tmp_path: Path):
    result, binary, install, _ = prepare(tmp_path, "test", fail_swap=True)
    assert result.returncode != 0
    assert "previous installation restored" in result.stderr
    assert binary.read_bytes() == b"previous-good-binary"
    assert (install / ".bridgesessions-version").read_text() == "old-version\n"


def test_daemon_stop_pattern_does_not_match_session_workers():
    script = (ROOT / "scripts/install.sh").read_text()
    match = re.search(r"DAEMON_PGREP_PATTERN='([^']+)'", script)
    assert match, "installer must use one narrowly scoped daemon process pattern"
    pattern = match.group(1)
    assert re.search(pattern, "/home/user/.local/bin/bridgesessions --daemon --config /home/user/.bridgesessions/config")
    assert not re.search(pattern, "/home/user/.local/bin/bridgesessions --session-worker --name tty --command 'echo --config /tmp/config'")
    assert not re.search(pattern, "/home/user/.local/bin/bridgesessions --cua-helper")
