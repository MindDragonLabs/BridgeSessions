import Link from "next/link";
import { Command } from "@/components/Blocks";
import {
  CLI,
  INSTALL_PS1,
  INSTALL_SH,
  RELEASE_TAG_URL,
  SHIPPING_ARCH_LINE,
  VERSION,
} from "@/lib/product";

const DOES = [
  "The session stays up after you close the laptop.",
  "You run a command or a script on the other machine.",
  "You move a file, a log, or a screenshot through the same program.",
  "On a Mac or Windows desktop, you can see the screen and click.",
  "An agent can do those same things. Claude, Codex, and OpenCode have a skill in the repo.",
];

export default function Home() {
  return (
    <div className="mx-auto max-w-3xl px-4 py-16 sm:px-6 sm:py-24">
      <p className="mb-8 font-mono text-[11px] tracking-[0.14em] text-mute uppercase">
        {VERSION}
      </p>

      <h1 className="font-display text-[2.6rem] leading-[1.05] text-paper sm:text-6xl">
        One program.
        <br />
        Every computer you already have.
      </h1>

      <p className="mt-8 max-w-xl text-lg leading-relaxed text-paper">
        You and your AI agent use the same tool to work on another machine. Open
        a terminal. Leave the job running when you disconnect. Send a file back.
        Look at the screen. Click if you have to.
      </p>

      <p className="mt-6 max-w-xl text-[15px] leading-relaxed text-steel">
        A few machines means a pile of tools. A remote terminal. Something to
        keep the session alive. Something to copy files. Something else for
        Windows. Your agent is stuck on its own computer the second the real
        work is somewhere else.
      </p>

      <p className="mt-6 max-w-xl text-[15px] leading-relaxed text-steel">
        SSH is fine. This is the extra layer: sessions, files, desktop, agents,
        mixed OS. Run this over there. Keep it alive. Move the file. Show me the
        screen. Click that. Bring it back.
      </p>

      <h2 className="mt-16 font-display text-3xl text-paper">What it does</h2>
      <ul className="mt-6 max-w-xl space-y-3 text-[15px] leading-relaxed text-paper">
        {DOES.map((line) => (
          <li key={line} className="border-l border-signal pl-4">
            {line}
          </li>
        ))}
      </ul>

      <section id="install" className="mt-16 scroll-mt-24">
        <h2 className="font-display text-3xl text-paper">Install</h2>
        <p className="mt-4 max-w-xl text-[15px] leading-relaxed text-steel">
          One path. Then check that it works.
        </p>
        <p className="mt-6 font-mono text-[11px] tracking-[0.12em] text-mute uppercase">
          Linux and Mac
        </p>
        <Command text={INSTALL_SH} />
        <p className="mt-6 font-mono text-[11px] tracking-[0.12em] text-mute uppercase">
          Windows
        </p>
        <Command text={INSTALL_PS1} />
        <p className="mt-6 font-mono text-[11px] tracking-[0.12em] text-mute uppercase">
          Then
        </p>
        <Command text={`${CLI} --version`} />
        <p className="mt-6 text-[15px] leading-relaxed text-steel">
          Builds for {SHIPPING_ARCH_LINE}. Get them from the{" "}
          <a href={RELEASE_TAG_URL} rel="noopener noreferrer" className="text-paper underline">
            GitHub release
          </a>
          . Next steps live on{" "}
          <Link href="/install" className="text-paper underline">
            the install page
          </Link>
          .
        </p>
      </section>
    </div>
  );
}
