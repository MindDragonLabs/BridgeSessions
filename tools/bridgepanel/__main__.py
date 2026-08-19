"""Entry point for `python3 -m tools.bridgepanel` and `python3 -m bridgepanel`."""
from __future__ import annotations

import sys

from . import main

if __name__ == "__main__":
    sys.exit(main())
