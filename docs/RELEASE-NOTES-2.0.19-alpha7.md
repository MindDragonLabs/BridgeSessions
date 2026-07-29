# BridgeSessions 2.0.19-alpha7

BridgeSessions 2.0.19-alpha7 repairs remote video capture on macOS 26 and closes the identity, installer, health-probe, finite-shell, and PTY-lifecycle regressions found during fleet recovery.

## Highlights

- Native ScreenCaptureKit capture on macOS; FFmpeg only encodes PNG frames to H.264.
- `capture-video <peer>` now runs on the requested peer over pinned direct TLS.
- Successful health probes drain late output until their nonce arrives.
- Exited sessions release PTY/ConPTY runtime handles immediately, and attached
  child reaping remains owned by the output poller so final death frames arrive.
- Explicit one-shot shell commands do not enter interactive reconnect mode merely because stdin is a PTY.
- `keygen` refuses to overwrite an existing `id_ed25519*` identity.
- Installer validates OS/architecture, executable format, and reported version before atomic replacement.
- Release tests validate Linux ELF, macOS Mach-O, and Windows PE artifacts.

## macOS permissions

ScreenCaptureKit requires **System Settings → Privacy & Security → Screen & System Audio Recording** approval for the installed BridgeSessions executable. Ad-hoc rebuilds change the executable CDHash; remove and re-add the rebuilt binary rather than toggling a stale row.

## Verified scenario

Linux CTest passed 336/336, macOS CTest passed 335/335, and the release pytest suite passed 31/31.

A live capture from test-pc5 was retrieved and inspected as H.264, 1920×810, 2 fps, 3 seconds, 6 frames. The tested file SHA-256 was `dae226d32cd16e2b13d21aa2d7899156dd5ef86fc9a9471a35b89490ff348761`.

## Upgrade caution

Do not run `keygen` as a connectivity repair. Existing identities and peer pins are preserved by the release, but intentional identity rotation still requires updating every affected peer pin.
