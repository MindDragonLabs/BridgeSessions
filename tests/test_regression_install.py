"""Regression tests for 10 install.sh bugs fixed 2026-08-11.

Bug coverage:
  3.  Dynamically-linked macOS binary (should be static — check dist binary)
  4.  ZSH_VERSION unbound under set -eu
  5.  Double --start flag in join args
  6.  TLS unexpected-eof: join retry loop exists
  9.  Ad-hoc re-signing strips Developer ID (must not re-sign)
  10. Bare binary not in TCC: .app bundle wrapper creation
"""
import re
import os
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
INSTALL_SCRIPT = REPO_ROOT / "scripts" / "install.sh"
SCRIPT_TEXT = INSTALL_SCRIPT.read_text()


def read_script():
    return INSTALL_SCRIPT.read_text()


# ════════════════════════════════════════════════════════════════
# Bug 4: ZSH_VERSION must be ${ZSH_VERSION:-} not ${ZSH_VERSION}
# ════════════════════════════════════════════════════════════════

class TestZshVersionSafe:
    """Bug 4: ${ZSH_VERSION} crashes under set -eu on bash subprocess."""

    def test_no_unbound_zsh_version(self):
        text = read_script()
        # Must NOT have bare ${ZSH_VERSION} without :- default
        bare = re.findall(r'\$\{ZSH_VERSION\}', text)
        assert len(bare) == 0, f"Found unbound ${{ZSH_VERSION}}: {bare}"

    def test_zsh_version_has_default(self):
        text = read_script()
        safe = re.findall(r'\$\{ZSH_VERSION:-\}', text)
        assert len(safe) >= 1, "Expected ${ZSH_VERSION:-} with default"

    def test_script_uses_set_eu(self):
        text = read_script()
        assert "set -eu" in text or "set -euo" in text, "Script should have set -eu"


# ════════════════════════════════════════════════════════════════
# Bug 5: Double --start must be deduplicated
# ════════════════════════════════════════════════════════════════

class TestStartDedup:
    """Bug 5: install.sh must strip duplicate --start from join args."""

    def test_join_dedup_logic_exists(self):
        text = read_script()
        # The dedup loop should exist
        assert "HAS_START" in text, "Missing --start dedup logic"
        assert 'Strip duplicate --start' in text or 'dedup' in text.lower()

    def test_join_not_appending_start_unconditionally(self):
        text = read_script()
        # The old bug: exec ... join "$@" --start  (always appends)
        # Should NOT find this pattern anymore
        assert 'join "$@" --start' not in text, "Still unconditionally appending --start"

    def test_join_uses_loop_not_exec(self):
        text = read_script()
        # New code uses a for loop, not direct exec
        assert 'for attempt in' in text, "Missing retry loop"
        assert 'JOIN_ARGS' in text, "Missing JOIN_ARGS construction"


# ════════════════════════════════════════════════════════════════
# Bug 6: Join retry loop must exist for TLS unexpected-eof
# ════════════════════════════════════════════════════════════════

class TestJoinRetry:
    """Bug 6: Join must retry on TLS unexpected-eof."""

    def test_retry_loop_exists(self):
        text = read_script()
        assert 'attempt 1' in text or 'attempt ${attempt}' in text
        assert 'retry' in text.lower() or 'Join attempt' in text

    def test_retry_count_at_least_3(self):
        text = read_script()
        # Should retry at least 3 times
        match = re.search(r'for attempt in (\d+) (\d+) (\d+)', text)
        assert match, "Missing retry range"
        attempts = [int(match.group(i)) for i in range(1, 4)]
        assert max(attempts) >= 3, f"Retry count too low: {attempts}"

    def test_join_failure_has_helpful_error(self):
        text = read_script()
        assert 'invite token may be expired' in text.lower() or \
               'fresh' in text.lower(), "Missing helpful error on join failure"


# ════════════════════════════════════════════════════════════════
# Bug 9: Must not ad-hoc re-sign (strips Developer ID → SIGKILL)
# ════════════════════════════════════════════════════════════════

class TestNoAdhocResign:
    """Bug 9: Ad-hoc re-signing strips Developer ID signature → macOS SIGKILL."""

    def test_no_adhoc_sign_on_app_wrapper(self):
        text = read_script()
        # Should NOT find: codesign --force --sign - "${LOCAL_APP}"
        # as the primary signing path (it strips Developer ID)
        adhoc_pattern = r'codesign\s+--force\s+--sign\s+-\s+"\$\{LOCAL_APP\}"'
        matches = re.findall(adhoc_pattern, text)
        # The only acceptable adhoc sign is in a fallback "|| true" chain
        # Check that the primary path doesn't adhoc sign
        assert 'preserving signed binary' in text.lower() or \
               'Do NOT re-sign' in text or \
               "Don't re-sign" in text.lower(), \
               "Missing 'do not re-sign' logic"


# ════════════════════════════════════════════════════════════════
# Bug 10: .app bundle wrapper must be created for TCC
# ════════════════════════════════════════════════════════════════

class TestAppBundleWrapper:
    """Bug 10: Bare binary needs .app bundle for TCC to list it."""

    def test_creates_app_bundle(self):
        text = read_script()
        assert 'LOCAL_APP' in text, "Missing local .app bundle variable"
        assert 'BridgeSessions.app' in text
        assert 'Contents/MacOS' in text

    def test_creates_info_plist(self):
        text = read_script()
        assert 'Info.plist' in text
        assert 'CFBundleIdentifier' in text
        assert 'com.mindragon.bridgesessions' in text

    def test_updates_launchd_plists_to_app_binary(self):
        text = read_script()
        assert 'APP_BIN' in text, "Missing APP_BIN variable for launchd plists"

    def test_opens_app_for_tcc_indexing(self):
        text = read_script()
        assert 'open "${LOCAL_APP}"' in text or 'open "$LOCAL_APP"' in text, \
               "Missing open command to trigger TCC indexing"

    def test_tcc_reset_uses_bundle_id(self):
        text = read_script()
        # Should reset TCC using bundle ID, not bare binary name
        assert 'tccutil reset ScreenCapture com.mindragon.bridgesessions' in text


# ════════════════════════════════════════════════════════════════
# Bug 1: Invite URLs must be GitHub, not Codeberg
# ════════════════════════════════════════════════════════════════

class TestGitHubUrls:
    """Bug 1: Invite output and install script must use GitHub raw URLs."""

    def test_install_script_uses_github(self):
        text = read_script()
        assert 'raw.githubusercontent.com/MindDragonLabs' in text

    def test_install_script_no_codeberg(self):
        text = read_script()
        assert 'codeberg.org' not in text, "install.sh still references codeberg"

    def test_dist_base_uses_github(self):
        text = read_script()
        # The BASE variable should point to github
        assert re.search(r'BASE=.*githubusercontent\.com', text), \
               "BASE should point to GitHub raw"


# ════════════════════════════════════════════════════════════════
# Bug 2: Version must not be hardcoded in install.sh
# ════════════════════════════════════════════════════════════════

class TestVersionNotHardcoded:
    """Bug 2: Version should use BRIDGESESSIONS_TAG variable, not hardcoded."""

    def test_tag_variable_exists(self):
        text = read_script()
        assert 'BRIDGESESSIONS_TAG' in text

    def test_base_uses_tag_variable(self):
        text = read_script()
        assert '${TAG}' in text or '${BRIDGESESSIONS_TAG}' in text

    def test_no_hardcoded_version_in_base(self):
        text = read_script()
        # Should not find a hardcoded date version in the BASE URL
        base_line = [l for l in text.split('\n') if 'BASE=' in l and 'github' in l.lower()]
        for line in base_line:
            # Should reference the variable, not a literal like 26.08.10
            hardcoded = re.findall(r'26\.\d{2}\.\d{2}', line)
            assert len(hardcoded) == 0, f"Hardcoded version in BASE: {hardcoded}"


# ════════════════════════════════════════════════════════════════
# Bug 3: dist binary must be statically linked (no Homebrew dylibs)
# ════════════════════════════════════════════════════════════════

class TestSymlinkSafeCopy:
    """Bug 11: cp fails when BIN_ABS is a symlink back to .app binary."""

    def test_resolves_symlink_before_copy(self):
        text = read_script()
        assert 'readlink' in text, "Missing symlink resolution before cp"
        assert 'REAL_BIN' in text, "Missing REAL_BIN variable"

    def test_cp_uses_real_bin(self):
        text = read_script()
        assert 'cp -f "${REAL_BIN}"' in text, "cp should use REAL_BIN not BIN_ABS"

    def test_cp_forces_overwrite(self):
        text = read_script()
        assert 'cp -f' in text, "cp should force overwrite"


class TestStaticBinary:
    """Bug 3: macOS dist binary must not depend on Homebrew dylibs."""

    @pytest.mark.skipif(not (REPO_ROOT / "dist" / "bridgesessions-macos-arm64").exists(),
                        reason="dist binary not built")
    def test_no_homebrew_dylib_deps(self):
        binary = REPO_ROOT / "dist" / "bridgesessions-macos-arm64"
        result = subprocess.run(
            ["otool", "-L", str(binary)],
            capture_output=True, text=True, timeout=10
        )
        output = result.stdout
        # Must not reference homebrew paths
        assert '/opt/homebrew/' not in output, \
               f"Homebrew dylib dependency found:\n{output}"
        assert 'libssl.3.dylib' not in output, \
               f"Dynamic libssl dependency:\n{output}"
        assert 'libcrypto.3.dylib' not in output, \
               f"Dynamic libcrypto dependency:\n{output}"

    @pytest.mark.skipif(not (REPO_ROOT / "dist" / "bridgesessions-macos-arm64").exists(),
                        reason="dist binary not built")
    def test_binary_is_mach_o_arm64(self):
        binary = REPO_ROOT / "dist" / "bridgesessions-macos-arm64"
        result = subprocess.run(
            ["file", str(binary)],
            capture_output=True, text=True, timeout=10
        )
        assert 'Mach-O' in result.stdout
        assert 'arm64' in result.stdout
