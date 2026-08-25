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
        assert not re.findall(adhoc_pattern, text)
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
        # Must be minddragon (not mindragon typo) — TCC keys on this id.
        assert 'com.minddragon.bridgesessions' in text
        assert 'com.mindragon.bridgesessions' not in text

    def test_updates_launchd_plists_to_app_binary(self):
        text = read_script()
        assert 'APP_BIN' in text, "Missing APP_BIN variable for launchd plists"

    def test_opens_app_for_tcc_indexing(self):
        text = read_script()
        # TCC indexing happens via lsregister, NOT 'open' (which launches
        # the binary bare without --config and causes dock bounce).
        assert 'lsregister' in text, "Missing lsregister for TCC indexing"
        # Must NOT 'open' the .app (bug: launches bare binary, dock bounce)
        assert 'open "${LOCAL_APP}"' not in text and 'open "$LOCAL_APP"' not in text, \
               "install.sh should not 'open' the .app bundle"

    def test_no_tcc_reset_of_stable_identity(self):
        text = read_script()
        # Reinstall must NOT wipe Screen Recording for the stable Developer ID
        # identity — that forced re-grant on every upgrade.
        assert 'tccutil reset ScreenCapture com.minddragon.bridgesessions' not in text
        assert 'tccutil reset Accessibility com.minddragon.bridgesessions' not in text
        assert 'Do NOT tccutil reset' in text or 'NEVER tccutil reset' in text

    def test_lsui_element_prevents_dock_bounce(self):
        text = read_script()
        assert 'LSUIElement' in text, \
               "Local .app plist missing LSUIElement (dock bounce bug)"

    def test_daemon_restarted_after_app_wrapper(self):
        text = read_script()
        # After creating the .app wrapper, both agents must restart onto
        # the bundle binary — the daemon started earlier is on the bare path.
        assert 'Daemons restarted from .app bundle' in text


# ════════════════════════════════════════════════════════════════
# Bug 1: Installer source and assets must use GitHub
# ════════════════════════════════════════════════════════════════

class TestGitHubUrls:
    """Installer source and binary assets must both use GitHub."""

    def test_install_script_uses_github(self):
        text = read_script()
        assert 'raw.githubusercontent.com/MindDragonLabs' in text


    def test_binary_base_uses_github_releases(self):
        text = read_script()
        assert re.search(r'BASE=.*github\.com/.*/releases/download', text), \
               "BASE should point to GitHub Releases"


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
        base_line = [line for line in text.split('\n') if 'BASE=' in line and 'github' in line.lower()]
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


# ════════════════════════════════════════════════════════════════
# Bugs 12–13: join client must not record unreachable addresses
# (host_addr 0.0.0.0 wildcard; ephemeral inbound source ports)
# ════════════════════════════════════════════════════════════════

MAIN_CPP = REPO_ROOT / "main.cpp"


class TestJoinClientAddrFixes:
    """Bugs 12+13 (2026-08-11): joiner wrote 'seed host 0.0.0.0:19949'
    (server wildcard listen addr) and seeds at EPHEMERAL source ports —
    both unreachable, so the new node could never dial out."""

    def test_host_addr_wildcard_substituted(self):
        text = MAIN_CPP.read_text()
        assert 'rfind("0.0.0.0", 0) == 0' in text, \
            "join client must substitute dialed addr when server sends 0.0.0.0"
        assert 'join_addr' in text

    def test_ephemeral_seed_ports_normalized(self):
        text = MAIN_CPP.read_text()
        assert ':19949' in text and 'all_digit' in text, \
            "join client must normalize ephemeral seed ports to canonical port"

    def test_join_start_prefers_service_manager(self):
        text = MAIN_CPP.read_text()
        assert 'launchctl kickstart' in text, \
            "join --start must prefer the launchd service over bare nohup"
        assert 'systemctl --user enable --now bridgesessions.service' in text, \
            "join/upgrade must enable --now even if the unit was disabled"
        assert 'systemctl --user disable bridgesessions.service' not in text, \
            "upgrade must never persist-disable the systemd unit"


class TestInstallLeavesDaemonEnabled:
    """2026-08-25: fecv3 inbound refused after upgrade left the unit disabled."""

    def test_install_sh_never_persist_disables(self):
        text = read_script()
        assert 'systemctl --user disable bridgesessions.service' not in text
        assert 'mask --runtime' in text
        assert 'enable --now' in text
        assert 'restore_daemon' in text
        assert 'DAEMON_WAS_STOPPED' in text

    def test_install_ps1_always_restores_daemon(self):
        text = (REPO_ROOT / "scripts" / "install.ps1").read_text()
        assert 'Restore-BridgeSessionsDaemon' in text
        assert 'ExecutionTimeLimit' in text
        assert '--daemon --config' in text
        assert 'Enable-ScheduledTask' in text
