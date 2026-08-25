/** Shipping product facts. Keep aligned with VERSION, SECURITY.md, LICENSE, and repo docs. */

export const VERSION = "2026.08.24-beta7";

export const PRODUCT = "BridgeSessions";
export const BINARY = "bridgesessions";
export const CLI = "bs";
export const LANGUAGE = "C++23";

export const LISTEN_PORT = 19949;
export const TRANSPORT = `Ed25519 mutual TLS over TCP/${LISTEN_PORT}`;
export const TLS_PROFILE = "TLS 1.2 compatibility profile";
export const TLS_DETAIL =
  "Current compatibility profile negotiates TLS 1.2.";

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
  "linux-x86_64, macos-arm64, and windows-x86_64.exe";
export const ISSUES_URL = `${REPO_URL}/issues`;
export const SECURITY_ADVISORY_URL = `${REPO_URL}/security/advisories/new`;
export const SECURITY_MD_URL = `${REPO_URL}/blob/main/SECURITY.md`;
export const LICENSE_URL = `${REPO_URL}/blob/main/LICENSE`;
export const AUDIT_URL = `${REPO_URL}/blob/main/AUDIT.md`;
export const SKILL_URL = `${REPO_URL}/blob/main/skills/bridgesessions/SKILL.md`;
export const AGENT_SKILL_INSTALL_URL = `${REPO_URL}/blob/main/scripts/install-agent-skill.sh`;
export const QUICKSTART_URL = `${REPO_URL}/blob/main/docs/QUICKSTART.md`;
export const USAGE_URL = `${REPO_URL}/blob/main/docs/usage.md`;
export const CONFIG_URL = `${REPO_URL}/blob/main/docs/configuration.md`;
export const CUA_URL = `${REPO_URL}/blob/main/docs/cua.md`;
export const PANEL_URL = `${REPO_URL}/blob/main/docs/bridge-panel.md`;
export const DESIGN_URL = `${REPO_URL}/blob/main/docs/design.md`;
export const PROVENANCE_URL = `${REPO_URL}/blob/main/docs/RELEASE-PROVENANCE.md`;
export const DOCS_SITE = "https://minddragonlabs.github.io/BridgeSessions";

export const INSTALL_SH =
  "curl -fsSL https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.sh | bash";
export const INSTALL_PS1 =
  "irm https://raw.githubusercontent.com/MindDragonLabs/BridgeSessions/main/scripts/install.ps1 | iex";

export const LINUX_MAC_BIN = "~/.local/bin/bridgesessions";
export const LINUX_MAC_CLI = "~/.local/bin/bs";
export const WINDOWS_BIN = "%LOCALAPPDATA%\\bridgesessions\\bridgesessions.exe";
export const STATE_DIR = "~/.bridgesessions/";

export const LICENSE_NAME = "Business Source License 1.1";
export const LICENSE_SHORT = "BSL 1.1";
export const LICENSE_CHANGE_DATE = "2030-07-16";
export const LICENSE_CHANGE_TO = "Apache-2.0";
export const LICENSE_LINE = `${LICENSE_SHORT}, source-available — not an Open Source license. Change date ${LICENSE_CHANGE_DATE} → ${LICENSE_CHANGE_TO}.`;

export const BETA_BOUNDARY =
  "An authorized peer has near-interactive host access. Use it only on machines and networks you control.";

export const DESCRIPTION =
  "A single C++23 executable for persistent shells, verified files, and computer-use automation across a pinned peer mesh. For humans and agents. This beta ships linux-x86_64, macos-arm64, and windows-x86_64.exe.";

export const NAV = [
  { href: "/install", label: "Install" },
  { href: "/#demo", label: "Demo" },
  { href: "/#security", label: "Security" },
  { href: "/#limitations", label: "Limitations" },
  { href: "/#cua", label: "CUA" },
  { href: "/#panel", label: "Panel" },
  { href: "/#agents", label: "Agents" },
  { href: "/#recovery", label: "Recovery" },
] as const;

/** Public site IA. Not an internal checklist. */
export const SITE_IA = [
  {
    href: "/install",
    title: "Install",
    brief: "One answer: current GitHub release, then bs --version and bs doctor.",
  },
  {
    href: "/#demo",
    title: "Demo",
    brief: "Reattach a named shell, move a file, capture a screen.",
  },
  {
    href: "/#security",
    title: "Security",
    brief: "Pinned Ed25519 mesh, host-level authorization, invite window.",
  },
  {
    href: "/#limitations",
    title: "Limitations",
    brief: "Small operator meshes. Three shipping artifacts. Beta upgrade discipline.",
  },
  {
    href: "/#cua",
    title: "CUA",
    brief: "Desktop capture and input on a trusted peer. Host-level, not low privilege.",
  },
  {
    href: "/#panel",
    title: "Bridge Panel",
    brief: "Optional local Markdown review UI. Not a new trust root.",
  },
  {
    href: "/#agents",
    title: "Agents",
    brief: "Same CLI as a human. Repo skill for Claude, Codex, and OpenCode.",
  },
  {
    href: "/#recovery",
    title: "Recovery",
    brief: "Unload leftovers, stop the tray, remove the binary, leave the mesh.",
  },
] as const;

/** KeepAlive LaunchAgents must be booted out before the plists are deleted. */
export const RECOVERY_MAC_BOOTOUT_CUA =
  "launchctl bootout gui/$(id -u)/com.bridgesessions.cua-helper";
export const RECOVERY_MAC_BOOTOUT_MENUBAR =
  "launchctl bootout gui/$(id -u)/com.minddragon.bridgesessions.menubar";
export const RECOVERY_MAC_BOOTOUT_MESH =
  "launchctl bootout gui/$(id -u)/com.bridgesessions.mesh";

/** Stop the tray first so it cannot restart the helper, then end the task. */
export const RECOVERY_WIN_STOP_TRAY =
  "Get-CimInstance Win32_Process |\n  Where-Object { $_.CommandLine -like '*bs_tray.ps1*' } |\n  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
export const RECOVERY_WIN_END_CUA_TASK =
  'schtasks /End /TN "BridgeSessions-CuaHelper"';
export const RECOVERY_WIN_KILL_HELPER =
  "Get-CimInstance Win32_Process |\n  Where-Object { $_.CommandLine -like '*--cua-helper*' } |\n  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
