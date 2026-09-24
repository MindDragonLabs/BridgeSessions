#!/usr/bin/env python3
"""BridgePanel launcher.

Documented start (KNOWN-ISSUES.md):

    python3 tools/bridgepanel/panel.py serve
    python3 tools/bridgepanel/panel.py          # defaults to serve
"""
from __future__ import annotations

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.dirname(_HERE)
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)

from bridgepanel import main  # noqa: E402


if __name__ == "__main__":
    if len(sys.argv) == 1:
        sys.argv.append("serve")
    raise SystemExit(main())
