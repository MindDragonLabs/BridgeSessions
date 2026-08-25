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
  RELEASE_TAG_URL,
  RELEASES_URL,
  SECURITY_ADVISORY_URL,
  SECURITY_MD_URL,
  SHA256SUMS_URL,
  STATE_DIR,
  USAGE_URL,
  VERSION,
  WINDOWS_BIN,
} from "@/lib/product";

export const metadata: Metadata = {
  title: `Install ${PRODUCT} ${VERSION}`,
  description: `Install the current GitHub release ${VERSION}. One answer for Linux, macOS, and Windows.`,
};

export default function InstallPage() {
  return (
    <>
      <Section>
        <Eyebrow>03 / Conversion path</Eyebrow>
        <p className="mt-4 inline-flex items-center gap-2 rounded-[3px] border border-ember/40 bg-ember/10 px-2 py-1 font-mono text-[11px] tracking-[0.12em] text-ember uppercase">
          Beta {VERSION}
        </p>
        <h1 className="font-display mt-6 max-w-[16ch] text-[2.6rem] leading-[0.95] text-paper sm:text-5xl">
          Install the current GitHub release.
        </h1>
        <p className="mt-4 max-w-2xl text-base leading-relaxed text-paper/85">
          This is the one install answer. GitHub is the only primary source.
          Installers fail closed unless the GitHub Release{" "}
          <code className="text-paper">SHA256SUMS</code> entry and embedded
          version match.
        </p>
      </Section>

      <Section>
        <Eyebrow>Linux / macOS</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">One curl.</h2>
        <div className="mt-6">
          <Command caption="Installer" code={INSTALL_SH} />
        </div>
        <p className="mt-4 text-sm text-steel">
          Places <code className="text-paper">{LINUX_MAC_BIN}</code> and{" "}
          <code className="text-paper">{LINUX_MAC_CLI}</code>.
        </p>
      </Section>

      <Section>
        <Eyebrow>Windows</Eyebrow>
        <h2 className="mt-3 text-2xl text-paper">One PowerShell line.</h2>
        <div className="mt-6">
          <Command caption="PowerShell" code={INSTALL_PS1} />
        </div>
        <p className="mt-4 text-sm text-steel">
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
          {VERSION} release and check the published checksums.
        </p>
        <ul className="mt-6 space-y-2 text-sm">
          <li>
            Release tag: <ExtLink href={RELEASE_TAG_URL}>{RELEASE_TAG_URL}</ExtLink>
          </li>
          <li>
            All releases: <ExtLink href={RELEASES_URL}>{RELEASES_URL}</ExtLink>
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
        <h2 className="mt-3 text-2xl text-paper">Point at the repo docs.</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          There is no dedicated uninstall script in the repository. This page
          does not invent one.
        </p>
        <ul className="mt-6 max-w-3xl space-y-2 text-sm text-paper/85">
          <li>
            Linux/macOS binary:{" "}
            <code className="text-paper">{LINUX_MAC_BIN}</code> /{" "}
            <code className="text-paper">{LINUX_MAC_CLI}</code>
          </li>
          <li>
            Windows binary: <code className="text-paper">{WINDOWS_BIN}</code>
          </li>
          <li>
            State, keys, and tokens:{" "}
            <code className="text-paper">{STATE_DIR}</code>
          </li>
          <li>
            Leaving a mesh is membership — pinned seeds and inbound{" "}
            <code className="text-paper">authorized_keys</code>. Start from{" "}
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
