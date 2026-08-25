import type { Metadata } from "next";
import Link from "next/link";
import { Command, ExtLink, Section } from "@/components/Blocks";
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
  RECOVERY_LINUX_STOP_DAEMON,
  RECOVERY_MAC_BOOTOUT_CUA,
  RECOVERY_MAC_BOOTOUT_MENUBAR,
  RECOVERY_MAC_BOOTOUT_MESH,
  RECOVERY_WIN_END_CUA_TASK,
  RECOVERY_WIN_END_MESH_TASK,
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
  description: `Install ${PRODUCT} ${VERSION} from GitHub. Artifacts: linux-x86_64, macos-arm64, and windows-x86_64.exe.`,
};

export default function InstallPage() {
  return (
    <>
      <Section>
        <p className="inline-flex items-center gap-2 rounded-[3px] border border-ember/40 bg-ember/10 px-2 py-1 font-mono text-[11px] tracking-[0.12em] text-ember uppercase">
          {VERSION}
        </p>
        <h1 className="font-display mt-6 text-[2.6rem] leading-[0.95] text-paper sm:text-5xl">
          Install {PRODUCT}
        </h1>
        <p className="mt-4 max-w-2xl text-base leading-relaxed text-paper/85">
          GitHub is the only source for this release. This release has three
          artifacts: {SHIPPING_ARCH_LINE}.
        </p>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          The installer stops if the GitHub Release{" "}
          <code className="text-paper">SHA256SUMS</code> entry or the embedded
          version does not match.
        </p>
      </Section>

      <Section>
        <h2 className="text-2xl text-paper">linux-x86_64 and macos-arm64</h2>
        <p className="mt-3 text-sm text-steel">Run this command.</p>
        <div className="mt-4">
          <Command caption="install.sh" code={INSTALL_SH} />
        </div>
        <p className="mt-4 text-sm text-steel">
          The installer gets <code className="text-paper">{SHIPPING_ASSETS[0]}</code>{" "}
          or <code className="text-paper">{SHIPPING_ASSETS[1]}</code>.
        </p>
        <p className="mt-2 text-sm text-steel">
          The installer puts the binary in{" "}
          <code className="text-paper">{LINUX_MAC_BIN}</code>. It also puts{" "}
          <code className="text-paper">{CLI}</code> in{" "}
          <code className="text-paper">{LINUX_MAC_CLI}</code>.
        </p>
      </Section>

      <Section>
        <h2 className="text-2xl text-paper">windows-x86_64.exe</h2>
        <p className="mt-3 text-sm text-steel">Run this command in PowerShell.</p>
        <div className="mt-4">
          <Command caption="install.ps1" code={INSTALL_PS1} />
        </div>
        <p className="mt-4 text-sm text-steel">
          The installer gets <code className="text-paper">{SHIPPING_ASSETS[2]}</code>.
        </p>
        <p className="mt-2 text-sm text-steel">
          The installer puts the binary in{" "}
          <code className="text-paper">{WINDOWS_BIN}</code>.
        </p>
      </Section>

      <Section>
        <h2 className="text-2xl text-paper">Verify</h2>
        <p className="mt-3 text-sm text-steel">Show the version.</p>
        <div className="mt-4">
          <Command caption="Version" code={`${CLI} --version`} />
        </div>
        <p className="mt-4 text-sm text-steel">
          The output must be <span className="text-paper">{VERSION}</span>.
        </p>
        <p className="mt-4 text-sm text-steel">Run the doctor command.</p>
        <div className="mt-4">
          <Command caption="Doctor" code={`${CLI} doctor`} />
        </div>
        <p className="mt-4 text-sm text-steel">
          Then read <ExtLink href={QUICKSTART_URL}>docs/QUICKSTART.md</ExtLink>.
        </p>
      </Section>

      <Section>
        <h2 className="text-2xl text-paper">Manual binaries</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          Use the installer if you can. If you get the files yourself, use the
          GitHub release tag. Do not use a{" "}
          <code className="text-paper">/latest</code> URL.
        </p>
        <p className="mt-3 text-sm text-steel">
          Check <code className="text-paper">SHA256SUMS</code>. The assets are{" "}
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
        <h2 className="text-2xl text-paper">Join</h2>
        <p className="mt-3 text-sm text-steel">On a pinned seed, run this command.</p>
        <div className="mt-4">
          <Command caption="Invite" code={`${CLI} invite`} />
        </div>
        <p className="mt-4 text-sm text-steel">
          On the new peer, run this command immediately.
        </p>
        <div className="mt-4">
          <Command
            caption="Join"
            code={`${CLI} join <seed-address>:19949 <single-use-token> --start`}
          />
        </div>
      </Section>

      <Section>
        <h2 className="text-2xl text-paper">Shell</h2>
        <p className="mt-3 text-sm text-steel">Start a named shell.</p>
        <div className="mt-4">
          <Command
            caption="Start"
            code={`${CLI} shell <peer> --name agent`}
          />
        </div>
        <p className="mt-4 text-sm text-steel">
          Press Ctrl-D to detach. The remote PTY stays.
        </p>
        <p className="mt-3 text-sm text-steel">
          Run the same command to attach again.
        </p>
        <div className="mt-4">
          <Command
            caption="Attach again"
            code={`${CLI} shell <peer> --name agent`}
          />
        </div>
      </Section>

      <Section>
        <h2 className="text-2xl text-paper">File</h2>
        <p className="mt-3 text-sm text-steel">Send a file and wait for the result.</p>
        <div className="mt-4">
          <Command
            caption="Send"
            code={`${CLI} file send <peer> ./artifact.bin --wait`}
          />
        </div>
        <p className="mt-4 text-sm text-steel">Get a file and wait for the result.</p>
        <div className="mt-4">
          <Command
            caption="Receive"
            code={`${CLI} file recv <peer> received/report.md --to ./report.md --wait`}
          />
        </div>
      </Section>

      <Section>
        <h2 className="text-2xl text-paper">CUA</h2>
        <p className="mt-3 text-sm text-steel">Capture the screen before you click.</p>
        <div className="mt-4">
          <Command
            caption="Capture"
            code={`${CLI} cua capture <peer> -o screen.png`}
          />
        </div>
        <p className="mt-4 max-w-2xl text-sm leading-relaxed text-steel">
          On Windows and macOS, start one{" "}
          <code className="text-paper">bridgesessions --cua-helper</code> in the
          user session. On macOS, give Screen Recording and Accessibility.
        </p>
        <p className="mt-3 text-sm text-steel">
          Read <ExtLink href={CUA_URL}>docs/cua.md</ExtLink>.
        </p>
      </Section>

      <Section id="recovery">
        <h2 className="text-2xl text-paper">Recovery</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          <code className="text-paper">scripts/uninstall.sh</code> and{" "}
          <code className="text-paper">uninstall.ps1</code> do not exist. Do
          not invent them.
        </p>
        <p className="mt-4 text-sm text-steel">Stop the daemon.</p>
        <div className="mt-4 grid gap-4">
          <Command caption="Linux" code={RECOVERY_LINUX_STOP_DAEMON} />
          <Command caption="macOS mesh" code={RECOVERY_MAC_BOOTOUT_MESH} />
          <Command caption="Windows mesh task" code={RECOVERY_WIN_END_MESH_TASK} />
        </div>
        <p className="mt-6 max-w-3xl text-sm leading-relaxed text-paper/85">
          On macOS the CUA helper LaunchAgent is KeepAlive. Unload the helper
          and the menubar agent before you remove the plist files.
        </p>
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
          On Windows, <code className="text-paper">BridgeSessions Tray.lnk</code>{" "}
          starts <code className="text-paper">bs_tray.ps1</code>. That script
          starts the helper again. Stop the tray first. Then end the{" "}
          <code className="text-paper">BridgeSessions-CuaHelper</code> task.
          Then remove files.
        </p>
        <div className="mt-4 grid gap-4">
          <Command caption="Stop Windows tray" code={RECOVERY_WIN_STOP_TRAY} />
          <Command
            caption="End BridgeSessions-CuaHelper"
            code={RECOVERY_WIN_END_CUA_TASK}
          />
          <Command
            caption="Stop leftover --cua-helper"
            code={RECOVERY_WIN_KILL_HELPER}
          />
        </div>
        <p className="mt-6 max-w-3xl text-sm leading-relaxed text-paper/85">
          Then delete the Startup shortcut. Unregister the task if you do not
          want it at the next logon. On Linux, stop{" "}
          <code className="text-paper">bs_tray.py</code> and remove{" "}
          <code className="text-paper">
            ~/.config/autostart/bridgesessions-tray.desktop
          </code>
          .
        </p>
        <p className="mt-4 text-sm text-steel">
          These names come from <code className="text-paper">scripts/install.sh</code>{" "}
          and <code className="text-paper">scripts/install.ps1</code>.
        </p>
        <p className="mt-4 text-sm text-paper/85">
          Remove the binary: <code className="text-paper">{LINUX_MAC_BIN}</code>,{" "}
          <code className="text-paper">{LINUX_MAC_CLI}</code>, or{" "}
          <code className="text-paper">{WINDOWS_BIN}</code>.
        </p>
        <p className="mt-3 text-sm text-paper/85">
          Remove state: <code className="text-paper">{STATE_DIR}</code>. On
          Windows the path is{" "}
          <code className="text-paper">%USERPROFILE%\.bridgesessions</code>.
        </p>
        <p className="mt-3 text-sm text-paper/85">
          To leave a mesh, remove this peer key from other peers&apos;{" "}
          <code className="text-paper">authorized_keys</code>. Also remove local
          seed pins. Read <ExtLink href={CONFIG_URL}>docs/configuration.md</ExtLink>,{" "}
          <ExtLink href={USAGE_URL}>docs/usage.md</ExtLink>, and{" "}
          <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink>.
        </p>
      </Section>

      <Section>
        <h2 className="text-2xl text-paper">Bugs and vulnerabilities</h2>
        <p className="mt-4 text-sm text-paper/85">
          Report unexpected behavior on{" "}
          <ExtLink href={ISSUES_URL}>GitHub issues</ExtLink>.
        </p>
        <p className="mt-3 text-sm text-paper/85">
          Report a vulnerability as specified in{" "}
          <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink>. Open a{" "}
          <ExtLink href={SECURITY_ADVISORY_URL}>
            private GitHub security advisory
          </ExtLink>
          . Do not put vulnerability details in a public issue.
        </p>
        <p className="mt-8">
          <Link href="/" className="text-signal">
            Home
          </Link>
        </p>
      </Section>
    </>
  );
}
