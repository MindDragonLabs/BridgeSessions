"""Writable-root mkdir / rename / trash. Inbox + ACL volumes only."""
from __future__ import annotations

import json
import os
import shutil
import time
from pathlib import Path

from .files import receive_dir, resolve_receive_path, safe_name, safe_relpath
from .volumes import is_hidden_name, normalize_rel, resolve_os_path, root_writable


def trash_dir() -> Path:
    raw = os.environ.get("BRIDGEPANEL_TRASH_DIR")
    if raw:
        return Path(raw)
    return Path.home() / ".bridgesessions" / "trash"


def _invalidate(machine: str, root: str) -> None:
    from .cache import listing_invalidate, listing_invalidate_self_aliases
    listing_invalidate(machine, root or "inbox")
    listing_invalidate_self_aliases(machine, root or "inbox")
    try:
        from .api import query_mesh_tree
        node = (query_mesh_tree().get("node") or "")
        if node:
            listing_invalidate_self_aliases(node, root or "inbox")
    except Exception:
        pass


def _clean_rel(root: str, rel: str) -> str | None:
    root = root or "inbox"
    if root in ("", "inbox"):
        raw = (rel or "").replace("\\", "/").strip()
        if not raw or raw in (".", "./"):
            return ""
        cleaned = safe_relpath(raw)
        if not cleaned:
            return None
        return cleaned
    return normalize_rel(root, rel)


def _local_inbox(rel: str, *, must_exist: bool) -> tuple[Path | None, str]:
    cleaned = _clean_rel("inbox", rel)
    if cleaned is None:
        return None, "path_rejected"
    target = resolve_receive_path(cleaned, must_exist=must_exist)
    if target is None:
        return None, "path_rejected" if must_exist else "path_rejected"
    return target, ""


def mkdir_path(machine: str, root: str, rel: str) -> dict:
    root = root or "inbox"
    if not root_writable(root, machine):
        return {"ok": False, "error": "write_not_allowed"}
    cleaned = _clean_rel(root, rel)
    if not cleaned:
        return {"ok": False, "error": "path_rejected"}
    if is_hidden_name(cleaned.split("/")[-1]):
        return {"ok": False, "error": "path_rejected"}
    from .api import is_self_node
    if is_self_node(machine) and root in ("", "inbox"):
        target, err = _local_inbox(cleaned, must_exist=False)
        if err or target is None:
            return {"ok": False, "error": err or "path_rejected"}
        if target.exists():
            return {"ok": False, "error": "exists"}
        try:
            receive_dir().mkdir(parents=True, exist_ok=True)
            target.mkdir(parents=False)
        except OSError as exc:
            return {"ok": False, "error": str(exc)}
        _invalidate(machine, "inbox")
        return {"ok": True, "path": cleaned, "kind": "dir"}
    return _volume_or_remote(machine, root, "mkdir", cleaned)


def rename_path(machine: str, root: str, rel: str, new_name: str) -> dict:
    root = root or "inbox"
    if not root_writable(root, machine):
        return {"ok": False, "error": "write_not_allowed"}
    cleaned = _clean_rel(root, rel)
    if not cleaned:
        return {"ok": False, "error": "path_rejected"}
    raw_name = (new_name or "").strip()
    if not raw_name or "/" in raw_name or "\\" in raw_name or raw_name in (".", ".."):
        return {"ok": False, "error": "name_rejected"}
    dest_name = safe_name(raw_name)
    if dest_name != raw_name:
        return {"ok": False, "error": "name_rejected"}
    if is_hidden_name(dest_name):
        return {"ok": False, "error": "path_rejected"}
    from .api import is_self_node
    if is_self_node(machine) and root in ("", "inbox"):
        src, err = _local_inbox(cleaned, must_exist=True)
        if err or src is None:
            return {"ok": False, "error": err or "not_found"}
        dest = src.parent / dest_name
        try:
            dest.relative_to(receive_dir().resolve())
        except (ValueError, OSError):
            return {"ok": False, "error": "path_rejected"}
        if dest.exists():
            return {"ok": False, "error": "exists"}
        try:
            src.rename(dest)
        except OSError as exc:
            return {"ok": False, "error": str(exc)}
        parent = "/".join(cleaned.split("/")[:-1])
        _invalidate(machine, "inbox")
        return {"ok": True, "path": (parent + "/" + dest_name).lstrip("/"), "from": cleaned}
    return _volume_or_remote(machine, root, "rename", cleaned, dest_name=dest_name)


def trash_path(machine: str, root: str, rel: str) -> dict:
    root = root or "inbox"
    if not root_writable(root, machine):
        return {"ok": False, "error": "write_not_allowed"}
    cleaned = _clean_rel(root, rel)
    if not cleaned:
        return {"ok": False, "error": "path_rejected"}
    from .api import is_self_node
    if is_self_node(machine) and root in ("", "inbox"):
        src, err = _local_inbox(cleaned, must_exist=True)
        if err or src is None:
            return {"ok": False, "error": err or "not_found"}
        stamp = time.strftime("%Y%m%dT%H%M%S")
        dest_dir = trash_dir() / f"{stamp}_{src.name[:80]}"
        try:
            dest_dir.mkdir(parents=True, exist_ok=False)
            dest = dest_dir / src.name
            shutil.move(str(src), str(dest))
            (dest_dir / "meta.json").write_text(
                json.dumps({"machine": machine, "root": root, "rel": cleaned,
                            "name": src.name, "when": stamp}),
                encoding="utf-8",
            )
        except OSError as exc:
            return {"ok": False, "error": str(exc)}
        _invalidate(machine, "inbox")
        return {"ok": True, "trashed": cleaned, "trash": str(dest_dir)}
    return _volume_or_remote(machine, root, "trash", cleaned)


def _volume_or_remote(
    machine: str, root: str, action: str, rel: str, dest_name: str = "",
) -> dict:
    """ACL-writable volume (local) or remote inbox/volume via run-script."""
    from .api import is_self_node, list_host_volumes, query_mesh_tree

    vols = list_host_volumes(machine).get("volumes") or []
    match = next((v for v in vols if v.get("token") == root), None)
    if root not in ("", "inbox") and (match is None or not match.get("writable") or not match.get("os_path")):
        return {"ok": False, "error": "write_not_allowed"}
    tree = query_mesh_tree()
    if is_self_node(machine, tree) and root not in ("", "inbox") and match:
        return _local_volume_op(match["os_path"], action, rel, dest_name, machine, root)
    return _remote_op(machine, root, action, rel, dest_name, match)


def _local_volume_op(root_os: str, action: str, rel: str, dest_name: str,
                     machine: str, root: str) -> dict:
    windows = os.name == "nt"
    abs_path, err = resolve_os_path(root_os, rel, windows=windows)
    if err:
        return {"ok": False, "error": err}
    target = Path(abs_path)
    root_real = os.path.realpath(root_os)
    try:
        real = os.path.realpath(target if target.exists() else target.parent)
        if real != root_real and not real.startswith(root_real.rstrip(os.sep) + os.sep):
            return {"ok": False, "error": "path_rejected"}
    except OSError:
        return {"ok": False, "error": "path_rejected"}
    try:
        if action == "mkdir":
            if target.exists():
                return {"ok": False, "error": "exists"}
            target.mkdir(parents=False)
        elif action == "rename":
            if not target.exists():
                return {"ok": False, "error": "not_found"}
            dest = target.parent / dest_name
            if dest.exists():
                return {"ok": False, "error": "exists"}
            target.rename(dest)
        elif action == "trash":
            if not target.exists():
                return {"ok": False, "error": "not_found"}
            stamp = time.strftime("%Y%m%dT%H%M%S")
            dest_dir = trash_dir() / f"{stamp}_{target.name[:80]}"
            dest_dir.mkdir(parents=True, exist_ok=False)
            shutil.move(str(target), str(dest_dir / target.name))
            (dest_dir / "meta.json").write_text(
                json.dumps({"machine": machine, "root": root, "rel": rel, "name": target.name, "when": stamp}),
                encoding="utf-8",
            )
        else:
            return {"ok": False, "error": "unknown_action"}
    except OSError as exc:
        return {"ok": False, "error": str(exc)}
    _invalidate(machine, root)
    return {"ok": True, "path": rel, "action": action}


def _remote_op(machine: str, root: str, action: str, rel: str, dest_name: str, match: dict | None) -> dict:
    from .api import _peer_for, _peer_windows, _run_remote_script, query_mesh_tree

    tree = query_mesh_tree()
    windows = _peer_windows(_peer_for(machine, tree))
    if root in ("", "inbox"):
        root_os = "~/.bridgesessions/received" if not windows else ""
        if windows:
            # Remote Windows inbox lives under the peer profile; list scripts expand it.
            root_os = r"$env:USERPROFILE\.bridgesessions\received"
    else:
        if not match or not match.get("os_path"):
            return {"ok": False, "error": "write_not_allowed"}
        root_os = match["os_path"]
    if windows:
        raw = _run_remote_script(machine, _ps_mutate(root_os, action, rel, dest_name), ".ps1", "powershell")
    else:
        raw = _run_remote_script(machine, _py_mutate(root_os, action, rel, dest_name), ".py", "python3")
    if not isinstance(raw, dict) or not raw.get("ok"):
        err = raw.get("error") if isinstance(raw, dict) else "op failed"
        return {"ok": False, "error": err or "op failed"}
    _invalidate(machine, root or "inbox")
    return {"ok": True, "path": rel, "action": action, "local": False}


def _py_mutate(root_os: str, action: str, rel: str, dest_name: str) -> str:
    return (
        "import json,os,shutil,time\n"
        f"root=os.path.expanduser({root_os!r})\n"
        f"rel={rel!r}; action={action!r}; dest_name={dest_name!r}\n"
        "root=os.path.realpath(root)\n"
        "prefix=root if root.endswith(os.sep) else root+os.sep\n"
        "path=os.path.realpath(os.path.join(root, rel)) if rel else root\n"
        "if path!=root and not path.startswith(prefix):\n"
        "    print(json.dumps({'ok':False,'error':'path_rejected'})); raise SystemExit(0)\n"
        "try:\n"
        "    if action=='mkdir':\n"
        "        if os.path.exists(path):\n"
        "            print(json.dumps({'ok':False,'error':'exists'})); raise SystemExit(0)\n"
        "        os.mkdir(path)\n"
        "    elif action=='rename':\n"
        "        dest=os.path.join(os.path.dirname(path), dest_name)\n"
        "        dreal=os.path.realpath(os.path.dirname(path))\n"
        "        if dreal!=root and not dreal.startswith(prefix):\n"
        "            print(json.dumps({'ok':False,'error':'path_rejected'})); raise SystemExit(0)\n"
        "        if os.path.exists(dest):\n"
        "            print(json.dumps({'ok':False,'error':'exists'})); raise SystemExit(0)\n"
        "        os.rename(path, dest)\n"
        "    elif action=='trash':\n"
        "        trash=os.path.join(root, '.bridgepanel-trash')\n"
        "        os.makedirs(trash, exist_ok=True)\n"
        "        stamp=time.strftime('%Y%m%dT%H%M%S')\n"
        "        dest=os.path.join(trash, stamp+'_'+os.path.basename(path))\n"
        "        shutil.move(path, dest)\n"
        "    else:\n"
        "        print(json.dumps({'ok':False,'error':'unknown_action'})); raise SystemExit(0)\n"
        "    print(json.dumps({'ok':True}))\n"
        "except OSError as e:\n"
        "    print(json.dumps({'ok':False,'error':str(e)}))\n"
    )


def _ps_mutate(root_os: str, action: str, rel: str, dest_name: str) -> str:
    # root_os may be a literal path or a PowerShell expression for inbox.
    if root_os.startswith("$"):
        root_expr = root_os
    else:
        root_expr = json.dumps(root_os)
    return (
        f"$root = {root_expr}\n"
        f"$rel = {json.dumps(rel)}\n"
        f"$action = {json.dumps(action)}\n"
        f"$destName = {json.dumps(dest_name)}\n"
        "if (-not (Test-Path -LiteralPath $root)) { New-Item -ItemType Directory -Force -Path $root | Out-Null }\n"
        "$prefix = $root.TrimEnd('\\') + '\\'\n"
        "if ($rel) { $p = Join-Path $root ($rel -replace '/','\\') } else { $p = $root }\n"
        "if ($p -ne $root -and -not $p.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {\n"
        "  Write-Output '{\"ok\":false,\"error\":\"path_rejected\"}'; exit 0\n"
        "}\n"
        "try {\n"
        "  if ($action -eq 'mkdir') {\n"
        "    if (Test-Path -LiteralPath $p) { Write-Output '{\"ok\":false,\"error\":\"exists\"}'; exit 0 }\n"
        "    New-Item -ItemType Directory -Path $p | Out-Null\n"
        "  } elseif ($action -eq 'rename') {\n"
        "    $dest = Join-Path (Split-Path -Parent $p) $destName\n"
        "    if (Test-Path -LiteralPath $dest) { Write-Output '{\"ok\":false,\"error\":\"exists\"}'; exit 0 }\n"
        "    Rename-Item -LiteralPath $p -NewName $destName\n"
        "  } elseif ($action -eq 'trash') {\n"
        "    $trash = Join-Path $root '.bridgepanel-trash'\n"
        "    if (-not (Test-Path -LiteralPath $trash)) { New-Item -ItemType Directory -Force -Path $trash | Out-Null }\n"
        "    $stamp = Get-Date -Format 'yyyyMMddTHHmmss'\n"
        "    $dest = Join-Path $trash ($stamp + '_' + (Split-Path -Leaf $p))\n"
        "    Move-Item -LiteralPath $p -Destination $dest\n"
        "  } else { Write-Output '{\"ok\":false,\"error\":\"unknown_action\"}'; exit 0 }\n"
        "  Write-Output '{\"ok\":true}'\n"
        "} catch {\n"
        "  $m = $_.Exception.Message.Replace('\"','')\n"
        "  Write-Output ('{\"ok\":false,\"error\":\"' + $m + '\"}')\n"
        "}\n"
    )
