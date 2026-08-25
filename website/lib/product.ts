/** Shipping facts. Keep aligned with VERSION and repo docs. */

export const VERSION = "2026.08.24-beta7";

export const PRODUCT = "BridgeSessions";
export const BINARY = "bridgesessions";
export const CLI = "bs";

export const REPO_URL = "https://github.com/MindDragonLabs/BridgeSessions";
export const RELEASE_TAG_URL = `${REPO_URL}/releases/tag/v${VERSION}`;
export const SHA256SUMS_URL = `${REPO_URL}/releases/download/v${VERSION}/SHA256SUMS`;

/** beta7 ships these three artifacts only. Do not imply Linux ARM or Intel Mac. */
export const SHIPPING_ASSETS = [
  "bridgesessions-linux-x86_64",
  "bridgesessions-macos-arm64",
  "bridgesessions-windows-x86_64.exe",
] as const;
export const SHIPPING_ARCH_LINE =
  "Linux x86_64, Mac Apple silicon, Windows x86_64";
export const ISSUES_URL = `${REPO_URL}/issues`;
export const SECURITY_ADVISORY_URL = `${REPO_URL}/security/advisories/new`;
export const SECURITY_MD_URL = `${REPO_URL}/blob/main/SECURITY.md`;
export const SKILL_URL = `${REPO_URL}/blob/main/skills/bridgesessions/SKILL.md`;
export const AGENT_SKILL_INSTALL_URL = `${REPO_URL}/blob/main/scripts/install-agent-skill.sh`;
export const QUICKSTART_URL = `${REPO_URL}/blob/main/docs/QUICKSTART.md`;
export const USAGE_URL = `${REPO_URL}/blob/main/docs/usage.md`;
export const CUA_URL = `${REPO_URL}/blob/main/docs/cua.md`;
export const DOCS_SITE = "https://minddragonlabs.github.io/BridgeSessions";

export const INSTALL_SH =
  "curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash";
export const INSTALL_PS1 =
  "irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex";

export const LINUX_MAC_BIN = "~/.local/bin/bridgesessions";
export const LINUX_MAC_CLI = "~/.local/bin/bs";
export const WINDOWS_BIN = "%LOCALAPPDATA%\\bridgesessions\\bridgesessions.exe";
export const STATE_DIR = "~/.bridgesessions/";

export const DESCRIPTION =
  "You and your AI agent use the same tool to work on another machine.";

export const NAV = [{ href: "/install", label: "Install" }] as const;

export const RECOVERY_MAC_BOOTOUT_MESH =
  "launchctl bootout gui/$(id -u)/com.bridgesessions.mesh";
export const RECOVERY_MAC_BOOTOUT_CUA =
  "launchctl bootout gui/$(id -u)/com.bridgesessions.cua-helper";
export const RECOVERY_MAC_BOOTOUT_MENUBAR =
  "launchctl bootout gui/$(id -u)/com.minddragon.bridgesessions.menubar";
export const RECOVERY_LINUX_STOP_DAEMON =
  "systemctl --user stop bridgesessions.service";
export const RECOVERY_WIN_END_MESH_TASK = 'schtasks /End /TN "BridgeSessions"';
export const RECOVERY_WIN_STOP_TRAY =
  "Get-CimInstance Win32_Process |\n  Where-Object { $_.CommandLine -like '*bs_tray.ps1*' } |\n  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
export const RECOVERY_WIN_END_CUA_TASK =
  'schtasks /End /TN "BridgeSessions-CuaHelper"';
export const RECOVERY_WIN_KILL_HELPER =
  "Get-CimInstance Win32_Process |\n  Where-Object { $_.CommandLine -like '*--cua-helper*' } |\n  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
