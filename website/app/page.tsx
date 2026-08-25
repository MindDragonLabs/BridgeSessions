import Link from "next/link";
import { Card, Command, ExtLink, Section } from "@/components/Blocks";
import {
  AGENT_SKILL_INSTALL_URL,
  AUDIT_URL,
  BETA_BOUNDARY,
  BINARY,
  CLI,
  CUA_URL,
  DOCS_SITE,
  LANGUAGE,
  LICENSE_CHANGE_DATE,
  LICENSE_CHANGE_TO,
  LICENSE_SHORT,
  LICENSE_URL,
  PANEL_URL,
  PRODUCT,
  RELEASE_TAG_URL,
  REPO_URL,
  SHIPPING_ARCH_LINE,
  SHIPPING_ASSETS,
  SECURITY_MD_URL,
  SKILL_URL,
  STATE_DIR,
  TLS_DETAIL,
  TRANSPORT,
  VERSION,
} from "@/lib/product";

export default function Home() {
  return (
    <>
      <section className="grid-field scroll-mt-24 border-b border-border">
        <div className="mx-auto max-w-6xl px-4 py-16 sm:px-6 sm:py-20">
          <p className="inline-flex items-center gap-2 rounded-[3px] border border-ember/40 bg-ember/10 px-2 py-1 font-mono text-[11px] tracking-[0.12em] text-ember uppercase">
            {VERSION}
          </p>
          <h1 className="font-display mt-6 text-[3.1rem] leading-[0.92] text-paper sm:text-6xl">
            {PRODUCT}
          </h1>
          <p className="mt-6 max-w-xl text-base leading-relaxed text-paper/85">
            {PRODUCT} is one {LANGUAGE} program. The binary name is {BINARY}.
            The command name is {CLI}.
          </p>
          <p className="mt-3 max-w-xl text-sm leading-relaxed text-steel">
            The program starts shells, sends files, and controls a desktop on a
            pinned peer mesh.
          </p>
          <p className="mt-3 max-w-xl text-sm leading-relaxed text-steel">
            This release has three artifacts: {SHIPPING_ARCH_LINE}.
          </p>
          <p className="mt-3 max-w-xl text-sm leading-relaxed text-steel">
            The source and the release are on GitHub. The mesh uses {TRANSPORT}.{" "}
            {TLS_DETAIL}
          </p>
          <p className="mt-3 max-w-xl text-sm leading-relaxed text-steel">
            The license is {LICENSE_SHORT}. {LICENSE_SHORT} is source-available.
            {LICENSE_SHORT} is not an Open Source license. The license changes
            to {LICENSE_CHANGE_TO} on {LICENSE_CHANGE_DATE}.
          </p>
          <p className="mt-3 max-w-xl text-sm leading-relaxed text-steel">
            {BETA_BOUNDARY}
          </p>
          <div className="mt-8">
            <Link
              href="/install"
              className="inline-flex rounded-[3px] bg-signal px-4 py-2.5 font-mono text-[12px] tracking-[0.12em] text-signal-ink uppercase"
            >
              Install
            </Link>
          </div>
          <dl className="mt-10 grid gap-3 border-t border-border pt-6 sm:grid-cols-2 lg:grid-cols-4">
            <Fact label="Version" value={VERSION} />
            <Fact label="Transport" value={TRANSPORT} />
            <Fact label="TLS" value={TLS_DETAIL} />
            <Fact label="Artifacts" value={SHIPPING_ARCH_LINE} />
          </dl>
          <p className="mt-6 text-sm text-steel">
            GitHub: <ExtLink href={REPO_URL}>{REPO_URL}</ExtLink>
          </p>
          <p className="mt-2 text-sm text-steel">
            Release: <ExtLink href={RELEASE_TAG_URL}>{RELEASE_TAG_URL}</ExtLink>
          </p>
          <p className="mt-2 text-sm text-steel">
            License: <ExtLink href={LICENSE_URL}>{LICENSE_URL}</ExtLink>
          </p>
        </div>
      </section>

      <Section id="security">
        <h2 className="font-display text-4xl text-paper">Security</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          {BETA_BOUNDARY}
        </p>
        <div className="mt-8 grid gap-3 md:grid-cols-2">
          <Card title="Authorized key">
            A key in <code className="text-paper">authorized_keys</code> gives
            near-interactive access. Authorization is at peer level. It is not
            per command.
          </Card>
          <Card title="Ed25519 pins">
            Seed and direct connections need explicit Ed25519 pins. The
            certificate key, Hello key, and configured pin must agree.
          </Card>
          <Card title="Invite">
            An invite window accepts an unknown certificate only to submit one
            token. Only a pinned seed can issue a mesh enrollment.
          </Card>
          <Card title="Local IPC">
            Local daemon IPC uses loopback and a token. The default port is
            19980. Do not use local IPC as the mesh trust root.
          </Card>
        </div>
        <p className="mt-6 text-sm text-steel">
          Read <ExtLink href={SECURITY_MD_URL}>SECURITY.md</ExtLink>.{" "}
          <ExtLink href={AUDIT_URL}>AUDIT.md</ExtLink> is an internal review. It
          is not a third-party audit.
        </p>
      </Section>

      <Section id="cua">
        <h2 className="font-display text-4xl text-paper">CUA</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          The command <code className="text-paper">{CLI} cua</code> captures the
          desktop of a trusted peer. It also sends mouse and keyboard input.
        </p>
        <div className="mt-8 grid gap-3 md:grid-cols-2">
          <Card title="Helper">
            On Windows and macOS, start one{" "}
            <code className="text-paper">{BINARY} --cua-helper</code> in the
            user session. Do not start a second helper on the same token file.
          </Card>
          <Card title="macOS">
            Give Screen Recording and Accessibility to the signed binary.
            Restart the helper after you change permissions.
          </Card>
          <Card title="Spectator">
            A spectator attachment cannot send CUA input. This includes video
            capture.
          </Card>
          <Card title="Helper IPC">
            The helper listens on loopback. The default port is 19986. Each
            request uses an owner-only token in{" "}
            <code className="text-paper">{STATE_DIR}</code>.
          </Card>
        </div>
        <p className="mt-6 text-sm text-steel">
          Capture the screen before you click. Read{" "}
          <ExtLink href={CUA_URL}>docs/cua.md</ExtLink>.
        </p>
      </Section>

      <Section id="panel">
        <h2 className="font-display text-4xl text-paper">Bridge Panel</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          Bridge Panel is a local web UI for Markdown and session output. It is
          optional. It is not a mesh trust root.
        </p>
        <div className="mt-8 grid gap-4 lg:grid-cols-2">
          <Command caption="Start Bridge Panel" code="python3 -m tools.bridgepanel" />
          <Command
            caption="Publish a document"
            code={`${CLI} pane publish report.md --session default --type documents --title 'Audit report'`}
          />
        </div>
        <p className="mt-6 max-w-3xl text-sm leading-relaxed text-paper/85">
          The default bind address is 127.0.0.1:9770. Writes need a bearer
          token. Do not expose Bridge Panel to the internet.
        </p>
        <p className="mt-3 text-sm text-steel">
          Read <ExtLink href={PANEL_URL}>docs/bridge-panel.md</ExtLink>.
        </p>
      </Section>

      <Section id="agents">
        <h2 className="font-display text-4xl text-paper">Agents</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          An agent uses the same {CLI} commands as a person. The repository
          skill is{" "}
          <ExtLink href={SKILL_URL}>skills/bridgesessions/SKILL.md</ExtLink>.
        </p>
        <p className="mt-4 text-sm text-steel">
          Install skill links with{" "}
          <ExtLink href={AGENT_SKILL_INSTALL_URL}>
            scripts/install-agent-skill.sh
          </ExtLink>
          . Operator rules are in <code className="text-paper">AGENTS.md</code>.
        </p>
      </Section>

      <Section>
        <h2 className="font-display text-4xl text-paper">Docs</h2>
        <p className="mt-3 max-w-2xl text-sm leading-relaxed text-steel">
          Operator docs are in <code className="text-paper">docs/</code> on
          GitHub and at <ExtLink href={DOCS_SITE}>{DOCS_SITE}</ExtLink>.
        </p>
        <p className="mt-4 text-sm text-steel">
          Artifacts in this release: {SHIPPING_ASSETS.join(", ")}.
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
