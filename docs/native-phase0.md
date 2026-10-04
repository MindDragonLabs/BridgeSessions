# BridgeSessions native shared core — phase 0 contract

## Status and scope

This document records the frozen client-side contract used by the phase-0
native lane. BridgePanel is the only server dependency. The routes below are
implemented in the candidate BridgePanel backend; a local fixture pass alone
does not prove a live server round trip.

All routes are prefixed with /api/v1. Every request carries:

    Authorization: Bearer <per-device-api-credential>
    Accept: application/json

Credentials never appear in URL query strings. API errors have meaningful HTTP
status codes and this JSON shape:

    {"ok":false,"error":{"code":"stable_code","message":"safe diagnostic"}}

The client bounds request/response sizes, headers, terminal output limits,
terminal dimensions, and HTTP time. A cancelled request stops before a new
write and the transport closes its connection on failure. Error messages are
redacted against the bearer value.

## Discovery and operations

| Method and route | Request | Required response |
| --- | --- | --- |
| GET /api/v1/capabilities | None | Live worker and adapter capabilities. |
| GET /api/v1/peers | None | Mesh peers and local machine name. |
| GET /api/v1/sessions?machine=... | URL-encoded machine name | Sessions belonging to that machine. |
| POST /api/v1/sessions | {machine,name,command,cols,rows} | {ok:true, ...stable session identity...}. name is stable within a machine; dimensions are bounded positive integers. |
| POST /api/v1/sessions/input | {machine,session,data_b64} | {ok:true} after the bytes are accepted by that session. Base64 decodes bytes without Unicode loss. |
| GET /api/v1/sessions/output?machine=...&session=...&offset=...&limit=65536 | Byte offset and bounded byte limit | {ok:true,offset,text_b64,reset}. offset is the next byte cursor returned by the daemon; assign it directly. reset:true means clear reconstructed partial data and continue at that cursor. |
| DELETE /api/v1/sessions?machine=...&session=... | Stable machine/session names | {ok:true} after termination was requested/confirmed. |
| GET /api/v1/files?machine=...&root=...&path=... | Server-defined root and confined relative path | Bounded listing metadata. Root/path authorization and traversal rejection are server responsibilities. |
| GET /api/v1/files/content?machine=...&root=...&path=... | Server-defined root and confined relative path | JSON with bounded content_b64 and metadata. |
| POST /api/v1/files/content | {machine,root,path,content_b64} | {ok:true,...} after a permitted bounded write. |
| GET /api/v1/agents | None | Stable agent identifiers and capability metadata. |
| POST /api/v1/chat | {machine,agent,prompt,request_id} | {ok:true,request_id,status:"pending"}. request_id is client-selected and idempotency-scoped. |
| GET /api/v1/chat/{request_id} | URL-encoded request identity | {ok:true,request_id,status,...reply...}. status distinguishes pending, running, completed, failed, and cancelled. |
| DELETE /api/v1/chat/{request_id} | Stable request identity | {ok:true,status:"cancelled"} or a meaningful already-terminal result. |

The C++ client exposes these operations. Chat and files are actual
transport/API boundaries; shells display server responses or errors and do
not invent capability flags.

## Transport and parsing rules

- http:// is accepted only for loopback fixture/development endpoints.
- Remote endpoints use https://; OpenSSL validates system/configured CA roots
  and the DNS/IP hostname. Certificate verification cannot be disabled.
- The parser rejects malformed status lines, duplicate response headers,
  oversized headers/bodies, invalid content lengths, invalid chunk framing,
  unsupported schemes, URL fragments, userinfo, and CR/LF header injection.
- Core transport defaults are 32 KiB response headers and 8 MiB request and
  response bodies, with a 5 second timeout. The v1 API caps JSON request bodies
  at 256 KiB, terminal input and output chunks at 65536 bytes, columns at 500,
  and rows at 300.
- HTTP connections are closed per request. This makes cancellation and
  credential isolation explicit for phase 0.
- Base64 is strict and output is decoded as bytes first. IncrementalUtf8Decoder
  converts UI text incrementally, retains an incomplete final code point across
  polls, and emits U+FFFD for invalid bytes. It does not alter byte offsets.
- reset is handled before advancing the output cursor. A client that sees reset
  must use the server offset, then continue from that byte offset.

## Proof commands

Build/test:

    cmake -S native -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug \
      -DBS_NATIVE_BUILD_TESTS=ON
    cmake --build build/native --parallel 2
    ctest --test-dir build/native --output-on-failure

The fixture reads full Content-Length bodies and takes synchronized request
snapshots. It verifies the real bearer header, JSON payloads, query encoding,
discovery, and session operations. Tables cover malformed HTTP/JSON/base64,
wrong types, bounds, and next-cursor/reset semantics; additional checks cover
timeout, cancellation, split UTF-8, and untrusted loopback TLS. Tests retain
assertions in Release builds and have a 20 second CTest timeout. An unavailable
loopback listener fails the fixture test and must be rerun by the parent.

For restricted environments, ./build/native/native_core_tests --offline runs
only UTF-8 and local URL/request/client validation. Its success does not claim
that loopback HTTP or TLS tests passed.

Live round trip, only against an approved test panel and machine:

    BRIDGEPANEL_URL='https://<approved-panel>' \
    BRIDGEPANEL_TOKEN='<ephemeral-device-token>' \
    BRIDGEPANEL_MACHINE='<approved-machine>' \
    ./build/native/native_phase0_smoke

The executable discovers capabilities, creates a uniquely named cat session,
sends a unique marker, polls byte output with bounded retries, and deletes the
session. It exits nonzero if any operation fails or the marker is not read
back. The marker is reconstructed across chunks; reset clears partial data.
Only --url, --machine, and --name are accepted as arguments. The credential
must come from BRIDGEPANEL_TOKEN; no token default is embedded.

## Backend and release gaps

No live proof was run from this restricted Linux environment. Remaining
release work includes live round-trip proof, validation of chat lifecycle and
confined file operations against the server, per-device enrollment/revocation,
and remote HTTPS deployment. It must also run the AppKit build/run on canonical
macOS, validate Win32/UIKit/NDK packaging on their hosts, and perform
code-signing/notarizing.
No release, push, tag, publish, deploy, credential rotation, or live-config
change is part of this lane.
