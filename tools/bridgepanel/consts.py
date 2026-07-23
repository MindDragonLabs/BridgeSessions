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
MAX_UPLOAD = 10 * 1024 * 1024  # 10 MB
BS_IPC_PORT = 19980
BS_IPC_TIMEOUT = 2  # seconds


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
