# BridgeSessions 2.0.20-alpha10

BridgeSessions 2.0.20-alpha10 ships the fleet-directory-driven mesh tooling, trust propagation fixes, remote version tracking, and fully path-sanitized release binaries.

## Highlights

- `peers add --pubkey` writes pinned seed entries, so peers added at runtime work with `require_seed_pins=true`.
- Fleet seed pins and the authorized-key roster can be synchronized from an external fleet directory; daemons hot-reload seeds within ~30 s with no restart.
- Remote peer version tracking in `bs fleet` output.
- Memory/RSS health-session leak fixed (daemon RSS back to ~15–20 MB under load).
- Windows daemon ships as a GUI-subsystem executable (`-mwindows`, `mainCRTStartup`) suitable for Scheduled Task operation.
- Release binaries are build-path sanitized: no usernames or home-directory paths baked into Linux, Windows, or macOS artifacts.

## Artifacts

SHA-256 checksums in `dist/SHA256SUMS`. Linux is a portable static x86-64 build (glibc 2.35 floor); macOS is arm64; Windows is x86-64 PE with OS-only DLL dependencies.

## Verified scenario

CTest suite passed 26/26 on touched suites; bridgepanel pytest passed 25/25; bs-client host-config tests passed 27 assertions / 5 cases. Cross-dial health verified across a 9-peer mesh (Linux, macOS, Windows).
