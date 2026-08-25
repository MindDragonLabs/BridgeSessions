"""Named volume roots for BridgePanel — classify, allow, list; writes only via ACL."""
from __future__ import annotations

import json
import os
import re
import stat
from pathlib import Path
from urllib.parse import unquote

from .files import file_kind, safe_relpath

OTHER_DISK_BYTES = 4 * 1000**4  # 4 TB
MIN_WIN_BYTES = 1 * 1000**3     # hide System Reserved-sized slices
HIDDEN_WIN_LABELS = {
    "system reserved", "recovery", "system", "efi", "esp",
}
HIDDEN_LINUX_MOUNTS = {
    "/boot", "/boot/efi", "/efi", "/proc", "/sys", "/dev", "/run",
    "/snap", "/var/lib/snapd",
}
SKIP_FS = {
    "tmpfs", "devtmpfs", "devfs", "overlay", "squashfs", "proc", "sysfs",
    "cgroup", "cgroup2", "bpf", "tracefs", "debugfs", "pstore", "hugetlbfs",
    "mqueue", "securityfs", "configfs", "fusectl", "rpc_pipefs", "binfmt_misc",
    "autofs", "nsfs", "ramfs", "efivarfs",
}
OK_LINUX_FS = {
    "ext2", "ext3", "ext4", "xfs", "btrfs", "zfs", "ntfs", "vfat", "exfat",
    "fuseblk", "apfs",
}
_REL_VOL = re.compile(r"^[A-Za-z0-9._ /()\[\]-]+$")
_HIDDEN_EXACT = {
    "authorized_keys", "known_hosts", "ipc-token", "config.yaml", "config.yml",
    "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519",
}
_HIDDEN_PREFIX = ("id_rsa", "id_dsa", "id_ecdsa", "id_ed25519", "id_")
_HIDDEN_SUFFIX = (".pem", ".key", ".p12", ".pfx")
DEFAULT_PINS = {
    "avirserver2016": ("C", "D", "E"),
    "avirserver2020": ("C", "D", "E"),
}


def volume_browse_enabled() -> bool:
    raw = os.environ.get("BRIDGEPANEL_VOLUME_BROWSE", "1").strip().lower()
    return raw not in ("0", "false", "off", "no")


def pin_tokens_for(host: str) -> tuple[str, ...]:
    return DEFAULT_PINS.get(host, ())


def inbox_volume(host: str) -> dict:
    return {
        "id": f"{host}:inbox",
        "token": "inbox",
        "label": "Inbox",
        "kind": "inbox",
        "os_path": "",
        "writable": True,
        "media_hint": "mixed",
        "bytes_total": 0,
        "bytes_free": 0,
        "group": "primary",
    }


def _win_token(device_id: str) -> str:
    letter = (device_id or "").strip().rstrip(":\\/").upper()
    return letter[:1] if letter and letter[0].isalpha() else ""


def classify_windows_volume(
    host: str,
    device_id: str,
    drive_type: int,
    fs: str,
    size: int,
    free: int,
    label: str,
    pins: tuple[str, ...] | None = None,
) -> dict | None:
    token = _win_token(device_id)
    if not token:
        return None
    if int(drive_type or 0) != 3:
        return None
    if not (fs or "").strip():
        return None
    if int(size or 0) < MIN_WIN_BYTES:
        return None
    lab = (label or "").strip()
    if lab.lower() in HIDDEN_WIN_LABELS:
        return None
    pins = pins if pins is not None else pin_tokens_for(host)
    group = "other" if int(size or 0) >= OTHER_DISK_BYTES and token not in pins else "primary"
    shown = f"{token}:"
    if lab:
        shown = f"{token}: · {lab}"
    return {
        "id": f"{host}:{token}",
        "token": token,
        "label": shown,
        "kind": "fixed",
        "os_path": f"{token}:\\",
        "writable": False,
        "media_hint": "mixed",
        "bytes_total": int(size or 0),
        "bytes_free": int(free or 0),
        "group": group,
    }


def _linux_token(target: str) -> str:
    if target == "/":
        return "/"
    return target.replace("/", "_")


def classify_linux_mount(
    host: str,
    target: str,
    fstype: str,
    size: int,
    free: int,
    pins: tuple[str, ...] | None = None,
) -> dict | None:
    target = target or ""
    if not target.startswith("/"):
        return None
    if target in HIDDEN_LINUX_MOUNTS or target.startswith("/proc") or target.startswith("/sys"):
        return None
    fs = (fstype or "").lower()
    if fs in SKIP_FS or (fs not in OK_LINUX_FS and not fs.startswith("fuse")):
        return None
    token = _linux_token(target)
    pins = pins if pins is not None else pin_tokens_for(host)
    group = "other" if int(size or 0) >= OTHER_DISK_BYTES and token not in pins else "primary"
    return {
        "id": f"{host}:{token}",
        "token": token,
        "label": target,
        "kind": "fixed",
        "os_path": target,
        "writable": False,
        "media_hint": "mixed",
        "bytes_total": int(size or 0),
        "bytes_free": int(free or 0),
        "group": group,
    }


def assemble_volumes(host: str, disks: list[dict | None]) -> list[dict]:
    out = [inbox_volume(host)]
    seen: set[str] = {"inbox"}
    for d in disks:
        if not d or d.get("token") in seen:
            continue
        seen.add(d["token"])
        out.append(d)
    return out


def browse_roots_path() -> Path:
    from .consts import config_home
    return config_home() / "browse_roots.json"


def load_browse_roots() -> dict:
    """Per-host ACL. Shape: {host: [{path, token?, label?, writable?}]}."""
    try:
        raw = json.loads(browse_roots_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return raw if isinstance(raw, dict) else {}


def acl_for(host: str) -> list[dict]:
    data = load_browse_roots()
    rows = data.get(host) or []
    if host in ("(local)", "local"):
        rows = list(rows) + list(data.get("(local)") or [])
    out: list[dict] = []
    for row in rows:
        if isinstance(row, dict) and (row.get("path") or row.get("token")):
            out.append(row)
    return out


def root_writable(root: str, host: str = "") -> bool:
    root = root or "inbox"
    if root in ("", "inbox"):
        return True
    if not host:
        return False
    for row in acl_for(host):
        if not row.get("writable"):
            continue
        token = str(row.get("token") or "").strip()
        path = str(row.get("path") or "").strip()
        if token and token == root:
            return True
        if path and _token_from_path(path) == root:
            return True
    return False


def _token_from_path(path: str) -> str:
    path = (path or "").strip()
    if re.match(r"^[A-Za-z]:[\\/]?$", path):
        return path[0].upper()
    if path.startswith("/"):
        return _linux_token(path)
    return Path(path.rstrip("\\/")).name or "extra"


def apply_acl(host: str, volumes: list[dict]) -> list[dict]:
    """Mark ACL-writable volumes and append extra confined roots."""
    vols = [dict(v) for v in volumes]
    by_token = {v.get("token"): v for v in vols}
    by_path: dict[str, dict] = {}
    for v in vols:
        p = os.path.normpath(str(v.get("os_path") or ""))
        if p:
            by_path[p] = v
    for row in acl_for(host):
        path = os.path.normpath(str(row.get("path") or "").strip())
        token = str(row.get("token") or "").strip()
        writable = bool(row.get("writable"))
        label = str(row.get("label") or "").strip()
        match = by_token.get(token) if token else None
        if match is None and path:
            match = by_path.get(path)
        if match is not None:
            if writable:
                match["writable"] = True
            if label:
                match["label"] = label
            continue
        if not path or path in HIDDEN_LINUX_MOUNTS:
            continue
        tok = token or _token_from_path(path)
        if tok in by_token:
            if writable:
                by_token[tok]["writable"] = True
            continue
        extra = {
            "id": f"{host}:{tok}",
            "token": tok,
            "label": label or tok,
            "kind": "home",
            "os_path": path,
            "writable": writable,
            "media_hint": "mixed",
            "bytes_total": 0,
            "bytes_free": 0,
            "group": "primary",
        }
        vols.append(extra)
        by_token[tok] = extra
        by_path[path] = extra
    return vols


def media_hint_from_items(items: list[dict]) -> str:
    files = [i for i in items if not i.get("dir")]
    if not files:
        return "mixed"
    media = sum(1 for i in files if i.get("kind") in ("image", "video"))
    return "media" if media / len(files) >= 0.6 else "mixed"


def is_hidden_name(name: str) -> bool:
    base = (name or "").split("/")[-1].split("\\")[-1]
    if not base:
        return True
    low = base.lower()
    if low in _HIDDEN_EXACT:
        return True
    if low.startswith(_HIDDEN_PREFIX):
        return True
    if low.endswith(_HIDDEN_SUFFIX):
        return True
    if low.startswith("id_") and "." not in low[3:]:
        return True
    return False


def filter_hidden(items: list[dict]) -> tuple[list[dict], int]:
    kept: list[dict] = []
    n = 0
    for it in items:
        if is_hidden_name(str(it.get("name") or "")):
            n += 1
            continue
        kept.append(it)
    return kept, n


def normalize_rel(root: str, rel: str) -> str | None:
    """Return a cleaned relative path, or None if rejected. Empty = root of volume."""
    root = root or "inbox"
    raw = unquote(rel or "").replace("\\", "/").strip()
    if root in ("", "inbox"):
        if not raw or raw in (".", "./"):
            return ""
        cleaned = safe_relpath(raw)
        if cleaned == "" and raw not in ("", ".", "./"):
            return None
        return cleaned
    if not raw or raw in (".", "./"):
        return ""
    if raw.startswith("//") or raw.startswith("\\\\"):
        return None
    if raw.startswith("\\\\?\\") or raw.startswith("//?/") or raw.startswith("\\\\.\\"):
        return None
    if re.match(r"^[A-Za-z]:", raw) and ("/" in raw[2:] or raw[2:3] not in ("", "/")):
        # C:foo or C:/abs — reject drive-relative / absolute
        return None
    if ":" in raw:
        return None
    parts: list[str] = []
    for part in raw.split("/"):
        if part in ("", "."):
            continue
        if part == ".." or not _REL_VOL.match(part):
            return None
        parts.append(part[:180])
    return "/".join(parts)[:800]


def resolve_os_path(root_os: str, rel: str, *, windows: bool) -> tuple[str, str]:
    """Join allowlisted os root + rel. Returns (path, err_token)."""
    cleaned = normalize_rel("vol", rel if root_os else rel)
    # normalize_rel with dummy root "vol" applies volume rules
    if cleaned is None:
        return "", "path_rejected"
    if windows:
        base = (root_os or "").replace("/", "\\")
        if not re.match(r"^[A-Za-z]:\\", base):
            return "", "path_rejected"
        prefix = base.rstrip("\\") + "\\"
        if cleaned:
            target = prefix + cleaned.replace("/", "\\")
        else:
            target = prefix
        # lexical collapse
        collapsed = _win_collapse(target)
        if not collapsed.upper().startswith(prefix.upper()) and collapsed.upper() + "\\" != prefix.upper():
            if collapsed.upper().rstrip("\\") != prefix.upper().rstrip("\\"):
                return "", "path_rejected"
        return collapsed, ""
    base = root_os or "/"
    if not base.startswith("/"):
        return "", "path_rejected"
    if cleaned:
        target = (base.rstrip("/") + "/" + cleaned) if base != "/" else "/" + cleaned
    else:
        target = base
    collapsed = os.path.normpath(target)
    if base == "/":
        if not collapsed.startswith("/"):
            return "", "path_rejected"
    else:
        if collapsed != base and not collapsed.startswith(base.rstrip("/") + "/"):
            return "", "path_rejected"
    return collapsed, ""


def _win_collapse(path: str) -> str:
    path = path.replace("/", "\\")
    m = re.match(r"^([A-Za-z]:)(.*)$", path)
    if not m:
        return path
    drive, rest = m.group(1), m.group(2)
    parts: list[str] = []
    for part in rest.split("\\"):
        if part in ("", "."):
            continue
        if part == "..":
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return drive + "\\" + "\\".join(parts)


def root_display(token: str) -> str:
    if token in ("", "inbox"):
        return "inbox"
    if len(token) == 1 and token.isalpha():
        return token.upper() + ":"
    if token == "/":
        return "/"
    if token.startswith("_"):
        return token.replace("_", "/")
    return token


def list_dir_os(abs_path: str, rel: str) -> dict:
    """List one local directory. Caller already validated prefix."""
    items: list[dict] = []
    try:
        for entry in os.scandir(abs_path):
            try:
                if entry.is_symlink():
                    continue
                st = entry.stat(follow_symlinks=False)
                if stat.S_ISLNK(st.st_mode):
                    continue
                is_dir = stat.S_ISDIR(st.st_mode)
            except OSError:
                continue
            items.append({
                "name": entry.name,
                "dir": is_dir,
                "size": 0 if is_dir else int(st.st_size),
                "mtime": int(st.st_mtime),
                "kind": file_kind(entry.name, is_dir),
            })
    except OSError as exc:
        return {"ok": False, "error": str(exc), "path": rel, "items": []}
    items.sort(key=lambda x: (not x["dir"], x["name"].lower()))
    kept, hidden = filter_hidden(items)
    cap = 5000
    truncated = len(kept) > cap
    kept = kept[:cap]
    return {
        "ok": True,
        "path": rel,
        "count": len(kept),
        "items": kept,
        "hidden_by_policy": hidden,
        "truncated": truncated,
        "media_hint": media_hint_from_items(kept),
    }


def list_local_linux_volumes(host: str) -> list[dict]:
    disks: list[dict | None] = []
    seen_dev: set[str] = set()
    try:
        lines = Path("/proc/self/mounts").read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return assemble_volumes(host, [])
    for line in lines:
        parts = line.split()
        if len(parts) < 3:
            continue
        src, target, fstype = parts[0], parts[1].replace("\\040", " "), parts[2]
        if src in seen_dev:
            continue
        try:
            st = os.statvfs(target)
        except OSError:
            continue
        size = int(st.f_frsize * st.f_blocks)
        free = int(st.f_frsize * st.f_bavail)
        classified = classify_linux_mount(host, target, fstype, size, free)
        if classified:
            seen_dev.add(src)
            disks.append(classified)
    return assemble_volumes(host, disks)


def decorate_inbox_listing(out: dict, rel: str) -> dict:
    items = list(out.get("items") or [])
    kept, hidden = filter_hidden(items)
    out = dict(out)
    out["items"] = kept
    out["count"] = len(kept)
    out["root"] = "inbox"
    out["path"] = rel
    out["hidden_by_policy"] = hidden
    out["truncated"] = False
    out["media_hint"] = media_hint_from_items(kept)
    return out
