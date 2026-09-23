# BridgePanel (tools/bridgepanel) — develop, deploy, verify

BridgePanel is the repo's Python-stdlib web panel (`server.py`, `panel_html.py`, `invites.py`).
It runs on a peer as the systemd **user** unit `bridgepanel.service`; the repo checkout on the
peer is the install location (e.g. `~/bridgesessions/tools/bridgepanel`). The panel token lives
at `~/.config/bridgepanel/token`; mutable state under `~/.local/share/bridgepanel/`.

## Auth model

Every request passes `authorized_path`. Two accepted token forms, identical authority:

- `Authorization: Bearer <token>` header, or
- a leading `/<token>/` URL path segment (`GET /<token>/`, `GET /<token>/api/...`).

The URL form is not optional sugar: browsers cannot attach headers to plain navigations or
document URLs, so any surface the panel's own HTML reaches must work as `/<token>/...`. When
adding an endpoint, keep it behind `authorized_path` and test BOTH forms.

Invite endpoints always require the token — never open them to trusted-IP access alone.
From an untrusted source IP the handler answers 404 (not 403) so the endpoint does not
advertise itself.

## Test in-process before deploying

Run the real handler in a local thread — no daemon, no port conflicts:

```python
from http.server import ThreadingHTTPServer
import bridgepanel as bp
srv = ThreadingHTTPServer(("127.0.0.1", 0), bp.BridgePanelHandler)
srv.bridgepanel_token = TOKEN
srv.trusted_ips = set()   # drop 127.0.0.1 from trusted so the token path is forced
# monkeypatch module seams as needed: invites.data_home, invites.bs_ipc, invites._tailscale_ip4
```

Drive it with `http.client.HTTPConnection` and assert status codes (see
`tools/bridgepanel/test_invites.py`, class `TestInvitesHttp`). Pyright flags the dynamic
`bridgepanel_token`/`trusted_ips` attributes on the server object — intentional, tests pass.

Run the focused suites from `tools/`:

```bash
python3 -m unittest bridgepanel.test_invites bridgepanel.test_auth
```

Some `TestHttpSurface` cases (file streaming/SSE/static caching) fail on a clean tree. When
failures don't touch your files, confirm they pre-exist on the untouched base before fixing
them — never carry an unrelated fix silently inside a feature commit.

## Deploy to a peer

```bash
tar czf /tmp/panel.tar.gz -C tools/bridgepanel server.py panel_html.py invites.py
bs file send <peer> /tmp/panel.tar.gz --wait      # parse dest= off the OK line
bs shell <peer> --cmd 'cd ~/bridgesessions/tools/bridgepanel && tar xzf "$(ls -t ~/.bridgesessions/received/panel.tar.gz* | head -1)"'
bs shell <peer> --cmd 'systemctl --user restart bridgepanel.service'
```

- The receive dir is staging: a name collision lands `panel.tar.gz.1`, `.2`, ... — `ls -t` the
  newest, never assume the bare name.
- Always `systemctl --user restart` after extract; the service imports modules once at start.
- The peer checkout may sit on a drifted branch with uncommitted (older) edits to the same
  files — diff the live files against your new ones before the tar overwrite, so you know
  exactly what you are replacing. The deploy is a file overwrite, not a git operation.
- Verify live with curl, both status gates:

```bash
TOK=$(cat ~/.config/bridgepanel/token); IP=100.x.y.z   # peer's tailscale IPv4
curl -s -o /dev/null -w "%{http_code}\n" http://$IP:9770/$TOK/api/invites   # expect 200
curl -s -o /dev/null -w "%{http_code}\n" http://$IP:9770/api/invites        # expect 404
```

Gate new endpoints with the same pattern, including their 400s: `POST /<token>/api/invites/page`
must answer 400 (not 500) for a bad `window_seconds` and for a non-hex token, and 404 without
the token.

Output from long compound `bs shell --cmd` commands can come back missing its leading lines —
buffer results into a file on the peer and `cat` it once at the end (see the failure table in
SKILL.md).

## Keep-alive: drain POST bodies

Every POST handler must read and discard exactly `Content-Length` body bytes before replying.
A handler that replies without draining leaves body bytes on a keep-alive connection; the next
request on that connection parses the body as a request line and the session desyncs with
confusing failures on the *following* request. `POST /api/invites` is the reference
implementation. This includes **reject paths**: 400s that skip the body read desync just the
same — call `_drain_body()` before every `reject()` on a POST route.

## Validate at the HTTP boundary

Anything a request feeds to `int()`, `re`, or a render path gets validated before use — an
uncaught `ValueError` from a bad `window_seconds` turned a malformed query into a 500. Coerce
with try/except and `reject()` 400. Validation constants live next to their enforcement in
`invites.py` (`TOKEN_PATTERN`) and are imported by `server.py`; never re-declare the token
regex inline in the HTTP layer, or the mint and render paths will accept different tokens.

## Join commands: two generators, one invariant

`invites.join_commands()` (Python) and `invCommands()` in `panel_html.INDEX_HTML` (JS) produce
the same four copy/paste commands independently. They WILL drift if edited in isolation — every
command-shape change must touch both, and `TestJsCommandParity` in `test_invites.py` enforces
it by evaluating the extracted JS through `node` and comparing output to the Python (skipped
when node is absent). Run `python3 -m unittest bridgepanel.test_invites` after either edit.
`reject()` replies text/plain, not JSON — assertions on error bodies should match plain text.

Verify emitted command strings by running the generator (node for the JS side) or checking
bytes with `repr()` — never by reading terminal output. Shell escaping makes `\n` and quote
ambiguities render doubled or invisible, so an eyeballed check proves nothing.
