"""Tests for the private BridgeSessions invite-page generator."""

import importlib.util
import stat
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT = REPO_ROOT / "scripts" / "generate-invite-page.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("generate_invite_page", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_parse_invite_output_extracts_token_and_seed_address():
    generator = load_generator()
    output = """Invite (valid 2h):  953f674ebb576634309e085bece7d482
One-liner:
  bridgesessions join 203.0.113.10:19949 --token-file <path> --start
"""

    invite = generator.parse_invite_output(output)

    assert invite.token == "953f674ebb576634309e085bece7d482"
    assert invite.seed_address == "203.0.113.10:19949"
    assert invite.window_seconds == 7200


@pytest.mark.parametrize(
    "output",
    [
        "Invite (valid 2h): not-a-token\nbridgesessions join host:19949 - --start\n",
        "Invite (valid 2h): 953f674ebb576634309e085bece7d482\n",
        "bridgesessions join host:19949 - --start\n",
    ],
)
def test_parse_invite_output_rejects_incomplete_or_unsafe_data(output):
    generator = load_generator()

    with pytest.raises(ValueError):
        generator.parse_invite_output(output)


def test_render_page_is_a_three_step_join_and_rejoin_guide():
    generator = load_generator()
    invite = generator.InviteInfo(
        token="953f674ebb576634309e085bece7d482",
        seed_address="203.0.113.10:19949",
    )

    page = generator.render_page(
        invite=invite,
        node_name="dave-pc",
        mesh_label='Jefferson & Friends <mesh>',
        version="26.09.15-r1",
        issued_at=datetime(2026, 9, 17, 12, 0, tzinfo=timezone.utc),
        window_seconds=300,
    )

    assert "Open PowerShell as Administrator" in page
    assert "Copy one setup command" in page
    assert "Confirm the connection" in page
    assert "New PC" in page
    assert "Rejoin or repair" in page
    assert "dave-pc" in page
    assert "203.0.113.10:19949" in page
    assert "26.09.15-r1" in page
    assert "953f674ebb576634309e085bece7d482" in page
    assert "Jefferson &amp; Friends &lt;mesh&gt;" in page
    assert 'data-expires-at="1789646700"' in page
    assert "navigator.clipboard" in page
    assert "connect-src 'none'" in page
    assert 'name="robots" content="noindex,nofollow,noarchive"' in page
    assert "--token" not in page


def test_build_commands_rejects_unsafe_node_names():
    generator = load_generator()
    invite = generator.InviteInfo(
        token="953f674ebb576634309e085bece7d482",
        seed_address="203.0.113.10:19949",
    )

    with pytest.raises(ValueError, match="node name"):
        generator.build_commands(invite, "dave-pc'; Remove-Item C:\\\\ -Recurse", "26.09.15-r1")


def test_cli_generates_private_page_without_printing_token(tmp_path):
    fake_bs = tmp_path / "fake-bs"
    fake_bs.write_text(
        """#!/bin/sh
case "$1" in
  invite)
    printf '%s\\n' 'Invite (valid 2h):  953f674ebb576634309e085bece7d482'
    printf '%s\\n' 'One-liner:' '  bridgesessions join 203.0.113.10:19949 --token-file <path> --start'
    ;;
  --version) printf '%s\\n' '26.09.15-r1' ;;
  *) exit 2 ;;
esac
"""
    )
    fake_bs.chmod(0o755)
    output = tmp_path / "dave-pc-invite.html"

    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--node",
            "dave-pc",
            "--mesh-label",
            "Jefferson's mesh",
            "--bridgesessions",
            str(fake_bs),
            "--output",
            str(output),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0, result.stderr
    assert output.exists()
    assert stat.S_IMODE(output.stat().st_mode) == 0o600
    assert "953f674ebb576634309e085bece7d482" not in result.stdout
    assert "953f674ebb576634309e085bece7d482" not in result.stderr
    assert str(output) in result.stdout
    assert "PRIVATE FILE" in result.stdout
    assert "953f674ebb576634309e085bece7d482" in output.read_text()


def test_cli_refuses_to_replace_an_existing_invite_without_force(tmp_path):
    output = tmp_path / "existing.html"
    output.write_text("keep me")

    result = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--node",
            "dave-pc",
            "--bridgesessions",
            "/does/not/matter",
            "--output",
            str(output),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode != 0
    assert output.read_text() == "keep me"
    assert "--force" in result.stderr
