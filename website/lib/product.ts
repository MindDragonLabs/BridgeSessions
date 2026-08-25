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
export const RELEASES_URL = `${REPO_URL}/releases`;
export const RELEASE_TAG_URL = `${REPO_URL}/releases/tag/v${VERSION}`;
export const SHA256SUMS_URL = `${REPO_URL}/releases/download/v${VERSION}/SHA256SUMS`;
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
export const LICENSE_LINE = `${LICENSE_SHORT}, source-available. Converts to ${LICENSE_CHANGE_TO} on ${LICENSE_CHANGE_DATE}.`;

export const BETA_BOUNDARY =
  "An authorized peer has near-interactive host access. Use it only on machines and networks you control.";

export const DESCRIPTION =
  "A single C++23 executable for persistent shells, verified files, and computer-use automation across a pinned peer mesh. For humans and agents, on machines you already control.";

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

export const WORKSTREAMS = [
  {
    id: "product-truth",
    num: "01",
    title: "Product truth",
    href: "/#product-truth",
    brief:
      "Exact version, TLS 1.2 compatibility, one install answer, GitHub-only source, BSL, beta boundaries.",
  },
  {
    id: "positioning",
    num: "02",
    title: "Positioning",
    href: "/#positioning",
    brief:
      "Control layer for machines you own. Agents and humans. Not enterprise IAM. Not hostile multi-tenant.",
  },
  {
    id: "conversion",
    num: "03",
    title: "Conversion path",
    href: "/install",
    brief:
      "One CTA: install the current GitHub release. Wow path, recovery via repo docs, issues and SECURITY.md.",
  },
  {
    id: "trust",
    num: "04",
    title: "Trust and proof",
    href: "/#security",
    brief:
      "Pins, authorized_keys, invite window, host-level auth, loopback IPC. CUA permissions. Internal review.",
  },
  {
    id: "ia",
    num: "05",
    title: "Find it",
    href: "/#map",
    brief:
      "Homepage plus /install. Demo, security, limitations, CUA, Bridge Panel, agents, recovery.",
  },
] as const;
