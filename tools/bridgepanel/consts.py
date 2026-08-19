"""BridgePanel — constants, VERSION, paths, IPC ports."""
from __future__ import annotations

import os
from pathlib import Path

APP = "BridgePanel"


def release_version() -> str:
    """Read the repository/package VERSION without maintaining a second copy."""
    override = os.environ.get("BRIDGEPANEL_VERSION")
    if override:
        return override
    here = Path(__file__).resolve()
    for parent in (here.parent, *list(here.parents)[:3]):
        candidate = parent / "VERSION"
        try:
            version = candidate.read_text(encoding="utf-8").strip()
        except OSError:
            continue
        if version:
            return version
    return "dev"


VERSION = release_version()
BUILDTAG = VERSION
DEFAULT_BIND = os.environ.get("BRIDGEPANEL_BIND", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("BRIDGEPANEL_PORT", "9770"))
MAX_UPLOAD = 10 * 1024 * 1024  # 10 MB — docs lane (/api/save)
_DEFAULT_FILE_UPLOAD = 256 * 1024 * 1024  # 256 MB — file lane default
BS_IPC_PORT = 19980
BS_IPC_TIMEOUT = 2  # seconds


def max_file_upload() -> int:
    """File-lane cap (/api/upload, /api/remote-file). Docs lane stays MAX_UPLOAD."""
    raw = os.environ.get("BRIDGEPANEL_MAX_FILE_UPLOAD", str(_DEFAULT_FILE_UPLOAD))
    try:
        return max(1, int(raw))
    except ValueError:
        return _DEFAULT_FILE_UPLOAD


def file_timeout_sec() -> float:
    raw = os.environ.get("BRIDGEPANEL_FILE_TIMEOUT", "600")
    try:
        return max(30.0, float(raw))
    except ValueError:
        return 600.0


# ── Paths ──────────────────────────────────────────────────────

def data_home() -> Path:
    return Path(os.environ.get("BRIDGEPANEL_HOME", Path.home() / ".local/share/bridgepanel"))


def config_home() -> Path:
    return Path(os.environ.get("BRIDGEPANEL_CONFIG", Path.home() / ".config/bridgepanel"))


def sessions_dir() -> Path:
    return data_home() / "sessions"


def token_path() -> Path:
    return config_home() / "token"


def state_path() -> Path:
    return data_home() / "state.json"
