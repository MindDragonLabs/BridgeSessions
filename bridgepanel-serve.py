#!/usr/bin/env python3
"""BridgePanel launcher — run from repo root."""
import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools", "bridgepanel"))
import bridgepanel
sys.exit(bridgepanel.main())
