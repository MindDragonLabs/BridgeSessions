# Bridge Panel — volume-aware navigation

This page describes how Bridge Panel lists files across named-volume roots on each peer. The feature is optional. Bridge Panel stays inbox-only when the operator chooses. The volume chips are an addition, not a replacement for the inbox view.

## Goal

An operator with multi-disk hosts needs to see what is on a peer disk without leaving the panel. The inbox is the agent drop-box. The other disks are read-only reference material for the operator's eyes.

The shipped design grows named-volume roots behind a feature flag. Inbox remains the default root when an operator selects a host. Other roots are read-only unless the operator explicitly marks them writable in the ACL file.

## Information architecture

The three columns keep their existing roles:

| Column | Role |
|---|---|
| Left | Mesh hosts. Online and offline peers appear. |
| Middle | Files on the selected host. A volume-chip row sits above the listing. |
| Right | Preview or editor. |

The volume row is a horizontally scrollable strip of chips. The first chip is always `Inbox`. The remaining chips are the host's primary disks on Windows, the host's mount points on Linux, and `/` plus `/Volumes` on macOS. Opt-in large disks live under a collapsed `Other disks` group.

### Breadcrumb

The breadcrumb follows a fixed grammar:

```text
<host> / <root> / <seg> / <seg> / <file>
```

Each node is an anchor. Clicking a segment navigates to that level. Past six segments, the middle collapses to an ellipsis that keeps the root and the last two segments visible. The grammar is the same on every host, with cosmetic differences for the root token:

- Windows: `desktop-1 / inbox`, `desktop-1 / C:`, `desktop-1 / D: / Backups / daily`.
- Linux: `node-b / inbox`, `node-b / / / srvnvme / iso`.
- macOS: `mac-build / inbox`, `mac-build / /`, `mac-build / Volumes / Data / …`.

The root segment is always labelled with the actual root being listed, never a placeholder.

### States

The middle column has explicit states for every root:

| State | Behavior |
|---|---|
| Loading | The breadcrumb renders from client state. The listing area shows a skeleton of eight rows with a caption and a cancel affordance after five seconds. |
| Empty folder | A single message plus, for the inbox only, the FilePond drop hint. |
| Host offline | The host row greys out. An open listing freezes with an inline banner that names the last successful refresh. |
| Permission denied or filtered | The lister returns a count of denied entries. The count renders as one non-clickable footer row. Names of denied entries never appear in the listing. |
| Volume vanished | The chip stays but is struck through. Clicking shows `volume not currently mounted`. |

## Root and volume model

Each root is a server-defined object:

```json
{
  "id": "desktop-1:D",
  "label": "D: · New Volume",
  "kind": "fixed",
  "os_path": "D:\\",
  "writable": false,
  "media_hint": "mixed",
  "bytes_total": 8378900000000,
  "bytes_free": 4311100000000
}
```

| Field | Meaning |
|---|---|
| `id` | Stable `host:token`. Token is `inbox`, `home`, drive letter, or mountpoint with `/` mapped to `_`. |
| `kind` | `inbox`, `home`, `fixed`, `removable`, `network`, or `system-hidden`. |
| `writable` | Server-computed. The shipped default is `true` for `inbox` and `false` otherwise. |
| `media_hint` | `mixed` or `media`. Derived from an extension histogram on first listing. |
| `bytes_total` and `bytes_free` | From the same probe call. Render as `4.2 TB free / 8.4 TB`. |

### Enumeration and filter rules

Enumeration runs server-side, before JSON serialization.

1. The `inbox` root is synthesized first. It is always present, even when every other probe fails.
2. **Windows.** Fixed NTFS or ReFS volumes only. Exclude by label and type: `System Reserved`, `Recovery`, EFI system partitions, any volume under one gigabyte without a matching filesystem label, and any CD-ROM with no media. Show the volume label when one exists.
3. **Linux.** Real mount points only. Parse `/proc/self/mounts`, keep filesystems in `{ext2, ext3, ext4, xfs, btrfs, zfs, ntfs, vfat, exfat}`, dedupe bind mounts by device, and exclude `/boot`, `/proc`, `/sys`, `/dev`, plus any `tmpfs`, `devtmpfs`, `overlay`, or snap squashfs.
4. **macOS.** `/` plus `/Volumes/*`, excluding the sealed system volume clone and anything that resolves to the same device as `/`.
5. **Opt-in tier.** Any fixed volume at or above the configured size threshold, or matching a configured name list, lives under a collapsed `Other disks` group. The threshold and the name list are configuration, not hard-coded.
6. `home` (`~`) is enumerated but not shown by default. The operator enables it per host in the ACL file.
7. `system-hidden` volumes are filtered before JSON serialization, never in the browser.

## Security

The shipped capability is reading outside `receive_dir`. Writing outside it does not ship in v1.

1. **Write scope is unchanged.** Inbox stays the only writable root in v1. The upload endpoint continues to resolve under `receive_dir` and rejects any root other than `inbox`. Writing beyond the inbox requires a per-root ACL and does not exist until the operator asks for it.
2. **`transfer.allow_sensitive_paths` stays off.** The volume browse path runs alongside the existing `bs run-script` channel and does not extend the file transfer path.
3. **`receive_dir` stays the inbox.** The daemon gains no new mesh-wide read primitive. Volume browsing is a panel-side ACL plus a versioned run-script that the daemon already runs.
4. **The daemon account is the read account.** On Windows the daemon may run as SYSTEM. Listing `C:\Users\<name>\Documents` as SYSTEM succeeds. The panel-side allowlist is the only barrier between the panel and every user profile on the box. Two consequences: `browse_roots` entries are exact prefixes matched after canonicalization, and per-user subtrees are not in the shipped allowlist.
5. **Path escape rejection, on every root.** Reject `..` after normalization on both separators. Reject NUL. On Windows roots reject drive-relative forms (`C:foo`), `\\?\` and `\\.\` device paths, UNC (`\\host\share`), and any name containing a colon after the drive root, which kills NTFS alternate data streams. Canonicalize with `realpath` or `GetFinalPathNameByHandle` and require the result to start with the allowlisted `os_path`. This closes symlink, junction, and mount-point escape.
6. **Unlistable names.** Filter `id_*` (SSH private keys), `*.pem`, `*.key`, `authorized_keys`, `ipc-token`, `.bridgesessions/config*`, and known wallet and secret basenames. The filter happens server-side. The browser never sees the names. A non-zero filter count shows as one footer row.
7. **Read and open.** Preview and download gain the same `root` and `path` validation. The existing preview size cap protects against an accidental 40 GB disk image in a video tag.
8. **Rate and scope sanity.** One in-flight listing request per host per panel session. Directory listings cap at five thousand entries with a `showing first 5,000` marker.

## API sketch

```text
GET /api/volumes?machine=<host>
→ 200 { "machine": "<host>", "volumes": [ Volume, … ] }   // inbox first
→ 503 { "error": "host_unreachable" }                     // offline

GET /api/files?machine=<host>&root=inbox&path=tools/scripts
→ 200 { "root": "inbox", "path": "tools/scripts",
        "entries": [ { "name", "kind", "size", "mtime", "ext" }, … ],
        "truncated": false, "hidden_by_policy": 2 }

GET /api/remote-file?machine=<host>&root=<root>&path=<path>   // extended
POST /api/upload                                               // existing
```

- `root` absent or `root=inbox` runs today's code path. The three additive envelope fields are safe.
- `root` other than `inbox` must match an allowlisted, non-hidden volume for that host, else `403 root_not_allowed`.
- Path violations return `400 path_rejected` with no filesystem detail.
- Errors are stable tokens, not strings, so the UI maps them to the states above.
- No new WebSocket channel and no new daemon RPC. The non-inbox lister is a versioned run-script. The script's output is strict JSON, parsed and re-validated server-side before the response is serialized.

## What this feature does not build

- A full Explorer chrome: no move, copy, rename, delete, or zip UI on any root.
- Write access beyond the inbox in v1. Phase 3 only ships if the operator asks.
- `transfer.allow_sensitive_paths` or `file.dest_allow_home` enabled anywhere.
- Browsing `~` or home volumes by default. Browsing per-user subtrees.
- Audio, media server features, sharing links, or any exposure beyond the panel origin.
- A second HTTP service, an embedded alternative file manager, or terminal-in-panel.
- Auto-defaulting to a disk on host select. The default is the inbox.

## Operational notes

- The feature ships behind `features.volume_browse`. The default is off. Off means today's panel.
- `browse_roots.json` is per host, with an absolute path allowlist. The shipped file is empty.
- A panel restart reads the new config. No daemon restart is required for an ACL change.
- Volume enumeration runs on the daemon over `bs run-script`. The script hash is pinned panel-side and re-validated on every release.

## Related

- [Bridge Panel](bridge-panel.md) — how to load and use the panel
- [Configuration](configuration.md) — `receive_dir` and transfer limits
- [Security](https://github.com/MindDragonLabs/BridgeSessions/blob/main/SECURITY.md) — trust boundary