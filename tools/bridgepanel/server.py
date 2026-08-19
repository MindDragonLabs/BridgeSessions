"""BridgePanel — HTTP handler, routing, API endpoints."""
from __future__ import annotations

import html as _html
import json
import sys
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlparse

from .api import (build_tree, daemon_connect_session, daemon_create_session,
                  daemon_session_input, query_mesh_tree, query_remote_scrollback,
                  query_remote_session_info, query_scrollback,
                  remote_file_recv, remote_file_send)
from .consts import MAX_UPLOAD, VERSION, APP
from .files import (markdown_to_html, resolve_file, safe_name,
                    safe_session_name, safe_type, sessions_dir)
from .panel_html import FAVICON_SVG, INDEX_HTML


def mesh_node_name() -> str:
    """Return the local mesh node name from MESH_TREE (best-effort)."""
    try:
        tree = query_mesh_tree()
        return tree.get("node", "")
    except Exception:
        return ""


class BridgePanelHandler(BaseHTTPRequestHandler):
    server_version = f"BridgePanel/{VERSION}"
    protocol_version = "HTTP/1.1"

    @property
    def token(self) -> str:
        return self.server.bridgepanel_token  # type: ignore[attr-defined]

    def log_message(self, format: str, *args) -> None:  # noqa: A002
        safe_args = tuple(str(v).replace(self.token, "<token>") for v in args)
        sys.stderr.write("%s %s\n" % (self.log_date_time_string(), format % safe_args))

    def security_headers(self, content_type: str, length: int | None = None) -> None:
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; "
            "style-src 'unsafe-inline'; "
            "script-src 'unsafe-inline'; "
            "img-src 'self' data:; "
            "connect-src 'self';"
        )
        if length is not None:
            self.send_header("Content-Length", str(length))

    def send_bytes(self, body: bytes, content_type: str, status: int = 200) -> None:
        self.send_response(status)
        self.security_headers(content_type, len(body))
        self.end_headers()
        self.wfile.write(body)

    def send_json(self, payload: dict, status: int = 200) -> None:
        self.send_bytes(
            json.dumps(payload).encode("utf-8"),
            "application/json; charset=utf-8",
            status,
        )

    def reject(self, status: int, message: str) -> None:
        self.send_bytes(message.encode("utf-8"), "text/plain; charset=utf-8", status)

    def _origin_allowed(self) -> bool:
        origin = self.headers.get("Origin", "")
        if not origin:
            return True
        host = (urlparse(origin).hostname or "").strip("[]")
        return host in ("127.0.0.1", "localhost", "::1") or host.startswith("127.")

    def _sse_write(self, payload: dict | None = None, comment: str | None = None) -> None:
        if comment is not None:
            self.wfile.write(f": {comment}\n\n".encode("utf-8"))
        else:
            self.wfile.write(b"data: " + json.dumps(payload).encode("utf-8") + b"\n\n")
        self.wfile.flush()

    def handle_stream(self, params: dict) -> None:
        """SSE scrollback for a local session. Reuses query_scrollback(read_since)."""
        session = params.get("session", [""])[0]
        machine = params.get("machine", [""])[0]
        try:
            since = int(params.get("since", ["0"])[0])
        except ValueError:
            since = 0
        if not session:
            self.reject(HTTPStatus.BAD_REQUEST, "session required")
            return
        if not self._origin_allowed():
            self.reject(HTTPStatus.FORBIDDEN, "origin not allowed")
            return

        once = params.get("once", ["0"])[0].lower() in ("1", "true", "yes")
        try:
            interval = float(params.get("interval", ["0.5"])[0])
        except ValueError:
            interval = 0.5
        interval = min(max(interval, 0.1), 5.0)

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "keep-alive")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.end_headers()

        is_remote = bool(machine) and machine not in ("", "(local)", mesh_node_name())
        try:
            if is_remote:
                payload = query_remote_scrollback(machine, session)
                self._sse_write(payload)
                return
            idle = 0
            first = True
            while True:
                d = query_scrollback(session, since)
                emit = first or bool(d.get("text") or d.get("reset") or d.get("error"))
                if emit:
                    self._sse_write(d)
                    first = False
                    if d.get("error") or once:
                        break
                    idle = 0
                else:
                    idle += 1
                    if idle % 20 == 0:
                        self._sse_write(comment="keepalive")
                    if once:
                        self._sse_write(d)
                        break
                try:
                    since = int(d.get("offset", since) or since)
                except (TypeError, ValueError):
                    pass
                time.sleep(interval)
        except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
            return
        finally:
            self.close_connection = True

    def authorized_path(self, *, require_token: bool = False) -> tuple[str, str] | None:
        parsed = urlparse(self.path)
        parts = parsed.path.split("/")
        trusted_ips = getattr(self.server, "trusted_ips", set())
        has_token = len(parts) >= 2 and __import__("secrets").compare_digest(parts[1], self.token)
        if self.client_address[0] in trusted_ips and not require_token:
            if has_token:
                parts = [""] + parts[2:]
            return "/" + "/".join(parts[1:]), parsed.query
        if not has_token:
            return None
        return "/" + "/".join(parts[2:]), parsed.query

    def do_GET(self) -> None:
        parsed = urlparse(self.path)

        # Health check (no auth)
        if parsed.path == "/healthz":
            self.send_json({"ok": True, "service": APP, "version": VERSION})
            return

        auth = self.authorized_path(require_token=False)
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return

        path, query = auth
        params = parse_qs(query)

        if path in ("/", ""):
            from .consts import BUILDTAG
            html = INDEX_HTML.replace("__BUILD_TAG__", f"{BUILDTAG}")
            self.send_bytes(html.encode("utf-8"), "text/html; charset=utf-8")
        elif path in ("/favicon.ico", "/favicon.svg"):
            self.send_response(200)
            self.security_headers("image/svg+xml; charset=utf-8", len(FAVICON_SVG))
            self.end_headers()
            self.wfile.write(FAVICON_SVG)
        elif path == "/api/tree":
            self.send_json(build_tree())
        elif path == "/api/machines":
            self.send_json(query_mesh_tree())
        elif path == "/api/stream":
            self.handle_stream(params)
        elif path == "/api/output":
            session = params.get("session", [""])[0]
            machine = params.get("machine", [""])[0]
            try:
                since = int(params.get("since", ["0"])[0])
            except ValueError:
                since = 0
            if not session:
                self.reject(HTTPStatus.BAD_REQUEST, "session required")
                return
            # Route: if machine is specified and it's not the local node,
            # query remote session info
            if machine and machine not in ("", "(local)", mesh_node_name()):
                self.send_json(query_remote_scrollback(machine, session))
            else:
                self.send_json(query_scrollback(session, since))
        elif path == "/api/content":
            session = params.get("session", [""])[0]
            dtype = params.get("type", ["documents"])[0]
            name = params.get("name", [""])[0]
            item = resolve_file(session, dtype, name)
            if not item:
                self.reject(HTTPStatus.NOT_FOUND, "File not found")
                return
            raw = item.read_text(encoding="utf-8", errors="replace")
            suffix = item.suffix.lower()
            is_md = suffix in (".md", ".markdown", "")
            self.send_json({
                "name": item.name,
                "raw": raw,
                "html": markdown_to_html(raw) if is_md
                else f'<pre style="font-family:var(--mono);font-size:13px;white-space:pre-wrap;word-break:break-all">{_html.escape(raw)}</pre>',
                "editable": True,
            })
        elif path == "/api/session/connect":
            session = params.get("session", [""])[0]
            machine = params.get("machine", [""])[0]
            if not session:
                self.reject(HTTPStatus.BAD_REQUEST, "session required")
                return
            peer = machine or "(peer)"
            cmd = daemon_connect_session(peer, session)
            self.send_json({"cmd": cmd, "machine": peer, "session": session})
        elif path == "/api/remote-file":
            machine = params.get("machine", [""])[0]
            remote_path = params.get("path", [""])[0]
            if not machine or not remote_path:
                self.reject(HTTPStatus.BAD_REQUEST, "machine and path required")
                return
            self.send_json(remote_file_recv(machine, remote_path))
        else:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self) -> None:
        auth = self.authorized_path(require_token=True)
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return
        path, _ = auth

        if path == "/api/save":
            try:
                length = int(self.headers.get("Content-Length", "0") or 0)
            except ValueError:
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
                return
            if length < 0:
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
                return
            if length > MAX_UPLOAD:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return
            try:
                raw_body = self.rfile.read(length) if length else b"{}"
                body = json.loads(raw_body)
            except (ValueError, json.JSONDecodeError, OSError):
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid JSON")
                return

            session = safe_session_name(body.get("session", ""))
            dtype = safe_type(body.get("type", ""))
            name = safe_name(body.get("name", ""))
            content = body.get("content", "")

            if not session or not name:
                self.reject(HTTPStatus.BAD_REQUEST, "Missing session or name")
                return

            if len(content.encode("utf-8")) > MAX_UPLOAD:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return

            target_dir = sessions_dir() / session / dtype
            target_dir.mkdir(parents=True, exist_ok=True)
            target = target_dir / name

            try:
                target.resolve().relative_to(sessions_dir().resolve())
            except (ValueError, OSError):
                self.reject(HTTPStatus.FORBIDDEN, "Path escape")
                return

            target.write_text(content, encoding="utf-8")
            self.send_json({"ok": True, "html": markdown_to_html(content)})
            return

        if path == "/api/session/input":
            try:
                length = int(self.headers.get("Content-Length", "0") or 0)
            except ValueError:
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
                return
            if length < 0 or length > 65536:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return
            try:
                raw_body = self.rfile.read(length) if length else b"{}"
                body = json.loads(raw_body)
            except (ValueError, json.JSONDecodeError, OSError):
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid JSON")
                return
            raw_session = body.get("session", "")
            data = body.get("data", "")
            if not str(raw_session).strip():
                self.reject(HTTPStatus.BAD_REQUEST, "session required")
                return
            session = safe_session_name(raw_session)
            if not isinstance(data, str):
                self.reject(HTTPStatus.BAD_REQUEST, "data must be a string")
                return
            self.send_json(daemon_session_input(session, data))
            return

        if path == "/api/session/create":
            try:
                length = int(self.headers.get("Content-Length", "0") or 0)
            except ValueError:
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
                return
            if length < 0 or length > MAX_UPLOAD:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return
            try:
                raw_body = self.rfile.read(length) if length else b"{}"
                body = json.loads(raw_body)
            except (ValueError, json.JSONDecodeError, OSError):
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid JSON")
                return

            name = safe_session_name(body.get("name", ""))
            machine = body.get("machine", "")
            command = body.get("command", "/bin/bash -l")
            cols = body.get("cols", 80)
            rows = body.get("rows", 24)

            if not name or not machine:
                self.reject(HTTPStatus.BAD_REQUEST, "Missing session name or machine")
                return

            result = daemon_create_session(machine, name, command, cols, rows)
            self.send_json(result)
            return

        if path == "/api/upload":
            try:
                length = int(self.headers.get("Content-Length", "0") or 0)
            except ValueError:
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
                return
            if length < 0 or length > MAX_UPLOAD:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return
            try:
                raw_body = self.rfile.read(length) if length else b"{}"
                body = json.loads(raw_body)
            except (ValueError, json.JSONDecodeError, OSError):
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid JSON")
                return

            machine = body.get("machine", "")
            remote_path = body.get("path", "")
            content = body.get("content", "")

            if not machine or not remote_path:
                self.reject(HTTPStatus.BAD_REQUEST, "Missing machine or path")
                return

            if len(content.encode("utf-8")) > MAX_UPLOAD:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return

            result = remote_file_send(machine, remote_path, content)
            self.send_json(result)
            return

        self.reject(HTTPStatus.NOT_FOUND, "Not found")
