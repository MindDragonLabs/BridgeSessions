import Link from "next/link";
import { Card, Command, ExtLink, Eyebrow, Section } from "@/components/Blocks";
import {
  AGENT_SKILL_INSTALL_URL,
  AUDIT_URL,
  BETA_BOUNDARY,
  BINARY,
  CLI,
  CONFIG_URL,
  CUA_URL,
  DESIGN_URL,
  DOCS_SITE,
  INSTALL_PS1,
  INSTALL_SH,
  ISSUES_URL,
  LANGUAGE,
  LICENSE_LINE,
  LICENSE_URL,
  LINUX_MAC_BIN,
  LINUX_MAC_CLI,
  PANEL_URL,
  PRODUCT,
  PROVENANCE_URL,
  QUICKSTART_URL,
  RELEASE_TAG_URL,
  RELEASES_URL,
  REPO_URL,
  SECURITY_ADVISORY_URL,
  SECURITY_MD_URL,
  SHA256SUMS_URL,
  SKILL_URL,
  STATE_DIR,
  TLS_DETAIL,
  TRANSPORT,
  USAGE_URL,
  VERSION,
  WINDOWS_BIN,
  WORKSTREAMS,
} from "@/lib/product";

export default function Home() {
  return (
    <>
      <section id="positioning" className="grid-field scroll-mt-24 border-b border-border">
        <div className="mx-auto max-w-6xl px-4 py-16 sm:px-6 sm:py-20">
          <Eyebrow>02 / Positioning</Eyebrow>
          <p className="mt-4 inline-flex items-center gap-2 rounded-[3px] border border-ember/40 bg-ember/10 px-2 py-1 font-mono text-[11px] tracking-[0.12em] text-ember uppercase">
            Beta {VERSION}
          </p>
          <h1 className="font-display mt-6 max-w-[14ch] text-[3.1rem] leading-[0.92] text-paper sm:text-6xl">
            One binary.
            <br />
            Every machine
            <br />
            you already own.
          </h1>
          <p className="mt-6 max-w-xl text-base leading-relaxed text-paper/85 sm:text-lg">
            {PRODUCT} is the control layer for machines you already operate. One{" "}
            {LANGUAGE} executable — {BINARY} / {CLI} — for humans and agents
            across Linux, macOS, and Windows.
          </p>
          <p className="mt-4 max-w-xl text-sm leading-relaxed text-steel">
            Persistent shells, verified files, and computer-use automation share
            one pinned peer identity. Not enterprise IAM. Not hostile
            multi-tenant access.
          </p>
          <div className="mt-8">
            <Link
              href="/install"
              className="inline-flex rounded-[3px] bg-signal px-4 py-2.5 font-mono text-[12px] tracking-[0.12em] text-signal-ink uppercase"
            >
              Try the release
            </Link>
          </div>
          <dl className="mt-10 grid gap-3 border-t border-border pt-6 sm:grid-cols-2 lg:grid-cols-4">
            <Fact label="Version" value={VERSION} />
            <Fact label="Transport" value={TRANSPORT} />
            <Fact label="Compatibility" value={TLS_DETAIL} />
            <Fact label="Platforms" value="Linux · macOS · Windows" />
          </dl>
        </div>
      </section>

      <Section id="map">
        <Eyebrow>05 / Find it</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Five workstreams, on this site.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          Homepage plus a focused <Link href="/install" className="text-signal">/install</Link>{" "}
          page. Every card below is a real destination.
        </p>
        <ol className="mt-8 grid gap-3 sm:grid-cols-2 lg:grid-cols-5">
          {WORKSTREAMS.map((item) => (
            <li key={item.id}>
              <Link
                href={item.href}
                className="block h-full rounded-[3px] border border-border bg-panel p-4 hover:border-signal/50"
              >
                <p className="font-mono text-[11px] tracking-[0.16em] text-signal uppercase">
                  {item.num}
                </p>
                <h3 className="mt-2 text-base text-paper">{item.title}</h3>
                <p className="mt-2 text-sm leading-relaxed text-steel">{item.brief}</p>
              </Link>
            </li>
          ))}
        </ol>
        <ul className="mt-8 grid gap-2 font-mono text-[12px] text-steel sm:grid-cols-2">
          <li>
            <Link href="/install" className="text-signal">
              /install
            </Link>{" "}
            — one install answer
          </li>
          <li>
            <a href="#demo" className="text-signal">
              #demo
            </a>{" "}
            — 15–25s placeholder + wow path
          </li>
          <li>
            <a href="#security" className="text-signal">
              #security
            </a>{" "}
            — trust model
          </li>
          <li>
            <a href="#limitations" className="text-signal">
              #limitations
            </a>{" "}
            — beta boundaries
          </li>
          <li>
            <a href="#cua" className="text-signal">
              #cua
            </a>{" "}
            — desktop permissions
          </li>
          <li>
            <a href="#panel" className="text-signal">
              #panel
            </a>{" "}
            — Bridge Panel
          </li>
          <li>
            <a href="#agents" className="text-signal">
              #agents
            </a>{" "}
            — Claude, Codex, OpenCode
          </li>
          <li>
            <a href="#recovery" className="text-signal">
              #recovery
            </a>{" "}
            — binary path and mesh exit
          </li>
        </ul>
      </Section>

      <Section id="product-truth">
        <Eyebrow>01 / Product truth</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          What ships today.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          One answer to “what do I install?”: the current GitHub release{" "}
          <span className="text-paper">{VERSION}</span>. GitHub is the only
          primary repo and release channel.
        </p>
        <div className="mt-8 grid gap-3 md:grid-cols-2">
          <Card title="Version" kicker="Badge">
            <p className="font-mono text-signal">{VERSION}</p>
            <p className="mt-2">
              Public beta. Probe live with{" "}
              <code className="text-paper">{CLI} --version</code>.
            </p>
          </Card>
          <Card title="Transport" kicker="Mesh">
            <p>{TRANSPORT}.</p>
            <p className="mt-2">{TLS_DETAIL}</p>
          </Card>
          <Card title="Install answer" kicker="One path">
            <p>
              Linux/macOS and Windows installer commands live on{" "}
              <Link href="/install" className="text-signal">
                /install
              </Link>
              . Manual binaries and{" "}
              <ExtLink href={SHA256SUMS_URL}>SHA256SUMS</ExtLink> are GitHub
              Release assets for {VERSION}.
            </p>
          </Card>
          <Card title="License" kicker="Source-available">
            <p>
              <ExtLink href={LICENSE_URL}>{LICENSE_LINE}</ExtLink>
            </p>
          </Card>
        </div>
        <p className="mt-6 text-sm text-steel">
          Repo: <ExtLink href={REPO_URL}>{REPO_URL}</ExtLink>
          <span className="mx-2">·</span>
          Releases: <ExtLink href={RELEASES_URL}>{RELEASES_URL}</ExtLink>
          <span className="mx-2">·</span>
          Docs: <ExtLink href={DOCS_SITE}>{DOCS_SITE}</ExtLink>
        </p>
      </Section>

      <Section id="conversion">
        <Eyebrow>03 / Conversion path</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Install the current GitHub release.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          One primary CTA. Then verify, join a pinned seed, and try the
          impressive thing: persistent reattach, a verified file move, or a CUA
          capture.
        </p>
        <div className="mt-8 grid gap-4 lg:grid-cols-2">
          <Command caption="Linux / macOS" code={INSTALL_SH} />
          <Command caption="Windows PowerShell" code={INSTALL_PS1} />
        </div>
        <p className="mt-6">
          <Link
            href="/install"
            className="inline-flex rounded-[3px] bg-signal px-4 py-2.5 font-mono text-[12px] tracking-[0.12em] text-signal-ink uppercase"
          >
            Try the release
          </Link>
        </p>
        <p className="mt-4 text-sm text-steel">
          After install: <code className="text-paper">{CLI} --version</code> and{" "}
          <code className="text-paper">{CLI} doctor</code>. Full join and wow
          path on <Link href="/install" className="text-signal">/install</Link>.
          Problems: <ExtLink href={ISSUES_URL}>GitHub issues</ExtLink>. Vulns:{" "}
          <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink>, not a public
          issue.
        </p>
      </Section>

      <Section id="demo" className="bg-panel/40">
        <Eyebrow>Demo</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Fifteen to twenty-five seconds.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          Labeled placeholder. No video file is shipped with this site. Run the
          wow path on a mesh you control.
        </p>
        <div
          className="mt-8 flex min-h-[220px] flex-col justify-between rounded-[3px] border border-dashed border-border bg-ink p-5 sm:min-h-[280px] sm:p-8"
          aria-label="15 to 25 second demo placeholder"
        >
          <p className="font-mono text-[11px] tracking-[0.18em] text-ember uppercase">
            Demo slot · 15–25s · placeholder · no video file
          </p>
          <p className="font-display max-w-[18ch] text-3xl text-paper sm:text-4xl">
            Reattach. Move a file. Capture a screen.
          </p>
          <p className="max-w-xl text-sm text-steel">
            The remote daemon owns the PTY. <code className="text-paper">Ctrl-D</code>{" "}
            detaches. Reuse <code className="text-paper">--name</code> to come
            back. Files wait for a final <code className="text-paper">OK</code>{" "}
            after SHA-256. Capture before you click.
          </p>
        </div>
        <div className="mt-6 grid gap-4 lg:grid-cols-3">
          <Command
            caption="Persistent reattach"
            code={`${CLI} shell <peer> --name agent\n# Ctrl-D detaches\n${CLI} shell <peer> --name agent`}
          />
          <Command
            caption="Verified file move"
            code={`${CLI} file send <peer> ./artifact.bin --wait`}
          />
          <Command
            caption="CUA capture"
            code={`${CLI} cua capture <peer> -o screen.png`}
          />
        </div>
      </Section>

      <Section id="security">
        <Eyebrow>04 / Trust and proof</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Trust model.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          {BETA_BOUNDARY}
        </p>
        <div className="mt-8 grid gap-3 md:grid-cols-2">
          <Card title="Authorized key" kicker="Host access">
            A key in <code className="text-paper">authorized_keys</code> has
            near-interactive host access. Authorization is host-level, not
            per-command.
          </Card>
          <Card title="Pinned mesh" kicker="Ed25519">
            Seed and direct connections require explicit Ed25519 pins.
            Certificate key, Hello key/name, and configured pin must agree.
          </Card>
          <Card title="Invite window" kicker="Join">
            A bounded invite window temporarily accepts an unknown certificate
            only to submit a single-use token. Only pinned seeds may issue
            accepted mesh-wide enrollments. See{" "}
            <ExtLink href={QUICKSTART_URL}>Quickstart</ExtLink>.
          </Card>
          <Card title="Loopback IPC" kicker="Local daemon">
            Local daemon IPC is loopback-only and token-authenticated. Default
            loopback port is 19980. Do not treat local IPC as a mesh trust
            root.
          </Card>
        </div>
        <p className="mt-6 text-sm text-steel">
          Policy: <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink>. Design
          limits: <ExtLink href={DESIGN_URL}>design.md</ExtLink>.
        </p>
      </Section>

      <Section id="limitations">
        <Eyebrow>Limitations / beta</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Beta on purpose.
        </h2>
        <ul className="mt-6 max-w-3xl space-y-3 text-sm leading-relaxed text-paper/85">
          <li>
            {PRODUCT} targets small, operator-controlled meshes — not internet
            scale.
          </li>
          <li>
            Peer authorization is host-level, not capability-scoped. A
            compromised authorized node can affect nodes that trust it.
          </li>
          <li>
            {TLS_DETAIL} Keys remain explicitly pinned.
          </li>
          <li>
            CUA depends on a user-session helper and OS permissions on Windows
            and macOS.
          </li>
          <li>
            Beta releases require active upgrade discipline. Installers require
            a matching GitHub Release <code className="text-paper">SHA256SUMS</code>{" "}
            entry and embedded version before replacement.
          </li>
        </ul>
        <p className="mt-6 text-sm text-steel">
          <ExtLink href={AUDIT_URL}>AUDIT.md</ExtLink> is an internal review of
          the shipping tree. It is not a third-party audit. Release verification
          evidence lives in{" "}
          <ExtLink href={PROVENANCE_URL}>docs/RELEASE-PROVENANCE.md</ExtLink>{" "}
          and the GitHub Release{" "}
          <ExtLink href={SHA256SUMS_URL}>SHA256SUMS</ExtLink> for {VERSION}.
        </p>
      </Section>

      <Section id="cua">
        <Eyebrow>CUA permissions</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Desktop control is host-level.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          <code className="text-paper">{CLI} cua</code> captures and drives a
          trusted peer&apos;s interactive desktop. A trusted peer can observe or
          control that desktop. CUA is not a low-privilege capability. Details
          in <ExtLink href={CUA_URL}>docs/cua.md</ExtLink>.
        </p>
        <div className="mt-8 grid gap-3 md:grid-cols-2">
          <Card title="Spectators" kicker="Input deny">
            Spectator attachments cannot send CUA input, including video
            capture.
          </Card>
          <Card title="Windows / macOS helper" kicker="User session">
            Start one <code className="text-paper">{BINARY} --cua-helper</code>{" "}
            in the interactive user session. Do not run duplicate helpers
            against one token file.
          </Card>
          <Card title="macOS TCC" kicker="Signed binary">
            Grant Screen Recording (capture/video) and Accessibility
            (mouse/keyboard) to the installed, Developer ID-signed app or
            binary. Restart the helper after changing permissions.
          </Card>
          <Card title="Helper IPC" kicker="Loopback">
            The helper listens only on loopback (default 19986) and
            authenticates every request with an owner-only token under{" "}
            <code className="text-paper">{STATE_DIR}</code>.
          </Card>
        </div>
      </Section>

      <Section id="panel">
        <Eyebrow>Bridge Panel</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Optional Markdown review.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          Bridge Panel is a local web UI for reviewing Markdown and session
          output from humans or agents. It is optional. It is not a new trust
          root. See <ExtLink href={PANEL_URL}>docs/bridge-panel.md</ExtLink>.
        </p>
        <div className="mt-8 grid gap-4 lg:grid-cols-2">
          <Command caption="Start from the repo" code="python3 -m tools.bridgepanel" />
          <Command
            caption="Publish a document"
            code={`${CLI} pane publish report.md --session default --type documents --title 'Audit report'`}
          />
        </div>
        <ul className="mt-6 max-w-3xl space-y-2 text-sm text-paper/85">
          <li>Binds 127.0.0.1:9770 by default.</li>
          <li>Talks to the local daemon over token-authenticated loopback IPC.</li>
          <li>Writes require a generated bearer token.</li>
          <li>Do not expose the panel directly to the internet.</li>
        </ul>
      </Section>

      <Section id="agents">
        <Eyebrow>Agent path</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Same CLI as a human.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          An agent on a machine you own can attach, move an artifact, and
          capture a desktop through the same pinned peer. Repo skills for
          Claude, Codex, and OpenCode live in{" "}
          <ExtLink href={SKILL_URL}>skills/bridgesessions/SKILL.md</ExtLink>.
        </p>
        <Command
          caption="One command each — shell, file, capture"
          code={`${CLI} shell <peer> --cmd 'hostname && uptime'\n${CLI} file send <peer> ./artifact.bin --wait\n${CLI} cua capture <peer> -o screen.png`}
        />
        <p className="mt-6 text-sm text-steel">
          Install harness links with{" "}
          <ExtLink href={AGENT_SKILL_INSTALL_URL}>
            scripts/install-agent-skill.sh
          </ExtLink>
          . Always-on operator rules: <code className="text-paper">AGENTS.md</code>.
        </p>
      </Section>

      <Section id="recovery">
        <Eyebrow>Uninstall / recovery</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Where the bits live.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          The repo does not ship a dedicated uninstall script. Do not invent
          one. Use the installer sources and product docs.
        </p>
        <div className="mt-8 grid gap-3 md:grid-cols-2">
          <Card title="Linux / macOS binary" kicker="Installer path">
            <p>
              <code className="text-paper">{LINUX_MAC_BIN}</code> with a{" "}
              <code className="text-paper">{LINUX_MAC_CLI}</code> symlink.
              Platform service units are generated by{" "}
              <code className="text-paper">scripts/install.sh</code>.
            </p>
          </Card>
          <Card title="Windows binary" kicker="Installer path">
            <p>
              <code className="text-paper">{WINDOWS_BIN}</code>. State still
              uses the user profile{" "}
              <code className="text-paper">{STATE_DIR}</code> directory.
            </p>
          </Card>
          <Card title="State directory" kicker="Config and keys">
            <p>
              <code className="text-paper">{STATE_DIR}</code> holds config,
              identity, <code className="text-paper">authorized_keys</code>,
              sessions, received files, and IPC tokens. See{" "}
              <ExtLink href={CONFIG_URL}>docs/configuration.md</ExtLink>.
            </p>
          </Card>
          <Card title="Leaving a mesh" kicker="Membership">
            <p>
              Mesh membership is the pinned seeds and inbound{" "}
              <code className="text-paper">authorized_keys</code> in that state
              directory. Read{" "}
              <ExtLink href={CONFIG_URL}>Configuration</ExtLink>,{" "}
              <ExtLink href={USAGE_URL}>Usage</ExtLink>, and{" "}
              <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink> instead of a
              made-up leave command.
            </p>
          </Card>
        </div>
        <p className="mt-6 text-sm text-steel">
          Bugs: <ExtLink href={ISSUES_URL}>GitHub issues</ExtLink>.
          Vulnerabilities:{" "}
          <ExtLink href={SECURITY_ADVISORY_URL}>private GitHub advisory</ExtLink>{" "}
          as described in <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink>
          — do not file vuln details as a public issue.
        </p>
      </Section>

      <Section>
        <Eyebrow>Docs</Eyebrow>
        <h2 className="font-display mt-3 text-4xl text-paper">
          Product docs stay in the repo.
        </h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          This marketing tree is not a cutover of a public domain. Canonical
          operator docs: <ExtLink href={DOCS_SITE}>{DOCS_SITE}</ExtLink> and{" "}
          <code className="text-paper">docs/</code> on GitHub.
        </p>
        <p className="mt-6 text-sm">
          <ExtLink href={QUICKSTART_URL}>Quickstart</ExtLink>
          <span className="mx-2 text-steel">·</span>
          <ExtLink href={USAGE_URL}>Usage</ExtLink>
          <span className="mx-2 text-steel">·</span>
          <ExtLink href={RELEASE_TAG_URL}>Release {VERSION}</ExtLink>
        </p>
      </Section>
    </>
  );
}

function Fact({ label, value }: { label: string; value: string }) {
  return (
    <div>
      <dt className="font-mono text-[11px] tracking-[0.16em] text-steel uppercase">
        {label}
      </dt>
      <dd className="mt-1 text-sm text-paper">{value}</dd>
    </div>
  );
}
