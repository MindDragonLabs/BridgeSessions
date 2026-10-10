"""Behavioral regressions for the fleet session evidence checks."""
from importlib.util import module_from_spec, spec_from_file_location
from pathlib import Path

import pytest


_PATH = Path(__file__).resolve().parents[1] / "scripts" / "fleet-session-check.py"
_SPEC = spec_from_file_location("fleet_session_check", _PATH)
_CHECK = module_from_spec(_SPEC)
_SPEC.loader.exec_module(_CHECK)


@pytest.mark.parametrize("data,encoded", [(b"a", "YQ"), (b"ab", "YWI"), (b"abc", "YWJj")])
def test_input_uses_protocol_unpadded_base64(monkeypatch, data, encoded):
    requests = []
    monkeypatch.setattr(_CHECK, "ipc", lambda request: requests.append(request) or "OK")
    assert _CHECK.input_session("peer", "session", data) == "OK"
    assert requests == [f"SESSION_INPUT peer session {encoded}"]


@pytest.mark.parametrize("data,encoded", [("a", "YQ"), ("ab", "YWI"), ("abc", "YWJj")])
def test_scrollback_accepts_protocol_unpadded_base64(monkeypatch, data, encoded):
    monkeypatch.setattr(_CHECK, "ipc", lambda _: f"OK {len(data)} {encoded}")
    assert _CHECK.scrollback("peer", "session") == (len(data), data)


@pytest.mark.parametrize("encoded", ["YQ==", "YR", "Y", "!!"])
def test_scrollback_rejects_noncanonical_base64(monkeypatch, encoded):
    monkeypatch.setattr(_CHECK, "ipc", lambda _: f"OK 1 {encoded}")
    with pytest.raises(RuntimeError, match="malformed"):
        _CHECK.scrollback("peer", "session")


def test_isolation_rejects_blank_capture_even_when_no_foreign_marker():
    passed, reason = _CHECK.isolation_verdict("", "", "OWN_A", "OWN_B")
    assert not passed
    assert "empty" in reason


def test_isolation_requires_executed_marker_in_correct_session():
    passed, _ = _CHECK.isolation_verdict("shell prompt only", "other text", "OWN_A", "OWN_B")
    assert not passed


def test_isolation_detects_cross_talk_between_named_sessions():
    passed, reason = _CHECK.isolation_verdict("OWN_A\nOWN_B\n", "OWN_B\n", "OWN_A", "OWN_B")
    assert not passed
    assert "cross-talk" in reason


def test_isolation_passes_only_with_both_owned_executed_markers():
    passed, _ = _CHECK.isolation_verdict("OWN_A\n", "OWN_B\n", "OWN_A", "OWN_B")
    assert passed


def test_wait_for_marker_observes_delayed_output(monkeypatch):
    ticks = iter([0.0, 0.2, 0.4, 0.6, 1.0])
    monkeypatch.setattr(_CHECK.time, "monotonic", lambda: next(ticks))
    monkeypatch.setattr(_CHECK.time, "sleep", lambda _delay: None)
    replies = iter([(0, ""), (0, ""), (9, "DELAYED_MARKER")])
    monkeypatch.setattr(_CHECK, "scrollback", lambda *_args: next(replies))
    assert _CHECK.wait_for_marker("peer", "session", "DELAYED_MARKER", timeout=0.9)


def test_wait_for_marker_fails_for_dead_or_unavailable_session(monkeypatch):
    ticks = iter([0.0, 0.3, 0.6, 1.0])
    monkeypatch.setattr(_CHECK.time, "monotonic", lambda: next(ticks))
    monkeypatch.setattr(_CHECK.time, "sleep", lambda _delay: None)

    def dead(*_args):
        raise RuntimeError("ERROR no live session")

    monkeypatch.setattr(_CHECK, "scrollback", dead)
    assert not _CHECK.wait_for_marker("peer", "dead", "NEVER", timeout=0.9)
