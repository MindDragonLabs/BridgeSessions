import type { Metadata } from "next";
import Link from "next/link";
import { Command, ExtLink, Eyebrow, Section } from "@/components/Blocks";
import {
  CLI,
  CONFIG_URL,
  CUA_URL,
  INSTALL_PS1,
  INSTALL_SH,
  ISSUES_URL,
  LINUX_MAC_BIN,
  LINUX_MAC_CLI,
  PRODUCT,
  QUICKSTART_URL,
  RECOVERY_MAC_BOOTOUT_CUA,
  RECOVERY_MAC_BOOTOUT_MENUBAR,
  RECOVERY_WIN_END_CUA_TASK,
  RECOVERY_WIN_KILL_HELPER,
  RECOVERY_WIN_STOP_TRAY,
  RELEASE_TAG_URL,
  SECURITY_ADVISORY_URL,
  SHIPPING_ARCH_LINE,
  SHIPPING_ASSETS,
  SECURITY_MD_URL,
  SHA256SUMS_URL,
  STATE_DIR,
  USAGE_URL,
  VERSION,
  WINDOWS_BIN,
} from "@/lib/product";

export const metadata: Metadata = {
  title: {
    absolute: `Install ${PRODUCT} ${VERSION}`,
  },
  description: `Install the current GitHub release ${VERSION}. Ships linux-x86_64, macos-arm64, and windows-x86_64.exe.`,
};

export default function InstallPage() {
  return (
    <>
      <Section>
        <Eyebrow>Install</Eyebrow>
        <p className="mt-4 inline-flex items-center gap-2 rounded-[3px] border border-ember/40 bg-ember/10 px-2 py-1 font-mono text-[11px] tracking-[0.12em] text-ember uppercase">
          Beta {VERSION}
        </p>
        <h1 className="font-display mt-6 max-w-[16ch] text-[2.6rem] leading-[0.95] text-paper sm:text-5xl">
          Install the current GitHub release.
        </h1>
        <p className="mt-4 max-w-2xl text-base leading-relaxed text-paper/85">
          This is the one install answer. GitHub is the only primary source.
          This beta ships {SHIPPING_ARCH_LINE} — not Linux ARM, not Intel Mac.
          Installers fail closed unless the GitHub Release{" "}
          <code className="text-paper">SHA256SUMS</code> entry and embedded
          version match.
        </p>
      </Section>

      <Section>
        <Eyebrow>linux-x86_64 / macos-arm64</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">One curl.</h2>
        <div className="mt-6">
          <Command caption="Installer" code={INSTALL_SH} />
        </div>
        <p className="mt-4 text-sm text-steel">
          Fetches <code className="text-paper">{SHIPPING_ASSETS[0]}</code> or{" "}
          <code className="text-paper">{SHIPPING_ASSETS[1]}</code>. Places{" "}
          <code className="text-paper">{LINUX_MAC_BIN}</code> and{" "}
          <code className="text-paper">{LINUX_MAC_CLI}</code>.
        </p>
      </Section>

      <Section>
        <Eyebrow>windows-x86_64.exe</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">One PowerShell line.</h2>
        <div className="mt-6">
          <Command caption="PowerShell" code={INSTALL_PS1} />
        </div>
        <p className="mt-4 text-sm text-steel">
          Fetches <code className="text-paper">{SHIPPING_ASSETS[2]}</code>.
          Places <code className="text-paper">{WINDOWS_BIN}</code>.
        </p>
      </Section>

      <Section>
        <Eyebrow>Verify</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">Confirm the line.</h2>
        <div className="mt-6 grid gap-4 lg:grid-cols-2">
          <Command caption="Version" code={`${CLI} --version`} />
          <Command caption="Doctor" code={`${CLI} doctor`} />
        </div>
        <p className="mt-4 text-sm text-steel">
          Expect <span className="text-paper">{VERSION}</span>. Then continue
          with <ExtLink href={QUICKSTART_URL}>docs/QUICKSTART.md</ExtLink>.
        </p>
      </Section>

      <Section>
        <Eyebrow>Manual binaries</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">GitHub Releases + SHA256SUMS.</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          Prefer the installer. If you fetch assets by hand, use the{" "}
          {VERSION} prerelease tag — not a <code className="text-paper">/latest</code>{" "}
          URL — and check the published checksums. Assets:{" "}
          {SHIPPING_ASSETS.join(", ")}.
        </p>
        <ul className="mt-6 space-y-2 text-sm">
          <li>
            Release tag: <ExtLink href={RELEASE_TAG_URL}>{RELEASE_TAG_URL}</ExtLink>
          </li>
          <li>
            Checksums: <ExtLink href={SHA256SUMS_URL}>{SHA256SUMS_URL}</ExtLink>
          </li>
        </ul>
      </Section>

      <Section>
        <Eyebrow>Join, then the wow</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">Install-to-wow.</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          On a pinned seed, invite. On the new node, join immediately. Then
          reattach a named shell, move a file, or capture a screen.
        </p>
        <div className="mt-6 grid gap-4">
          <Command
            caption="Pinned seed"
            code={`${CLI} invite`}
          />
          <Command
            caption="New node"
            code={`${CLI} join <seed-address>:19949 <single-use-token> --start`}
          />
          <Command
            caption="Persistent reattach"
            code={`${CLI} shell <peer> --name agent\n# Ctrl-D detaches; the remote PTY stays\n${CLI} shell <peer> --name agent`}
          />
          <Command
            caption="Verified file move"
            code={`${CLI} file send <peer> ./artifact.bin --wait\n${CLI} file recv <peer> received/report.md --to ./report.md --wait`}
          />
          <Command
            caption="CUA capture"
            code={`${CLI} cua capture <peer> -o screen.png`}
          />
        </div>
        <p className="mt-4 text-sm text-steel">
          Windows/macOS CUA needs one{" "}
          <code className="text-paper">bridgesessions --cua-helper</code> in
          the interactive user session. macOS also needs Screen Recording and
          Accessibility. Capture before clicking.{" "}
          <ExtLink href={CUA_URL}>docs/cua.md</ExtLink>
        </p>
      </Section>

      <Section id="recovery">
        <Eyebrow>Uninstall / recovery</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">Stop, remove, leave.</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          <code className="text-paper">scripts/uninstall.sh</code> and{" "}
          <code className="text-paper">uninstall.ps1</code> do not exist. This
          page does not invent them.
        </p>
        <ul className="mt-6 max-w-3xl space-y-2 text-sm text-paper/85">
          <li>
            Stop the daemon: Linux{" "}
            <code className="text-paper">
              systemctl --user stop bridgesessions.service
            </code>
            ; macOS{" "}
            <code className="text-paper">
              launchctl bootout gui/$(id -u)/com.bridgesessions.mesh
            </code>
            ; Windows Task Scheduler task{" "}
            <code className="text-paper">BridgeSessions</code>.
          </li>
          <li>
            On macOS the CUA helper LaunchAgent is KeepAlive. Unload it (and
            the optional menubar agent) before deleting the plists, or launchd
            restarts them:
          </li>
        </ul>
        <div className="mt-4 grid gap-4">
          <Command caption="Unload macOS CUA helper" code={RECOVERY_MAC_BOOTOUT_CUA} />
          <Command
            caption="Unload macOS menubar"
            code={RECOVERY_MAC_BOOTOUT_MENUBAR}
          />
        </div>
        <p className="mt-4 max-w-3xl text-sm leading-relaxed text-steel">
          Then remove{" "}
          <code className="text-paper">
            ~/Library/LaunchAgents/com.bridgesessions.cua-helper.plist
          </code>{" "}
          and{" "}
          <code className="text-paper">
            ~/Library/LaunchAgents/com.minddragon.bridgesessions.menubar.plist
          </code>
          .
        </p>
        <p className="mt-6 max-w-3xl text-sm leading-relaxed text-paper/85">
          On Windows the Startup shortcut{" "}
          <code className="text-paper">BridgeSessions Tray.lnk</code> runs{" "}
          <code className="text-paper">bs_tray.ps1</code>, which restarts the
          helper. Stop the tray and end the{" "}
          <code className="text-paper">BridgeSessions-CuaHelper</code> task
          before deleting files:
        </p>
        <div className="mt-4 grid gap-4">
          <Command caption="Stop Windows tray" code={RECOVERY_WIN_STOP_TRAY} />
          <Command
            caption="End BridgeSessions-CuaHelper"
            code={RECOVERY_WIN_END_CUA_TASK}
          />
          <Command
            caption="Kill leftover --cua-helper"
            code={RECOVERY_WIN_KILL_HELPER}
          />
        </div>
        <ul className="mt-6 max-w-3xl space-y-2 text-sm text-paper/85">
          <li>
            Then delete the Startup shortcut, unregister the task if you want
            it gone at next logon, and remove leftover files. Linux: remove{" "}
            <code className="text-paper">
              ~/.config/autostart/bridgesessions-tray.desktop
            </code>{" "}
            and stop <code className="text-paper">bs_tray.py</code> if it is
            running. Names come from{" "}
            <code className="text-paper">scripts/install.sh</code> and{" "}
            <code className="text-paper">scripts/install.ps1</code>.
          </li>
          <li>
            Remove the binary:{" "}
            <code className="text-paper">{LINUX_MAC_BIN}</code> /{" "}
            <code className="text-paper">{LINUX_MAC_CLI}</code> or{" "}
            <code className="text-paper">{WINDOWS_BIN}</code>
          </li>
          <li>
            Remove state: <code className="text-paper">{STATE_DIR}</code>{" "}
            (Windows:{" "}
            <code className="text-paper">%USERPROFILE%\.bridgesessions</code>)
          </li>
          <li>
            Leave a mesh: remove this node&apos;s key from other peers&apos;{" "}
            <code className="text-paper">authorized_keys</code> and drop local
            seed pins. See{" "}
            <ExtLink href={CONFIG_URL}>Configuration</ExtLink>,{" "}
            <ExtLink href={USAGE_URL}>Usage</ExtLink>, and{" "}
            <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink>.
          </li>
        </ul>
      </Section>

      <Section>
        <Eyebrow>Report problems</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">Bugs vs vulns.</h2>
        <ul className="mt-6 max-w-3xl space-y-2 text-sm text-paper/85">
          <li>
            Unexpected behavior:{" "}
            <ExtLink href={ISSUES_URL}>GitHub issues</ExtLink>
          </li>
          <li>
            Vulnerabilities: follow{" "}
            <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink> and open a{" "}
            <ExtLink href={SECURITY_ADVISORY_URL}>
              private GitHub security advisory
            </ExtLink>
            . Do not file vuln details as a public issue.
          </li>
        </ul>
        <p className="mt-8">
          <Link href="/#security" className="text-signal">
            Security and limitations
          </Link>
          <span className="mx-3 text-steel">·</span>
          <Link href="/#demo" className="text-signal">
            Demo slot
          </Link>
          <span className="mx-3 text-steel">·</span>
          <Link href="/#agents" className="text-signal">
            Agent path
          </Link>
        </p>
      </Section>
    </>
  );
}
