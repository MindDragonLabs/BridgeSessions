"""BridgePanel — HTTP handler, routing, API endpoints."""
from __future__ import annotations

import base64
import hashlib
import html as _html
import json
import re
import sys
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from .api import (build_tree, daemon_connect_session, daemon_create_session,
                  daemon_session_input, is_self_node, list_host_files,
                  list_host_volumes, query_mesh_tree, query_remote_scrollback,
                  query_scrollback, read_local_inbox_file, read_volume_file,
                  remote_file_recv, remote_file_send, write_local_inbox_file,
                  write_volume_file)
from .ops import mkdir_path, rename_path, trash_path
from . import auth as panel_auth
from .consts import APP, MAX_UPLOAD, VERSION, max_file_upload
from .files import (file_kind, markdown_to_html, resolve_file, safe_name,
                    safe_relpath, safe_session_name, safe_type, sessions_dir)
from .invites import TOKEN_PATTERN, list_invites, mint_invite, render_invite_page, seed_info
from .panel_html import FAVICON_SVG, INDEX_HTML

STATIC_DIR = Path(__file__).resolve().parent / "static"
STATIC_FILES = {
    "toastui-editor-all.min.js": "application/javascript; charset=utf-8",
    "toastui-editor.min.css": "text/css; charset=utf-8",
    "toastui-editor-dark.min.css": "text/css; charset=utf-8",
    "filepond.min.js": "application/javascript; charset=utf-8",
    "filepond.min.css": "text/css; charset=utf-8",
    "filepond-plugin-file-validate-size.min.js": "application/javascript; charset=utf-8",
    "codemirror-bundle.min.js": "application/javascript; charset=utf-8",
}

def mesh_node_name() -> str:
    """Return the local mesh node name from MESH_TREE (best-effort)."""
    tree = query_mesh_tree()
    return tree.get("node", "")


class BridgePanelHandler(BaseHTTPRequestHandler):
    server_version = f"BridgePanel/{VERSION}"
    protocol_version = "HTTP/1.1"

    @property
    def token(self) -> str:
        return self.server.bridgepanel_token  # type: ignore[attr-defined]

    def log_message(self, format: str, *args) -> None:  # noqa: A002
        safe_args = tuple(
            value.replace(self.token, "<token>") if isinstance(value, str) else value
            for value in args
        )
        src = self.client_address[0] if self.client_address else "-"
        sys.stderr.write("%s %s %s\n" % (self.log_date_time_string(), src, format % safe_args))

    def security_headers(self, content_type: str, length: int | None = None,
                         cache_control: str | None = None) -> None:
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", cache_control or "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Frame-Options", "DENY")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; "
            "style-src 'self' 'unsafe-inline'; "
            "script-src 'self' 'unsafe-inline'; "
            "img-src 'self' data: blob:; "
            "media-src 'self' blob:; "
            "object-src 'self'; "
            "frame-src 'self'; "
            "font-src 'self' data:; "
            "connect-src 'self';"
        )
        if length is not None:
            self.send_header("Content-Length", str(length))

    def send_bytes(self, body: bytes, content_type: str, status: int = 200,
                   cache_control: str | None = None) -> None:
        self.send_response(status)
        self.security_headers(content_type, len(body), cache_control=cache_control)
        self.end_headers()
        self.wfile.write(body)

    def send_file_bytes(self, body: bytes, content_type: str, filename: str,
                        inline: bool = False, cacheable: bool = False,
                        etag: str | None = None, allow_frame: bool = False) -> None:
        fname = safe_name(filename or "download")
        disp = "inline" if inline else "attachment"
        self.send_response(200)
        self.send_header("Content-Type", content_type or "application/octet-stream")
        self.send_header("Content-Disposition", f'{disp}; filename="{fname}"')
        self.send_header("Content-Length", str(len(body)))
        if cacheable and inline:
            self.send_header("Cache-Control", "private, max-age=60")
            if etag:
                self.send_header("ETag", etag)
        else:
            self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        if not allow_frame:
            self.send_header("X-Frame-Options", "DENY")
        self.send_header("Referrer-Policy", "no-referrer")
        self.end_headers()
        self.wfile.write(body)

    def _file_result_response(self, result: dict, download: bool, inline: bool) -> None:
        if not result.get("ok"):
            self.send_json(result)
            return
        kind = result.get("kind") or file_kind(result.get("name") or "")
        is_media = kind in ("image", "video", "pdf")
        if download:
            self.send_file_bytes(
                result.get("data") or b"",
                result.get("content_type") or "application/octet-stream",
                result.get("name") or "download",
                inline=False,
            )
            return
        if inline and is_media:
            body = result.get("data") or b""
            etag = '"' + hashlib.sha256(body).hexdigest()[:16] + '"'
            if (self.headers.get("If-None-Match") or "") == etag:
                self.send_response(304)
                self.send_header("ETag", etag)
                self.send_header("Cache-Control", "private, max-age=60")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                return
            self.send_file_bytes(
                body,
                result.get("content_type") or "application/octet-stream",
                result.get("name") or "download",
                inline=True,
                cacheable=True,
                etag=etag,
                allow_frame=(kind == "pdf"),
            )
            return
        if result.get("is_text"):
            self.send_json({k: v for k, v in result.items() if k != "data"})
            return
        self.send_file_bytes(
            result.get("data") or b"",
            result.get("content_type") or "application/octet-stream",
            result.get("name") or "download",
            inline=False,
        )

    def _serve_static(self, path: str) -> bool:
        """Serve a same-origin vendored asset. True if this request is /static/*."""
        if not path.startswith("/static/"):
            return False
        name = path[len("/static/"):]
        if not name or name != Path(name).name or name not in STATIC_FILES:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return True
        fpath = (STATIC_DIR / name).resolve()
        if fpath.parent != STATIC_DIR.resolve():
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return True
        try:
            data = fpath.read_bytes()
        except OSError:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return True
        self.send_bytes(data, STATIC_FILES[name],
                        cache_control="public, max-age=86400, immutable")
        return True

    def send_json(self, payload: dict, status: int = 200) -> None:
        self.send_bytes(
            json.dumps(payload).encode("utf-8"),
            "application/json; charset=utf-8",
            status,
        )

    def reject(self, status: int, message: str) -> None:
        self.send_bytes(message.encode("utf-8"), "text/plain; charset=utf-8", status)

    def _read_json_body(self, limit: int) -> dict | None:
        """Read one bounded JSON object with a deadline and no trailing data."""
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except (TypeError, ValueError):
            self.reject(HTTPStatus.BAD_REQUEST, "Invalid Content-Length")
            return None
        if length < 2:
            self.reject(HTTPStatus.BAD_REQUEST, "JSON body required")
            return None
        if length > limit:
            self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
            return None
        old_timeout = self.connection.gettimeout()
        try:
            self.connection.settimeout(5.0)
            raw = self.rfile.read(length)
        except (TimeoutError, OSError):
            self.reject(HTTPStatus.REQUEST_TIMEOUT, "Request body timeout")
            return None
        finally:
            self.connection.settimeout(old_timeout)
        if len(raw) != length:
            self.reject(HTTPStatus.BAD_REQUEST, "Incomplete request body")
            return None
        try:
            value = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, ValueError, json.JSONDecodeError):
            self.reject(HTTPStatus.BAD_REQUEST, "Invalid JSON")
            return None
        if not isinstance(value, dict):
            self.reject(HTTPStatus.BAD_REQUEST, "JSON object required")
            return None
        return value

    def _drain_body(self) -> None:
        """Consume ignored bodies or close, preventing keep-alive desync."""
        try:
            length = int(self.headers.get("Content-Length", "0") or 0)
        except (TypeError, ValueError):
            self.close_connection = True
            return
        if length <= 0:
            return
        if length > MAX_UPLOAD:
            self.close_connection = True
            return
        try:
            remaining = length
            while remaining:
                chunk = self.rfile.read(min(remaining, 65536))
                if not chunk:
                    self.close_connection = True
                    return
                remaining -= len(chunk)
        except (BrokenPipeError, ConnectionResetError, TimeoutError, OSError):
            self.close_connection = True

    def _verified_https_or_local(self) -> bool:
        host = (self.client_address[0] if self.client_address else "").strip("[]")
        if host in ("127.0.0.1", "::1", "localhost") or host.startswith("127."):
            return True
        if getattr(self.server, "is_https", False):
            return True
        proxies = {x.strip() for x in __import__("os").environ.get("BRIDGEPANEL_TRUSTED_PROXY_IPS", "").split(",") if x.strip()}
        return host in proxies and self.headers.get("X-Forwarded-Proto", "").lower() == "https"

    def _forwarded_https(self) -> bool:
        """True only when a configured trusted proxy reports an https hop.

        X-Forwarded-Proto is a plain request header, so any client can set it.
        It is only meaningful when the immediate peer is a proxy this
        deployment actually trusts, which is what
        BRIDGESPANEL_TRUSTED_PROXY_IPS lists.
        """
        proxies = {x.strip() for x in __import__("os").environ.get("BRIDGESPANEL_TRUSTED_PROXY_IPS", "").split(",") if x.strip()}
        if not proxies:
            return False
        client = self.client_address[0] if getattr(self, "client_address", None) else ""
        return client in proxies and self.headers.get("X-Forwarded-Proto", "").lower() == "https"

    def _v1_auth(self) -> bool:
        return bool(getattr(self, "auth_scopes", set()) & {"admin", "read", "write", "sessions", "chat"})

    def _v1_scope_allowed(self, method: str, path: str) -> bool:
        scopes = getattr(self, "auth_scopes", set())
        if "admin" in scopes:
            return True
        if path.startswith("/api/v1/auth/tokens"):
            return False
        if path.startswith("/api/v1/chat"):
            required = "chat"
        elif path.startswith("/api/v1/sessions"):
            required = "sessions" if method in ("POST", "DELETE") else "read"
        elif method == "POST" and path == "/api/v1/files/content":
            required = "write"
        else:
            required = "read"
        return required in scopes

    def _origin_allowed(self) -> bool:
        origin = self.headers.get("Origin", "")
        if not origin:
            return True
        try:
            parsed = urlparse(origin)
            request = urlparse("//" + self.headers.get("Host", ""))
            proxies = {x.strip() for x in __import__("os").environ.get("BRIDGEPANEL_TRUSTED_PROXY_IPS", "").split(",") if x.strip()}
            client = self.client_address[0] if getattr(self, "client_address", None) else ""
            https = (getattr(self.server, "is_https", False)
                     or client in proxies and self.headers.get("X-Forwarded-Proto", "").lower() == "https")
            expected_scheme = "https" if https else "http"
            port = parsed.port or (443 if parsed.scheme == "https" else 80)
            request_port = request.port or (443 if parsed.scheme == "https" else 80)
            return (parsed.scheme == expected_scheme and bool(request.hostname)
                    and parsed.hostname == request.hostname and port == request_port
                    and not parsed.username and not parsed.password
                    and parsed.path in ("", "/") and not parsed.query and not parsed.fragment)
        except ValueError:
            return False

    def _cookie_csrf_allowed(self) -> bool:
        return (getattr(self, "auth_kind", "") != "cookie"
                or bool(self.headers.get("Origin")) and self._origin_allowed())

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
        auth_header = self.headers.get("Authorization", "")
        scheme, _, bearer = auth_header.partition(" ")
        self.auth_scopes = set()
        self.auth_identity = None
        self.auth_kind = None
        if auth_header:
            if scheme.lower() != "bearer" or not bearer or len(bearer) > 512:
                return None
            self.auth_scopes = panel_auth.credential_scope(bearer, self.token)
            self.auth_identity = panel_auth.credential_identity(bearer, self.token)
            self.auth_kind = "bearer"
        else:
            self.auth_identity = panel_auth.cookie_identity(self.headers.get("Cookie", ""))
            if self.auth_identity:
                self.auth_scopes = {"admin", "read", "write", "sessions", "chat"}
                self.auth_kind = "cookie"
        if not self.auth_scopes or not self.auth_identity:
            return None
        if require_token and "admin" not in self.auth_scopes and "write" not in self.auth_scopes:
            return None
        return parsed.path, parsed.query

    def do_GET(self) -> None:
        parsed = urlparse(self.path)

        # Health check (no auth)
        if parsed.path == "/healthz":
            self.send_json({"ok": True, "service": APP, "version": VERSION})
            return

        if parsed.path == "/api/login" and panel_auth.login_enabled():
            self.send_bytes(panel_auth.LOGIN_HTML.encode("utf-8"), "text/html; charset=utf-8")
            return

        if parsed.path == "/api/invites":
            if not self.authorized_path(require_token=True) or "admin" not in self.auth_scopes:
                self.reject(HTTPStatus.NOT_FOUND, "Not found")
                return
            self.send_json(list_invites())
            return

        if parsed.path.startswith("/api/v1/"):
            if not self.authorized_path(require_token=False):
                self.send_json({"ok": False, "error": {"code": "unauthorized", "message": "authentication required"}}, HTTPStatus.UNAUTHORIZED)
                return
            if not self._v1_scope_allowed("GET", parsed.path):
                self.send_json({"ok": False, "error": {"code": "forbidden", "message": "credential scope does not permit this route"}}, HTTPStatus.FORBIDDEN)
                return
            from . import v1
            status, payload = v1.dispatch("GET", parsed.path, parse_qs(parsed.query), identity=self.auth_identity)
            self.send_json(payload, status)
            return

        auth = self.authorized_path(require_token=False)
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return

        path, query = auth
        params = parse_qs(query)

        if self._serve_static(path):
            return

        if path in ("/", ""):
            from .consts import BUILDTAG
            from .lang import js_table
            html = (
                INDEX_HTML
                .replace("__BUILD_TAG__", f"{BUILDTAG}")
                .replace("__ASSET_BASE__", "/")
                .replace("__LANG_TABLE__", js_table())
            )
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
        elif path == "/api/volumes":
            machine = params.get("machine", [""])[0]
            if not machine:
                self.reject(HTTPStatus.BAD_REQUEST, "machine required")
                return
            self.send_json(list_host_volumes(machine))
        elif path == "/api/files":
            machine = params.get("machine", [""])[0]
            root = params.get("root", ["inbox"])[0] or "inbox"
            raw_path = params.get("path", [""])[0]
            if not machine:
                self.reject(HTTPStatus.BAD_REQUEST, "machine required")
                return
            if root in ("", "inbox"):
                rel = safe_relpath(raw_path)
            else:
                from .volumes import normalize_rel
                rel = normalize_rel(root, raw_path)
                if rel is None:
                    self.send_json({"ok": False, "error": "path_rejected", "root": root, "items": []})
                    return
            refresh = params.get("refresh", ["0"])[0].lower() in ("1", "true", "yes")
            self.send_json(list_host_files(machine, rel, root=root, refresh=refresh))
        elif path == "/api/open-path":
            machine = params.get("machine", [""])[0]
            root = params.get("root", ["inbox"])[0] or "inbox"
            raw_path = params.get("path", [""])[0]
            cwd = params.get("cwd", [""])[0]
            if not machine:
                self.reject(HTTPStatus.BAD_REQUEST, "machine required")
                return
            from .api import resolve_open_path
            self.send_json(resolve_open_path(machine, root, raw_path, cwd))
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
            root = params.get("root", ["inbox"])[0] or "inbox"
            remote_path = params.get("path", [""])[0]
            if not machine or not remote_path:
                self.reject(HTTPStatus.BAD_REQUEST, "machine and path required")
                return
            download = params.get("download", ["0"])[0].lower() in ("1", "true", "yes")
            inline = params.get("inline", ["0"])[0].lower() in ("1", "true", "yes")
            if root in ("", "inbox"):
                remote_path = safe_relpath(remote_path)
                if not remote_path:
                    self.reject(HTTPStatus.BAD_REQUEST, "machine and path required")
                    return
                if is_self_node(machine):
                    result = read_local_inbox_file(remote_path)
                else:
                    result = remote_file_recv(machine, remote_path)
                    if result.get("ok"):
                        result["kind"] = file_kind(result.get("name") or remote_path)
            else:
                result = read_volume_file(machine, root, remote_path)
            self._file_result_response(result, download, inline)
        else:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")

    def do_POST(self) -> None:
        parsed = urlparse(self.path)

        if parsed.path == "/api/login":
            if not panel_auth.login_enabled() or not self._verified_https_or_local() or not self._origin_allowed():
                self.reject(HTTPStatus.FORBIDDEN, "password login requires local or verified HTTPS transport")
                return
            body = self._read_json_body(16 * 1024)
            if body is None:
                return
            if set(body) != {"user", "pass"} or not isinstance(body.get("user"), str) or not isinstance(body.get("pass"), str):
                self.reject(HTTPStatus.BAD_REQUEST, "user and pass are required")
                return
            if not panel_auth.verify_password(body["user"], body["pass"], self.client_address[0]):
                self.reject(HTTPStatus.UNAUTHORIZED, "invalid credentials")
                return
            cookie = panel_auth.new_session()
            # Mark the cookie Secure whenever the session could have travelled
            # over TLS. X-Forwarded-Proto is attacker-controlled, so it only
            # counts when the request actually came from a configured trusted
            # proxy (BRIDGESPANEL_TRUSTED_PROXY_IPS), the same rule
            # _origin_allowed() already applies. Trusting the bare header would
            # let any client drop the Secure flag by omitting it.
            secure = (getattr(self.server, "is_https", False)
                      or self._forwarded_https())
            flags = "; HttpOnly; SameSite=Strict; Path=/"
            if secure:
                flags += "; Secure"
            self.send_response(200)
            self.security_headers("application/json; charset=utf-8", len(b'{"ok":true}'))
            self.send_header("Set-Cookie", f"{panel_auth.COOKIE}={cookie}{flags}")
            self.end_headers()
            self.wfile.write(b'{"ok":true}')
            return

        if self.headers.get("Cookie") and not self.headers.get("Authorization") and (not self.headers.get("Origin") or not self._origin_allowed()):
            self.reject(HTTPStatus.FORBIDDEN, "origin is not allowed")
            return

        if parsed.path.startswith("/api/v1/"):
            if not self.authorized_path(require_token=False):
                self.send_json({"ok": False, "error": {"code": "unauthorized", "message": "authentication required"}}, HTTPStatus.UNAUTHORIZED)
                return
            if not self._v1_scope_allowed("POST", parsed.path):
                self.send_json({"ok": False, "error": {"code": "forbidden", "message": "credential scope does not permit this route"}}, HTTPStatus.FORBIDDEN)
                return
            if not self._cookie_csrf_allowed():
                self.send_json({"ok": False, "error": {"code": "origin_denied", "message": "origin is not allowed"}}, HTTPStatus.FORBIDDEN)
                return
            body = self._read_json_body(256 * 1024)
            if body is None:
                return
            from . import v1
            status, payload = v1.dispatch("POST", parsed.path, parse_qs(parsed.query), body, identity=self.auth_identity)
            self.send_json(payload, status)
            return

        auth = self.authorized_path(require_token=True)
        if not auth:
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return
        path, _ = auth

        if path == "/api/invites":
            self._drain_body()
            if "admin" not in self.auth_scopes:
                self.reject(HTTPStatus.FORBIDDEN, "administrator credential required")
                return
            self.send_json(mint_invite())
            return

        if path == "/api/invites/page":
            body = self._read_json_body(64 * 1024)
            if body is None:
                return
            if set(body) - {"token", "seed", "window_seconds", "expires_at"} or not isinstance(body.get("token"), str):
                self.reject(HTTPStatus.BAD_REQUEST, "token is required")
                return
            token = body["token"].strip()
            if not re.fullmatch(TOKEN_PATTERN, token):
                self.reject(HTTPStatus.BAD_REQUEST, "invalid token")
                return
            seed = str(body.get("seed") or seed_info().get("addr", ""))
            try:
                window = int(body.get("window_seconds") or 300)
            except (TypeError, ValueError):
                self.reject(HTTPStatus.BAD_REQUEST, "invalid window_seconds")
                return
            record = {"token": token, "seed": seed, "window_seconds": window, "expires_at": str(body.get("expires_at") or "")}
            self.send_json({"ok": True, "page": render_invite_page(record)})
            return

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
            cap = max_file_upload()
            if length < 0 or length > cap:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return
            try:
                raw_body = self.rfile.read(length) if length else b"{}"
                body = json.loads(raw_body)
            except (ValueError, json.JSONDecodeError, OSError):
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid JSON")
                return

            machine = body.get("machine", "")
            root = body.get("root") or "inbox"
            content = body.get("content", "")
            content_b64 = body.get("content_b64")

            if not machine:
                self.reject(HTTPStatus.BAD_REQUEST, "Missing machine or path")
                return
            from .volumes import normalize_rel, root_writable
            if root in ("", "inbox"):
                remote_path = safe_relpath(body.get("path", ""))
            else:
                remote_path = normalize_rel(root, body.get("path", ""))
                if remote_path is None:
                    self.send_json({"ok": False, "error": "path_rejected"})
                    return
            if not remote_path:
                self.reject(HTTPStatus.BAD_REQUEST, "Missing machine or path")
                return
            if not root_writable(root, machine):
                self.send_json({"ok": False, "error": "write_inbox_only" if root not in ("", "inbox") else "write_not_allowed"})
                return

            if content_b64:
                try:
                    data = base64.b64decode(content_b64)
                except (ValueError, TypeError):
                    self.reject(HTTPStatus.BAD_REQUEST, "Invalid content_b64")
                    return
            else:
                if not isinstance(content, str):
                    self.reject(HTTPStatus.BAD_REQUEST, "content must be a string")
                    return
                data = content.encode("utf-8")

            if len(data) > cap:
                self.reject(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "Content too large")
                return

            if root in ("", "inbox"):
                if is_self_node(machine):
                    result = write_local_inbox_file(remote_path, data)
                else:
                    result = remote_file_send(machine, remote_path, data)
            else:
                result = write_volume_file(machine, root, remote_path, data)
            self.send_json(result)
            return

        if path in ("/api/mkdir", "/api/rename", "/api/trash"):
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
            if not isinstance(body, dict):
                self.reject(HTTPStatus.BAD_REQUEST, "Invalid JSON")
                return
            machine = str(body.get("machine") or "")
            root = str(body.get("root") or "inbox")
            remote_path = str(body.get("path") or "")
            if not machine or not remote_path:
                self.reject(HTTPStatus.BAD_REQUEST, "Missing machine or path")
                return
            if path == "/api/mkdir":
                self.send_json(mkdir_path(machine, root, remote_path))
                return
            if path == "/api/rename":
                self.send_json(rename_path(machine, root, remote_path, str(body.get("name") or "")))
                return
            self.send_json(trash_path(machine, root, remote_path))
            return

        self.reject(HTTPStatus.NOT_FOUND, "Not found")

    def do_DELETE(self) -> None:
        parsed = urlparse(self.path)
        if not parsed.path.startswith("/api/v1/"):
            self.reject(HTTPStatus.NOT_FOUND, "Not found")
            return
        if not self.authorized_path(require_token=False):
            self.send_json({"ok": False, "error": {"code": "unauthorized", "message": "authentication required"}}, HTTPStatus.UNAUTHORIZED)
            return
        if not self._v1_scope_allowed("DELETE", parsed.path):
            self.send_json({"ok": False, "error": {"code": "forbidden", "message": "credential scope does not permit this route"}}, HTTPStatus.FORBIDDEN)
            return
        if not self._cookie_csrf_allowed():
            self.send_json({"ok": False, "error": {"code": "origin_denied", "message": "origin is not allowed"}}, HTTPStatus.FORBIDDEN)
            return
        from . import v1
        status, payload = v1.dispatch("DELETE", parsed.path, parse_qs(parsed.query), identity=self.auth_identity)
        self.send_json(payload, status)
