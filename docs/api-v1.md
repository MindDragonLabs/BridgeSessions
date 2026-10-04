# BridgePanel API v1

The v1 HTTP API is the control surface for a native BridgeSessions worker. It
is JSON over the existing private panel listener and is frozen for this lane.
Every route requires an administrator session or a non-expired scoped API
token. There is no trusted-IP exception. Errors have this shape and use a
meaningful HTTP status:

```json
{"ok":false,"error":{"code":"invalid_name","message":"..."}}
```

Machines and sessions are stable names (`.` means the local worker). Session
output is actual bytes, represented as base64; it is never replaced with
metadata or a rendered transcript.

| Method | Route | Request / result |
|---|---|---|
| GET | `/api/v1/capabilities` | Reports live worker/adapter availability. |
| GET | `/api/v1/peers` | Mesh peers and local name. |
| GET | `/api/v1/sessions?machine=...` | Sessions for one machine. |
| POST | `/api/v1/sessions` | `{machine,name,command,cols?,rows?}`. |
| POST | `/api/v1/sessions/input` | `{machine,session,data_b64}`. |
| GET | `/api/v1/sessions/output?machine=...&session=...&offset=0&limit=65536` | `{ok,offset,text_b64,reset}`. |
| DELETE | `/api/v1/sessions?machine=...&session=...` | Kills one session. |
| GET | `/api/v1/files?machine=...&root=inbox&path=...` | Existing permitted listing operations. |
| GET | `/api/v1/files/content?...` | `{content_b64,sha256}` for permitted reads. |
| POST | `/api/v1/files/content` | `{machine,root,path,content_b64}`; existing ACLs remain authoritative. |
| GET | `/api/v1/agents` | Configured, actually available adapters. |
| POST | `/api/v1/chat` | `{machine,agent,prompt,request_id}`; returns an async request. |
| GET | `/api/v1/chat/{request_id}` | Request lifecycle and result. |
| DELETE | `/api/v1/chat/{request_id}` | Cancels a pending/running request. |

Administrator-only device-token management is also available at
`/api/v1/auth/tokens`: GET lists redacted active tokens, POST accepts
`{label,scopes,ttl?}` and returns the token once, and DELETE
`/api/v1/auth/tokens/{id}` revokes it. Device tokens cannot mint or revoke
other tokens.

Native control IPC is intentionally limited to these commands:

```text
SESSION_INPUT <machine> <session> <b64>
SESSION_SCROLLBACK <machine> <session> <offset> <limit>
SESSION_KILL <machine> <session>
```

The panel validates names and bounds before sending them. A missing worker is
reported as unavailable; capabilities never claim a fabricated wired state.
Chat uses the frozen `status` property: `pending`, `running`, `completed`,
`failed`, or `cancelled`. DELETE preserves completed/failed/cancelled statuses.
Request IDs belong to the authenticated credential identity. Reusing one with
a different payload returns 409; other credentials cannot read or cancel it.
Results expire after one hour. Storage is capped at 256 rows; two processes
may run and eight requests may wait. Capacity exhaustion returns 429. Each
process has a 120-second deadline and 64 KiB output limit. Cancellation and
timeout kill the subprocess group.

Headless chat requires POSIX process groups and supports only `machine: "."`;
remote machines and unsupported platforms fail explicitly.
`BRIDGEPANEL_AGENT_ADAPTERS` enables an allowlist of `claude`, `codex`, `grok`,
`hermes`, `kimi`, and `mcode`. Each requires an absolute executable path in
`BRIDGEPANEL_AGENT_<UPPERCASE_NAME>_EXECUTABLE` and an existing dedicated
credential home in `BRIDGEPANEL_AGENT_<UPPERCASE_NAME>_HOME`. The panel's own
home/config directory is rejected. An unsupported name or missing executable
or credential home is reported unavailable. Availability means the headless
contract can be launched, not that provider authentication/inference has been
verified; runtime failures return a failed request honestly.

Arguments follow the installed `cli-sub` headless contracts and are passed as
an argv list. Each request runs in a disposable working directory, with only
the selected adapter HOME/XDG directories, a restricted PATH, and locale.
Panel credentials and ambient provider environment variables are never
inherited. Provider login belongs in that dedicated adapter home. No provider
request is made during tests.

`BRIDGEPANEL_AGENT_FIXTURES=1` enables a labeled synthetic fixture only.
`tools/bridgepanel/v1_e2e_harness.py` checks socket framing against an IPC
fixture; it does not prove real daemon execution or live inference. The
parent/backend real-daemon harness owns verification of the vertical slice.

`BRIDGEPANEL_BS_BINARY` may point to an executable candidate binary. It is
validated consistently for session creation and existing transfer helpers.
