"""BridgePanel: a reading and writing surface for human-agent communication.

Organized by session (from BridgeSessions or filesystem), with two document
types: comms (short messages) and documents (long-form reports). Markdown
is rendered in a clean reading pane with an edit marker for in-browser editing.

No third-party dependencies. Works over any private network (VPN, LAN, loopback).
Never binds to a public address.
"""
from __future__ import annotations

import argparse
import json
import os
import secrets
import socket  # noqa: F401 — re-exported for test monkeypatching
import sys
import time
from http.server import ThreadingHTTPServer
from pathlib import Path

from .api import (build_tree, bs_ipc, bs_ipc_token, query_bs_sessions,
                  query_mesh_tree, query_scrollback)  # noqa: F401 — test access
from .consts import (APP, BUILDTAG, DEFAULT_BIND, DEFAULT_PORT, VERSION,
                     config_home, data_home, sessions_dir, state_path,
                     token_path)
from .files import (inline_markup, markdown_to_html, resolve_file, safe_name,
                    safe_session_name, safe_type)  # noqa: F401 — test access
from .server import BridgePanelHandler  # noqa: F401 — server use


# ── Token / state ──────────────────────────────────────────────

def read_state() -> dict:
    try:
        return json.loads(state_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"current": None, "updated": 0}


def write_state(data: dict) -> None:
    data_home().mkdir(parents=True, exist_ok=True)
    tmp = state_path().with_suffix(".tmp")
    tmp.write_text(json.dumps(data), encoding="utf-8")
    os.replace(tmp, state_path())


def ensure_dirs() -> str:
    config_home().mkdir(parents=True, exist_ok=True, mode=0o700)
    sessions_dir().mkdir(parents=True, exist_ok=True, mode=0o700)

    tp = token_path()
    if not tp.exists():
        tp.write_text(secrets.token_urlsafe(24), encoding="utf-8")
    tp.chmod(0o600)
    token = tp.read_text(encoding="utf-8").strip()
    if len(token) < 24:
        raise RuntimeError("Token too short")

    if not state_path().exists():
        write_state({"current": None, "updated": time.time()})

    _seed_sample_data()
    return token


def _seed_sample_data() -> None:
    """Create sample documents so the panel isn't empty on first launch."""
    base = sessions_dir()

    samples = {
        "hermes/comms/sample-instructions.md": (
            "# Instructions\n\n"
            "This is the **comms** channel — short messages between you and the agent.\n\n"
            "- Instructions you write here are picked up by the agent.\n"
            "- Agent responses appear here too.\n"
            "- Press `e` to edit any file in the panel.\n"
        ),
        "hermes/documents/sample-report.md": (
            "# Sample Report\n\n"
            "This is the **documents** area — long-form output that's hard to read in a terminal.\n\n"
            "When the agent produces a multi-page audit, code review, or debug trace, it lands here.\n"
            "You read it comfortably in this pane, and optionally edit with the edit marker.\n\n"
            "## How it works\n\n"
            "1. Agent publishes: `bridgepanel.py publish --session hermes --type documents report.md`\n"
            "2. Panel auto-refreshes — no spinner, content just appears\n"
            "3. Click any file in the tree to read it\n"
            "4. Press `e` or click Edit to modify — saves back to disk\n\n"
            "## Markdown support\n\n"
            "Tables, code blocks, lists, blockquotes — all rendered.\n\n"
            "| Feature | Status |\n|---------|--------|\n"
            "| Sessions | From BridgeSessions or filesystem |\n"
            "| Edit | Press `e` for raw markdown |\n"
            "| Networking | VPN, LAN, loopback — never public |\n\n"
            "> The terminal is for commands. This panel is for reading.\n"
        ),
    }

    for relpath, content in samples.items():
        fp = base / relpath
        if not fp.exists():
            fp.parent.mkdir(parents=True, exist_ok=True)
            fp.write_text(content, encoding="utf-8")


# ── CLI ────────────────────────────────────────────────────────

def _bind_is_loopback(bind: str) -> bool:
    host = (bind or "").strip().strip("[]")
    if host in ("127.0.0.1", "localhost", "::1"):
        return True
    return host.startswith("127.")


def serve(bind: str, port: int) -> None:
    if not _bind_is_loopback(bind):
        raise ValueError(
            f"Refusing non-loopback bind {bind!r}; BridgePanel listens on 127.0.0.1 only"
        )
    token = ensure_dirs()
    server = ThreadingHTTPServer((bind, port), BridgePanelHandler)
    server.bridgepanel_token = token  # type: ignore[attr-defined]
    trusted_ips = {
        v.strip()
        for v in os.environ.get("BRIDGEPANEL_TRUSTED_IPS", "").split(",")
        if v.strip()
    }
    setattr(server, "trusted_ips", trusted_ips)
    print(f"{APP} {VERSION} listening on http://{bind}:{port}/{token}/", flush=True)
    server.serve_forever()


def publish(source: Path, session: str, dtype: str, title: str | None) -> Path:
    ensure_dirs()
    source = source.expanduser().resolve(strict=True)
    if not source.is_file():
        raise ValueError(f"Not a file: {source}")
    name = safe_name(title or source.name)
    if title and not Path(name).suffix and source.suffix:
        name += source.suffix
    target_dir = sessions_dir() / safe_session_name(session) / safe_type(dtype)
    target_dir.mkdir(parents=True, exist_ok=True)
    target = target_dir / name
    target.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")
    return target


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Reading and writing surface for human-agent communication"
    )
    sub = parser.add_subparsers(dest="command", required=True)

    serve_p = sub.add_parser("serve", help="start the web service")
    serve_p.add_argument("--bind", default=DEFAULT_BIND)
    serve_p.add_argument("--port", type=int, default=DEFAULT_PORT)

    pub_p = sub.add_parser("publish", help="copy a file into a session")
    pub_p.add_argument("source", type=Path)
    pub_p.add_argument("--session", default="default")
    pub_p.add_argument("--type", default="documents", choices=["comms", "documents"])
    pub_p.add_argument("--title")

    note_p = sub.add_parser("note", help="publish stdin as a markdown file")
    note_p.add_argument("--session", default="default")
    note_p.add_argument("--type", default="comms")
    note_p.add_argument("--title", default="Note.md")

    sub.add_parser("url", help="print the private panel URL").add_argument(
        "--bind", default=DEFAULT_BIND
    )
    sub.add_parser("tree", help="print the session tree as JSON")

    args = parser.parse_args()

    if args.command == "serve":
        serve(args.bind, args.port)
    elif args.command == "publish":
        target = publish(args.source, args.session, args.type, args.title)
        print(target)
    elif args.command == "note":
        ensure_dirs()
        text = sys.stdin.read()
        name = safe_name(args.title)
        if not Path(name).suffix:
            name += ".md"
        target_dir = (
            sessions_dir() / safe_session_name(args.session) / safe_type(args.type)
        )
        target_dir.mkdir(parents=True, exist_ok=True)
        target = target_dir / name
        target.write_text(text, encoding="utf-8")
        print(target)
    elif args.command == "tree":
        print(json.dumps(build_tree(), indent=2))
    elif args.command == "url":
        token = ensure_dirs()
        bind = args.bind
        print(f"http://{bind}:{DEFAULT_PORT}/{token}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
