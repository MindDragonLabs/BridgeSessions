"""In-process caches for BridgePanel listings and mesh tree.

`bs script run` is not used here: it always execs bash and its push path is
POSIX `test`/`mv`. Windows peers (the slow listings) cannot use it, and the
panel needs captured JSON stdout from `run-script`. Cache the *results*
instead of pretending the daemon grew a file cache.
"""
from __future__ import annotations

import os
import threading
import time
from typing import Callable


def _env_float(name: str, default: float) -> float:
    raw = (os.environ.get(name) or "").strip()
    if not raw:
        return default
    try:
        return max(0.0, float(raw))
    except ValueError:
        return default


def fresh_sec() -> float:
    return _env_float("BRIDGEPANEL_LIST_CACHE_FRESH", 20.0)


def stale_sec() -> float:
    return _env_float("BRIDGEPANEL_LIST_CACHE_STALE", 120.0)


def tree_ttl_sec() -> float:
    return _env_float("BRIDGEPANEL_TREE_CACHE_TTL", 2.0)


MAX_LISTINGS = 256

_lock = threading.Lock()
_listings: dict[tuple[str, str, str], tuple[float, dict]] = {}
_tree: tuple[float, dict] | None = None
_inflight: set[tuple[str, str, str]] = set()


def cache_key(machine: str, root: str, path: str) -> tuple[str, str, str]:
    return ((machine or "").strip(), (root or "inbox").strip() or "inbox", path or "")


def listing_get(key: tuple[str, str, str]) -> tuple[dict | None, str]:
    """Return (payload, state) where state is miss|fresh|stale|expired."""
    now = time.time()
    with _lock:
        hit = _listings.get(key)
        if hit is None:
            return None, "miss"
        ts, payload = hit
        age = now - ts
        fresh = fresh_sec()
        stale = max(stale_sec(), fresh)
        if fresh <= 0:
            return payload, "expired"
        if age < fresh:
            return payload, "fresh"
        if age < stale:
            return payload, "stale"
        return payload, "expired"


def listing_put(key: tuple[str, str, str], payload: dict) -> None:
    if not isinstance(payload, dict) or not payload.get("ok"):
        return
    stored = {k: v for k, v in payload.items() if k not in ("cached", "stale")}
    now = time.time()
    with _lock:
        if len(_listings) >= MAX_LISTINGS:
            oldest = min(_listings.items(), key=lambda item: item[1][0])[0]
            _listings.pop(oldest, None)
        _listings[key] = (now, stored)


def listing_invalidate(machine: str | None = None, root: str | None = None) -> None:
    with _lock:
        if machine is None:
            _listings.clear()
            return
        host = (machine or "").strip()
        token = (root or "").strip() if root is not None else None
        drop = [
            key for key in _listings
            if key[0] == host and (token is None or key[1] == token)
        ]
        for key in drop:
            _listings.pop(key, None)


def listing_invalidate_self_aliases(node: str, root: str = "inbox") -> None:
    listing_invalidate("(local)", root)
    listing_invalidate("local", root)
    if node and node not in ("(local)", "local"):
        listing_invalidate(node, root)


def kick_listing_refresh(
    key: tuple[str, str, str],
    loader: Callable[[], dict],
) -> None:
    with _lock:
        if key in _inflight:
            return
        _inflight.add(key)

    def _run() -> None:
        try:
            payload = loader()
            if isinstance(payload, dict) and payload.get("ok"):
                listing_put(key, payload)
        except Exception:
            pass
        finally:
            with _lock:
                _inflight.discard(key)

    threading.Thread(target=_run, name="bp-list-refresh", daemon=True).start()


def tree_get() -> dict | None:
    ttl = tree_ttl_sec()
    now = time.time()
    with _lock:
        if _tree is None:
            return None
        ts, payload = _tree
        if ttl <= 0 or now - ts >= ttl:
            return None
        return payload


def tree_put(payload: dict) -> None:
    if not isinstance(payload, dict):
        return
    with _lock:
        global _tree
        _tree = (time.time(), payload)


def tree_invalidate() -> None:
    with _lock:
        global _tree
        _tree = None


def clear_caches() -> None:
    with _lock:
        global _tree
        _listings.clear()
        _inflight.clear()
        _tree = None
